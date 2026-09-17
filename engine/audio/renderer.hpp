#pragma once
#include "audio/effect.hpp"
#include "audio/loudness.hpp"
#include "domain/session.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
namespace daw {
// Runtime-only latency supplied by graph nodes outside the renderer. It is
// deliberately separate from State; Renderer adds the measured latency of
// persisted Track/Bus insert chains itself. Empty vectors mean no additional
// node latency.
struct GraphLatencyPlan {
  std::vector<uint32_t> trackNodeFrames;
  std::vector<uint32_t> busNodeFrames;
};
// The graph keeps the finite and unbounded parts of a plug-in tail separate.
// `finiteFrames` is bounded by the renderer's 30-second safety ceiling.
struct GraphTailSummary {
  uint32_t finiteFrames = 0;
  bool hasInfiniteTail = false;
  bool operator==(const GraphTailSummary &) const = default;
};
GraphTailSummary serialTail(GraphTailSummary left,
                            GraphTailSummary right) noexcept;
GraphTailSummary parallelTail(GraphTailSummary left,
                              GraphTailSummary right) noexcept;
// One scheduled channel-voice message on the project timeline (absolute
// frames). The Renderer builds these from Track::midiClips in prepare().
struct MidiPlannedEvent {
  uint64_t frame = 0;
  PreparedMidiEvent event;
};
// Per-track note plan: sorted on/off pairs drained over contiguous render
// ranges with a monotonic cursor. Built on the control thread; the cursor and
// sounding state are only mutated by the audio callback, which is the single
// reader per the Renderer contract above. Deterministic rules:
//  - note on at clip.start + note.start, note off at clip.start +
//    note.start + note.length; at an equal frame the off sorts first;
//  - drain() emits events with frame in [blockStart, blockEnd) at
//    sampleOffset = frame - blockStart; events past the capacity stay at the
//    cursor and are emitted next block clamped to offset 0 (late, never lost);
//  - on a loop wrap wrapCutOffs() emits one velocity-0 note off at offset 0
//    per still-sounding pitch (its off lay beyond loopEnd, unreachable in
//    looped playback), then reset(loopBegin) rewinds so those notes restart
//    on the next pass: no stuck notes and no clipped chords.
class MidiTrackPlan {
 public:
  void build(const std::vector<MidiClip> &clips) {
    events_.clear();
    cursor_ = 0;
    sounding_.fill(0);
    for (const auto &clip : clips)
      for (const auto &note : clip.notes) {
        events_.push_back({clip.start + note.start,
                           {0U, note.channel, note.pitch, note.velocity, false}});
        events_.push_back({clip.start + note.start + note.length,
                           {0U, note.channel, note.pitch, 0U, true}});
      }
    std::sort(events_.begin(), events_.end(), [](const auto &a, const auto &b) {
      if (a.frame != b.frame) return a.frame < b.frame;
      return a.event.noteOff && !b.event.noteOff;
    });
  }
  // Rewind the cursor to the first event at or after position; clears sound.
  void reset(uint64_t position) {
    cursor_ = static_cast<size_t>(
        std::lower_bound(events_.begin(), events_.end(), position,
                         [](const MidiPlannedEvent &e, uint64_t p) {
                           return e.frame < p;
                         }) -
        events_.begin());
    sounding_.fill(0);
  }
  bool empty() const noexcept { return events_.empty(); }
  uint32_t drain(uint64_t blockStart, uint64_t blockEnd,
                 PreparedMidiEvent *out, uint32_t capacity) {
    uint32_t written = 0;
    while (cursor_ < events_.size() && events_[cursor_].frame < blockEnd) {
      if (written == capacity) break; // carried, not lost
      const auto &planned = events_[cursor_];
      auto event = planned.event;
      event.sampleOffset = planned.frame > blockStart
                               ? static_cast<uint32_t>(planned.frame - blockStart)
                               : 0U;
      touch(event);
      out[written++] = event;
      ++cursor_;
    }
    return written;
  }
  uint32_t wrapCutOffs(PreparedMidiEvent *out, uint32_t capacity) {
    uint32_t written = 0;
    for (unsigned bit = 0; bit < 2048U && written < capacity; ++bit) {
      if (!(sounding_[bit >> 6] & (1ULL << (bit & 63U)))) continue;
      sounding_[bit >> 6] &= ~(1ULL << (bit & 63U));
      out[written++] = {0U, static_cast<uint8_t>(bit >> 7),
                        static_cast<uint8_t>(bit & 127U), 0U, true};
    }
    return written;
  }

 private:
  void touch(const PreparedMidiEvent &e) {
    const unsigned bit = (unsigned(e.channel & 15U) << 7) | unsigned(e.pitch & 127U);
    if (e.noteOff)
      sounding_[bit >> 6] &= ~(1ULL << (bit & 63U));
    else
      sounding_[bit >> 6] |= 1ULL << (bit & 63U);
  }
  std::vector<MidiPlannedEvent> events_;
  size_t cursor_ = 0;
  std::array<uint64_t, 32> sounding_{};
};
// One RT reader. prepare/reset only after output unit is stopped and
// uninitialized. Gain/telemetry atomics are the only concurrently accessed
// mutable state.
class Renderer {
  static constexpr size_t kRuntimeInsertCapacity = kMaxProjectPluginInserts;
  struct RuntimeInsertSlot {
    std::atomic<uint64_t> id{0};
    std::atomic<uint32_t> state{static_cast<uint32_t>(PreparedEffectRuntimeState::Unprepared)};
    std::atomic<uint32_t> extraPipelineLatency{0};
    std::atomic<uint32_t> faultCode{0};
  };
  struct Voice {
    size_t gainIndex = 0;
    std::shared_ptr<const Clip> clip;
    uint64_t start = 0, offset = 0, length = 0, fadeIn = 0, fadeOut = 0;
    // Per-region clip gain, pre-converted to linear. An untouched region
    // carries 0 dB and therefore multiplies by exactly 1.0.
    float gain = 1.0f;
    // Looped regions wrap reads inside [offset, offset+loopSpan); 0 is plain play.
    uint64_t loopSpan = 0;
    // Clip pan coefficients (same linear law as track pan; center = 1/1).
    float panLeft = 1.0f, panRight = 1.0f;
  };
  struct SendRoute {
    size_t track = 0, bus = 0;
    float gain = 0;
    bool preFader = false;
  };
  // One prepared edge delay. Storage is allocated/reset only by prepare();
  // process() performs bounded array reads/writes and never allocates.
  struct PdcDelay {
    std::vector<float> left, right;
    size_t cursor = 0;
    void configure(uint32_t frames);
    void process(float inputLeft, float inputRight, float &outputLeft,
                 float &outputRight) noexcept;
  };
  struct VolumeAutomationRoute {
    std::vector<AutomationPoint> points;
    size_t nextPoint = 0;
    uint64_t lastFrame = 0;
    bool hasLastFrame = false;
  };
  struct ParameterAutomationRoute {
    uint32_t parameterID = 0;
    std::vector<PluginParameterAutomationPoint> points;
    size_t nextPoint = 0;
    uint64_t lastFrame = 0;
    bool hasLastFrame = false;
  };
  using EffectAutomationPlan = std::vector<ParameterAutomationRoute>;
  using ChainAutomationPlan = std::vector<EffectAutomationPlan>;
  std::vector<Voice> voices;
  std::vector<size_t> activeGains;
  std::vector<VolumeAutomationRoute> volumeAutomation;
  std::vector<VolumeAutomationRoute> panAutomation, busGainAutomation;
  VolumeAutomationRoute masterGainAutomation;
  std::vector<int16_t> trackOutputs, busOutputs;
  std::vector<size_t> busOrder;
  std::vector<SendRoute> sends;
  // UI writes targets; only the render thread mutates smoothSendGains.
  // Capacity follows 256 tracks * 8 sends. Routes/IDs change only in prepare().
  std::array<std::atomic<float>, 256 * 8> sendGainTargets{};
  std::array<float, 256 * 8> smoothSendGains{};
  std::array<std::atomic<float>, 256 * 8> sendPanTargets{}, sendIndependentTargets{};
  std::array<float, 256 * 8> smoothSendPans{}, smoothSendIndependent{};
  std::array<float, 256> lastSendFaders{};
  static_assert(std::atomic<float>::is_always_lock_free);
  void processSend(size_t index,float inL,float inR,float postL,float postR,
                   float gate,float fader,float& outL,float& outR) noexcept;
  std::array<uint64_t, 256 * 8> sendTrackIDs{}, sendBusIDs{};
  // sendRanges[track]..sendRanges[track+1] indexes that track's sends.
  std::vector<size_t> sendRanges;
  std::vector<PdcDelay> trackMainPdc, sendPdc, busOutputPdc;
  // Owned, initialized only on the control thread. The callback traverses
  // these fixed chains but never constructs, destroys, or reconfigures them.
  std::vector<std::vector<std::unique_ptr<PreparedEffect>>> trackEffects,
      busEffects;
  std::vector<ChainAutomationPlan> trackEffectAutomation, busEffectAutomation;
  std::vector<std::vector<uint64_t>> trackEffectIDs, busEffectIDs;
  // Planar workspaces, allocated for the fixed maximum before playback.
  // Indexed as node * kRenderBlockFrames + frame.
  std::vector<float> trackBlockLeft, trackBlockRight, busBlockLeft,
      busBlockRight;
  std::vector<std::unique_ptr<PreparedEffect>> masterEffects;
  ChainAutomationPlan masterEffectAutomation;
  std::vector<uint64_t> masterEffectIDs;
  // Fixed slots are published only after preparation. Runtime readers only
  // perform atomic loads, while the callback can refresh a proxy's status.
  std::array<RuntimeInsertSlot, kRuntimeInsertCapacity> runtimeInserts{};
  // Allocated in prepare() to the persisted project-wide automation limit.
  // Callback code writes a prefix then passes it synchronously to one effect.
  std::vector<PreparedParameterEvent> parameterEvents;
  // Per-track MIDI plans, indexed by the same track ordinal as trackEffects.
  // The callback only mutates each plan's private cursor/sounding state.
  std::vector<MidiTrackPlan> midiPlans;
  static constexpr uint32_t kMaxMidiEventsPerBlock = 1024U;
  std::vector<PreparedMidiEvent> midiEvents;
  std::array<std::atomic<float>, 256> leftGains{}, rightGains{},
      preFaderGates{}, leftPans{}, rightPans{}, trackFaderGains{};
  std::array<float, 256> smoothLeft{}, smoothRight{}, smoothPreFaderGate{},
      smoothLeftPan{}, smoothRightPan{}, smoothTrackFader{};
  std::array<std::atomic<float>, 16> busLeftGains{}, busRightGains{},
      busFaderGains{}, busPans{}, busGates{};
  std::array<float, 16> smoothBusLeft{}, smoothBusRight{}, smoothBusFader{},
      smoothBusPan{}, smoothBusGate{};
  // The audio callback is the sole writer. UI/control readers consume the
  // latest whole-block peaks through lock-free atomics.
  std::array<std::atomic<float>, 256> trackMeterLeft{}, trackMeterRight{};
  std::array<std::atomic<float>, 16> busMeterLeft{}, busMeterRight{};
  std::atomic<float> masterMeterLeft{0}, masterMeterRight{0};
  // BS.1770-4 momentary/short-term master loudness, fed from the RT path.
  MasterLoudnessTracker loudnessTracker;
  uint32_t preparedTrackCount = 0, preparedBusCount = 0;
  std::atomic<float> masterGain{1};
  float smoothMaster = 1;
  std::atomic<uint32_t> masterLatency{0}, declaredFiniteTail{0};
  std::atomic<bool> declaredInfiniteTail{false};
  // Metronome: the audio callback reads only the atomic switch and the two
  // persisted timeline lanes copied by prepare(); the click is synthesized
  // post-master in renderInternal and skipped by the export path.
  std::atomic<bool> metronomeOn{false};
  // Timeline-only copy of the tempo/signature lanes prepared from State so the
  // beat math runs through the domain converters (State::frameAtBeats etc.)
  // instead of a duplicated arithmetic path. Control thread writes in prepare().
  State metronomeTimeline{};
  // cursor is the input/timeline cursor. Audible position trails it by the
  // published graph latency and is tracked independently for transport UI.
  uint64_t cursor = 0, length = 0, loopBegin = 0, loopEnd = 0, processTime = 0,
           transportStart = 0, renderedInputFrames = 0;
  bool looping = false;
  std::atomic<uint64_t> touchPluginID{0};
  std::atomic<uint32_t> touchParameterID{0};
  std::atomic<float> touchNormalized{0};
  std::atomic<bool> touchActive{false};
  float volumeAutomationGain(size_t track, uint64_t timeline) noexcept;
  double automationValue(VolumeAutomationRoute &, uint64_t timeline) noexcept;
  float parameterAutomationValue(ParameterAutomationRoute &,
                                 uint64_t timeline) noexcept;
  bool processChain(std::vector<std::unique_ptr<PreparedEffect>> &,
                    ChainAutomationPlan &, const std::vector<uint64_t> &, float *,
                    float *, uint32_t, uint64_t, uint64_t,
                    std::span<const PreparedMidiEvent> midi = {}) noexcept;
  void publishRuntimeStatus(uint64_t id,
                            PreparedEffectRuntimeStatus status) noexcept;
  // Shared playback loop. suppressMetronome removes the click track from an
  // otherwise sample-identical render; only the offline export path uses it.
  void renderInternal(float *left, float *right, uint32_t frames,
                      bool suppressMetronome) noexcept;
  void updateAudiblePosition() noexcept;
  void clearMeters() noexcept;

public:
  std::atomic<bool> playing{false};
  std::atomic<uint64_t> position{0}, callbacks{0}, clipped{0}, pluginErrors{0};
  std::atomic<uint64_t> audiblePosition{0};
  std::atomic<float> peak{0};
  void prepare(const State &, uint64_t startFrame = 0, uint64_t loopStart = 0,
               uint64_t loopEndFrame = 0);
  // Effect-host slices pass initialized track/bus latency here. This method
  // validates/allocates the PDC plan before the audio callback resumes.
  void prepare(const State &, const GraphLatencyPlan &, uint64_t startFrame = 0,
               uint64_t loopStart = 0, uint64_t loopEndFrame = 0);
  void updateMix(const State &) noexcept;
  void updateGains(const State &state) noexcept { updateMix(state); }
  uint64_t duration() const { return length; } // control thread only
  // Total algorithmic delay of currently active master inserts. It is an
  // atomic live-transport value: prepare updates it on the control thread;
  // audio and transport readers may query it without touching AU objects.
  uint32_t masterLatencyFrames() const noexcept {
    return masterLatency.load(std::memory_order_acquire);
  }
  // Legacy automatic view: finite decay plus an unbounded declaration is
  // bounded to the historic 30-second ceiling.
  uint32_t declaredTailFrames() const noexcept {
    const auto finite = declaredFiniteTail.load(std::memory_order_acquire);
    return declaredInfiniteTail.load(std::memory_order_acquire)
               ? std::max(finite, uint32_t{48000 * 30})
               : finite;
  }
  GraphTailSummary tailSummary() const noexcept {
    return {declaredFiniteTail.load(std::memory_order_acquire),
            declaredInfiniteTail.load(std::memory_order_acquire)};
  }
  uint64_t audiblePositionFrames() const noexcept {
    return audiblePosition.load(std::memory_order_acquire);
  }
  // Current callback's post-insert/post-fader peaks. Indices are prepared
  // State track/bus indices; false means no prepared strip at that index.
  bool trackMeter(size_t preparedIndex, float &left, float &right) const noexcept;
  bool busMeter(size_t preparedIndex, float &left, float &right) const noexcept;
  void masterMeter(float &left, float &right) const noexcept;
  // Post-master-inserts loudness in LUFS; -200 = fully quiet or unmeasured.
  // Updated on the audio thread at every 100 ms energy block boundary.
  void masterLoudness(float &momentary, float &shortTerm) const noexcept {
    momentary = loudnessTracker.momentaryLufs();
    shortTerm = loudnessTracker.shortTermLufs();
  }
  // Returns false only when this prepared graph has no record for insertID.
  // The returned fields are lock-free snapshots and do not mutate State.
  bool insertRuntimeStatus(uint64_t insertID,
                           PreparedEffectRuntimeStatus &out) const noexcept;
  // Call from the output stop path to clear visible telemetry immediately.
  void resetMeters() noexcept { clearMeters(); }
  // Control-thread test seam: when set, prepare() instantiates inserts with
  // this factory instead of the real AU/VST3 host paths. Production leaves it
  // null; the audio callback never reads it.
  std::function<std::unique_ptr<PreparedEffect>(const PluginInsert &)>
      insertFactoryForTest;
  // Exactly one live touch may be active. The override is runtime-only and
  // does not mutate persisted automation lanes or plug-in state.
  bool beginPluginParameterTouch(uint64_t pluginID, uint32_t parameterID,
                                 float normalizedValue) noexcept;
  bool updatePluginParameterTouch(uint64_t pluginID, uint32_t parameterID,
                                  float normalizedValue) noexcept;
  bool endPluginParameterTouch(uint64_t pluginID,
                               uint32_t parameterID) noexcept;
  void cancelPluginParameterTouch() noexcept;
  void render(float *left, float *right, uint32_t frames) noexcept;
  // Offline export render: identical to render() except that the metronome is
  // suppressed even while setMetronome(true), so bounced files never carry a
  // monitoring click.
  void renderExport(float *left, float *right, uint32_t frames) noexcept {
    renderInternal(left, right, frames, true);
  }
  // Monitoring switch; may be flipped live (atomic, control thread writes).
  void setMetronome(bool enabled) noexcept {
    metronomeOn.store(enabled, std::memory_order_relaxed);
  }
  bool metronome() const noexcept {
    return metronomeOn.load(std::memory_order_relaxed);
  }
  // Offline-only tail drain. It advances the prepared track/bus/master
  // graph and its PDC edges with silence; it never advances transport.
  void renderTail(float *left, float *right, uint32_t frames) noexcept;
};
} // namespace daw
