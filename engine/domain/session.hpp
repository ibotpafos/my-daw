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
struct Region { uint64_t start=0, sourceOffset=0, length=0, fadeIn=0, fadeOut=0; uint32_t take=0; bool operator==(const Region&) const = default; };
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
struct PluginInsert { uint64_t id=0; uint32_t type=0, subtype=0, manufacturer=0; std::string name; bool bypassed=false; uint32_t latencyFrames=0; std::vector<uint8_t> state; std::vector<PluginParameterAutomationLane> parameterAutomation; PluginHostingMode hostingMode=PluginHostingMode::InProcess; bool operator==(const PluginInsert&) const = default; };
bool isVst3PluginInsert(const PluginInsert& plugin) noexcept;
struct Track { uint64_t id; std::string name; double gain; std::shared_ptr<const Clip> audio = {}; std::vector<Region> regions; double pan=0; bool muted=false, solo=false; uint64_t baseStart=0; std::vector<Take> takes; uint64_t outputBus=0; std::vector<Send> sends; std::vector<AutomationPoint> volumeAutomation; std::vector<AutomationPoint> panAutomation; std::vector<PluginInsert> inserts; bool operator==(const Track&) const = default; };
struct Bus { uint64_t id=0; std::string name; double gain=0; double pan=0; bool muted=false; uint64_t outputBus=0; std::vector<AutomationPoint> gainAutomation; std::vector<PluginInsert> inserts; bool operator==(const Bus&) const = default; };
struct State { uint64_t revision = 0; uint64_t nextID = 1; std::vector<Track> tracks; double masterGain=0; std::vector<AutomationPoint> masterGainAutomation; std::vector<Bus> buses; std::vector<PluginInsert> masterInserts; };
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
    void import(const std::string&, std::shared_ptr<const Clip>, uint64_t expected);
    void importAt(const std::string&, std::shared_ptr<const Clip>, uint64_t start, uint64_t expected);
    void rename(uint64_t id, const std::string&, uint64_t expected);
    void gain(uint64_t id, double value, uint64_t expected);
    void pan(uint64_t id, double value, uint64_t expected);
    void mute(uint64_t id, bool value, uint64_t expected);
    void solo(uint64_t id, bool value, uint64_t expected);
    void masterGain(double value, uint64_t expected);
    void addBus(const std::string& name,uint64_t expected);
    void renameBus(uint64_t id,const std::string& name,uint64_t expected);
    void busGain(uint64_t id,double value,uint64_t expected);
    void busPan(uint64_t id,double value,uint64_t expected);
    void busMute(uint64_t id,bool value,uint64_t expected);
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
    void addTake(uint64_t id,const std::string& name,std::shared_ptr<const Clip>,uint64_t start,uint64_t expected);
    void addTakes(uint64_t id,std::vector<Take> takes,uint64_t expected);
    void compRange(uint64_t id,uint32_t take,uint64_t start,uint64_t length,uint64_t expected);
    void undo(uint64_t expected);
    void redo(uint64_t expected);
    void replace(State state);
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
