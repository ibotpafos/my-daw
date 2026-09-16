#pragma once
#include <cstddef>
#include <cstdint>
#include "audio/clip.hpp"
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>
namespace daw {
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
enum class FadeShape : uint8_t { Linear=0, Smooth=1, EqualPower=2 };
constexpr bool isFadeShape(FadeShape shape) noexcept { return shape==FadeShape::Linear || shape==FadeShape::Smooth || shape==FadeShape::EqualPower; }
// Shapes deliberately trail the historic aggregate fields, so old aggregate
// initializers keep their exact linear behaviour.
struct Region { uint64_t start=0, sourceOffset=0, length=0, fadeIn=0, fadeOut=0; uint32_t take=0; double gain=0.0; uint32_t color=0; bool muted=false, looped=false; double pan=0.0; FadeShape fadeInShape=FadeShape::Linear, fadeOutShape=FadeShape::Linear; bool autoFadeIn=false, autoFadeOut=false; bool operator==(const Region&) const = default; };
struct Take { std::string name; uint64_t start=0; std::shared_ptr<const Clip> audio; bool operator==(const Take&) const = default; };
struct Send { uint64_t bus=0; double gain=-12; bool preFader=false; bool operator==(const Send&) const = default; };
// Ordered timeline points for the track fader. Frames are project frames at
// the fixed 48 kHz session rate; an empty lane means the static track gain.
struct AutomationPoint { uint64_t frame=0; double gainDb=0; bool operator==(const AutomationPoint&) const = default; };
enum class AutomationTarget : uint8_t { TrackVolume=1, TrackPan=2, BusGain=3, MasterGain=4 };
enum class AutomationWriteMode : uint8_t { Touch=1, Latch=2 };
enum class PluginOwner : uint8_t { Track=1, Bus=2, Master=3 };
enum class PluginHostingMode : uint8_t { InProcess=1, OutOfProcess=2 };
struct PluginParameterAutomationPoint { uint64_t frame=0; double normalizedValue=0; bool operator==(const PluginParameterAutomationPoint&) const = default; };
struct PluginParameterAutomationLane { uint32_t parameterID=0; std::string name; std::vector<PluginParameterAutomationPoint> points; bool operator==(const PluginParameterAutomationLane&) const = default; };
enum class WorkflowOperationKind : uint8_t { RenameTrack, SetTrackGain };
// A deliberately small atomic workflow vocabulary. New kinds must be added
// explicitly so preview and commit always have identical semantics.
struct WorkflowOperation { WorkflowOperationKind kind=WorkflowOperationKind::RenameTrack; uint64_t trackID=0; std::string name; double gainDb=0; };
struct WorkflowTrackSummary { uint64_t id=0; std::string name; double gainDb=0; bool operator==(const WorkflowTrackSummary&) const = default; };
struct WorkflowPreview { uint64_t baseRevision=0; uint64_t afterRevision=0; std::vector<WorkflowTrackSummary> before; std::vector<WorkflowTrackSummary> after; };
// Analysis stays outside the domain. This is the bounded result it may supply
// to the deterministic vocal-track planner.
struct VocalGainSuggestion { uint64_t trackID=0; double suggestedGainDb=0; };
// Audio Units retain their historic three four-character component IDs.  A
// VST3 has no compatible component triple, so the all-zero triple is a durable
// sentinel.  Its state is always a validated MDVS envelope; the envelope owns
// the VST3 class FUID and module identity.  Keeping this distinction inside
// the existing state BLOB preserves legacy AU projects while format v13 adds
// owner-scoped insert persistence without changing the plug-in payload.
constexpr uint32_t kVst3PluginComponentSentinel = 0;
constexpr size_t kMaxAudioUnitPluginStateBytes = 1024 * 1024;
constexpr size_t kMaxVst3PluginStateBytes = 16 * 1024 * 1024;
constexpr size_t kMaxProjectPluginStateBytes = 16 * 1024 * 1024;
constexpr size_t kMaxPluginInsertsPerOwner = 4;
constexpr size_t kMaxProjectPluginInserts = 64;
constexpr size_t kMaxPluginParameterAutomationLanes = 128;
constexpr size_t kMaxPluginParameterAutomationPoints = 2048;
constexpr size_t kMaxProjectPluginParameterAutomationPoints = 65536;
// MIDI-1 data layer: notes are stored clip-relative on the 48 kHz project
// timeline. The lane is an editor row owned by the UI; the domain only keeps
// it non-negative. Rendering, delivery to instruments, and UI arrive in later
// arcs, so nothing consumes these vectors yet.
constexpr uint64_t kMaxMidiFrame = 1ULL << 40;
constexpr uint64_t kMaxMidiNoteLength = 10ULL * 48000;
constexpr size_t kMaxMidiClipsPerTrack = 64;
constexpr size_t kMaxMidiNotesPerProject = 65536;
struct MidiNote { uint64_t start=0, length=0; uint8_t pitch=0, channel=0, velocity=0; bool operator==(const MidiNote&) const = default; };
struct MidiClip { uint64_t start=0, length=0; std::vector<MidiNote> notes; int track=-1; uint32_t color=0; bool operator==(const MidiClip&) const = default; };
// Project tempo/time-signature maps. Both lanes are ordered by project frame
// (48 kHz, bounded by kMaxMidiFrame), never duplicate a frame, and always keep
// their primary point at frame 0 (120 BPM, 4/4). Frames are absolute timeline
// positions; a point holds effect until the next point or session end.
constexpr double kMinTempoBpm=20.0, kMaxTempoBpm=999.0; // valid range is (20, 999]
constexpr double kDefaultTempoBpm=120.0;
constexpr size_t kMaxTempoPointsPerProject=64;
constexpr size_t kMaxTimeSignaturePointsPerProject=64;
constexpr uint8_t kDefaultTimeSignatureNumerator=4, kDefaultTimeSignatureDenominator=8;
struct TempoPoint { uint64_t frame=0; double bpm=kDefaultTempoBpm; bool operator==(const TempoPoint&) const = default; };
struct TimeSignaturePoint { uint64_t frame=0; uint8_t numerator=kDefaultTimeSignatureNumerator, denominator=kDefaultTimeSignatureDenominator; bool operator==(const TimeSignaturePoint&) const = default; };
// Time signatures use power-of-two note values: 1, 2, 4, 8, 16 or 32.
constexpr bool isTimeSignatureDenominator(uint8_t denominator) noexcept {
    return denominator!=0 && (denominator & (denominator-1))==0 && denominator<=32;
}
// Project markers (locators) name positions on the shared 48 kHz timeline.
// The lane is ordered by frame, never duplicates a frame, and stays below
// kMaxMidiFrame. Unlike the tempo and meter maps a marker lane is optional:
// validate() accepts the empty vector, so a fresh project carries none. Names
// reuse the shared track-name rule (validateName: strict UTF-8, 1-120 characters).
constexpr size_t kMaxMarkersPerProject=256;
struct Marker { uint64_t frame=0; std::string name; bool operator==(const Marker&) const = default; };
struct PluginInsert { uint64_t id=0; uint32_t type=0, subtype=0, manufacturer=0; std::string name; bool bypassed=false; uint32_t latencyFrames=0; std::vector<uint8_t> state; std::vector<PluginParameterAutomationLane> parameterAutomation; PluginHostingMode hostingMode=PluginHostingMode::InProcess; bool operator==(const PluginInsert&) const = default; };
bool isVst3PluginInsert(const PluginInsert& plugin) noexcept;
struct Track { uint64_t id; std::string name; double gain; uint32_t color = 0; std::shared_ptr<const Clip> audio = {}; std::vector<Region> regions; double pan=0; bool muted=false, solo=false; uint64_t baseStart=0; std::vector<Take> takes; uint64_t outputBus=0; std::vector<Send> sends; std::vector<AutomationPoint> volumeAutomation; std::vector<AutomationPoint> panAutomation; std::vector<PluginInsert> inserts; std::vector<MidiClip> midiClips; bool operator==(const Track&) const = default; };
struct Bus { uint64_t id=0; std::string name; double gain=0; double pan=0; bool muted=false; bool solo=false; uint64_t outputBus=0; std::vector<AutomationPoint> gainAutomation; std::vector<PluginInsert> inserts; bool operator==(const Bus&) const = default; };
struct State {
    uint64_t revision = 0;
    uint64_t nextID = 1;
    std::vector<Track> tracks;
    double masterGain=0;
    bool masterSolo=false;
    std::vector<AutomationPoint> masterGainAutomation;
    std::vector<Bus> buses;
    std::vector<PluginInsert> masterInserts;
    // Every project carries at least the primary frame-0 point; validate()
    // rejects empty or unsorted lanes, so the beat converters below never
    // have to model a "no tempo" case beyond their defensive defaults.
    std::vector<TempoPoint> tempo{{0,kDefaultTempoBpm}};
    std::vector<TimeSignaturePoint> timeSignatures{{0,kDefaultTimeSignatureNumerator,kDefaultTimeSignatureDenominator}};
    // Optional locator lane; the empty vector is a valid default. Commands keep
    // it sorted by frame, so readers and undo snapshots never see a gap or a
    // duplicate position.
    std::vector<Marker> markers;
    // VST3 editor proxy state for isolated inserts. One editor per insert; the
    // view is managed in a disposable helper process via shared memory.
    struct Vst3EditorState {
        std::string viewType;   // e.g. "editor", "generic"
        int32_t width = 0;
        int32_t height = 0;
        bool attached = false;
    };
    std::vector<Vst3EditorState> vst3Editors;
    // Beat positions count whole quarter notes from frame 0 across the tempo
    // map: one beat spans 60/bpm seconds at the fixed 48 kHz project rate.
    // Conversion accumulates per-segment durations in long double and rounds
    // once, so at the kMaxMidiFrame limit (2^40 frames ≈ 36.5 hours) the
    // round-trip error stays under one frame (well below 1 ms); every frame
    // integer is exactly representable in double along the whole timeline.
    double bpmAtFrame(uint64_t frame) const;
    double beatsAtFrame(uint64_t frame) const;
    // The inverse of beatsAtFrame, rounded to the nearest frame. Negative,
    // non-finite, or past-timeline beat positions throw Error.
    uint64_t frameAtBeats(double beats) const;
};
void validateName(const std::string& name);
void validate(const State& state);
std::vector<WorkflowOperation> prepareVocalTracks(const State& state,
                                                   const std::vector<uint64_t>& selectedTrackIDs,
                                                   const std::string& namingBase,
                                                   const std::vector<VocalGainSuggestion>& suggestions);
class Session {
public:
    const State& state() const { return current; }
    bool canUndo() const { return !past.empty(); }
    bool canRedo() const { return !future.empty(); }
    void add(const std::string&, uint64_t expected);
    // Removes the complete track object in one revision.  The snapshot-based
    // history retains its clips, takes, routing, automation, and inserts so
    // Undo restores the exact prior track without allocating new IDs.
    void removeTrack(uint64_t id, uint64_t expected);
    // Places the identified track at newIndex in the resulting track order.
    // newIndex is zero-based and must be strictly less than track count; a
    // move to the current index is a validated no-op and does not commit.
    void moveTrack(uint64_t id, uint32_t newIndex, uint64_t expected);
    void import(const std::string&, std::shared_ptr<const Clip>, uint64_t expected);
    void importAt(const std::string&, std::shared_ptr<const Clip>, uint64_t start, uint64_t expected);
    void rename(uint64_t id, const std::string&, uint64_t expected);
    void gain(uint64_t id, double value, uint64_t expected);
    void pan(uint64_t id, double value, uint64_t expected);
    void mute(uint64_t id, bool value, uint64_t expected);
    void solo(uint64_t id, bool value, uint64_t expected);
    // Track mute/solo/color/gain/duplicate follow the audio-clip convention:
    // expected revision, silent no-op when the target value is already identical,
    // and one snapshot-based undo entry per committed command.
    void setTrackMuted(uint64_t id, bool muted, uint64_t expected);
    void setTrackSolo(uint64_t id, bool solo, uint64_t expected);
    void setTrackColor(uint64_t id, uint32_t color, uint64_t expected);
    void setTrackGain(uint64_t id, double gainDb, uint64_t expected);
    uint64_t duplicateTrack(uint64_t id, uint64_t expected);
    void masterGain(double value, uint64_t expected);
    void setMasterSolo(bool solo, uint64_t expected);
    void addBus(const std::string& name,uint64_t expected);
    void deleteBus(uint64_t id,uint64_t expected);
    void renameBus(uint64_t id,const std::string& name,uint64_t expected);
    void busGain(uint64_t id,double value,uint64_t expected);
    void busPan(uint64_t id,double value,uint64_t expected);
    void busMute(uint64_t id,bool value,uint64_t expected);
    void busSolo(uint64_t id,bool value,uint64_t expected);
    void routeTrack(uint64_t trackID,uint64_t busID,uint64_t expected);
    void routeBus(uint64_t busID,uint64_t outputBusID,uint64_t expected);
    void upsertSend(uint64_t trackID,uint64_t busID,double gain,bool preFader,uint64_t expected);
    void removeSend(uint64_t trackID,uint64_t busID,uint64_t expected);
    void upsertTrackVolumeAutomation(uint64_t trackID,uint64_t frame,double gainDb,uint64_t expected);
    void removeTrackVolumeAutomation(uint64_t trackID,uint64_t frame,uint64_t expected);
    void upsertTrackPanAutomation(uint64_t trackID,uint64_t frame,double pan,uint64_t expected);
    void removeTrackPanAutomation(uint64_t trackID,uint64_t frame,uint64_t expected);
    void upsertBusGainAutomation(uint64_t busID,uint64_t frame,double gainDb,uint64_t expected);
    void removeBusGainAutomation(uint64_t busID,uint64_t frame,uint64_t expected);
    void upsertMasterGainAutomation(uint64_t frame,double gainDb,uint64_t expected);
    void removeMasterGainAutomation(uint64_t frame,uint64_t expected);
    // A gesture isolates high-rate fader writes from the authoritative State.
    // It validates every sample but performs exactly one commit/Undo entry at
    // end. Latch writes a terminal hold point at endFrame; Touch does not.
    void beginAutomationGesture(AutomationTarget target,uint64_t targetID,AutomationWriteMode mode,uint64_t expected);
    void writeAutomationGesture(uint64_t frame,double value);
    void endAutomationGesture(uint64_t endFrame,uint64_t expected);
    void cancelAutomationGesture() noexcept;
    bool automationGestureActive() const noexcept;
    WorkflowPreview previewWorkflow(const std::vector<WorkflowOperation>& batch,uint64_t expected) const;
    void commitWorkflow(const std::vector<WorkflowOperation>& batch,uint64_t expected);
    void addMasterInsert(PluginInsert plugin,uint64_t expected);
    void moveMasterInsert(uint64_t id,uint32_t newIndex,uint64_t expected);
    void removeMasterInsert(uint64_t id,uint64_t expected);
    void bypassMasterInsert(uint64_t id,bool bypassed,uint64_t expected);
    void setMasterInsertHostingMode(uint64_t id,PluginHostingMode mode,uint64_t expected);
    void updateMasterInsertState(uint64_t id,std::vector<uint8_t> state,uint32_t latencyFrames,uint64_t expected);
    void addTrackInsert(uint64_t trackID,PluginInsert plugin,uint64_t expected);
    void moveTrackInsert(uint64_t trackID,uint64_t id,uint32_t newIndex,uint64_t expected);
    void removeTrackInsert(uint64_t trackID,uint64_t id,uint64_t expected);
    void bypassTrackInsert(uint64_t trackID,uint64_t id,bool bypassed,uint64_t expected);
    void setTrackInsertHostingMode(uint64_t trackID,uint64_t id,PluginHostingMode mode,uint64_t expected);
    void updateTrackInsertState(uint64_t trackID,uint64_t id,std::vector<uint8_t> state,uint32_t latencyFrames,uint64_t expected);
    void addBusInsert(uint64_t busID,PluginInsert plugin,uint64_t expected);
    void moveBusInsert(uint64_t busID,uint64_t id,uint32_t newIndex,uint64_t expected);
    void removeBusInsert(uint64_t busID,uint64_t id,uint64_t expected);
    void bypassBusInsert(uint64_t busID,uint64_t id,bool bypassed,uint64_t expected);
    void setBusInsertHostingMode(uint64_t busID,uint64_t id,PluginHostingMode mode,uint64_t expected);
    void updateBusInsertState(uint64_t busID,uint64_t id,std::vector<uint8_t> state,uint32_t latencyFrames,uint64_t expected);
    void upsertPluginParameterAutomation(PluginOwner owner,uint64_t ownerID,uint64_t pluginID,uint32_t parameterID,const std::string& name,uint64_t frame,double normalizedValue,uint64_t expected);
    void removePluginParameterAutomation(PluginOwner owner,uint64_t ownerID,uint64_t pluginID,uint32_t parameterID,uint64_t frame,uint64_t expected);
    void beginPluginParameterAutomationGesture(PluginOwner owner,uint64_t ownerID,uint64_t pluginID,uint32_t parameterID,const std::string& name,AutomationWriteMode mode,uint64_t expected);
    void writePluginParameterAutomationGesture(uint64_t frame,double normalizedValue);
    void endPluginParameterAutomationGesture(uint64_t endFrame,uint64_t expected);
    void cancelPluginParameterAutomationGesture() noexcept;
    bool pluginParameterAutomationGestureActive() const noexcept;
    void editClip(uint64_t id, uint32_t index, uint64_t start, uint64_t offset, uint64_t length, uint64_t expected);
    void editClipFull(uint64_t id, uint32_t index, uint64_t start, uint64_t offset, uint64_t length, uint64_t fadeIn, uint64_t fadeOut, uint64_t expected);
    void splitClip(uint64_t id, uint32_t index, uint64_t frame, uint64_t expected);
    void duplicateClip(uint64_t id, uint32_t index, uint64_t expected);
    void deleteClip(uint64_t id, uint32_t index, uint64_t expected);
    void setClipFades(uint64_t id, uint32_t index, uint64_t fadeIn, uint64_t fadeOut, uint64_t expected);
    void setCrossfade(uint64_t id,uint32_t leftIndex,uint64_t duration,uint64_t expected);
    // Atomically creates, resizes, or removes a crossfade and assigns its
    // shared curve. This is intentionally separate from setCrossfade() so the
    // legacy operation keeps its exact linear/no-op semantics.
    void setCrossfadeShaped(uint64_t id,uint32_t leftIndex,uint64_t duration,FadeShape shape,uint64_t expected);
    void setClipFadeShapes(uint64_t id,uint32_t index,FadeShape fadeIn,FadeShape fadeOut,uint64_t expected);
    void setCrossfadeShape(uint64_t id,uint32_t leftIndex,FadeShape shape,uint64_t expected);
    // MIDI clip commands follow the audio-clip convention: positional vector
    // index, expected revision, silent no-op when the target value is already
    // identical, and one snapshot-based undo entry per committed command.
    void addMidiClip(uint64_t trackID,MidiClip clip,uint64_t expected);
    void removeMidiClip(uint64_t trackID,uint32_t index,uint64_t expected);
    void setMidiNotes(uint64_t trackID,uint32_t index,std::vector<MidiNote> notes,uint64_t expected);
    // Live-record append: adds every note of `batch` to the end of the clip's
    // existing note vector under exactly the same validate() rules as the other
    // MIDI commands. Capture order is preserved verbatim — the model never
    // requires a sorted lane, only that clips stay disjoint and each note fits
    // whole inside its own clip. The clip window is never grown, so a note past
    // its end is rejected just as it would be by setMidiNotes. One batch is
    // bounded by the project note budget, an empty batch is a silent no-op, and
    // a committed append is one undo entry.
    void appendMidiNotes(uint64_t trackID,uint32_t index,const std::vector<MidiNote>& batch,uint64_t expected);
    void moveMidiClip(uint64_t trackID,uint32_t index,uint64_t newStart,uint64_t expected);
    void trimMidiClip(uint64_t trackID,uint32_t index,uint64_t newStart,uint64_t newLength,uint64_t expected);
    void splitMidiClip(uint64_t trackID,uint32_t index,uint64_t atFrame,uint64_t expected);
    // Clip and MIDI-clip property commands: expected revision, silent no-op when
    // the target value is already identical, one snapshot-based undo entry.
    void setClipColor(uint64_t id, uint32_t clipIndex, uint32_t color, uint64_t expected);
    void setClipGain(uint64_t id, uint32_t clipIndex, double gainDb, uint64_t expected);
    // Playback state (v20): mute drops the region's voice from the render plan;
    // loop lets a region outlive its slice and wraps reads inside
    // [sourceOffset, sourceFrames). Un-looping a region longer than its slice
    // honestly rejects through validate().
    void setClipMuted(uint64_t id, uint32_t clipIndex, bool muted, uint64_t expected);
    void setClipLooped(uint64_t id, uint32_t clipIndex, bool looped, uint64_t expected);
    // Stereo pan in normalized position -1..1 (0 = center), the same linear
    // unity-center law as the track fader's pan.
    void setClipPan(uint64_t id, uint32_t clipIndex, double pan, uint64_t expected);
    // Multi-selection group operations: one revision for the whole group.
    // deleteClips sorts/uniques indices; when they cover every audio region it
    // clears the media and leaves the same reusable empty track. nudgeClips
    // translates selected regions together and re-sorts by start, so
    // overlapping audio clips automatically receive a linear crossfade whose
    // duration equals their overlap; validate() retains the geometry limits.
    void deleteClips(uint64_t id, std::vector<uint32_t> indices, uint64_t expected);
    void nudgeClips(uint64_t id, std::vector<uint32_t> indices, int64_t deltaFrames, uint64_t expected);
    void setMidiClipColor(uint64_t trackID, uint32_t index, uint32_t color, uint64_t expected);
    void transposeMidiClip(uint64_t trackID, uint32_t index, int8_t semitones, uint64_t expected);
    void quantizeMidiClip(uint64_t trackID, uint32_t index, double gridBeats, uint64_t expected);
    // Cross-track clipboard moves. Copy keeps the source; a move retains the
    // source track's media ownership, so its lone region cannot leave even
    // though deleteClip may instead turn that source track empty.
    // Paste of an audio region requires the target track to carry the very same
    // take at the region's take index (take indices are track-local). MIDI keeps
    // its lane: validate() only requires non-negativity. Audio paste/move may
    // overlap and automatically assigns a linear crossfade; timeline limits
    // and every capacity rule stay owned by validate().
    void copyClipToTrack(uint64_t sourceTrack, uint32_t index, uint64_t targetTrack, uint64_t start, uint64_t expected);
    void moveClipToTrack(uint64_t sourceTrack, uint32_t index, uint64_t targetTrack, uint64_t start, uint64_t expected);
    void copyMidiClipToTrack(uint64_t sourceTrack, uint32_t index, uint64_t targetTrack, uint64_t start, uint64_t expected);
    void moveMidiClipToTrack(uint64_t sourceTrack, uint32_t index, uint64_t targetTrack, uint64_t start, uint64_t expected);
    // Tempo/time-signature map commands follow the automation convention:
    // expected revision, upsert replaces a same-frame point, an identical
    // value is a silent no-op, and the primary frame-0 point cannot be
    // removed because the project must always carry one tempo and one meter.
    void setTempoAt(uint64_t frame,double bpm,uint64_t expected);
    void removeTempo(uint64_t frame,uint64_t expected);
    void setTimeSignatureAt(uint64_t frame,uint8_t numerator,uint8_t denominator,uint64_t expected);
    void removeTimeSignature(uint64_t frame,uint64_t expected);
    // Marker commands follow the tempo convention: expected revision, one
    // snapshot-based undo entry per committed command, and a silent no-op when
    // the name is already identical. A duplicate frame is rejected rather than
    // upserted because a locator identifies a unique position. Frames never
    // move: relocating a marker is a remove plus add pair in v0.
    void addMarker(uint64_t frame,const std::string& name,uint64_t expected);
    void removeMarker(uint64_t frame,uint64_t expected);
    void renameMarker(uint64_t frame,const std::string& name,uint64_t expected);
    void addTake(uint64_t id,const std::string& name,std::shared_ptr<const Clip>,uint64_t start,uint64_t expected);
    void addTakes(uint64_t id,std::vector<Take> takes,uint64_t expected);
    // First audio recorded into an otherwise ordinary empty track becomes its
    // base clip, preserving the selected track identity and settings.
    void materializeRecordedTrack(uint64_t id,const std::string& name,std::shared_ptr<const Clip>,uint64_t start,uint64_t expected);
    void compRange(uint64_t id,uint32_t take,uint64_t start,uint64_t length,uint64_t expected);
    void undo(uint64_t expected);
    void redo(uint64_t expected);
    void replace(State state);

    // VST3 editor proxy for isolated inserts. Requires OutOfProcess hosting mode
    // and the VST3 editor helper (DAW_BUILD_VST3_EDITOR_HELPER).
    void openVst3Editor(const PluginInsert& insert, const std::string& viewType, uint64_t expected);
    void closeVst3Editor(const PluginInsert& insert, uint64_t expected);
    bool resizeVst3Editor(const PluginInsert& insert, int32_t width, int32_t height, uint64_t expected);
    float getVst3EditorParameter(const PluginInsert& insert, uint32_t parameterID, uint64_t expected);
    void setVst3EditorParameter(const PluginInsert& insert, uint32_t parameterID, float normalizedValue, uint64_t expected);
    void idleVst3Editor(const PluginInsert& insert, uint64_t expected);
    State::Vst3EditorState vst3EditorView(const PluginInsert& insert) const;
private:
    State current;
    std::vector<State> past, future;
    struct AutomationGesture { AutomationTarget target; uint64_t targetID; AutomationWriteMode mode; uint64_t baseRevision; State working; bool hasWritten=false; uint64_t lastFrame=0; double lastValue=0; };
    struct PluginParameterAutomationGesture { PluginOwner owner; uint64_t ownerID; uint64_t pluginID; uint32_t parameterID; std::string name; AutomationWriteMode mode; uint64_t baseRevision; State working; bool hasWritten=false; bool changed=false; uint64_t lastFrame=0; double lastValue=0; };
    std::optional<AutomationGesture> gesture;
    std::optional<PluginParameterAutomationGesture> pluginParameterGesture;
    void check(uint64_t expected) const;
    void commit(State next);
};
State readDraft(const std::string& path);
// Optional observer for deterministic process-crash tests: 1=temp, 2=snapshot written/fsynced/closed, 3=renamed.
using SaveObserver = void (*)(int);
void writeDraft(const State&, const std::string& path, SaveObserver observer = nullptr);
}
