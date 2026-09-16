#include "audio/renderer.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace daw {
static_assert(std::atomic<float>::is_always_lock_free &&
              std::atomic<uint64_t>::is_always_lock_free &&
              std::atomic<bool>::is_always_lock_free);
namespace {
constexpr uint32_t kRenderBlockFrames = 4096;
constexpr uint32_t kMaximumPdcEdgeFrames = 48000 * 10;
constexpr uint64_t kMaximumPdcStereoFrames = 48000ULL * 20;
constexpr uint32_t kMaximumDeclaredTailFrames = 48000 * 30;
float gain(double db) noexcept {
  return db <= -120 ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0));
}
// --- Metronome synthesis (offline-safe for the RT callback: no allocation,
// no locks; the only exceptions are caught at the timeline ceiling below). ---
// One blip is 5 ms of sine at the fixed 48 kHz project rate, shaped by a
// cos^2 attack and decay (1 ms each): sin^2 ramp in, cos^2 ramp out. The
// amplitude is -18 dBFS (0.12) and the bar-start accent uses 1500 Hz against
// the ordinary 1000 Hz beat.
constexpr double kPi = 3.14159265358979323846;
constexpr double kProjectFrameRate = 48000.0;
constexpr uint32_t kBlipFrames = 5 * 48;        // 5 ms
constexpr uint32_t kBlipRampFrames = 48;        // 1 ms attack and 1 ms decay
constexpr float kBlipAmplitude = 0.12f;         // -18 dBFS
constexpr double kBlipFrequency = 1000.0;
constexpr double kBlipAccentFrequency = 1500.0;
// A 64-point tempo map can concentrate at most one boundary beat per segment
// inside a (block+5 ms) window, so 128 slots bound the worst case without
// ever touching the heap.
constexpr size_t kMaxBlipsPerBlock = 128;
const inline std::array<float, kBlipRampFrames> kBlipAttack = [] {
  std::array<float, kBlipRampFrames> values{};
  for (uint32_t j = 0; j < kBlipRampFrames; ++j) {
    const double t =
        std::sin(kPi * double(j) / (2.0 * double(kBlipRampFrames)));
    values[j] = float(t * t);
  }
  return values;
}();
const inline std::array<float, kBlipRampFrames> kBlipDecay = [] {
  std::array<float, kBlipRampFrames> values{};
  for (uint32_t j = 0; j < kBlipRampFrames; ++j) {
    const double t =
        std::cos(kPi * double(j) / (2.0 * double(kBlipRampFrames)));
    values[j] = float(t * t);
  }
  return values;
}();
struct BeatBlip {
  uint64_t frame = 0;
  bool accent = false;
};
// The first whole beat whose timeline frame is >= target; UINT64_MAX means no
// such beat exists on the timeline. All beat arithmetic goes through the
// domain converters instead of duplicating tempo math here.
uint64_t firstBeatAtOrAfter(const State &state, uint64_t target) {
  const double approx = state.beatsAtFrame(target);
  uint64_t beat = 0;
  if (std::isfinite(approx) && approx > 1.0)
    beat = static_cast<uint64_t>(std::floor(approx)) - 1; // conservative
  for (;;) {
    uint64_t frame;
    try {
      frame = state.frameAtBeats(static_cast<double>(beat));
    } catch (...) {
      return UINT64_MAX; // past the 2^40-frame ceiling; monotonic, so none later
    }
    if (frame >= target)
      return beat;
    ++beat;
  }
}
// Bar grouping: quarter-note beats count from the frame-0 anchor and the
// count restarts at the newest point that changed the numerator; a
// numerator-neutral signature change leaves the grid running. The governing
// numerator is the window length in beats.
bool isBarStartBeat(const State &state, uint64_t beat, uint64_t frame) {
  const auto &points = state.timeSignatures;
  size_t index = 0;
  uint8_t numerator = kDefaultTimeSignatureNumerator;
  bool found = false;
  for (size_t i = points.size(); i-- > 0;)
    if (points[i].frame <= frame) {
      index = i;
      numerator = points[i].numerator;
      found = true;
      break;
    }
  if (!found || numerator == 0)
    numerator = kDefaultTimeSignatureNumerator; // defensive; validated lanes
  size_t origin = found ? index : 0;
  while (origin > 0 && points[origin - 1].numerator == numerator)
    --origin;
  const uint64_t originBeat = firstBeatAtOrAfter(state, points[origin].frame);
  if (originBeat == UINT64_MAX || beat < originBeat)
    return false;
  return (beat - originBeat) % numerator == 0;
}
// Every beat whose 5 ms blip overlaps [blockStart, blockStart+count). Block
// windows are half-open and contiguous, so a beat is scheduled exactly once
// per playback pass: the walk starts kBlipFrames-1 frames early to catch the
// tail of a blip that began in a previous block, while the mixer clamps each
// blip to the samples this block actually owns. Seeks and loop wraps
// re-derive the walk from the absolute timeline, so clicks follow the
// transport. frameAtBeats throws only past the project ceiling; caught.
size_t beatBlips(const State &state, uint64_t blockStart, uint32_t count,
                 BeatBlip *out, size_t capacity) {
  if (count == 0 || blockStart >= kMaxMidiFrame)
    return 0;
  const uint64_t windowBegin =
      blockStart > kBlipFrames - 1 ? blockStart - (kBlipFrames - 1) : 0;
  const uint64_t windowEnd = blockStart + uint64_t(count);
  uint64_t beat = firstBeatAtOrAfter(state, windowBegin);
  if (beat == UINT64_MAX)
    return 0;
  size_t written = 0;
  for (size_t probed = 0; written < capacity && probed < capacity; ++probed) {
    uint64_t frame;
    try {
      frame = state.frameAtBeats(static_cast<double>(beat));
    } catch (...) {
      break; // past the timeline ceiling: no further beats exist
    }
    if (frame >= windowEnd)
      break;
    if (frame >= windowBegin)
      out[written++] = {frame, isBarStartBeat(state, beat, frame)};
    ++beat;
  }
  return written;
}
// Sums the overlapping blips into a finalized master block, both channels.
// Sample phase runs on the absolute timeline frame, so a blip split across a
// block edge is bit-identical to one rendered whole.
void mixMetronome(const State &state, uint64_t blockStart, uint32_t count,
                  float *outLeft, float *outRight) noexcept {
  BeatBlip blips[kMaxBlipsPerBlock];
  const size_t scheduled =
      beatBlips(state, blockStart, count, blips, kMaxBlipsPerBlock);
  for (size_t i = 0; i < scheduled; ++i) {
    const auto &blip = blips[i];
    const double frequency =
        blip.accent ? kBlipAccentFrequency : kBlipFrequency;
    const uint64_t blockEnd = blockStart + count;
    const uint64_t begin = std::max(blip.frame, blockStart);
    const uint64_t end = std::min(blip.frame + kBlipFrames, blockEnd);
    for (uint64_t g = begin; g < end; ++g) {
      const uint32_t j = static_cast<uint32_t>(g - blip.frame);
      float envelope = 1.0f;
      if (j < kBlipRampFrames)
        envelope = kBlipAttack[j];
      else if (j >= kBlipFrames - kBlipRampFrames)
        envelope = kBlipDecay[j - (kBlipFrames - kBlipRampFrames)];
      const float value =
          kBlipAmplitude * envelope *
          static_cast<float>(std::sin(2.0 * kPi * frequency *
                                      static_cast<double>(g) /
                                      kProjectFrameRate));
      const uint32_t offset = static_cast<uint32_t>(g - blockStart);
      outLeft[offset] += value;
      outRight[offset] += value;
    }
  }
}
} // namespace

GraphTailSummary serialTail(GraphTailSummary left,
                            GraphTailSummary right) noexcept {
  const auto finite = uint64_t(left.finiteFrames) + right.finiteFrames;
  return {static_cast<uint32_t>(std::min<uint64_t>(
              finite, kMaximumDeclaredTailFrames)),
          left.hasInfiniteTail || right.hasInfiniteTail};
}

GraphTailSummary parallelTail(GraphTailSummary left,
                              GraphTailSummary right) noexcept {
  return {std::max(left.finiteFrames, right.finiteFrames),
          left.hasInfiniteTail || right.hasInfiniteTail};
}

void Renderer::PdcDelay::configure(uint32_t frames) {
  if (frames > kMaximumPdcEdgeFrames)
    throw Error("PDC edge delay exceeds 10 seconds");
  left.assign(frames, 0.0f);
  right.assign(frames, 0.0f);
  cursor = 0;
}
void Renderer::PdcDelay::process(float inLeft, float inRight, float &outLeft,
                                 float &outRight) noexcept {
  if (left.empty()) {
    outLeft = inLeft;
    outRight = inRight;
    return;
  }
  outLeft = left[cursor];
  outRight = right[cursor];
  left[cursor] = inLeft;
  right[cursor] = inRight;
  if (++cursor == left.size())
    cursor = 0;
}
double Renderer::automationValue(VolumeAutomationRoute &route,
                                 uint64_t timeline) noexcept {
  if (route.hasLastFrame && timeline < route.lastFrame)
    route.nextPoint = 0;
  route.hasLastFrame = true;
  route.lastFrame = timeline;
  const auto &points = route.points;
  if (timeline <= points.front().frame)
    return points.front().gainDb;
  while (route.nextPoint < points.size() &&
         points[route.nextPoint].frame <= timeline)
    ++route.nextPoint;
  if (route.nextPoint == points.size())
    return points.back().gainDb;
  const auto &prior = points[route.nextPoint - 1];
  const auto &next = points[route.nextPoint];
  return prior.gainDb + (next.gainDb - prior.gainDb) *
                            double(timeline - prior.frame) /
                            double(next.frame - prior.frame);
}
float Renderer::volumeAutomationGain(size_t track, uint64_t timeline) noexcept {
  return gain(automationValue(volumeAutomation[track], timeline));
}
float Renderer::parameterAutomationValue(ParameterAutomationRoute &route,
                                         uint64_t timeline) noexcept {
  if (route.hasLastFrame && timeline < route.lastFrame)
    route.nextPoint = 0;
  route.hasLastFrame = true;
  route.lastFrame = timeline;
  const auto &points = route.points;
  if (timeline <= points.front().frame)
    return static_cast<float>(points.front().normalizedValue);
  while (route.nextPoint < points.size() &&
         points[route.nextPoint].frame <= timeline)
    ++route.nextPoint;
  if (route.nextPoint == points.size())
    return static_cast<float>(points.back().normalizedValue);
  const auto &prior = points[route.nextPoint - 1];
  const auto &next = points[route.nextPoint];
  return static_cast<float>(prior.normalizedValue +
                            (next.normalizedValue - prior.normalizedValue) *
                                double(timeline - prior.frame) /
                                double(next.frame - prior.frame));
}
bool Renderer::beginPluginParameterTouch(uint64_t pluginID, uint32_t parameterID,
                                         float normalizedValue) noexcept {
  if (!pluginID || !std::isfinite(normalizedValue) || normalizedValue < 0.0f || normalizedValue > 1.0f)
    return false;
  touchActive.store(false, std::memory_order_release);
  touchPluginID.store(pluginID, std::memory_order_relaxed);
  touchParameterID.store(parameterID, std::memory_order_relaxed);
  touchNormalized.store(normalizedValue, std::memory_order_relaxed);
  touchActive.store(true, std::memory_order_release);
  return true;
}
bool Renderer::updatePluginParameterTouch(uint64_t pluginID, uint32_t parameterID,
                                          float normalizedValue) noexcept {
  if (!std::isfinite(normalizedValue) || normalizedValue < 0.0f || normalizedValue > 1.0f ||
      !touchActive.load(std::memory_order_acquire) ||
      touchPluginID.load(std::memory_order_relaxed) != pluginID ||
      touchParameterID.load(std::memory_order_relaxed) != parameterID)
    return false;
  touchNormalized.store(normalizedValue, std::memory_order_release);
  return true;
}
bool Renderer::endPluginParameterTouch(uint64_t pluginID, uint32_t parameterID) noexcept {
  if (!touchActive.load(std::memory_order_acquire) ||
      touchPluginID.load(std::memory_order_relaxed) != pluginID ||
      touchParameterID.load(std::memory_order_relaxed) != parameterID)
    return false;
  touchActive.store(false, std::memory_order_release);
  return true;
}
void Renderer::cancelPluginParameterTouch() noexcept { touchActive.store(false, std::memory_order_release); }
void Renderer::updateAudiblePosition() noexcept {
  const auto latency = masterLatency.load(std::memory_order_acquire);
  const auto elapsed = renderedInputFrames > latency ? renderedInputFrames - latency : 0;
  uint64_t audible = transportStart + elapsed;
  if (looping && audible >= loopEnd) {
    const auto span = loopEnd - loopBegin;
    audible = loopBegin + (audible - loopEnd) % span;
  } else if (!looping) {
    audible = std::min(audible, length);
  }
  audiblePosition.store(audible, std::memory_order_release);
}
bool Renderer::processChain(std::vector<std::unique_ptr<PreparedEffect>> &chain,
                            ChainAutomationPlan &automation,
                            const std::vector<uint64_t> &effectIDs, float *left,
                            float *right, uint32_t frames, uint64_t time,
                            uint64_t timeline,
                            std::span<const PreparedMidiEvent> midi) noexcept {
  bool okay = true;
  if (chain.size() != automation.size() || chain.size() != effectIDs.size())
    return false;
  for (size_t effectIndex = 0; effectIndex < chain.size(); ++effectIndex) {
    size_t count = 0;
    const bool touch = timeline != std::numeric_limits<uint64_t>::max() &&
                       touchActive.load(std::memory_order_acquire) &&
                       touchPluginID.load(std::memory_order_relaxed) == effectIDs[effectIndex];
    const auto touchParameter = touch ? touchParameterID.load(std::memory_order_relaxed) : 0;
    const auto touchValue = touch ? touchNormalized.load(std::memory_order_relaxed) : 0.0f;
    if (timeline != std::numeric_limits<uint64_t>::max()) {
      auto &plan = automation[effectIndex];
      for (auto &lane : plan) {
        if (lane.points.empty())
          continue;
        const auto append = [&](uint32_t offset, float value) {
          if (count >= parameterEvents.size())
            return false;
          parameterEvents[count++] = {lane.parameterID, offset,
                                      std::clamp(value, 0.0f, 1.0f)};
          return true;
        };
        if (touch && lane.parameterID == touchParameter)
          continue;
        if (!append(0, parameterAutomationValue(lane, timeline))) {
          okay = false;
          break;
        }
        const auto endTimeline = timeline + frames - 1;
        while (lane.nextPoint < lane.points.size() &&
               lane.points[lane.nextPoint].frame <= endTimeline) {
          const auto &point = lane.points[lane.nextPoint++];
          if (point.frame > timeline &&
              !append(static_cast<uint32_t>(point.frame - timeline),
                      static_cast<float>(point.normalizedValue))) {
            okay = false;
            break;
          }
        }
        if (!okay)
          break;
        if (frames > 1 &&
            !append(frames - 1, parameterAutomationValue(lane, endTimeline))) {
          okay = false;
          break;
        }
      }
    }
    if (touch) {
      if (count >= parameterEvents.size())
        okay = false;
      else
        parameterEvents[count++] = {touchParameter, 0, std::clamp(touchValue, 0.0f, 1.0f)};
    }
    const bool processed = okay && chain[effectIndex]->process(
        left, right, frames, time,
        std::span<const PreparedParameterEvent>(parameterEvents.data(), count),
        midi);
    auto runtime = chain[effectIndex]->runtimeStatus();
    if (!processed && runtime.state == PreparedEffectRuntimeState::ActiveInProcess)
      runtime.state = PreparedEffectRuntimeState::DryFallback;
    publishRuntimeStatus(effectIDs[effectIndex], runtime);
    if (!processed)
      okay = false;
  }
  return okay;
}

void Renderer::publishRuntimeStatus(uint64_t id,
                                    PreparedEffectRuntimeStatus status) noexcept {
  if (!id)
    return;
  for (auto &slot : runtimeInserts) {
    if (slot.id.load(std::memory_order_acquire) != id)
      continue;
    slot.extraPipelineLatency.store(status.extraPipelineLatencyFrames,
                                    std::memory_order_relaxed);
    slot.faultCode.store(status.faultCode, std::memory_order_relaxed);
    slot.state.store(static_cast<uint32_t>(status.state),
                     std::memory_order_release);
    return;
  }
}

bool Renderer::insertRuntimeStatus(uint64_t insertID,
                                   PreparedEffectRuntimeStatus &out) const noexcept {
  if (!insertID)
    return false;
  for (const auto &slot : runtimeInserts) {
    if (slot.id.load(std::memory_order_acquire) != insertID)
      continue;
    out.extraPipelineLatencyFrames = slot.extraPipelineLatency.load(std::memory_order_relaxed);
    out.faultCode = slot.faultCode.load(std::memory_order_relaxed);
    out.state = static_cast<PreparedEffectRuntimeState>(
        slot.state.load(std::memory_order_acquire));
    return true;
  }
  return false;
}

void Renderer::prepare(const State &state, uint64_t start, uint64_t loopStart,
                       uint64_t loopEndFrame) {
  static const GraphLatencyPlan zero;
  prepare(state, zero, start, loopStart, loopEndFrame);
}
void Renderer::prepare(const State &state, const GraphLatencyPlan &nodeLatency,
                       uint64_t start, uint64_t loopStart,
                       uint64_t loopEndFrame) {
  validate(state);
  if ((!nodeLatency.trackNodeFrames.empty() &&
       nodeLatency.trackNodeFrames.size() != state.tracks.size()) ||
      (!nodeLatency.busNodeFrames.empty() &&
       nodeLatency.busNodeFrames.size() != state.buses.size()))
    throw Error("Graph latency plan does not match routing graph");
  const auto trackNode = [&](size_t i) {
    return nodeLatency.trackNodeFrames.empty() ? 0U
                                               : nodeLatency.trackNodeFrames[i];
  };
  const auto busNode = [&](size_t i) {
    return nodeLatency.busNodeFrames.empty() ? 0U
                                             : nodeLatency.busNodeFrames[i];
  };
  for (size_t i = 0; i < state.tracks.size(); ++i)
    if (trackNode(i) > kMaximumPdcEdgeFrames)
      throw Error("PDC track path exceeds 10 seconds");
  for (size_t i = 0; i < state.buses.size(); ++i)
    if (busNode(i) > kMaximumPdcEdgeFrames)
      throw Error("PDC bus node exceeds 10 seconds");

  std::vector<Voice> nextVoices;
  std::vector<size_t> nextActive;
  size_t instrumentVoices = 0;
  uint64_t end = 0;
  for (size_t i = 0; i < state.tracks.size(); ++i) {
    const auto &track = state.tracks[i];
    if (track.audio) {
      nextActive.push_back(i);
      for (const auto &region : track.regions) {
        if (region.muted) continue; // a muted region gets no voice at all
        const auto source =
            region.take == 0 ? track.audio : track.takes[region.take - 1].audio;
        const uint64_t loopSpan =
            region.looped ? source->frames() - region.sourceOffset : 0;
        // Clip pan folds into the voice with the same linear law as track
        // pan (unity center), so panning never changes program level midway.
        const double pan = region.pan < -1.0 ? -1.0 : (region.pan > 1.0 ? 1.0 : region.pan);
        const float panLeft = pan > 0 ? float(1 - pan) : 1.0f;
        const float panRight = pan < 0 ? float(1 + pan) : 1.0f;
        nextVoices.push_back({i, source, region.start, region.sourceOffset,
                              region.length, region.fadeIn, region.fadeOut,
                              region.fadeInShape, region.fadeOutShape,
                              gain(region.gain), loopSpan, panLeft, panRight});
        end = std::max(end, region.start + region.length);
      }
    } else if (!track.midiClips.empty() || !track.inserts.empty()) {
      // Instrument voice: a track without audio media but with MIDI to play
      // or an insert chain to host gets a rendered slot — the per-block
      // zero-filled track buffer passes through the track chain, and the
      // track's MIDI plan modulates it (an instrument insert turns the
      // silence into sound). Bypassed or non-producing inserts honestly
      // render silence. A track with neither clips nor inserts is not a
      // voice and stays out of the graph.
      nextActive.push_back(i);
      ++instrumentVoices;
    }
    for (const auto &clip : track.midiClips)
      end = std::max(end, clip.start + clip.length);
  }
  if (nextVoices.empty() && instrumentVoices == 0)
    throw Error("Import audio or add a MIDI/instrument track");
  // MIDI plans are built for every track ordinal; only active track chains
  // (audio or instrument voices) drain them during render().
  std::vector<MidiTrackPlan> nextMidiPlans(state.tracks.size());
  for (size_t i = 0; i < state.tracks.size(); ++i)
    if (!state.tracks[i].midiClips.empty())
      nextMidiPlans[i].build(state.tracks[i].midiClips);
  if (start > end)
    throw Error("Playback position exceeds project duration");
  const bool nextLooping = loopStart != 0 || loopEndFrame != 0;
  if (nextLooping && (loopEndFrame <= loopStart || loopEndFrame > end))
    throw Error("Invalid playback loop");
  if (nextLooping && start >= loopEndFrame)
    start = loopStart;
  const auto findBus = [&](uint64_t id) -> int16_t {
    if (!id)
      return -1;
    const auto it = std::find_if(state.buses.begin(), state.buses.end(),
                                 [&](const auto &bus) { return bus.id == id; });
    return static_cast<int16_t>(it - state.buses.begin());
  };
  std::vector<int16_t> nextTrackOutputs, nextBusOutputs;
  std::vector<SendRoute> nextSends;
  std::vector<size_t> nextSendRanges(state.tracks.size() + 1);
  std::vector<VolumeAutomationRoute> nextVolume, nextPan, nextBusAutomation;
  nextTrackOutputs.reserve(state.tracks.size());
  nextVolume.reserve(state.tracks.size());
  nextPan.reserve(state.tracks.size());
  nextBusAutomation.reserve(state.buses.size());
  for (size_t i = 0; i < state.tracks.size(); ++i) {
    nextSendRanges[i] = nextSends.size();
    const auto &track = state.tracks[i];
    nextTrackOutputs.push_back(findBus(track.outputBus));
    nextVolume.push_back({track.volumeAutomation});
    nextPan.push_back({track.panAutomation});
    for (const auto &send : track.sends)
      nextSends.push_back({i, static_cast<size_t>(findBus(send.bus)),
                           gain(send.gain), send.preFader});
  }
  nextSendRanges.back() = nextSends.size();
  std::vector<size_t> indegree(state.buses.size());
  nextBusOutputs.reserve(state.buses.size());
  for (const auto &bus : state.buses) {
    const auto output = findBus(bus.outputBus);
    nextBusOutputs.push_back(output);
    if (output >= 0)
      ++indegree[static_cast<size_t>(output)];
    nextBusAutomation.push_back({bus.gainAutomation});
  }
  std::vector<size_t> nextBusOrder;
  for (size_t i = 0; i < indegree.size(); ++i)
    if (!indegree[i])
      nextBusOrder.push_back(i);
  for (size_t i = 0; i < nextBusOrder.size(); ++i) {
    const auto output = nextBusOutputs[nextBusOrder[i]];
    if (output >= 0 && !--indegree[static_cast<size_t>(output)])
      nextBusOrder.push_back(static_cast<size_t>(output));
  }

  uint64_t unavailable = 0;
  struct PendingRuntimeStatus {
    uint64_t id = 0;
    PreparedEffectRuntimeStatus status{};
  };
  std::vector<PendingRuntimeStatus> nextRuntimeStatuses;
  nextRuntimeStatuses.reserve(kMaxProjectPluginInserts);
  const auto makeChain =
      [&](const std::vector<PluginInsert> &inserts,
          std::vector<std::unique_ptr<PreparedEffect>> &chain,
          ChainAutomationPlan &automation, std::vector<uint64_t> &effectIDs) {
        uint64_t total = 0;
        for (const auto &insert : inserts) {
          if (insert.bypassed) {
            nextRuntimeStatuses.push_back(
                {insert.id, {PreparedEffectRuntimeState::Unprepared, 0, 0}});
            continue;
          }
          std::unique_ptr<PreparedEffect> effect;
          try {
            effect = insertFactoryForTest
                         ? insertFactoryForTest(insert)
                         : (isVst3Insert(insert) ? prepareVst3Effect(insert)
                                                 : prepareAudioUnit(insert));
          } catch (...) {
            ++unavailable;
            nextRuntimeStatuses.push_back(
                {insert.id, {PreparedEffectRuntimeState::Failed, 0, 1}});
            continue;
          }
          if (!effect) {
            ++unavailable;
            nextRuntimeStatuses.push_back(
                {insert.id, {PreparedEffectRuntimeState::Failed, 0, 1}});
            continue;
          }
          nextRuntimeStatuses.push_back({insert.id, effect->runtimeStatus()});
          total += effect->latencyFrames();
          if (total > kMaximumPdcEdgeFrames)
            throw Error("Plug-in chain latency exceeds 10 seconds");
          EffectAutomationPlan plan;
          plan.reserve(insert.parameterAutomation.size());
          for (const auto &lane : insert.parameterAutomation)
            if (!lane.points.empty()) plan.push_back({lane.parameterID, lane.points});
          chain.push_back(std::move(effect));
          automation.push_back(std::move(plan));
          effectIDs.push_back(insert.id);
        }
        return static_cast<uint32_t>(total);
      };
  std::vector<std::vector<std::unique_ptr<PreparedEffect>>> nextTrackEffects(
      state.tracks.size()),
      nextBusEffects(state.buses.size());
  std::vector<ChainAutomationPlan> nextTrackAutomation(state.tracks.size()),
      nextBusAutomationPlans(state.buses.size());
  std::vector<std::vector<uint64_t>> nextTrackEffectIDs(state.tracks.size()),
      nextBusEffectIDs(state.buses.size());
  std::vector<uint32_t> trackInsertLatency(state.tracks.size()),
      busInsertLatency(state.buses.size());
  for (size_t i = 0; i < state.tracks.size(); ++i)
    trackInsertLatency[i] = makeChain(
        state.tracks[i].inserts, nextTrackEffects[i], nextTrackAutomation[i], nextTrackEffectIDs[i]);
  for (size_t i = 0; i < state.buses.size(); ++i)
    busInsertLatency[i] = makeChain(state.buses[i].inserts, nextBusEffects[i],
                                    nextBusAutomationPlans[i], nextBusEffectIDs[i]);
  std::vector<std::unique_ptr<PreparedEffect>> nextMasterEffects;
  ChainAutomationPlan nextMasterAutomation;
  std::vector<uint64_t> nextMasterEffectIDs;
  const auto masterInsertLatency =
      makeChain(state.masterInserts, nextMasterEffects, nextMasterAutomation, nextMasterEffectIDs);
  const auto chainTail = [](const auto &chain) {
    GraphTailSummary result;
    for (const auto &effect : chain) {
      const auto tail = effect->tail();
      result = serialTail(result, {tail.finiteFrames, tail.infinite});
    }
    return result;
  };
  std::vector<GraphTailSummary> trackEffectTail(state.tracks.size()),
      busEffectTail(state.buses.size());
  for (size_t i = 0; i < state.tracks.size(); ++i) trackEffectTail[i] = chainTail(nextTrackEffects[i]);
  for (size_t i = 0; i < state.buses.size(); ++i) busEffectTail[i] = chainTail(nextBusEffects[i]);
  const auto masterEffectTail = chainTail(nextMasterEffects);

  // Runtime graph latency is compiled leaf-to-root. Delay buffers live on
  // incoming edges, so every sum is phase/time aligned before a bus/master FX
  // chain.
  std::vector<bool> trackProduces(state.tracks.size()),
      busProduces(state.buses.size());
  for (const auto i : nextActive)
    trackProduces[i] = true;
  std::vector<uint32_t> trackLatency(state.tracks.size()),
      busIncoming(state.buses.size()), busLatency(state.buses.size()),
      trackDelay(state.tracks.size()), sendDelay(nextSends.size()),
      busDelay(state.buses.size());
  for (size_t i = 0; i < state.tracks.size(); ++i)
    if (trackProduces[i]) {
      const uint64_t total = uint64_t(trackNode(i)) + trackInsertLatency[i];
      if (total > kMaximumPdcEdgeFrames)
        throw Error("PDC track path exceeds 10 seconds");
      trackLatency[i] = static_cast<uint32_t>(total);
      const auto output = nextTrackOutputs[i];
      if (output >= 0) {
        busProduces[static_cast<size_t>(output)] = true;
        busIncoming[static_cast<size_t>(output)] =
            std::max(busIncoming[static_cast<size_t>(output)], trackLatency[i]);
      }
    }
  for (const auto &send : nextSends)
    if (trackProduces[send.track]) {
      busProduces[send.bus] = true;
      busIncoming[send.bus] =
          std::max(busIncoming[send.bus], trackLatency[send.track]);
    }
  for (const auto i : nextBusOrder)
    if (busProduces[i]) {
      const uint64_t total =
          uint64_t(busIncoming[i]) + busNode(i) + busInsertLatency[i];
      if (total > kMaximumPdcEdgeFrames)
        throw Error("PDC bus path exceeds 10 seconds");
      busLatency[i] = static_cast<uint32_t>(total);
      const auto output = nextBusOutputs[i];
      if (output >= 0) {
        busProduces[static_cast<size_t>(output)] = true;
        busIncoming[static_cast<size_t>(output)] =
            std::max(busIncoming[static_cast<size_t>(output)], busLatency[i]);
      }
    }
  std::vector<GraphTailSummary> trackTail(state.tracks.size()),
      busIncomingTail(state.buses.size()), busTail(state.buses.size());
  for (size_t i = 0; i < state.tracks.size(); ++i)
    if (trackProduces[i]) {
      trackTail[i] = trackEffectTail[i];
      const auto output = nextTrackOutputs[i];
      if (output >= 0)
        busIncomingTail[static_cast<size_t>(output)] = parallelTail(
            busIncomingTail[static_cast<size_t>(output)], trackTail[i]);
    }
  for (const auto &send : nextSends)
    if (trackProduces[send.track])
      busIncomingTail[send.bus] = parallelTail(busIncomingTail[send.bus],
                                                trackTail[send.track]);
  for (const auto i : nextBusOrder)
    if (busProduces[i]) {
      busTail[i] = serialTail(busIncomingTail[i], busEffectTail[i]);
      const auto output = nextBusOutputs[i];
      if (output >= 0)
        busIncomingTail[static_cast<size_t>(output)] = parallelTail(
            busIncomingTail[static_cast<size_t>(output)], busTail[i]);
    }
  GraphTailSummary rootTail;
  for (size_t i = 0; i < state.tracks.size(); ++i)
    if (trackProduces[i] && nextTrackOutputs[i] < 0)
      rootTail = parallelTail(rootTail, trackTail[i]);
  for (size_t i = 0; i < state.buses.size(); ++i)
    if (busProduces[i] && nextBusOutputs[i] < 0)
      rootTail = parallelTail(rootTail, busTail[i]);
  const auto declaredTailValue = serialTail(rootTail, masterEffectTail);
  uint32_t masterPdcLatency = 0;
  for (size_t i = 0; i < state.tracks.size(); ++i)
    if (trackProduces[i] && nextTrackOutputs[i] < 0)
      masterPdcLatency = std::max(masterPdcLatency, trackLatency[i]);
  for (size_t i = 0; i < state.buses.size(); ++i)
    if (busProduces[i] && nextBusOutputs[i] < 0)
      masterPdcLatency = std::max(masterPdcLatency, busLatency[i]);
  for (size_t i = 0; i < state.tracks.size(); ++i)
    if (trackProduces[i]) {
      const auto output = nextTrackOutputs[i];
      trackDelay[i] = (output < 0 ? masterPdcLatency
                                  : busIncoming[static_cast<size_t>(output)]) -
                      trackLatency[i];
    }
  for (size_t i = 0; i < nextSends.size(); ++i)
    if (trackProduces[nextSends[i].track])
      sendDelay[i] =
          busIncoming[nextSends[i].bus] - trackLatency[nextSends[i].track];
  for (size_t i = 0; i < state.buses.size(); ++i)
    if (busProduces[i]) {
      const auto output = nextBusOutputs[i];
      busDelay[i] = (output < 0 ? masterPdcLatency
                                : busIncoming[static_cast<size_t>(output)]) -
                    busLatency[i];
    }
  uint64_t pdcStorage = 0;
  const auto reserve = [&](uint32_t delay) {
    if (delay > kMaximumPdcEdgeFrames ||
        pdcStorage > kMaximumPdcStereoFrames - delay)
      throw Error("PDC delay-line budget exceeded");
    pdcStorage += delay;
  };
  std::vector<PdcDelay> nextTrackPdc(state.tracks.size()),
      nextSendPdc(nextSends.size()), nextBusPdc(state.buses.size());
  for (size_t i = 0; i < nextTrackPdc.size(); ++i) {
    reserve(trackDelay[i]);
    nextTrackPdc[i].configure(trackDelay[i]);
  }
  for (size_t i = 0; i < nextSendPdc.size(); ++i) {
    reserve(sendDelay[i]);
    nextSendPdc[i].configure(sendDelay[i]);
  }
  for (size_t i = 0; i < nextBusPdc.size(); ++i) {
    reserve(busDelay[i]);
    nextBusPdc[i].configure(busDelay[i]);
  }

  voices = std::move(nextVoices);
  activeGains = std::move(nextActive);
  volumeAutomation = std::move(nextVolume);
  panAutomation = std::move(nextPan);
  busGainAutomation = std::move(nextBusAutomation);
  masterGainAutomation = {state.masterGainAutomation};
  trackOutputs = std::move(nextTrackOutputs);
  busOutputs = std::move(nextBusOutputs);
  busOrder = std::move(nextBusOrder);
  sends = std::move(nextSends);
  sendRanges = std::move(nextSendRanges);
  trackMainPdc = std::move(nextTrackPdc);
  sendPdc = std::move(nextSendPdc);
  busOutputPdc = std::move(nextBusPdc);
  trackEffects = std::move(nextTrackEffects);
  midiPlans = std::move(nextMidiPlans);
  midiEvents.assign(kMaxMidiEventsPerBlock, {});
  busEffects = std::move(nextBusEffects);
  trackEffectAutomation = std::move(nextTrackAutomation);
  busEffectAutomation = std::move(nextBusAutomationPlans);
  trackEffectIDs = std::move(nextTrackEffectIDs);
  busEffectIDs = std::move(nextBusEffectIDs);
  masterEffectAutomation = std::move(nextMasterAutomation);
  masterEffectIDs = std::move(nextMasterEffectIDs);
  parameterEvents.assign(kMaxProjectPluginParameterAutomationPoints +
                             kMaxPluginParameterAutomationLanes * 2,
                         {});
  trackBlockLeft.assign(state.tracks.size() * kRenderBlockFrames, 0.0f);
  trackBlockRight.assign(state.tracks.size() * kRenderBlockFrames, 0.0f);
  busBlockLeft.assign(state.buses.size() * kRenderBlockFrames, 0.0f);
  busBlockRight.assign(state.buses.size() * kRenderBlockFrames, 0.0f);
  masterEffects = std::move(nextMasterEffects);
  for (auto &slot : runtimeInserts) {
    slot.state.store(static_cast<uint32_t>(PreparedEffectRuntimeState::Unprepared),
                     std::memory_order_relaxed);
    slot.extraPipelineLatency.store(0, std::memory_order_relaxed);
    slot.faultCode.store(0, std::memory_order_relaxed);
    slot.id.store(0, std::memory_order_release);
  }
  for (const auto &entry : nextRuntimeStatuses) {
    if (!entry.id)
      continue;
    for (auto &slot : runtimeInserts) {
      if (slot.id.load(std::memory_order_relaxed) != 0)
        continue;
      slot.extraPipelineLatency.store(entry.status.extraPipelineLatencyFrames,
                                      std::memory_order_relaxed);
      slot.faultCode.store(entry.status.faultCode, std::memory_order_relaxed);
      slot.state.store(static_cast<uint32_t>(entry.status.state),
                       std::memory_order_relaxed);
      slot.id.store(entry.id, std::memory_order_release);
      break;
    }
  }
  preparedTrackCount = static_cast<uint32_t>(trackEffects.size());
  preparedBusCount = static_cast<uint32_t>(busEffects.size());
  clearMeters();
  smoothLeft.fill(0);
  smoothRight.fill(0);
  smoothPreFaderGate.fill(0);
  smoothLeftPan.fill(0);
  smoothRightPan.fill(0);
  smoothTrackFader.fill(0);
  smoothBusLeft.fill(0);
  smoothBusRight.fill(0);
  smoothBusFader.fill(0);
  smoothBusPan.fill(0);
  smoothBusGate.fill(0);
  length = end;
  // Copy the beat lanes so renderInternal can schedule clicks through the
  // domain converters without reaching back into the caller's State.
  metronomeTimeline.tempo = state.tempo;
  metronomeTimeline.timeSignatures = state.timeSignatures;
  loopBegin = loopStart;
  loopEnd = loopEndFrame;
  looping = nextLooping;
  cursor = start;
  transportStart = start;
  renderedInputFrames = 0;
  processTime = 0;
  position.store(start);
  audiblePosition.store(start);
  for (auto &plan : midiPlans) plan.reset(start);
  callbacks.store(0);
  clipped.store(0);
  pluginErrors.store(unavailable);
  peak.store(0);
  const uint64_t publishedLatency =
      uint64_t(masterInsertLatency) + masterPdcLatency;
  if (publishedLatency > std::numeric_limits<uint32_t>::max())
    throw Error("Total graph latency exceeds limit");
  masterLatency.store(static_cast<uint32_t>(publishedLatency),
                      std::memory_order_release);
  declaredFiniteTail.store(declaredTailValue.finiteFrames,
                            std::memory_order_release);
  declaredInfiniteTail.store(declaredTailValue.hasInfiniteTail,
                             std::memory_order_release);
  updateMix(state);
  smoothMaster = masterGain.load(std::memory_order_relaxed);
}

void Renderer::updateMix(const State &state) noexcept {
  const bool anySolo = std::any_of(state.tracks.begin(), state.tracks.end(),
                                   [](const auto &t) { return t.solo; });
  for (size_t i = 0; i < state.tracks.size() && i < leftGains.size(); ++i) {
    const auto &t = state.tracks[i];
    const bool audible = !t.muted && (!anySolo || t.solo);
    const float lp = t.pan > 0 ? float(1 - t.pan) : 1.0f,
                rp = t.pan < 0 ? float(1 + t.pan) : 1.0f, fader = gain(t.gain);
    leftGains[i].store(audible ? fader * lp : 0);
    rightGains[i].store(audible ? fader * rp : 0);
    leftPans[i].store(lp);
    rightPans[i].store(rp);
    trackFaderGains[i].store(fader);
    preFaderGates[i].store(audible ? 1.0f : 0.0f);
  }
  for (size_t i = 0; i < state.buses.size() && i < busLeftGains.size(); ++i) {
    const auto &b = state.buses[i];
    const float lp = b.pan > 0 ? float(1 - b.pan) : 1.0f,
                rp = b.pan < 0 ? float(1 + b.pan) : 1.0f, fader = gain(b.gain),
                gate = b.muted ? 0.0f : 1.0f;
    busLeftGains[i].store(fader * lp * gate);
    busRightGains[i].store(fader * rp * gate);
    busFaderGains[i].store(fader);
    busPans[i].store(float(b.pan));
    busGates[i].store(gate);
  }
  masterGain.store(gain(state.masterGain));
}

void Renderer::clearMeters() noexcept {
  for (auto &value : trackMeterLeft)
    value.store(0.0f, std::memory_order_release);
  for (auto &value : trackMeterRight)
    value.store(0.0f, std::memory_order_release);
  for (auto &value : busMeterLeft)
    value.store(0.0f, std::memory_order_release);
  for (auto &value : busMeterRight)
    value.store(0.0f, std::memory_order_release);
  masterMeterLeft.store(0.0f, std::memory_order_release);
  masterMeterRight.store(0.0f, std::memory_order_release);
  loudnessTracker.reset();
}

bool Renderer::trackMeter(size_t preparedIndex, float &left,
                          float &right) const noexcept {
  if (preparedIndex >= preparedTrackCount ||
      preparedIndex >= trackMeterLeft.size())
    return false;
  left = trackMeterLeft[preparedIndex].load(std::memory_order_acquire);
  right = trackMeterRight[preparedIndex].load(std::memory_order_acquire);
  return true;
}

bool Renderer::busMeter(size_t preparedIndex, float &left,
                        float &right) const noexcept {
  if (preparedIndex >= preparedBusCount || preparedIndex >= busMeterLeft.size())
    return false;
  left = busMeterLeft[preparedIndex].load(std::memory_order_acquire);
  right = busMeterRight[preparedIndex].load(std::memory_order_acquire);
  return true;
}

void Renderer::masterMeter(float &left, float &right) const noexcept {
  left = masterMeterLeft.load(std::memory_order_acquire);
  right = masterMeterRight.load(std::memory_order_acquire);
}

void Renderer::renderTail(float *left, float *right, uint32_t frames) noexcept {
  std::fill_n(left, frames, 0.0f);
  std::fill_n(right, frames, 0.0f);
  clearMeters();
  if (!frames)
    return;
  callbacks.fetch_add(1, std::memory_order_relaxed);
  uint32_t done = 0;
  while (done < frames) {
    const auto count = std::min(kRenderBlockFrames, frames - done);
    for (const auto track : activeGains) {
      auto *l = trackBlockLeft.data() + track * kRenderBlockFrames;
      auto *r = trackBlockRight.data() + track * kRenderBlockFrames;
      std::fill_n(l, count, 0.0f);
      std::fill_n(r, count, 0.0f);
      if (!processChain(trackEffects[track], trackEffectAutomation[track], trackEffectIDs[track], l, r, count, processTime, std::numeric_limits<uint64_t>::max()))
        pluginErrors.fetch_add(1, std::memory_order_relaxed);
    }
    for (size_t bus = 0; bus < busOutputs.size(); ++bus) {
      std::fill_n(busBlockLeft.data() + bus * kRenderBlockFrames, count, 0.0f);
      std::fill_n(busBlockRight.data() + bus * kRenderBlockFrames, count, 0.0f);
    }
    auto *outL = left + done;
    auto *outR = right + done;
    for (uint32_t f = 0; f < count; ++f) {
      for (const auto track : activeGains) {
        const auto inL = trackBlockLeft[track * kRenderBlockFrames + f],
                   inR = trackBlockRight[track * kRenderBlockFrames + f];
        const auto postL = inL * smoothLeft[track],
                   postR = inR * smoothRight[track];
        float routeL, routeR;
        trackMainPdc[track].process(postL, postR, routeL, routeR);
        const auto output = trackOutputs[track];
        if (output < 0) {
          outL[f] += routeL;
          outR[f] += routeR;
        } else {
          busBlockLeft[size_t(output) * kRenderBlockFrames + f] += routeL;
          busBlockRight[size_t(output) * kRenderBlockFrames + f] += routeR;
        }
        for (size_t s = sendRanges[track]; s < sendRanges[track + 1]; ++s) {
          const auto sendL = sends[s].preFader ? inL * smoothPreFaderGate[track]
                                               : postL,
                     sendR = sends[s].preFader ? inR * smoothPreFaderGate[track]
                                               : postR;
          sendPdc[s].process(sendL * sends[s].gain, sendR * sends[s].gain,
                             routeL, routeR);
          busBlockLeft[sends[s].bus * kRenderBlockFrames + f] += routeL;
          busBlockRight[sends[s].bus * kRenderBlockFrames + f] += routeR;
        }
      }
    }
    for (const auto bus : busOrder) {
      auto *l = busBlockLeft.data() + bus * kRenderBlockFrames;
      auto *r = busBlockRight.data() + bus * kRenderBlockFrames;
      if (!processChain(busEffects[bus], busEffectAutomation[bus], busEffectIDs[bus], l, r, count, processTime, std::numeric_limits<uint64_t>::max()))
        pluginErrors.fetch_add(1, std::memory_order_relaxed);
      for (uint32_t f = 0; f < count; ++f) {
        float routeL, routeR;
        busOutputPdc[bus].process(l[f] * smoothBusLeft[bus],
                                  r[f] * smoothBusRight[bus], routeL, routeR);
        const auto output = busOutputs[bus];
        if (output < 0) {
          outL[f] += routeL;
          outR[f] += routeR;
        } else {
          busBlockLeft[size_t(output) * kRenderBlockFrames + f] += routeL;
          busBlockRight[size_t(output) * kRenderBlockFrames + f] += routeR;
        }
      }
    }
    for (uint32_t f = 0; f < count; ++f) {
      outL[f] *= smoothMaster;
      outR[f] *= smoothMaster;
    }
    if (!processChain(masterEffects, masterEffectAutomation, masterEffectIDs, outL, outR, count, processTime, std::numeric_limits<uint64_t>::max()))
      pluginErrors.fetch_add(1, std::memory_order_relaxed);
    processTime += count;
    done += count;
  }
  float p = 0;
  uint64_t limited = 0;
  for (uint32_t f = 0; f < frames; ++f) {
    p = std::max({p, std::abs(left[f]), std::abs(right[f])});
    if (std::abs(left[f]) > 1 || std::abs(right[f]) > 1)
      ++limited;
    left[f] = std::clamp(left[f], -1.0f, 1.0f);
    right[f] = std::clamp(right[f], -1.0f, 1.0f);
  }
  clipped.fetch_add(limited, std::memory_order_relaxed);
  peak.store(p, std::memory_order_relaxed);
}

void Renderer::render(float *left, float *right, uint32_t frames) noexcept {
  renderInternal(left, right, frames, false);
}

void Renderer::renderInternal(float *left, float *right, uint32_t frames,
                              bool suppressMetronome) noexcept {
  std::fill_n(left, frames, 0.0f);
  std::fill_n(right, frames, 0.0f);
  callbacks.fetch_add(1, std::memory_order_relaxed);
  if (!playing.load(std::memory_order_relaxed)) {
    clearMeters();
    peak.store(0, std::memory_order_relaxed);
    return;
  }
  const auto available =
      looping
          ? frames
          : static_cast<uint32_t>(std::min<uint64_t>(frames, length - cursor));
  std::array<float, 256> trackPeakLeft{}, trackPeakRight{};
  std::array<float, 16> busPeakLeft{}, busPeakRight{};
  const auto timelineAt = [&](uint64_t base, uint32_t offset) {
    uint64_t t = base + offset;
    if (looping && t >= loopEnd) {
      const auto span = loopEnd - loopBegin;
      t = loopBegin + (t - loopEnd) % span;
    }
    return t;
  };
  uint32_t done = 0;
  while (done < available) {
    // Preserve loopEnd as the externally visible position when a callback
    // lands exactly on the boundary; wrap only when the next block begins.
    const bool wrapped = looping && cursor == loopEnd;
    if (wrapped)
      cursor = loopBegin;
    const auto untilLoop = looping ? static_cast<uint32_t>(loopEnd - cursor) : kRenderBlockFrames;
    const auto count = std::min({kRenderBlockFrames, available - done, untilLoop});
    for (const auto track : activeGains) {
      std::fill_n(trackBlockLeft.data() + track * kRenderBlockFrames, count,
                  0.0f);
      std::fill_n(trackBlockRight.data() + track * kRenderBlockFrames, count,
                  0.0f);
    }
    for (size_t bus = 0; bus < busOutputs.size(); ++bus) {
      std::fill_n(busBlockLeft.data() + bus * kRenderBlockFrames, count, 0.0f);
      std::fill_n(busBlockRight.data() + bus * kRenderBlockFrames, count, 0.0f);
    }
    for (uint32_t f = 0; f < count; ++f) {
      const auto t = timelineAt(cursor, f);
      for (const auto &voice : voices)
        if (t >= voice.start && t - voice.start < voice.length) {
          const auto local = t - voice.start;
          const auto source = voice.offset + (voice.loopSpan ? local % voice.loopSpan : local);
          float envelope = 1;
          const auto shaped = [](float value, FadeShape shape) {
            if (shape == FadeShape::Linear) return value; // exact legacy path
            // Non-linear curves only describe the normalized fade interval.
            // Without this clamp smoothstep/sine would extrapolate before or
            // after the fade and could invert or mute ordinary clip material.
            value = std::clamp(value, 0.0f, 1.0f);
            if (shape == FadeShape::Smooth) return value * value * (3.0f - 2.0f * value);
            return std::sin(value * 1.57079632679489661923f);
          };
          if (voice.fadeIn)
            envelope = std::min(envelope, shaped(voice.fadeIn == 1 ? 0.0f : float(local) / float(voice.fadeIn - 1), voice.fadeInShape));
          if (voice.fadeOut)
            envelope = std::min(envelope, shaped(voice.fadeOut == 1 ? 0.0f : float(voice.length - 1 - local) / float(voice.fadeOut - 1), voice.fadeOutShape));
          const auto &pcm = voice.clip->samples();
          // Envelope and per-clip gain fold into one scalar; both are
          // prepared off-thread so the callback only multiplies.
          envelope *= voice.gain;
          trackBlockLeft[voice.gainIndex * kRenderBlockFrames + f] +=
              pcm[source * 2] * envelope * voice.panLeft;
          trackBlockRight[voice.gainIndex * kRenderBlockFrames + f] +=
              pcm[source * 2 + 1] * envelope * voice.panRight;
        }
    }
    for (const auto track : activeGains) {
      uint32_t midiCount = 0;
      if (track < midiPlans.size() && !midiPlans[track].empty()) {
        auto &plan = midiPlans[track];
        if (wrapped) {
          midiCount = plan.wrapCutOffs(midiEvents.data(), kMaxMidiEventsPerBlock);
          plan.reset(cursor);
        }
        if (midiCount < kMaxMidiEventsPerBlock)
          midiCount += plan.drain(cursor, cursor + count,
                                  midiEvents.data() + midiCount,
                                  kMaxMidiEventsPerBlock - midiCount);
      }
      if (!processChain(trackEffects[track], trackEffectAutomation[track], trackEffectIDs[track],
                        trackBlockLeft.data() + track * kRenderBlockFrames,
                        trackBlockRight.data() + track * kRenderBlockFrames,
                        count, processTime, cursor,
                        std::span<const PreparedMidiEvent>(midiEvents.data(), midiCount)))
        pluginErrors.fetch_add(1, std::memory_order_relaxed);
    }
    auto *outL = left + done;
    auto *outR = right + done;
    for (uint32_t f = 0; f < count; ++f) {
      const auto t = timelineAt(cursor, f);
      constexpr float smooth = 0.004166667f;
      for (const auto track : activeGains) {
        smoothLeft[track] += (leftGains[track].load(std::memory_order_relaxed) -
                              smoothLeft[track]) *
                             smooth;
        smoothRight[track] +=
            (rightGains[track].load(std::memory_order_relaxed) -
             smoothRight[track]) *
            smooth;
        smoothPreFaderGate[track] +=
            (preFaderGates[track].load(std::memory_order_relaxed) -
             smoothPreFaderGate[track]) *
            smooth;
        smoothLeftPan[track] +=
            (leftPans[track].load(std::memory_order_relaxed) -
             smoothLeftPan[track]) *
            smooth;
        smoothRightPan[track] +=
            (rightPans[track].load(std::memory_order_relaxed) -
             smoothRightPan[track]) *
            smooth;
        smoothTrackFader[track] +=
            (trackFaderGains[track].load(std::memory_order_relaxed) -
             smoothTrackFader[track]) *
            smooth;
      }
      for (const auto track : activeGains) {
        const auto inL = trackBlockLeft[track * kRenderBlockFrames + f],
                   inR = trackBlockRight[track * kRenderBlockFrames + f];
        const bool automatedVolume = !volumeAutomation[track].points.empty(),
                   automatedPan = !panAutomation[track].points.empty();
        const auto fader = automatedVolume ? volumeAutomationGain(track, t)
                                           : smoothTrackFader[track];
        const auto pan = automatedPan
                             ? float(automationValue(panAutomation[track], t))
                             : 0.0f;
        const auto lp = automatedPan ? (pan > 0 ? 1 - pan : 1)
                                     : smoothLeftPan[track],
                   rp = automatedPan ? (pan < 0 ? 1 + pan : 1)
                                     : smoothRightPan[track];
        // The ordinary fader path uses the already-combined smoothed channel
        // gain. Multiplying the three component smoothers would cube the fade
        // curve at every start. Component values are needed only when volume
        // or pan automation replaces part of that combined gain.
        const auto postL = !automatedVolume && !automatedPan
                               ? inL * smoothLeft[track]
                               : inL * fader * lp * smoothPreFaderGate[track];
        const auto postR = !automatedVolume && !automatedPan
                               ? inR * smoothRight[track]
                               : inR * fader * rp * smoothPreFaderGate[track];
        trackPeakLeft[track] = std::max(trackPeakLeft[track], std::abs(postL));
        trackPeakRight[track] = std::max(trackPeakRight[track], std::abs(postR));
        float routeL, routeR;
        trackMainPdc[track].process(postL, postR, routeL, routeR);
        const auto output = trackOutputs[track];
        if (output < 0) {
          outL[f] += routeL;
          outR[f] += routeR;
        } else {
          busBlockLeft[size_t(output) * kRenderBlockFrames + f] += routeL;
          busBlockRight[size_t(output) * kRenderBlockFrames + f] += routeR;
        }
        for (size_t s = sendRanges[track]; s < sendRanges[track + 1]; ++s) {
          const auto sendL = sends[s].preFader ? inL * smoothPreFaderGate[track]
                                               : postL,
                     sendR = sends[s].preFader ? inR * smoothPreFaderGate[track]
                                               : postR;
          sendPdc[s].process(sendL * sends[s].gain, sendR * sends[s].gain,
                             routeL, routeR);
          busBlockLeft[sends[s].bus * kRenderBlockFrames + f] += routeL;
          busBlockRight[sends[s].bus * kRenderBlockFrames + f] += routeR;
        }
      }
    }
    for (const auto bus : busOrder) {
      auto *l = busBlockLeft.data() + bus * kRenderBlockFrames;
      auto *r = busBlockRight.data() + bus * kRenderBlockFrames;
      if (!processChain(busEffects[bus], busEffectAutomation[bus], busEffectIDs[bus], l, r, count, processTime, cursor))
        pluginErrors.fetch_add(1, std::memory_order_relaxed);
      for (uint32_t f = 0; f < count; ++f) {
        constexpr float smooth = 0.004166667f;
        smoothBusLeft[bus] +=
            (busLeftGains[bus].load(std::memory_order_relaxed) -
             smoothBusLeft[bus]) *
            smooth;
        smoothBusRight[bus] +=
            (busRightGains[bus].load(std::memory_order_relaxed) -
             smoothBusRight[bus]) *
            smooth;
        smoothBusFader[bus] +=
            (busFaderGains[bus].load(std::memory_order_relaxed) -
             smoothBusFader[bus]) *
            smooth;
        smoothBusPan[bus] +=
            (busPans[bus].load(std::memory_order_relaxed) - smoothBusPan[bus]) *
            smooth;
        smoothBusGate[bus] += (busGates[bus].load(std::memory_order_relaxed) -
                               smoothBusGate[bus]) *
                              smooth;
        const auto t = timelineAt(cursor, f);
        const bool automated = !busGainAutomation[bus].points.empty();
        const auto fader =
            automated ? gain(automationValue(busGainAutomation[bus], t))
                      : smoothBusFader[bus];
        const auto lp =
                       automated
                           ? (smoothBusPan[bus] > 0 ? 1 - smoothBusPan[bus] : 1)
                           : 1.0f,
                   rp =
                       automated
                           ? (smoothBusPan[bus] < 0 ? 1 + smoothBusPan[bus] : 1)
                           : 1.0f;
        const auto postL = automated ? l[f] * fader * lp * smoothBusGate[bus]
                                     : l[f] * smoothBusLeft[bus],
                   postR = automated ? r[f] * fader * rp * smoothBusGate[bus]
                                     : r[f] * smoothBusRight[bus];
        busPeakLeft[bus] = std::max(busPeakLeft[bus], std::abs(postL));
        busPeakRight[bus] = std::max(busPeakRight[bus], std::abs(postR));
        float routeL, routeR;
        busOutputPdc[bus].process(postL, postR, routeL, routeR);
        const auto output = busOutputs[bus];
        if (output < 0) {
          outL[f] += routeL;
          outR[f] += routeR;
        } else {
          busBlockLeft[size_t(output) * kRenderBlockFrames + f] += routeL;
          busBlockRight[size_t(output) * kRenderBlockFrames + f] += routeR;
        }
      }
    }
    for (uint32_t f = 0; f < count; ++f) {
      smoothMaster +=
          (masterGain.load(std::memory_order_relaxed) - smoothMaster) *
          0.004166667f;
      const auto m = masterGainAutomation.points.empty()
                         ? smoothMaster
                         : gain(automationValue(masterGainAutomation,
                                                timelineAt(cursor, f)));
      outL[f] *= m;
      outR[f] *= m;
    }
    if (!processChain(masterEffects, masterEffectAutomation, masterEffectIDs, outL, outR, count, processTime, cursor))
      pluginErrors.fetch_add(1, std::memory_order_relaxed);
    // Loudness metering reads the final program output but not the click
    // track: EBU practice meters program material, the metronome is monitor.
    loudnessTracker.process(outL, outR, count);
    // The click track joins after master gain and master inserts: monitoring
    // only, never processed, summed to both channels at cursor-relative
    // positions (frame - blockStart). The offline export render suppresses it.
    if (!suppressMetronome && metronomeOn.load(std::memory_order_relaxed))
      mixMetronome(metronomeTimeline, cursor, count, outL, outR);
    processTime += count;
    cursor += count;
    done += count;
  }
  float p = 0, masterPeakL = 0, masterPeakR = 0;
  uint64_t limited = 0;
  for (uint32_t f = 0; f < available; ++f) {
    p = std::max({p, std::abs(left[f]), std::abs(right[f])});
    masterPeakL = std::max(masterPeakL, std::abs(left[f]));
    masterPeakR = std::max(masterPeakR, std::abs(right[f]));
    if (std::abs(left[f]) > 1 || std::abs(right[f]) > 1)
      ++limited;
    left[f] = std::clamp(left[f], -1.0f, 1.0f);
    right[f] = std::clamp(right[f], -1.0f, 1.0f);
  }
  clipped.fetch_add(limited, std::memory_order_relaxed);
  peak.store(p, std::memory_order_relaxed);
  for (size_t index = 0; index < preparedTrackCount; ++index) {
    trackMeterLeft[index].store(trackPeakLeft[index], std::memory_order_release);
    trackMeterRight[index].store(trackPeakRight[index], std::memory_order_release);
  }
  for (size_t index = 0; index < preparedBusCount; ++index) {
    busMeterLeft[index].store(busPeakLeft[index], std::memory_order_release);
    busMeterRight[index].store(busPeakRight[index], std::memory_order_release);
  }
  masterMeterLeft.store(masterPeakL, std::memory_order_release);
  masterMeterRight.store(masterPeakR, std::memory_order_release);
  renderedInputFrames += available;
  updateAudiblePosition();
  position.store(cursor, std::memory_order_relaxed);
  if (!looping && cursor == length)
    playing.store(false, std::memory_order_relaxed);
}
} // namespace daw
