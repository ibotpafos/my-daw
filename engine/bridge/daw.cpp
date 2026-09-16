#include "daw.h"
#include "domain/session.hpp"
#include "audio/output.hpp"
#include "audio/import_job.hpp"
#include "audio/input.hpp"
#include "audio/duplex.hpp"
#include "audio/recording.hpp"
#include "audio/export.hpp"
#include "audio/loudness.hpp"
#include "audio/effect.hpp"
#include "audio/plugin_parameters.hpp"
#include "audio/analysis.hpp"
#include "plugins/plugin_descriptor.hpp"
#include "plugins/vst3_catalog.hpp"
#include "export/dawproject_export.hpp"
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach_time.h>
#include "platform/macos/midi_input.hpp"
#include "platform/macos/au_scanner.hpp"
#include "platform/macos/au_scan_cache.hpp"
#include "platform/macos/vst3_scanner.hpp"
#include "platform/macos/vst3_scan_cache.hpp"
#endif
#include "storage/save_job.hpp"
#include "storage/project_package.hpp"
#include "jobs/limiter.hpp"
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#ifndef DAW_VST3_RUNTIME_AVAILABLE
#define DAW_VST3_RUNTIME_AVAILABLE 0
#endif
struct LivePluginParameterGesture {
    uint64_t pluginID = 0;
    uint32_t parameterID = 0;
};
struct Vst3ParameterCache {
    uint64_t insertID = 0, revision = 0;
    std::vector<daw::Vst3Parameter> parameters;
};
namespace {
enum class PlaybackPreparationStatus : uint32_t {
    preparing = 0,
    ready = 1,
    failed = 2,
    canceled = 3,
    consumed = 4
};
struct PlaybackPreparation {
    uint64_t revision = 0, generation = 0, startFrame = 0, loopStart = 0, loopEnd = 0;
    std::atomic<PlaybackPreparationStatus> status{PlaybackPreparationStatus::preparing};
    std::atomic<bool> cancel{false};
    std::mutex publication;
    std::unique_ptr<daw::Output> output;
    std::string error;
    std::shared_ptr<daw::BackgroundJobPermit> permit;
};
}
struct daw_session {
    daw::Session model;
    std::unique_ptr<daw::Output> output;
    std::shared_ptr<PlaybackPreparation> playbackPreparation;
    std::unique_ptr<daw::Input> input;
    std::unique_ptr<daw::Duplex> duplex;
    std::vector<daw::AudioUnitDescriptor> auCatalog;
    std::vector<daw::Vst3ScanCacheEntry> vst3Catalog;
    std::optional<LivePluginParameterGesture> pluginParameterGesture;
    std::optional<Vst3ParameterCache> vst3ParameterCache;
    uint64_t playbackGeneration = 1, lastCallbacks = 0, recordLastCallbacks = 0, selectedFrame = 0,
             recordStart = 0, recordTarget = 0, loopStart = 0, loopEnd = 0;
    bool loopEnabled = false;
    int32_t transientOutputState = 0;
    std::chrono::steady_clock::time_point lastProgress, recordProgress;
    std::shared_ptr<uint8_t> lifetime = std::make_shared<uint8_t>(0);
    uint64_t projectEpoch = 1;
#ifdef __APPLE__
    // One live CoreMIDI capture source for the whole session (v0 limit) plus
    // the enumeration the device queries report.
    std::unique_ptr<daw::MidiInput> midiInput;
    std::vector<daw::MidiInputDevice> midiDevices;
    // Drains reuse both buffers so a 10 Hz UI poll does not allocate.
    std::vector<daw::MidiCapturedEvent> midiCaptured;
#endif
    std::vector<daw::RecordedMidiEvent> midiConverted;
    // Live MIDI take: the recorder itself is platform-free, so arming, polling
    // and status keep their shape on every build; only opening a source needs
    // CoreMIDI. midiAnchor* is the (transport frame, host clock) drain base
    // documented in daw.h and midiClipStart the clip start it is rebased on.
    std::optional<daw::MidiRecorder> midiRecorder;
    uint32_t midiInputID = 0;
    uint64_t midiAnchorNs = 0, midiAnchorFrame = 0, midiClipStart = 0, midiRingBase = 0;
    // Metronome intent lives here, not on a renderer: every graph this session
    // prepares inherits it, so the switch survives stop -> play and a device
    // change, and nothing of it is persisted into the project.
    bool metronomeEnabled = false;
    uint64_t recordPrerollFrames = 0;
    bool recordMonitor = false;
    bool autoMonitorOnArm = true; /* arm → auto-enable input monitoring */
    char error[512] = {};
};
struct daw_save_job {
    std::shared_ptr<daw::SaveResult> result;
};
struct daw_export_job {
    std::shared_ptr<daw::ExportResult> result;
};
struct daw_dawproject_job {
    std::shared_ptr<daw::dawproject::ExportResult> result;
};
enum class ImportIntent : uint8_t { Track, Take };
enum class ImportFormat : uint8_t { Wav, Aiff };
struct daw_import_job {
    std::shared_ptr<daw::ImportJobResult> result;
    std::weak_ptr<uint8_t> owner;
    uint64_t projectEpoch = 0, baseRevision = 0, trackID = 0, startFrame = 0;
    ImportIntent intent = ImportIntent::Track;
    ImportFormat format = ImportFormat::Wav;
    std::string name;
};
#ifdef __APPLE__
struct daw_au_scan_job {
    std::future<daw::IsolatedAudioUnitScan> future;
    std::optional<daw::IsolatedAudioUnitScan> result;
};
struct daw_vst3_scan_job {
    std::future<daw::IsolatedVst3Scan> future;
    std::optional<daw::IsolatedVst3Scan> result;
};
#else
struct daw_au_scan_job {};
struct daw_vst3_scan_job {};
#endif
namespace {
template <class Fn> int guard(daw_session *s, Fn fn) noexcept {
    if (!s)
        return 1;
    try {
        fn();
        s->error[0] = 0;
        return 0;
    } catch (const std::exception &e) {
        std::snprintf(s->error, sizeof(s->error), "%s", e.what());
    } catch (...) {
        std::snprintf(s->error, sizeof(s->error), "Unknown core error");
    }
    return 1;
}
uint64_t duration(daw_session *s) {
    uint64_t frames = 0;
    for (const auto &t : s->model.state().tracks)
        for (const auto &region : t.regions)
            frames = std::max(frames, region.start + region.length);
    return frames;
}
void cancelPlaybackPreparation(daw_session *s) noexcept {
    auto preparation = std::move(s->playbackPreparation);
    if (!preparation)
        return;
    preparation->cancel.store(true, std::memory_order_release);
    std::unique_ptr<daw::Output> readyOutput;
    {
        std::lock_guard lock(preparation->publication);
        readyOutput = std::move(preparation->output);
        auto status = preparation->status.load(std::memory_order_acquire);
        if (status == PlaybackPreparationStatus::preparing ||
            status == PlaybackPreparationStatus::ready)
            preparation->status.store(PlaybackPreparationStatus::canceled,
                                      std::memory_order_release);
    }
}
void invalidatePlaybackPreparation(daw_session *s) noexcept {
    ++s->playbackGeneration;
    cancelPlaybackPreparation(s);
}
void cancelStalePlaybackPreparation(daw_session *s) noexcept {
    if (s->playbackPreparation)
        invalidatePlaybackPreparation(s);
}
void beginPlaybackPreparation(daw_session *s) {
    invalidatePlaybackPreparation(s);
    if (s->selectedFrame >= duration(s))
        s->selectedFrame = 0;
    if (s->output) {
        if (s->output->renderer.playing.load(std::memory_order_acquire))
            return;
        s->output.reset();
    }
    auto permit = daw::tryAcquireBackgroundJob();
    if (!permit)
        throw daw::Error("Too many background jobs are active");
    auto preparation = std::make_shared<PlaybackPreparation>();
    preparation->revision = s->model.state().revision;
    preparation->generation = s->playbackGeneration;
    preparation->startFrame = s->selectedFrame;
    preparation->loopStart = s->loopEnabled ? s->loopStart : 0;
    preparation->loopEnd = s->loopEnabled ? s->loopEnd : 0;
    preparation->permit = std::move(permit);
    auto snapshot = s->model.state();
    std::thread worker([preparation, snapshot = std::move(snapshot)]() mutable {
        try {
            if (preparation->cancel.load(std::memory_order_acquire))
                return;
            auto output = daw::makeOutput();
            output->prepare(snapshot, preparation->startFrame, preparation->loopStart,
                            preparation->loopEnd);
            std::lock_guard lock(preparation->publication);
            if (preparation->cancel.load(std::memory_order_acquire) ||
                preparation->status.load(std::memory_order_acquire) !=
                    PlaybackPreparationStatus::preparing)
                return;
            preparation->output = std::move(output);
            preparation->status.store(PlaybackPreparationStatus::ready, std::memory_order_release);
        } catch (const std::exception &error) {
            std::lock_guard lock(preparation->publication);
            if (preparation->cancel.load(std::memory_order_acquire))
                return;
            preparation->error = error.what();
            preparation->status.store(PlaybackPreparationStatus::failed, std::memory_order_release);
        } catch (...) {
            std::lock_guard lock(preparation->publication);
            if (preparation->cancel.load(std::memory_order_acquire))
                return;
            preparation->error = "Unknown playback preparation error";
            preparation->status.store(PlaybackPreparationStatus::failed, std::memory_order_release);
        }
    });
    try {
        worker.detach();
    } catch (...) {
        if (worker.joinable())
            worker.join();
        throw;
    }
    s->transientOutputState = 6;
    s->playbackPreparation = preparation;
}
void pollPlaybackPreparation(daw_session *s) {
    auto preparation = s->playbackPreparation;
    if (!preparation)
        return;
    const auto status = preparation->status.load(std::memory_order_acquire);
    if (status == PlaybackPreparationStatus::preparing)
        return;
    if (status == PlaybackPreparationStatus::canceled) {
        if (s->playbackPreparation == preparation)
            s->playbackPreparation.reset();
        s->transientOutputState = 0;
        return;
    }
    if (status == PlaybackPreparationStatus::failed) {
        std::string error;
        {
            std::lock_guard lock(preparation->publication);
            error = preparation->error;
        }
        if (s->playbackPreparation == preparation)
            s->playbackPreparation.reset();
        s->transientOutputState = 7;
        throw daw::Error(error.empty() ? "Playback preparation failed" : error);
    }
    if (status != PlaybackPreparationStatus::ready)
        return;
    if (preparation->generation != s->playbackGeneration ||
        preparation->revision != s->model.state().revision) {
        cancelPlaybackPreparation(s);
        s->transientOutputState = 0;
        return;
    }
    std::unique_ptr<daw::Output> output;
    {
        std::lock_guard lock(preparation->publication);
        output = std::move(preparation->output);
        preparation->status.store(PlaybackPreparationStatus::consumed, std::memory_order_release);
    }
    if (s->playbackPreparation == preparation)
        s->playbackPreparation.reset();
    if (!output)
        throw daw::Error("Prepared playback graph is unavailable");
    // A prepared graph starts with the session's monitoring intent, so the
    // metronome switch survives stop -> play, a device change and every
    // re-prepare. It is runtime-only: nothing of it reaches State or storage.
    output->renderer.setMetronome(s->metronomeEnabled);
    try {
        output->startPrepared();
    } catch (...) {
        s->transientOutputState = 7;
        throw;
    }
    s->output = std::move(output);
    s->lastCallbacks = 0;
    s->lastProgress = std::chrono::steady_clock::now();
    s->transientOutputState = 0;
}
void resetTransport(daw_session *s) {
    invalidatePlaybackPreparation(s);
    s->output.reset();
    s->vst3ParameterCache.reset();
    const auto frames = duration(s);
    s->selectedFrame = std::min(s->selectedFrame, frames);
    if (s->loopEnabled && s->loopEnd > frames) {
        s->loopEnabled = false;
        s->loopStart = 0;
        s->loopEnd = 0;
    }
}
void rebuildOutputAfterAutomationCommit(daw_session *s) {
    if (!s->output)
        return;
    const bool resume = s->output->renderer.playing.load();
    const auto audible = s->output->renderer.audiblePositionFrames();
    s->output->stop();
    s->output.reset();
    s->selectedFrame = std::min(audible, duration(s));
    if (resume) {
        if (s->loopEnabled && s->selectedFrame >= s->loopEnd)
            s->selectedFrame = s->loopStart;
        beginPlaybackPreparation(s);
    }
}
const char *required(const char *s) {
    if (!s)
        throw daw::Error("Missing text argument");
    return s;
}
daw::WavFormat exportFormat(int32_t format) {
    if (format == 1)
        return daw::WavFormat::PCM24;
    if (format == 2)
        return daw::WavFormat::Float32;
    throw daw::Error("Unsupported WAV export format");
}
daw::ExportOptions exportOptions(const daw_export_options *raw) {
    if (!raw || raw->struct_size != sizeof(daw_export_options) ||
        raw->version != DAW_EXPORT_OPTIONS_VERSION)
        throw daw::Error("Invalid export options");
    daw::ExportOptions result;
    switch (raw->tail_mode) {
    case DAW_EXPORT_TAIL_AUTOMATIC:
        result.tailMode = daw::ExportTailMode::Automatic;
        break;
    case DAW_EXPORT_TAIL_NONE:
        result.tailMode = daw::ExportTailMode::None;
        break;
    case DAW_EXPORT_TAIL_MANUAL_LIMIT:
        result.tailMode = daw::ExportTailMode::ManualLimit;
        break;
    default:
        throw daw::Error("Unsupported export tail mode");
    }
    result.manualTailFrames = raw->manual_tail_frames;
    return result;
}
void writeExportTailSummary(const daw::ExportTailSummary &summary, daw_export_tail_summary *out) {
    if (!out || out->struct_size != sizeof(daw_export_tail_summary))
        throw daw::Error("Invalid export tail summary");
    out->version = DAW_EXPORT_TAIL_SUMMARY_VERSION;
    out->finite_tail_frames = summary.finiteTailFrames;
    out->selected_tail_frames = summary.selectedTailFrames;
    out->infinite_tail_detected = summary.infiniteTailDetected ? 1 : 0;
}
bool recordingActive(const daw_session *s) {
    return s->input || s->duplex;
}
constexpr daw::AutomationTarget automationTarget(int32_t target) {
    switch (target) {
    case DAW_AUTOMATION_TRACK_VOLUME:
        return daw::AutomationTarget::TrackVolume;
    case DAW_AUTOMATION_TRACK_PAN:
        return daw::AutomationTarget::TrackPan;
    case DAW_AUTOMATION_BUS_GAIN:
        return daw::AutomationTarget::BusGain;
    case DAW_AUTOMATION_MASTER_GAIN:
        return daw::AutomationTarget::MasterGain;
    default:
        throw daw::Error("Unsupported automation target");
    }
}
constexpr daw::AutomationWriteMode automationMode(int32_t mode) {
    switch (mode) {
    case DAW_AUTOMATION_TOUCH:
        return daw::AutomationWriteMode::Touch;
    case DAW_AUTOMATION_LATCH:
        return daw::AutomationWriteMode::Latch;
    default:
        throw daw::Error("Unsupported automation write mode");
    }
}
template <size_t N> void copyText(char (&destination)[N], const std::string &source) {
    std::memset(destination, 0, N);
    std::memcpy(destination, source.data(), std::min(source.size(), N - 1));
}
std::vector<daw::WorkflowOperation> workflowOperations(const daw_workflow_operation *operations,
                                                       uint32_t count) {
    if (!operations || count == 0 || count > 100)
        throw daw::Error("Workflow requires 1–100 operations");
    std::vector<daw::WorkflowOperation> result;
    result.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const auto &item = operations[index];
        if (item.struct_size != sizeof(daw_workflow_operation))
            throw daw::Error("Workflow operation ABI mismatch");
        daw::WorkflowOperation operation;
        operation.trackID = item.track_id;
        operation.gainDb = item.gain_db;
        if (item.kind == 1) {
            operation.kind = daw::WorkflowOperationKind::RenameTrack;
            const auto length = strnlen(item.name, sizeof(item.name));
            if (length == sizeof(item.name))
                throw daw::Error("Workflow name is not terminated");
            operation.name.assign(item.name, length);
        } else if (item.kind == 2)
            operation.kind = daw::WorkflowOperationKind::SetTrackGain;
        else
            throw daw::Error("Unsupported workflow operation");
        result.push_back(std::move(operation));
    }
    return result;
}
struct VocalPlan {
    std::vector<uint64_t> ids;
    std::vector<daw::TrackAnalysis> analyses;
    std::vector<daw::WorkflowOperation> operations;
    daw::WorkflowPreview preview;
};
double amplitudeDb(double value) {
    return value > 0 ? std::max(-120.0, 20.0 * std::log10(value)) : -120.0;
}
VocalPlan vocalPlan(daw_session *s, const uint64_t *selected, uint32_t count, const char *base,
                    double targetRms, double peakCeiling, double doubleOffset, uint64_t revision) {
    if (!selected || count == 0 || count > 32)
        throw daw::Error("Vocal workflow requires 1–32 selected tracks");
    if (!std::isfinite(targetRms) || targetRms < -60 || targetRms > -6 ||
        !std::isfinite(peakCeiling) || peakCeiling < -18 || peakCeiling > 0 ||
        !std::isfinite(doubleOffset) || doubleOffset < -24 || doubleOffset > 0)
        throw daw::Error("Vocal workflow targets are outside their ranges");
    VocalPlan plan;
    plan.ids.assign(selected, selected + count);
    std::vector<daw::VocalGainSuggestion> suggestions;
    plan.analyses.reserve(count);
    suggestions.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const auto analysis = daw::analyzeTrack(s->model.state(), selected[index]);
        plan.analyses.push_back(analysis);
        const auto peak = amplitudeDb(std::max(analysis.peakLeft, analysis.peakRight));
        const auto rms = amplitudeDb(std::sqrt((double(analysis.rmsLeft) * analysis.rmsLeft +
                                                double(analysis.rmsRight) * analysis.rmsRight) *
                                               0.5));
        const auto roleOffset = index == 0 ? 0.0 : doubleOffset;
        const auto track = std::find_if(s->model.state().tracks.begin(),
                                        s->model.state().tracks.end(), [&](const auto &item) {
                                            return item.id == selected[index];
                                        });
        double proposed = analysis.analyzedFrames == 0
                              ? track->gain
                              : std::min(targetRms + roleOffset - rms, peakCeiling - peak);
        suggestions.push_back({selected[index], std::clamp(proposed, -120.0, 24.0)});
    }
    plan.operations =
        daw::prepareVocalTracks(s->model.state(), plan.ids, required(base), suggestions);
    plan.preview = s->model.previewWorkflow(plan.operations, revision);
    return plan;
}
void startRecording(daw_session *s, uint64_t startFrame, const char *recoveryPath,
                    uint64_t target) {
    if (recordingActive(s))
        throw daw::Error("Recording is already active");
    if (startFrame >= 48000 * 600)
        throw daw::Error("Recording position must be before the 10 minute timeline limit");
    invalidatePlaybackPreparation(s);
    size_t audioTracks = 0, audioAssets = 0, audioBytes = 0;
    const daw::Track *targetTrack = nullptr;
    for (const auto &track : s->model.state().tracks)
        if (track.audio) {
            ++audioTracks;
            ++audioAssets;
            audioBytes += track.audio->samples().size() * sizeof(float);
            for (const auto &take : track.takes) {
                ++audioAssets;
                audioBytes += take.audio->samples().size() * sizeof(float);
            }
            if (track.id == target)
                targetTrack = &track;
        }
    if (target) {
        if (!targetTrack)
            throw daw::Error("Take target track not found");
        if (targetTrack->takes.size() >= 15)
            throw daw::Error("Track supports at most 16 takes");
    } else {
        if (s->model.state().tracks.size() >= 256)
            throw daw::Error("Draft supports at most 256 tracks");
        if (audioTracks >= 8)
            throw daw::Error("Prototype supports at most 8 audio tracks");
    }
    if (audioAssets >= 32)
        throw daw::Error("Prototype supports at most 32 takes");
    constexpr size_t byteLimit = 64 * 1024 * 1024;
    if (audioBytes >= byteLimit)
        throw daw::Error("No project capacity remains for recording");
    auto memoryFrames = (byteLimit - audioBytes) / (2 * sizeof(float));
    auto timelineFrames = 48000 * 600 - startFrame;
    auto capacity =
        std::min<uint64_t>({48000 * 60, static_cast<uint64_t>(memoryFrames), timelineFrames});
    const bool loopRecording = target && s->loopEnabled;
    if (loopRecording) {
        if (startFrame != s->loopStart)
            throw daw::Error("Loop recording must start at the loop boundary");
        const auto loopFrames = s->loopEnd - s->loopStart;
        const auto takeSlots = std::min<size_t>(15 - targetTrack->takes.size(), 32 - audioAssets);
        capacity = std::min<uint64_t>(
            {48000 * 60, static_cast<uint64_t>(memoryFrames), loopFrames * takeSlots});
    }
    if (!capacity)
        throw daw::Error("No project capacity remains for recording");
    if (s->output) {
        s->output->stop();
        s->output.reset();
    }
    auto path = std::string(required(recoveryPath));
    if (path.empty())
        throw daw::Error("Missing recording recovery path");
    if (loopRecording) {
        auto duplex = daw::makeDuplex(s->model.state(), capacity, path, startFrame, s->loopStart,
                                      s->loopEnd, s->recordPrerollFrames, s->recordMonitor);
        duplex->renderer.setMetronome(s->metronomeEnabled);
        duplex->start();
        s->duplex = std::move(duplex);
    } else {
        auto input = daw::makeInput(capacity, path, startFrame);
        input->start();
        s->input = std::move(input);
    }
    s->recordStart = startFrame;
    s->recordTarget = target;
    s->selectedFrame = startFrame;
    s->recordLastCallbacks = 0;
    s->lastCallbacks = 0;
    s->recordProgress = std::chrono::steady_clock::now();
    s->lastProgress = s->recordProgress;
}
void validateInsertOwner(int32_t owner, uint64_t ownerID) {
    switch (owner) {
    case DAW_INSERT_OWNER_MASTER:
        if (ownerID)
            throw daw::Error("Master insert owner ID must be zero");
        return;
    case DAW_INSERT_OWNER_TRACK:
    case DAW_INSERT_OWNER_BUS:
        if (!ownerID)
            throw daw::Error("Track and bus insert owners require an ID");
        return;
    default:
        throw daw::Error("Unsupported insert owner");
    }
}
daw::PluginOwner pluginOwner(int32_t owner) {
    switch (owner) {
    case DAW_INSERT_OWNER_MASTER:
        return daw::PluginOwner::Master;
    case DAW_INSERT_OWNER_TRACK:
        return daw::PluginOwner::Track;
    case DAW_INSERT_OWNER_BUS:
        return daw::PluginOwner::Bus;
    default:
        throw daw::Error("Unsupported insert owner");
    }
}
const std::vector<daw::PluginInsert> &insertsFor(const daw::State &state, int32_t owner,
                                                 uint64_t ownerID) {
    validateInsertOwner(owner, ownerID);
    switch (owner) {
    case DAW_INSERT_OWNER_MASTER:
        return state.masterInserts;
    case DAW_INSERT_OWNER_TRACK: {
        const auto track =
            std::find_if(state.tracks.begin(), state.tracks.end(), [&](const auto &item) {
                return item.id == ownerID;
            });
        if (track == state.tracks.end())
            throw daw::Error("Track not found");
        return track->inserts;
    }
    case DAW_INSERT_OWNER_BUS: {
        const auto bus =
            std::find_if(state.buses.begin(), state.buses.end(), [&](const auto &item) {
                return item.id == ownerID;
            });
        if (bus == state.buses.end())
            throw daw::Error("Bus not found");
        return bus->inserts;
    }
    default:
        throw daw::Error("Unsupported insert owner");
    }
}
const daw::PluginInsert &insertFor(const daw::State &state, int32_t owner, uint64_t ownerID,
                                   uint64_t insertID) {
    const auto &inserts = insertsFor(state, owner, ownerID);
    const auto insert = std::find_if(inserts.begin(), inserts.end(), [&](const auto &item) {
        return item.id == insertID;
    });
    if (insert == inserts.end())
        throw daw::Error("Insert not found");
    return *insert;
}
uint32_t insertHostingFormat(const daw::PluginInsert &plugin) noexcept {
    if (daw::isVst3PluginInsert(plugin))
        return DAW_INSERT_HOSTING_FORMAT_VST3;
#ifdef __APPLE__
    AudioComponentDescription description{plugin.type, plugin.subtype, plugin.manufacturer, 0, 0};
    if (const auto component = AudioComponentFindNext(nullptr, &description)) {
        AudioComponentDescription actual{};
        if (AudioComponentGetDescription(component, &actual) == noErr &&
            (actual.componentFlags & kAudioComponentFlag_IsV3AudioUnit))
            return DAW_INSERT_HOSTING_FORMAT_AUV3;
    }
#endif
    return DAW_INSERT_HOSTING_FORMAT_AUV2;
}
uint32_t supportedHostingModes(const daw::PluginInsert &plugin) noexcept {
    const auto format = insertHostingFormat(plugin);
    if (format == DAW_INSERT_HOSTING_FORMAT_VST3) {
#if DAW_VST3_RUNTIME_AVAILABLE
        return DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS |
               DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS;
#else
        return DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS;
#endif
    }
    if (format == DAW_INSERT_HOSTING_FORMAT_AUV3)
        return DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS |
               DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS;
    return DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS;
}
daw::PluginHostingMode hostingMode(uint32_t mode) {
    switch (mode) {
    case DAW_INSERT_HOSTING_MODE_IN_PROCESS:
        return daw::PluginHostingMode::InProcess;
    case DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS:
        return daw::PluginHostingMode::OutOfProcess;
    default:
        throw daw::Error("Unsupported plug-in hosting mode");
    }
}
uint32_t bridgeHostingMode(daw::PluginHostingMode mode) {
    switch (mode) {
    case daw::PluginHostingMode::InProcess:
        return DAW_INSERT_HOSTING_MODE_IN_PROCESS;
    case daw::PluginHostingMode::OutOfProcess:
        return DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS;
    }
    throw daw::Error("Unknown plug-in hosting mode");
}
void writeInsertHostingStatus(const daw::PluginInsert &plugin, daw_insert_hosting_status *out) {
    if (!out || out->struct_size != sizeof(daw_insert_hosting_status))
        throw daw::Error("Insert hosting status ABI mismatch");
    *out = {};
    out->struct_size = sizeof(daw_insert_hosting_status);
    out->version = DAW_INSERT_HOSTING_STATUS_VERSION;
    out->format = insertHostingFormat(plugin);
    out->selected_mode = bridgeHostingMode(plugin.hostingMode);
    out->supported_modes = supportedHostingModes(plugin);
}
uint32_t bridgeRuntimeState(daw::PreparedEffectRuntimeState state) noexcept {
    switch (state) {
    case daw::PreparedEffectRuntimeState::Unprepared:
        return DAW_INSERT_RUNTIME_UNPREPARED;
    case daw::PreparedEffectRuntimeState::ActiveInProcess:
        return DAW_INSERT_RUNTIME_ACTIVE_IN_PROCESS;
    case daw::PreparedEffectRuntimeState::ActiveIsolated:
        return DAW_INSERT_RUNTIME_ACTIVE_ISOLATED;
    case daw::PreparedEffectRuntimeState::DryFallback:
        return DAW_INSERT_RUNTIME_DRY_FALLBACK;
    case daw::PreparedEffectRuntimeState::Failed:
        return DAW_INSERT_RUNTIME_FAILED;
    }
    return DAW_INSERT_RUNTIME_FAILED;
}
void writeInsertRuntimeStatus(const daw::Renderer *renderer, uint64_t insertID,
                              daw_insert_runtime_status *out) {
    if (!out || out->struct_size != sizeof(daw_insert_runtime_status))
        throw daw::Error("Insert runtime status ABI mismatch");
    *out = {};
    out->struct_size = sizeof(daw_insert_runtime_status);
    out->version = DAW_INSERT_RUNTIME_STATUS_VERSION;
    if (!renderer) {
        out->state = DAW_INSERT_RUNTIME_UNPREPARED;
        out->fault_code = DAW_INSERT_RUNTIME_FAULT_RESTART_REQUIRED;
        return;
    }
    daw::PreparedEffectRuntimeStatus status{};
    if (!renderer->insertRuntimeStatus(insertID, status)) {
        out->state = DAW_INSERT_RUNTIME_UNPREPARED;
        out->fault_code = DAW_INSERT_RUNTIME_FAULT_RESTART_REQUIRED;
        return;
    }
    out->state = bridgeRuntimeState(status.state);
    out->extra_pipeline_latency_frames = status.extraPipelineLatencyFrames;
    out->fault_code = status.faultCode;
}
void requireParameterEditor(const daw::PluginInsert &plugin) {
    if (plugin.hostingMode != daw::PluginHostingMode::OutOfProcess)
        return;
    if (!daw::isVst3PluginInsert(plugin))
        throw daw::Error(
            "Isolated Audio Unit parameters require a remote parameter editor; switch this insert to in-process to edit its state");
#if !DAW_VST3_RUNTIME_AVAILABLE
    throw daw::Error(
        "Isolated VST3 parameter editing requires the managed VST3 runtime in this build");
#endif
}
const std::vector<daw::Vst3Parameter> &vst3EditorParameters(daw_session *s,
                                                            const daw::PluginInsert &plugin) {
    requireParameterEditor(plugin);
    const auto revision = s->model.state().revision;
    if (s->vst3ParameterCache && s->vst3ParameterCache->insertID == plugin.id &&
        s->vst3ParameterCache->revision == revision)
        return s->vst3ParameterCache->parameters;
    auto parameters = daw::vst3Parameters(plugin);
    s->vst3ParameterCache = Vst3ParameterCache{plugin.id, revision, std::move(parameters)};
    return s->vst3ParameterCache->parameters;
}
daw::Vst3EffectSnapshot setVst3EditorParameter(daw_session *, const daw::PluginInsert &plugin,
                                               uint32_t parameterID, float value) {
    requireParameterEditor(plugin);
    return daw::setVst3Parameter(plugin, parameterID, value);
}
void writeVst3Parameter(const daw::Vst3Parameter &parameter, daw_au_parameter *out) {
    out->id = parameter.id;
    out->minimum = 0;
    out->maximum = 1;
    out->value = parameter.normalizedValue;
    out->normalized_value = parameter.normalizedValue;
    out->writable = 1;
    out->indexed = parameter.stepCount > 0;
    copyText(out->name, parameter.title);
}
void readChannelMeter(const daw::State &state, const daw::Renderer &renderer, int32_t owner,
                      uint64_t ownerID, float &left, float &right) {
    validateInsertOwner(owner, ownerID);
    if (!renderer.playing.load(std::memory_order_acquire))
        return;
    switch (owner) {
    case DAW_INSERT_OWNER_MASTER:
        renderer.masterMeter(left, right);
        return;
    case DAW_INSERT_OWNER_TRACK: {
        const auto track =
            std::find_if(state.tracks.begin(), state.tracks.end(), [&](const auto &item) {
                return item.id == ownerID;
            });
        if (track == state.tracks.end())
            throw daw::Error("Track not found");
        const auto index = static_cast<size_t>(std::distance(state.tracks.begin(), track));
        if (!renderer.trackMeter(index, left, right)) {
            left = 0;
            right = 0;
        }
        return;
    }
    case DAW_INSERT_OWNER_BUS: {
        const auto bus =
            std::find_if(state.buses.begin(), state.buses.end(), [&](const auto &item) {
                return item.id == ownerID;
            });
        if (bus == state.buses.end())
            throw daw::Error("Bus not found");
        const auto index = static_cast<size_t>(std::distance(state.buses.begin(), bus));
        if (!renderer.busMeter(index, left, right)) {
            left = 0;
            right = 0;
        }
        return;
    }
    default:
        throw daw::Error("Unsupported insert owner");
    }
}
bool insertAvailable(const daw_session *s, const daw::PluginInsert &plugin) {
    if (daw::isVst3PluginInsert(plugin)) {
        const auto envelope = daw::decodeVst3StateEnvelope(plugin.state);
        const auto expectedClass =
            envelope ? daw::textualVst3Fuid(envelope->descriptor.vst3ClassFuid) : std::string{};
        return envelope &&
               std::any_of(s->vst3Catalog.begin(), s->vst3Catalog.end(), [&](const auto &entry) {
                   return entry.available &&
                          entry.plugin.modulePath == envelope->descriptor.modulePath &&
                          entry.plugin.moduleFingerprint == envelope->descriptor.fingerprint &&
                          entry.plugin.classId == expectedClass;
               });
    }
    return daw::audioUnitAvailable({plugin.type, plugin.subtype, plugin.manufacturer, plugin.name});
}
void writePlugin(daw_session *s, const daw::PluginInsert &plugin, daw_plugin *out) {
    if (!out || out->struct_size != sizeof(daw_plugin))
        throw daw::Error("Plugin ABI mismatch");
    *out = {};
    out->struct_size = sizeof(daw_plugin);
    out->id = plugin.id;
    out->type = plugin.type;
    out->subtype = plugin.subtype;
    out->manufacturer = plugin.manufacturer;
    out->bypassed = plugin.bypassed;
    out->latency_frames = plugin.latencyFrames;
    out->available = insertAvailable(s, plugin) ? 1 : 0;
    copyText(out->name, plugin.name);
}
}
extern "C" {
daw_session *daw_create() {
    try {
        return new daw_session;
    } catch (...) {
        return nullptr;
    }
}
void daw_destroy(daw_session *s) {
    if (s) {
        s->lifetime.reset();
        cancelPlaybackPreparation(s);
    }
    delete s;
}
int daw_get_snapshot(daw_session *s, daw_snapshot *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_snapshot))
            throw daw::Error("Snapshot ABI mismatch");
        out->revision = s->model.state().revision;
        out->track_count = static_cast<uint32_t>(s->model.state().tracks.size());
        out->can_undo = s->model.canUndo();
        out->can_redo = s->model.canRedo();
        out->master_gain_db = s->model.state().masterGain;
        out->bus_count = static_cast<uint32_t>(s->model.state().buses.size());
        out->master_insert_count = static_cast<uint32_t>(s->model.state().masterInserts.size());
    });
}
int daw_get_track(daw_session *s, uint32_t index, daw_track *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_track))
            throw daw::Error("Track ABI mismatch");
        if (index >= s->model.state().tracks.size())
            throw daw::Error("Track index out of range");
        const auto &t = s->model.state().tracks[index];
        out->id = t.id;
        out->gain_db = t.gain;
        out->audio_frames = t.audio ? t.audio->frames() : 0;
        out->clip_count = static_cast<uint32_t>(t.regions.size());
        out->pan = t.pan;
        out->muted = t.muted;
        out->solo = t.solo;
        out->take_count = t.audio ? static_cast<uint32_t>(t.takes.size() + 1) : 0;
        out->output_bus_id = t.outputBus;
        out->send_count = static_cast<uint32_t>(t.sends.size());
        out->color = t.color;
        std::memset(out->name, 0, sizeof(out->name));
        std::memcpy(out->name, t.name.data(), t.name.size());
    });
}
int daw_add_track(daw_session *s, const char *name, uint64_t rev) {
    return guard(s, [&] {
        s->model.add(required(name), rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_remove_track(daw_session *s, uint64_t id, uint64_t rev) {
    return guard(s, [&] {
        s->model.removeTrack(id, rev);
        resetTransport(s);
    });
}
int daw_move_track(daw_session *s, uint64_t id, uint32_t index, uint64_t rev) {
    return guard(s, [&] {
        s->model.moveTrack(id, index, rev);
        resetTransport(s);
    });
}
int daw_rename_track(daw_session *s, uint64_t id, const char *name, uint64_t rev) {
    return guard(s, [&] {
        s->model.rename(id, required(name), rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_set_gain(daw_session *s, uint64_t id, double gain, uint64_t rev) {
    return guard(s, [&] {
        s->model.gain(id, gain, rev);
        cancelStalePlaybackPreparation(s);
        if (s->output)
            s->output->renderer.updateMix(s->model.state());
    });
}
int daw_set_pan(daw_session *s, uint64_t id, double pan, uint64_t rev) {
    return guard(s, [&] {
        s->model.pan(id, pan, rev);
        cancelStalePlaybackPreparation(s);
        if (s->output)
            s->output->renderer.updateMix(s->model.state());
    });
}
int daw_set_mute(daw_session *s, uint64_t id, int32_t muted, uint64_t rev) {
    return guard(s, [&] {
        if (muted != 0 && muted != 1)
            throw daw::Error("Mute must be 0 or 1");
        s->model.mute(id, muted, rev);
        cancelStalePlaybackPreparation(s);
        if (s->output)
            s->output->renderer.updateMix(s->model.state());
    });
}
int daw_set_solo(daw_session *s, uint64_t id, int32_t solo, uint64_t rev) {
    return guard(s, [&] {
        if (solo != 0 && solo != 1)
            throw daw::Error("Solo must be 0 or 1");
        s->model.solo(id, solo, rev);
        cancelStalePlaybackPreparation(s);
        if (s->output)
            s->output->renderer.updateMix(s->model.state());
    });
}
int daw_set_master_gain(daw_session *s, double gain, uint64_t rev) {
    return guard(s, [&] {
        s->model.masterGain(gain, rev);
        cancelStalePlaybackPreparation(s);
        if (s->output)
            s->output->renderer.updateMix(s->model.state());
    });
}
int daw_add_bus(daw_session *s, const char *name, uint64_t rev) {
    return guard(s, [&] {
        s->model.addBus(required(name), rev);
        resetTransport(s);
    });
}
int daw_get_bus(daw_session *s, uint32_t index, daw_bus *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_bus))
            throw daw::Error("Bus ABI mismatch");
        if (index >= s->model.state().buses.size())
            throw daw::Error("Bus index out of range");
        const auto &bus = s->model.state().buses[index];
        *out = {};
        out->struct_size = sizeof(daw_bus);
        out->id = bus.id;
        out->gain_db = bus.gain;
        out->pan = bus.pan;
        out->muted = bus.muted;
        out->output_bus_id = bus.outputBus;
        std::memcpy(out->name, bus.name.data(), bus.name.size());
    });
}
int daw_rename_bus(daw_session *s, uint64_t id, const char *name, uint64_t rev) {
    return guard(s, [&] {
        s->model.renameBus(id, required(name), rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_set_bus_gain(daw_session *s, uint64_t id, double gain, uint64_t rev) {
    return guard(s, [&] {
        s->model.busGain(id, gain, rev);
        cancelStalePlaybackPreparation(s);
        if (s->output)
            s->output->renderer.updateMix(s->model.state());
    });
}
int daw_set_bus_pan(daw_session *s, uint64_t id, double pan, uint64_t rev) {
    return guard(s, [&] {
        s->model.busPan(id, pan, rev);
        cancelStalePlaybackPreparation(s);
        if (s->output)
            s->output->renderer.updateMix(s->model.state());
    });
}
int daw_set_bus_mute(daw_session *s, uint64_t id, int32_t muted, uint64_t rev) {
    return guard(s, [&] {
        if (muted != 0 && muted != 1)
            throw daw::Error("Mute must be 0 or 1");
        s->model.busMute(id, muted, rev);
        cancelStalePlaybackPreparation(s);
        if (s->output)
            s->output->renderer.updateMix(s->model.state());
    });
}
int daw_set_track_output(daw_session *s, uint64_t track, uint64_t bus, uint64_t rev) {
    return guard(s, [&] {
        s->model.routeTrack(track, bus, rev);
        resetTransport(s);
    });
}
int daw_set_bus_output(daw_session *s, uint64_t id, uint64_t output, uint64_t rev) {
    return guard(s, [&] {
        s->model.routeBus(id, output, rev);
        resetTransport(s);
    });
}
int daw_create_bus(daw_session *s, const char *name, uint64_t *out_bus_id, uint64_t rev) {
    return guard(s, [&] {
        const std::string text = required(name);
        if (text.empty())
            throw daw::Error("Bus name required");
        s->model.addBus(text, rev);
        if (out_bus_id)
            *out_bus_id = s->model.state().buses.back().id;
        cancelStalePlaybackPreparation(s);
    });
}
int daw_get_bus_count(daw_session *s, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Bus count output required");
        *count = (uint32_t)s->model.state().buses.size();
    });
}
int daw_delete_bus(daw_session *s, uint64_t id, uint64_t rev) {
    return guard(s, [&] {
        s->model.deleteBus(id, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_get_send(daw_session *s, uint64_t trackID, uint32_t index, daw_send *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_send))
            throw daw::Error("Send ABI mismatch");
        const auto track = std::find_if(s->model.state().tracks.begin(),
                                        s->model.state().tracks.end(), [&](const auto &item) {
                                            return item.id == trackID;
                                        });
        if (track == s->model.state().tracks.end() || index >= track->sends.size())
            throw daw::Error("Send index out of range");
        const auto &send = track->sends[index];
        *out = {sizeof(daw_send), send.bus, send.gain, send.preFader ? 1 : 0};
    });
}
int daw_upsert_send(daw_session *s, uint64_t track, uint64_t bus, double gain, int32_t pre,
                    uint64_t rev) {
    return guard(s, [&] {
        if (pre != 0 && pre != 1)
            throw daw::Error("Send tap must be 0 or 1");
        s->model.upsertSend(track, bus, gain, pre != 0, rev);
        resetTransport(s);
    });
}
int daw_remove_send(daw_session *s, uint64_t track, uint64_t bus, uint64_t rev) {
    return guard(s, [&] {
        s->model.removeSend(track, bus, rev);
        resetTransport(s);
    });
}
int daw_get_track_volume_automation_count(daw_session *s, uint64_t trackID, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing automation point count");
        const auto track = std::find_if(s->model.state().tracks.begin(),
                                        s->model.state().tracks.end(), [&](const auto &item) {
                                            return item.id == trackID;
                                        });
        if (track == s->model.state().tracks.end())
            throw daw::Error("Track not found");
        *count = static_cast<uint32_t>(track->volumeAutomation.size());
    });
}
int daw_get_track_volume_automation_point(daw_session *s, uint64_t trackID, uint32_t index,
                                          daw_automation_point *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_automation_point))
            throw daw::Error("Automation point ABI mismatch");
        const auto track = std::find_if(s->model.state().tracks.begin(),
                                        s->model.state().tracks.end(), [&](const auto &item) {
                                            return item.id == trackID;
                                        });
        if (track == s->model.state().tracks.end() || index >= track->volumeAutomation.size())
            throw daw::Error("Automation point index out of range");
        const auto &point = track->volumeAutomation[index];
        *out = {sizeof(daw_automation_point), point.frame, point.gainDb};
    });
}
int daw_upsert_track_volume_automation_point(daw_session *s, uint64_t trackID, uint64_t frame,
                                             double gain, uint64_t rev) {
    return guard(s, [&] {
        s->model.upsertTrackVolumeAutomation(trackID, frame, gain, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_remove_track_volume_automation_point(daw_session *s, uint64_t trackID, uint64_t frame,
                                             uint64_t rev) {
    return guard(s, [&] {
        s->model.removeTrackVolumeAutomation(trackID, frame, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_get_track_pan_automation_count(daw_session *s, uint64_t trackID, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing automation point count");
        const auto track = std::find_if(s->model.state().tracks.begin(),
                                        s->model.state().tracks.end(), [&](const auto &item) {
                                            return item.id == trackID;
                                        });
        if (track == s->model.state().tracks.end())
            throw daw::Error("Track not found");
        *count = static_cast<uint32_t>(track->panAutomation.size());
    });
}
int daw_get_track_pan_automation_point(daw_session *s, uint64_t trackID, uint32_t index,
                                       daw_automation_point *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_automation_point))
            throw daw::Error("Automation point ABI mismatch");
        const auto track = std::find_if(s->model.state().tracks.begin(),
                                        s->model.state().tracks.end(), [&](const auto &item) {
                                            return item.id == trackID;
                                        });
        if (track == s->model.state().tracks.end() || index >= track->panAutomation.size())
            throw daw::Error("Automation point index out of range");
        const auto &point = track->panAutomation[index];
        *out = {sizeof(daw_automation_point), point.frame, point.gainDb};
    });
}
int daw_upsert_track_pan_automation_point(daw_session *s, uint64_t trackID, uint64_t frame,
                                          double pan, uint64_t rev) {
    return guard(s, [&] {
        s->model.upsertTrackPanAutomation(trackID, frame, pan, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_remove_track_pan_automation_point(daw_session *s, uint64_t trackID, uint64_t frame,
                                          uint64_t rev) {
    return guard(s, [&] {
        s->model.removeTrackPanAutomation(trackID, frame, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_get_bus_gain_automation_count(daw_session *s, uint64_t busID, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing automation point count");
        const auto bus = std::find_if(s->model.state().buses.begin(), s->model.state().buses.end(),
                                      [&](const auto &item) {
                                          return item.id == busID;
                                      });
        if (bus == s->model.state().buses.end())
            throw daw::Error("Bus not found");
        *count = static_cast<uint32_t>(bus->gainAutomation.size());
    });
}
int daw_get_bus_gain_automation_point(daw_session *s, uint64_t busID, uint32_t index,
                                      daw_automation_point *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_automation_point))
            throw daw::Error("Automation point ABI mismatch");
        const auto bus = std::find_if(s->model.state().buses.begin(), s->model.state().buses.end(),
                                      [&](const auto &item) {
                                          return item.id == busID;
                                      });
        if (bus == s->model.state().buses.end() || index >= bus->gainAutomation.size())
            throw daw::Error("Automation point index out of range");
        const auto &point = bus->gainAutomation[index];
        *out = {sizeof(daw_automation_point), point.frame, point.gainDb};
    });
}
int daw_upsert_bus_gain_automation_point(daw_session *s, uint64_t busID, uint64_t frame,
                                         double gain, uint64_t rev) {
    return guard(s, [&] {
        s->model.upsertBusGainAutomation(busID, frame, gain, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_remove_bus_gain_automation_point(daw_session *s, uint64_t busID, uint64_t frame,
                                         uint64_t rev) {
    return guard(s, [&] {
        s->model.removeBusGainAutomation(busID, frame, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_get_master_gain_automation_count(daw_session *s, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing automation point count");
        *count = static_cast<uint32_t>(s->model.state().masterGainAutomation.size());
    });
}
int daw_get_master_gain_automation_point(daw_session *s, uint32_t index,
                                         daw_automation_point *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_automation_point))
            throw daw::Error("Automation point ABI mismatch");
        if (index >= s->model.state().masterGainAutomation.size())
            throw daw::Error("Automation point index out of range");
        const auto &point = s->model.state().masterGainAutomation[index];
        *out = {sizeof(daw_automation_point), point.frame, point.gainDb};
    });
}
int daw_upsert_master_gain_automation_point(daw_session *s, uint64_t frame, double gain,
                                            uint64_t rev) {
    return guard(s, [&] {
        s->model.upsertMasterGainAutomation(frame, gain, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_remove_master_gain_automation_point(daw_session *s, uint64_t frame, uint64_t rev) {
    return guard(s, [&] {
        s->model.removeMasterGainAutomation(frame, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_begin_automation_gesture(daw_session *s, int32_t target, uint64_t targetID, int32_t mode,
                                 uint64_t rev) {
    return guard(s, [&] {
        s->model.beginAutomationGesture(automationTarget(target), targetID, automationMode(mode),
                                        rev);
    });
}
int daw_write_automation_gesture(daw_session *s, uint64_t frame, double value) {
    return guard(s, [&] {
        s->model.writeAutomationGesture(frame, value);
    });
}
int daw_end_automation_gesture(daw_session *s, uint64_t endFrame, uint64_t rev) {
    return guard(s, [&] {
        s->model.endAutomationGesture(endFrame, rev);
        rebuildOutputAfterAutomationCommit(s);
    });
}
void daw_cancel_automation_gesture(daw_session *s) {
    if (s)
        s->model.cancelAutomationGesture();
}
int daw_begin_insert_parameter_automation_gesture(daw_session *s, int32_t owner, uint64_t ownerID,
                                                  uint64_t insertID, uint32_t parameterID,
                                                  const char *name, int32_t mode, uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        if (s->pluginParameterGesture)
            throw daw::Error("Plug-in parameter automation gesture is already active");
        (void)insertFor(s->model.state(), owner, ownerID, insertID);
        const auto parameterName = name ? std::string(name) : std::string{};
        s->model.beginPluginParameterAutomationGesture(pluginOwner(owner), ownerID, insertID,
                                                       parameterID, parameterName,
                                                       automationMode(mode), rev);
        s->pluginParameterGesture = {insertID, parameterID};
    });
}
int daw_write_insert_parameter_automation_gesture(daw_session *s, uint64_t frame,
                                                  double normalized) {
    return guard(s, [&] {
        if (!s->pluginParameterGesture)
            throw daw::Error("No plug-in parameter automation gesture is active");
        if (!std::isfinite(normalized) || normalized < 0 || normalized > 1)
            throw daw::Error("Plug-in automation value must be normalized 0…1");
        s->model.writePluginParameterAutomationGesture(frame, normalized);
        if (s->output) {
            const auto live = *s->pluginParameterGesture;
            if (!s->output->renderer.updatePluginParameterTouch(live.pluginID, live.parameterID,
                                                                static_cast<float>(normalized)))
                s->output->renderer.beginPluginParameterTouch(live.pluginID, live.parameterID,
                                                              static_cast<float>(normalized));
        }
    });
}
int daw_end_insert_parameter_automation_gesture(daw_session *s, uint64_t endFrame, uint64_t rev) {
    return guard(s, [&] {
        if (!s->pluginParameterGesture)
            throw daw::Error("No plug-in parameter automation gesture is active");
        const auto live = *s->pluginParameterGesture;
        s->model.endPluginParameterAutomationGesture(endFrame, rev);
        if (s->output)
            s->output->renderer.endPluginParameterTouch(live.pluginID, live.parameterID);
        s->pluginParameterGesture.reset();
        rebuildOutputAfterAutomationCommit(s);
    });
}
void daw_cancel_insert_parameter_automation_gesture(daw_session *s) {
    if (!s)
        return;
    s->model.cancelPluginParameterAutomationGesture();
    if (s->output)
        s->output->renderer.cancelPluginParameterTouch();
    s->pluginParameterGesture.reset();
}
int daw_scan_supported_au(daw_session *s, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing Audio Unit count");
        s->auCatalog = daw::supportedAudioUnits();
        *count = static_cast<uint32_t>(s->auCatalog.size());
    });
}
daw_au_scan_job *daw_begin_installed_au_scan(const char *helperPath, uint32_t timeoutMs) {
#ifdef __APPLE__
    try {
        if (!helperPath || !*helperPath || timeoutMs < 100 || timeoutMs > 30000)
            return nullptr;
        auto job = std::make_unique<daw_au_scan_job>();
        const std::string path(helperPath);
        job->future = std::async(std::launch::async, [path, timeoutMs] {
            return daw::scanAudioUnitsIsolated(path, std::chrono::milliseconds(timeoutMs));
        });
        return job.release();
    } catch (...) {
        return nullptr;
    }
#else
    (void)helperPath;
    (void)timeoutMs;
    return nullptr;
#endif
}
int daw_poll_installed_au_scan(daw_au_scan_job *job, daw_au_scan_status *out) {
#ifdef __APPLE__
    if (!job || !out || out->struct_size != sizeof(daw_au_scan_status))
        return 1;
    *out = {};
    out->struct_size = sizeof(daw_au_scan_status);
    if (!job->result) {
        if (job->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            out->status = 0;
            return 0;
        }
        try {
            job->result = job->future.get();
        } catch (const std::exception &error) {
            out->status = 2;
            std::snprintf(out->error, sizeof(out->error), "%s", error.what());
            return 0;
        } catch (...) {
            out->status = 2;
            std::snprintf(out->error, sizeof(out->error), "Unknown scanner error");
            return 0;
        }
    }
    out->available_count = static_cast<uint32_t>(job->result->available.size());
    out->quarantined_count = static_cast<uint32_t>(job->result->quarantined.size());
    if (!job->result->helperError.empty()) {
        out->status = 2;
        std::snprintf(out->error, sizeof(out->error), "%s", job->result->helperError.c_str());
    } else
        out->status = 1;
    return 0;
#else
    (void)job;
    (void)out;
    return 1;
#endif
}
int daw_apply_installed_au_scan(daw_session *s, daw_au_scan_job *job, uint32_t *available,
                                uint32_t *quarantined) {
    return guard(s, [&] {
#ifdef __APPLE__
        if (!job || !available || !quarantined || !job->result || !job->result->helperError.empty())
            throw daw::Error("Audio Unit scan is not ready");
        s->auCatalog = job->result->available;
        *available = static_cast<uint32_t>(job->result->available.size());
        *quarantined = static_cast<uint32_t>(job->result->quarantined.size());
#else
    (void)job;(void)available;(void)quarantined;throw daw::Error("Audio Unit scanning requires macOS");
#endif
    });
}
int daw_save_installed_au_scan_cache(daw_session *s, daw_au_scan_job *job, const char *path) {
    return guard(s, [&] {
#ifdef __APPLE__
        if (!job || !job->result || !job->result->helperError.empty())
            throw daw::Error("Audio Unit scan is not ready");
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto cache = daw::makeAudioUnitScanCache(*job->result, static_cast<uint64_t>(now));
        std::string error;
        if (!daw::writeAudioUnitScanCache(cache, required(path), error))
            throw daw::Error(error);
#else
    (void)job;(void)path;throw daw::Error("Audio Unit scanning requires macOS");
#endif
    });
}
int daw_load_installed_au_scan_cache(daw_session *s, const char *helperPath, const char *path,
                                     uint32_t *available, uint32_t *quarantined,
                                     uint32_t *invalidated) {
    return guard(s, [&] {
#ifdef __APPLE__
        if (!available || !quarantined || !invalidated)
            throw daw::Error("Missing Audio Unit cache counts");
        std::string error;
        const auto cache = daw::readAudioUnitScanCache(required(path), error);
        if (!error.empty())
            throw daw::Error(error);
        s->auCatalog.clear();
        *available = 0;
        *quarantined = 0;
        *invalidated = 0;
        if (!cache)
            return;
        const auto current =
            daw::enumerateAudioUnitsIsolated(required(helperPath), std::chrono::milliseconds(1500));
        const auto freshness = daw::freshAudioUnitScanCache(*cache, current);
        *invalidated = freshness.invalidatedEntries;
        for (const auto &entry : freshness.fresh.entries) {
            if (entry.available) {
                s->auCatalog.push_back(entry.descriptor);
                ++*available;
            } else
                ++*quarantined;
        }
        if (freshness.invalidatedEntries) {
            if (!daw::writeAudioUnitScanCache(freshness.fresh, path, error))
                throw daw::Error(error);
        }
#else
    (void)helperPath;(void)path;(void)available;(void)quarantined;(void)invalidated;throw daw::Error("Audio Unit scanning requires macOS");
#endif
    });
}
void daw_release_installed_au_scan(daw_au_scan_job *job) {
    delete job;
}
daw_vst3_scan_job *daw_begin_installed_vst3_scan(const char *helperPath, uint32_t timeoutMs) {
#ifdef __APPLE__
    try {
        if (!helperPath || !*helperPath || timeoutMs < 100 || timeoutMs > 30000)
            return nullptr;
        auto job = std::make_unique<daw_vst3_scan_job>();
        const std::string path(helperPath);
        job->future = std::async(std::launch::async, [path, timeoutMs] {
            return daw::scanVst3PluginsIsolated(path, std::chrono::milliseconds(timeoutMs));
        });
        return job.release();
    } catch (...) {
        return nullptr;
    }
#else
    (void)helperPath;
    (void)timeoutMs;
    return nullptr;
#endif
}
int daw_poll_installed_vst3_scan(daw_vst3_scan_job *job, daw_vst3_scan_status *out) {
#ifdef __APPLE__
    if (!job || !out || out->struct_size != sizeof(daw_vst3_scan_status))
        return 1;
    *out = {};
    out->struct_size = sizeof(daw_vst3_scan_status);
    if (!job->result) {
        if (job->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            out->status = 0;
            return 0;
        }
        try {
            job->result = job->future.get();
        } catch (const std::exception &error) {
            out->status = 2;
            copyText(out->error, std::string(error.what()));
            return 0;
        } catch (...) {
            out->status = 2;
            copyText(out->error, std::string("Unknown VST3 scanner error"));
            return 0;
        }
    }
    out->available_count = static_cast<uint32_t>(job->result->available.size());
    out->quarantined_count = static_cast<uint32_t>(job->result->quarantined.size());
    if (!job->result->helperError.empty()) {
        out->status = 2;
        copyText(out->error, job->result->helperError);
    } else
        out->status = 1;
    return 0;
#else
    (void)job;
    (void)out;
    return 1;
#endif
}
int daw_apply_installed_vst3_scan(daw_session *s, daw_vst3_scan_job *job, uint32_t *available,
                                  uint32_t *quarantined) {
    return guard(s, [&] {
#ifdef __APPLE__
        if (!job || !available || !quarantined || !job->result || !job->result->helperError.empty())
            throw daw::Error("VST3 scan is not ready");
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto cache = daw::makeVst3ScanCache(*job->result, static_cast<uint64_t>(now));
        s->vst3Catalog = cache.entries;
        *available = static_cast<uint32_t>(job->result->available.size());
        *quarantined = static_cast<uint32_t>(job->result->quarantined.size());
#else
    (void)job;(void)available;(void)quarantined;throw daw::Error("VST3 scanning requires macOS");
#endif
    });
}
int daw_save_installed_vst3_scan_cache(daw_vst3_scan_job *job, const char *path) {
#ifdef __APPLE__
    try {
        if (!job || !job->result || !job->result->helperError.empty() || !path)
            return 1;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        const auto cache = daw::makeVst3ScanCache(*job->result, static_cast<uint64_t>(now));
        std::string error;
        return daw::writeVst3ScanCache(cache, path, error) ? 0 : 1;
    } catch (...) {
        return 1;
    }
#else
    (void)job;
    (void)path;
    return 1;
#endif
}
int daw_load_installed_vst3_scan_cache(daw_session *s, const char *helperPath, const char *path,
                                       uint32_t *available, uint32_t *quarantined,
                                       uint32_t *invalidated) {
    return guard(s, [&] {
#ifdef __APPLE__
        if (!available || !quarantined || !invalidated)
            throw daw::Error("Missing VST3 cache counts");
        std::string error;
        const auto cache = daw::readVst3ScanCache(required(path), error);
        if (!error.empty())
            throw daw::Error(error);
        s->vst3Catalog.clear();
        *available = 0;
        *quarantined = 0;
        *invalidated = 0;
        if (!cache)
            return;
        const auto current = daw::enumerateVst3PluginsIsolated(required(helperPath),
                                                               std::chrono::milliseconds(1500));
        const auto freshness = daw::freshVst3ScanCache(*cache, current);
        if (!current.helperError.empty())
            throw daw::Error(current.helperError);
        *invalidated = freshness.invalidatedEntries;
        s->vst3Catalog = freshness.fresh.entries;
        for (const auto &entry : s->vst3Catalog) {
            if (entry.available)
                ++*available;
            else
                ++*quarantined;
        }
        if (freshness.invalidatedEntries && !daw::writeVst3ScanCache(freshness.fresh, path, error))
            throw daw::Error(error);
#else
    (void)helperPath;(void)path;(void)available;(void)quarantined;(void)invalidated;throw daw::Error("VST3 scanning requires macOS");
#endif
    });
}
void daw_release_installed_vst3_scan(daw_vst3_scan_job *job) {
    delete job;
}
int daw_get_installed_vst3_count(daw_session *s, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing VST3 catalog count");
        *count = static_cast<uint32_t>(s->vst3Catalog.size());
    });
}
int daw_get_installed_vst3(daw_session *s, uint32_t index, daw_vst3_component *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_vst3_component))
            throw daw::Error("VST3 catalog ABI mismatch");
        if (index >= s->vst3Catalog.size())
            throw daw::Error("VST3 catalog index out of range");
        const auto &entry = s->vst3Catalog[index];
        *out = {};
        out->struct_size = sizeof(daw_vst3_component);
        out->available = entry.available ? 1 : 0;
        out->flags = entry.plugin.instrument ? static_cast<uint32_t>(DAW_VST3_FLAG_INSTRUMENT) : 0u;
        copyText(out->class_id, entry.plugin.classId);
        copyText(out->module_fingerprint, entry.plugin.moduleFingerprint);
        copyText(out->module_path, entry.plugin.modulePath);
        copyText(out->name, entry.plugin.name);
        copyText(out->vendor, entry.plugin.vendor);
        copyText(out->version, entry.plugin.version);
        copyText(out->quarantine_reason, entry.quarantineReason);
    });
}
int daw_add_master_vst3(daw_session *s, uint32_t index, uint64_t rev) {
    return guard(s, [&] {
        if (index >= s->vst3Catalog.size())
            throw daw::Error("VST3 catalog index out of range");
        const auto &item = s->vst3Catalog[index];
        if (!item.available)
            throw daw::Error("Selected VST3 is quarantined or unavailable");
        const auto classID = daw::parseTextualVst3Fuid(item.plugin.classId);
        if (!classID)
            throw daw::Error("VST3 catalog FUID is invalid");
        daw::Vst3StateEnvelope envelope;
        envelope.descriptor.format = daw::PluginFormat::VST3;
        envelope.descriptor.vst3ClassFuid = *classID;
        envelope.descriptor.modulePath = item.plugin.modulePath;
        envelope.descriptor.vendor = item.plugin.vendor;
        envelope.descriptor.version = item.plugin.version;
        envelope.descriptor.fingerprint = item.plugin.moduleFingerprint;
        daw::PluginInsert insert{
            0, 0, 0, 0, item.plugin.name, false, 0, daw::encodeVst3StateEnvelope(envelope), {}};
        const auto snapshot = daw::snapshotVst3Effect(insert);
        insert.latencyFrames = snapshot.latencyFrames;
        insert.state = snapshot.state;
        s->model.addMasterInsert(std::move(insert), rev);
        resetTransport(s);
    });
}
int daw_get_supported_au(daw_session *s, uint32_t index, daw_au_component *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_au_component))
            throw daw::Error("Audio Unit catalog ABI mismatch");
        if (index >= s->auCatalog.size())
            throw daw::Error("Audio Unit catalog index out of range");
        const auto &item = s->auCatalog[index];
        *out = {};
        out->struct_size = sizeof(daw_au_component);
        out->type = item.type;
        out->subtype = item.subtype;
        out->manufacturer = item.manufacturer;
        std::memcpy(out->name, item.name.data(), item.name.size());
    });
}
int daw_add_master_au(daw_session *s, uint32_t type, uint32_t subtype, uint32_t manufacturer,
                      uint64_t rev) {
    return guard(s, [&] {
        const auto found =
            std::find_if(s->auCatalog.begin(), s->auCatalog.end(), [&](const auto &item) {
                return item.type == type && item.subtype == subtype &&
                       item.manufacturer == manufacturer;
            });
        if (found == s->auCatalog.end())
            throw daw::Error("Scan and choose a supported Apple Audio Unit");
        auto snapshot = daw::snapshotAudioUnit(*found);
        s->model.addMasterInsert({0,
                                  type,
                                  subtype,
                                  manufacturer,
                                  std::move(snapshot.name),
                                  false,
                                  snapshot.latencyFrames,
                                  std::move(snapshot.state),
                                  {}},
                                 rev);
        resetTransport(s);
    });
}
int daw_get_master_insert(daw_session *s, uint32_t index, daw_plugin *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_plugin))
            throw daw::Error("Plugin ABI mismatch");
        if (index >= s->model.state().masterInserts.size())
            throw daw::Error("Master insert index out of range");
        const auto &plugin = s->model.state().masterInserts[index];
        *out = {};
        out->struct_size = sizeof(daw_plugin);
        out->id = plugin.id;
        out->type = plugin.type;
        out->subtype = plugin.subtype;
        out->manufacturer = plugin.manufacturer;
        out->bypassed = plugin.bypassed;
        out->latency_frames = plugin.latencyFrames;
        if (daw::isVst3PluginInsert(plugin)) {
            const auto envelope = daw::decodeVst3StateEnvelope(plugin.state);
            const auto expectedClass =
                envelope ? daw::textualVst3Fuid(envelope->descriptor.vst3ClassFuid) : std::string{};
            out->available =
                envelope && std::any_of(s->vst3Catalog.begin(), s->vst3Catalog.end(),
                                        [&](const auto &entry) {
                                            return entry.available &&
                                                   entry.plugin.modulePath ==
                                                       envelope->descriptor.modulePath &&
                                                   entry.plugin.moduleFingerprint ==
                                                       envelope->descriptor.fingerprint &&
                                                   entry.plugin.classId == expectedClass;
                                        })
                    ? 1
                    : 0;
        } else
            out->available = daw::audioUnitAvailable(
                                 {plugin.type, plugin.subtype, plugin.manufacturer, plugin.name})
                                 ? 1
                                 : 0;
        copyText(out->name, plugin.name);
    });
}
int daw_get_master_insert_parameter_count(daw_session *s, uint64_t id, uint32_t *count) {
    return daw_get_insert_parameter_count(s, DAW_INSERT_OWNER_MASTER, 0, id, count);
}
int daw_get_master_insert_parameter(daw_session *s, uint64_t id, uint32_t index,
                                    daw_au_parameter *out) {
    return daw_get_insert_parameter(s, DAW_INSERT_OWNER_MASTER, 0, id, index, out);
}
int daw_set_master_insert_parameter(daw_session *s, uint64_t id, uint32_t parameterID, float value,
                                    uint64_t rev) {
    return daw_set_insert_parameter(s, DAW_INSERT_OWNER_MASTER, 0, id, parameterID, value, rev);
}
int daw_set_master_insert_bypass(daw_session *s, uint64_t id, int32_t bypassed, uint64_t rev) {
    return guard(s, [&] {
        if (bypassed != 0 && bypassed != 1)
            throw daw::Error("Bypass must be 0 or 1");
        s->model.bypassMasterInsert(id, bypassed != 0, rev);
        resetTransport(s);
    });
}
int daw_move_master_insert(daw_session *s, uint64_t id, uint32_t index, uint64_t rev) {
    return guard(s, [&] {
        s->model.moveMasterInsert(id, index, rev);
        resetTransport(s);
    });
}
int daw_remove_master_insert(daw_session *s, uint64_t id, uint64_t rev) {
    return guard(s, [&] {
        s->model.removeMasterInsert(id, rev);
        resetTransport(s);
    });
}
int daw_add_insert_vst3(daw_session *s, int32_t owner, uint64_t ownerID, uint32_t index,
                        uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        if (index >= s->vst3Catalog.size())
            throw daw::Error("VST3 catalog index out of range");
        const auto &item = s->vst3Catalog[index];
        if (!item.available)
            throw daw::Error("Selected VST3 is quarantined or unavailable");
        const auto classID = daw::parseTextualVst3Fuid(item.plugin.classId);
        if (!classID)
            throw daw::Error("VST3 catalog FUID is invalid");
        daw::Vst3StateEnvelope envelope;
        envelope.descriptor.format = daw::PluginFormat::VST3;
        envelope.descriptor.vst3ClassFuid = *classID;
        envelope.descriptor.modulePath = item.plugin.modulePath;
        envelope.descriptor.vendor = item.plugin.vendor;
        envelope.descriptor.version = item.plugin.version;
        envelope.descriptor.fingerprint = item.plugin.moduleFingerprint;
        daw::PluginInsert insert{
            0, 0, 0, 0, item.plugin.name, false, 0, daw::encodeVst3StateEnvelope(envelope), {}};
        const auto snapshot = daw::snapshotVst3Effect(insert);
        insert.latencyFrames = snapshot.latencyFrames;
        insert.state = snapshot.state;
        switch (owner) {
        case DAW_INSERT_OWNER_MASTER:
            if (ownerID)
                throw daw::Error("Master insert owner ID must be zero");
            s->model.addMasterInsert(std::move(insert), rev);
            break;
        case DAW_INSERT_OWNER_TRACK:
            s->model.addTrackInsert(ownerID, std::move(insert), rev);
            break;
        case DAW_INSERT_OWNER_BUS:
            s->model.addBusInsert(ownerID, std::move(insert), rev);
            break;
        default:
            throw daw::Error("Unsupported insert owner");
        }
        resetTransport(s);
    });
}
int daw_add_insert_au(daw_session *s, int32_t owner, uint64_t ownerID, uint32_t type,
                      uint32_t subtype, uint32_t manufacturer, uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        const auto found =
            std::find_if(s->auCatalog.begin(), s->auCatalog.end(), [&](const auto &item) {
                return item.type == type && item.subtype == subtype &&
                       item.manufacturer == manufacturer;
            });
        if (found == s->auCatalog.end())
            throw daw::Error("Scan and choose a supported Apple Audio Unit");
        auto snapshot = daw::snapshotAudioUnit(*found);
        daw::PluginInsert insert{0,
                                 type,
                                 subtype,
                                 manufacturer,
                                 std::move(snapshot.name),
                                 false,
                                 snapshot.latencyFrames,
                                 std::move(snapshot.state),
                                 {}};
        switch (owner) {
        case DAW_INSERT_OWNER_MASTER:
            if (ownerID)
                throw daw::Error("Master insert owner ID must be zero");
            s->model.addMasterInsert(std::move(insert), rev);
            break;
        case DAW_INSERT_OWNER_TRACK:
            s->model.addTrackInsert(ownerID, std::move(insert), rev);
            break;
        case DAW_INSERT_OWNER_BUS:
            s->model.addBusInsert(ownerID, std::move(insert), rev);
            break;
        default:
            throw daw::Error("Unsupported insert owner");
        }
        resetTransport(s);
    });
}
int daw_get_insert_count(daw_session *s, int32_t owner, uint64_t ownerID, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing insert count");
        *count = static_cast<uint32_t>(insertsFor(s->model.state(), owner, ownerID).size());
    });
}
int daw_get_insert(daw_session *s, int32_t owner, uint64_t ownerID, uint32_t index,
                   daw_plugin *out) {
    return guard(s, [&] {
        const auto &inserts = insertsFor(s->model.state(), owner, ownerID);
        if (index >= inserts.size())
            throw daw::Error("Insert index out of range");
        writePlugin(s, inserts[index], out);
    });
}
int daw_get_insert_hosting_status(daw_session *s, int32_t owner, uint64_t ownerID,
                                  uint64_t insertID, daw_insert_hosting_status *out) {
    return guard(s, [&] {
        writeInsertHostingStatus(insertFor(s->model.state(), owner, ownerID, insertID), out);
    });
}
int daw_get_insert_runtime_status(daw_session *s, int32_t owner, uint64_t ownerID,
                                  uint64_t insertID, daw_insert_runtime_status *out) {
    return guard(s, [&] {
        (void)insertFor(s->model.state(), owner, ownerID, insertID);
        writeInsertRuntimeStatus(s->output ? &s->output->renderer : nullptr, insertID, out);
    });
}
int daw_set_insert_hosting_mode(daw_session *s, int32_t owner, uint64_t ownerID, uint64_t insertID,
                                uint32_t mode, uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        const auto &insert = insertFor(s->model.state(), owner, ownerID, insertID);
        const auto requested = hostingMode(mode);
        const auto supported = supportedHostingModes(insert);
        const auto flag = requested == daw::PluginHostingMode::InProcess
                              ? DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS
                              : DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS;
        if (!(supported & flag))
            throw daw::Error("Requested plug-in hosting mode is unavailable for this insert");
        switch (owner) {
        case DAW_INSERT_OWNER_MASTER:
            s->model.setMasterInsertHostingMode(insertID, requested, rev);
            break;
        case DAW_INSERT_OWNER_TRACK:
            s->model.setTrackInsertHostingMode(ownerID, insertID, requested, rev);
            break;
        case DAW_INSERT_OWNER_BUS:
            s->model.setBusInsertHostingMode(ownerID, insertID, requested, rev);
            break;
        default:
            throw daw::Error("Unsupported insert owner");
        }
        resetTransport(s);
    });
}
int daw_get_insert_parameter_count(daw_session *s, int32_t owner, uint64_t ownerID,
                                   uint64_t insertID, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing parameter count");
        const auto &insert = insertFor(s->model.state(), owner, ownerID, insertID);
        if (daw::isVst3PluginInsert(insert))
            *count = static_cast<uint32_t>(vst3EditorParameters(s, insert).size());
        else {
            requireParameterEditor(insert);
            *count = static_cast<uint32_t>(daw::audioUnitParameters(insert).size());
        }
    });
}
int daw_get_insert_parameter(daw_session *s, int32_t owner, uint64_t ownerID, uint64_t insertID,
                             uint32_t index, daw_au_parameter *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_au_parameter))
            throw daw::Error("Plug-in parameter ABI mismatch");
        const auto &insert = insertFor(s->model.state(), owner, ownerID, insertID);
        *out = {};
        out->struct_size = sizeof(daw_au_parameter);
        if (daw::isVst3PluginInsert(insert)) {
            const auto &parameters = vst3EditorParameters(s, insert);
            if (index >= parameters.size())
                throw daw::Error("VST3 parameter index out of range");
            writeVst3Parameter(parameters[index], out);
        } else {
            requireParameterEditor(insert);
            const auto parameters = daw::audioUnitParameters(insert);
            if (index >= parameters.size())
                throw daw::Error("Audio Unit parameter index out of range");
            const auto &parameter = parameters[index];
            out->id = parameter.id;
            out->minimum = parameter.minimum;
            out->maximum = parameter.maximum;
            out->value = parameter.value;
            out->normalized_value = parameter.normalizedValue;
            out->writable = parameter.writable;
            out->logarithmic = parameter.logarithmic;
            out->indexed = parameter.indexed;
            copyText(out->name, parameter.name);
        }
    });
}
int daw_set_insert_parameter(daw_session *s, int32_t owner, uint64_t ownerID, uint64_t insertID,
                             uint32_t parameterID, float value, uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        if (s->model.state().revision != rev)
            throw daw::Error("Revision conflict: refresh the project");
        if (s->pluginParameterGesture || s->model.pluginParameterAutomationGestureActive())
            throw daw::Error("Automation gesture is active");
        const auto &insert = insertFor(s->model.state(), owner, ownerID, insertID);
        std::vector<uint8_t> state;
        uint32_t latency = 0;
        if (daw::isVst3PluginInsert(insert)) {
            auto snapshot = setVst3EditorParameter(s, insert, parameterID, value);
            state = std::move(snapshot.state);
            latency = snapshot.latencyFrames;
        } else {
            requireParameterEditor(insert);
            auto snapshot = daw::setAudioUnitParameter(insert, parameterID, value);
            state = std::move(snapshot.state);
            latency = snapshot.latencyFrames;
        }
        switch (owner) {
        case DAW_INSERT_OWNER_MASTER:
            s->model.updateMasterInsertState(insertID, std::move(state), latency, rev);
            break;
        case DAW_INSERT_OWNER_TRACK:
            s->model.updateTrackInsertState(ownerID, insertID, std::move(state), latency, rev);
            break;
        case DAW_INSERT_OWNER_BUS:
            s->model.updateBusInsertState(ownerID, insertID, std::move(state), latency, rev);
            break;
        default:
            throw daw::Error("Unsupported insert owner");
        }
        resetTransport(s);
    });
}
int daw_set_insert_bypass(daw_session *s, int32_t owner, uint64_t ownerID, uint64_t insertID,
                          int32_t bypassed, uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        if (bypassed != 0 && bypassed != 1)
            throw daw::Error("Bypass must be 0 or 1");
        switch (owner) {
        case DAW_INSERT_OWNER_MASTER:
            s->model.bypassMasterInsert(insertID, bypassed != 0, rev);
            break;
        case DAW_INSERT_OWNER_TRACK:
            s->model.bypassTrackInsert(ownerID, insertID, bypassed != 0, rev);
            break;
        case DAW_INSERT_OWNER_BUS:
            s->model.bypassBusInsert(ownerID, insertID, bypassed != 0, rev);
            break;
        default:
            throw daw::Error("Unsupported insert owner");
        }
        resetTransport(s);
    });
}
int daw_move_insert(daw_session *s, int32_t owner, uint64_t ownerID, uint64_t insertID,
                    uint32_t index, uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        switch (owner) {
        case DAW_INSERT_OWNER_MASTER:
            s->model.moveMasterInsert(insertID, index, rev);
            break;
        case DAW_INSERT_OWNER_TRACK:
            s->model.moveTrackInsert(ownerID, insertID, index, rev);
            break;
        case DAW_INSERT_OWNER_BUS:
            s->model.moveBusInsert(ownerID, insertID, index, rev);
            break;
        default:
            throw daw::Error("Unsupported insert owner");
        }
        resetTransport(s);
    });
}
int daw_remove_insert(daw_session *s, int32_t owner, uint64_t ownerID, uint64_t insertID,
                      uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        switch (owner) {
        case DAW_INSERT_OWNER_MASTER:
            s->model.removeMasterInsert(insertID, rev);
            break;
        case DAW_INSERT_OWNER_TRACK:
            s->model.removeTrackInsert(ownerID, insertID, rev);
            break;
        case DAW_INSERT_OWNER_BUS:
            s->model.removeBusInsert(ownerID, insertID, rev);
            break;
        default:
            throw daw::Error("Unsupported insert owner");
        }
        resetTransport(s);
    });
}
int daw_get_insert_parameter_automation_count(daw_session *s, int32_t owner, uint64_t ownerID,
                                              uint64_t insertID, uint32_t parameterID,
                                              uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing plug-in automation point count");
        const auto &insert = insertFor(s->model.state(), owner, ownerID, insertID);
        const auto lane = std::find_if(insert.parameterAutomation.begin(),
                                       insert.parameterAutomation.end(), [&](const auto &item) {
                                           return item.parameterID == parameterID;
                                       });
        *count = lane == insert.parameterAutomation.end()
                     ? 0
                     : static_cast<uint32_t>(lane->points.size());
    });
}
int daw_get_insert_parameter_automation_point(daw_session *s, int32_t owner, uint64_t ownerID,
                                              uint64_t insertID, uint32_t parameterID,
                                              uint32_t index,
                                              daw_plugin_parameter_automation_point *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_plugin_parameter_automation_point))
            throw daw::Error("Plug-in automation point ABI mismatch");
        const auto &insert = insertFor(s->model.state(), owner, ownerID, insertID);
        const auto lane = std::find_if(insert.parameterAutomation.begin(),
                                       insert.parameterAutomation.end(), [&](const auto &item) {
                                           return item.parameterID == parameterID;
                                       });
        if (lane == insert.parameterAutomation.end() || index >= lane->points.size())
            throw daw::Error("Plug-in automation point index out of range");
        const auto &point = lane->points[index];
        *out = {sizeof(daw_plugin_parameter_automation_point), point.frame, point.normalizedValue};
    });
}
int daw_upsert_insert_parameter_automation_point(daw_session *s, int32_t owner, uint64_t ownerID,
                                                 uint64_t insertID, uint32_t parameterID,
                                                 const char *name, uint64_t frame,
                                                 double normalized, uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        if (!std::isfinite(normalized) || normalized < 0 || normalized > 1)
            throw daw::Error("Plug-in automation value must be normalized 0…1");
        const auto parameterName = name ? std::string(name) : std::string{};
        s->model.upsertPluginParameterAutomation(pluginOwner(owner), ownerID, insertID, parameterID,
                                                 parameterName, frame, normalized, rev);
        resetTransport(s);
    });
}
int daw_remove_insert_parameter_automation_point(daw_session *s, int32_t owner, uint64_t ownerID,
                                                 uint64_t insertID, uint32_t parameterID,
                                                 uint64_t frame, uint64_t rev) {
    return guard(s, [&] {
        validateInsertOwner(owner, ownerID);
        s->model.removePluginParameterAutomation(pluginOwner(owner), ownerID, insertID, parameterID,
                                                 frame, rev);
        resetTransport(s);
    });
}
int daw_preview_workflow(daw_session *s, const daw_workflow_operation *operations, uint32_t count,
                         uint64_t rev, daw_workflow_change *changes, uint32_t capacity,
                         uint32_t *changeCount, uint64_t *afterRevision) {
    return guard(s, [&] {
        if (!changeCount || !afterRevision || (!changes && capacity))
            throw daw::Error("Invalid workflow preview output");
        const auto preview = s->model.previewWorkflow(workflowOperations(operations, count), rev);
        std::vector<std::pair<daw::WorkflowTrackSummary, daw::WorkflowTrackSummary>> changed;
        for (size_t index = 0; index < preview.before.size(); ++index)
            if (preview.before[index] != preview.after[index])
                changed.push_back({preview.before[index], preview.after[index]});
        *changeCount = static_cast<uint32_t>(changed.size());
        *afterRevision = preview.afterRevision;
        if (!changes)
            return;
        if (capacity < changed.size())
            throw daw::Error("Workflow preview buffer is too small");
        for (size_t index = 0; index < changed.size(); ++index) {
            auto &out = changes[index];
            if (out.struct_size != sizeof(daw_workflow_change))
                throw daw::Error("Workflow preview ABI mismatch");
            out = {};
            out.struct_size = sizeof(daw_workflow_change);
            out.track_id = changed[index].first.id;
            out.before_gain_db = changed[index].first.gainDb;
            out.after_gain_db = changed[index].second.gainDb;
            std::memcpy(out.before_name, changed[index].first.name.data(),
                        changed[index].first.name.size());
            std::memcpy(out.after_name, changed[index].second.name.data(),
                        changed[index].second.name.size());
        }
    });
}
int daw_commit_workflow(daw_session *s, const daw_workflow_operation *operations, uint32_t count,
                        uint64_t rev) {
    return guard(s, [&] {
        s->model.commitWorkflow(workflowOperations(operations, count), rev);
        resetTransport(s);
    });
}
int daw_preview_vocal_preparation(daw_session *s, const uint64_t *selected, uint32_t count,
                                  const char *base, double targetRms, double peakCeiling,
                                  double doubleOffset, uint64_t rev, daw_vocal_preview *items,
                                  uint32_t capacity, uint32_t *itemCount, uint64_t *afterRevision) {
    return guard(s, [&] {
        if (!itemCount || !afterRevision || (!items && capacity))
            throw daw::Error("Invalid vocal preview output");
        const auto plan =
            vocalPlan(s, selected, count, base, targetRms, peakCeiling, doubleOffset, rev);
        *itemCount = count;
        *afterRevision = plan.preview.afterRevision;
        if (!items)
            return;
        if (capacity < count)
            throw daw::Error("Vocal preview buffer is too small");
        for (uint32_t index = 0; index < count; ++index) {
            auto &out = items[index];
            if (out.struct_size != sizeof(daw_vocal_preview))
                throw daw::Error("Vocal preview ABI mismatch");
            const auto &analysis = plan.analyses[index];
            const auto peak = amplitudeDb(std::max(analysis.peakLeft, analysis.peakRight));
            const auto rms = amplitudeDb(std::sqrt((double(analysis.rmsLeft) * analysis.rmsLeft +
                                                    double(analysis.rmsRight) * analysis.rmsRight) *
                                                   0.5));
            const auto before = std::find_if(plan.preview.before.begin(), plan.preview.before.end(),
                                             [&](const auto &track) {
                                                 return track.id == plan.ids[index];
                                             });
            const auto after = std::find_if(plan.preview.after.begin(), plan.preview.after.end(),
                                            [&](const auto &track) {
                                                return track.id == plan.ids[index];
                                            });
            out = {};
            out.struct_size = sizeof(daw_vocal_preview);
            out.track_id = plan.ids[index];
            out.analyzed_frames = analysis.analyzedFrames;
            out.peak_db = peak;
            out.rms_db = rms;
            out.proposed_gain_db = after->gainDb;
            out.predicted_peak_db = peak + after->gainDb;
            out.predicted_rms_db = rms + after->gainDb;
            out.peak_limited =
                (after->gainDb < targetRms + (index == 0 ? 0.0 : doubleOffset) - rms - 0.0001) ? 1
                                                                                               : 0;
            std::memcpy(out.before_name, before->name.data(), before->name.size());
            std::memcpy(out.after_name, after->name.data(), after->name.size());
        }
    });
}
int daw_commit_vocal_preparation(daw_session *s, const uint64_t *selected, uint32_t count,
                                 const char *base, double targetRms, double peakCeiling,
                                 double doubleOffset, uint64_t rev) {
    return guard(s, [&] {
        auto plan = vocalPlan(s, selected, count, base, targetRms, peakCeiling, doubleOffset, rev);
        s->model.commitWorkflow(plan.operations, rev);
        resetTransport(s);
    });
}
namespace {
// ABI-level sanity only: shape, ranges and bounds a caller could not possibly
// mean. Timeline, overlap and capacity rules stay inside the domain's Validate.
void validateMidiNoteArray(const daw_midi_note *notes, uint32_t count) {
    if (count > DAW_MIDI_NOTES_PER_CALL)
        throw daw::Error("MIDI note reads and writes are limited to 8192 notes per call");
    if (count && !notes)
        throw daw::Error("MIDI note array is missing");
    for (uint32_t index = 0; index < count; ++index) {
        const auto &note = notes[index];
        if (note.struct_size != sizeof(daw_midi_note) || note.version != DAW_MIDI_NOTE_VERSION)
            throw daw::Error("MIDI note ABI mismatch");
        if (note.pitch > 127)
            throw daw::Error("MIDI pitch must be 0..127");
        if (note.channel > 15)
            throw daw::Error("MIDI channel must be 0..15");
        if (note.velocity < 1 || note.velocity > 127)
            throw daw::Error("MIDI velocity must be 1..127");
    }
}
void toDomainNotes(const daw_midi_note *notes, uint32_t count, std::vector<daw::MidiNote> &out) {
    out.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
        out.push_back({notes[index].start, notes[index].length, notes[index].pitch,
                       notes[index].channel, notes[index].velocity});
}
}
int daw_add_midi_clip(daw_session *s, uint64_t trackID, const daw_midi_clip *clip,
                      const daw_midi_note *notes, uint32_t noteCount, uint64_t rev) {
    return guard(s, [&] {
        if (!clip || clip->struct_size != sizeof(daw_midi_clip) ||
            clip->version != DAW_MIDI_CLIP_VERSION)
            throw daw::Error("MIDI clip ABI mismatch");
        if (clip->lane < 0)
            throw daw::Error("MIDI clip lane must be non-negative");
        if (!clip->length)
            throw daw::Error("MIDI clip length must be positive");
        if (clip->note_count != noteCount)
            throw daw::Error("MIDI clip note_count must match the supplied note array");
        validateMidiNoteArray(notes, noteCount);
        std::vector<daw::MidiNote> converted;
        toDomainNotes(notes, noteCount, converted);
        s->model.addMidiClip(
            trackID, {clip->start, clip->length, std::move(converted), clip->lane, clip->color},
            rev);
    });
}
int daw_remove_midi_clip(daw_session *s, uint64_t trackID, uint32_t index, uint64_t rev) {
    return guard(s, [&] {
        s->model.removeMidiClip(trackID, index, rev);
    });
}
int daw_append_midi_notes(daw_session *s, uint64_t trackID, uint32_t index,
                          const daw_midi_note *notes, uint32_t noteCount, uint64_t rev) {
    return guard(s, [&] {
        validateMidiNoteArray(notes, noteCount);
        std::vector<daw::MidiNote> converted;
        toDomainNotes(notes, noteCount, converted);
        s->model.appendMidiNotes(trackID, index, converted, rev);
    });
}
int daw_set_midi_clip_color(daw_session *s, uint64_t trackID, uint32_t index, uint32_t color,
                            uint64_t rev) {
    return guard(s, [&] {
        s->model.setMidiClipColor(trackID, index, color, rev);
    });
}
int daw_transpose_midi_clip(daw_session *s, uint64_t trackID, uint32_t index, int32_t semitones,
                            uint64_t rev) {
    return guard(s, [&] {
        if (semitones < -127 || semitones > 127)
            throw daw::Error("Transpose must stay within +/-127 semitones");
        s->model.transposeMidiClip(trackID, index, static_cast<int8_t>(semitones), rev);
    });
}
int daw_quantize_midi_clip(daw_session *s, uint64_t trackID, uint32_t index, double gridBeats,
                           uint64_t rev) {
    return guard(s, [&] {
        if (!std::isfinite(gridBeats) || gridBeats <= 0 || gridBeats > 64)
            throw daw::Error("Quantize grid must be a finite beat value in (0, 64]");
        s->model.quantizeMidiClip(trackID, index, gridBeats, rev);
    });
}
int daw_set_midi_notes(daw_session *s, uint64_t trackID, uint32_t index, const daw_midi_note *notes,
                       uint32_t noteCount, uint64_t rev) {
    return guard(s, [&] {
        validateMidiNoteArray(notes, noteCount);
        std::vector<daw::MidiNote> converted;
        toDomainNotes(notes, noteCount, converted);
        s->model.setMidiNotes(trackID, index, std::move(converted), rev);
    });
}
int daw_move_midi_clip(daw_session *s, uint64_t trackID, uint32_t index, uint64_t newStart,
                       uint64_t rev) {
    return guard(s, [&] {
        s->model.moveMidiClip(trackID, index, newStart, rev);
    });
}
int daw_trim_midi_clip(daw_session *s, uint64_t trackID, uint32_t index, uint64_t newStart,
                       uint64_t newLength, uint64_t rev) {
    return guard(s, [&] {
        s->model.trimMidiClip(trackID, index, newStart, newLength, rev);
    });
}
int daw_split_midi_clip(daw_session *s, uint64_t trackID, uint32_t index, uint64_t atFrame,
                        uint64_t rev) {
    return guard(s, [&] {
        s->model.splitMidiClip(trackID, index, atFrame, rev);
    });
}
int daw_get_midi_clip_count(daw_session *s, uint64_t trackID, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing MIDI clip count output");
        for (const auto &track : s->model.state().tracks)
            if (track.id == trackID) {
                *count = static_cast<uint32_t>(track.midiClips.size());
                return;
            }
        throw daw::Error("Track not found");
    });
}
int daw_get_midi_clip(daw_session *s, uint64_t trackID, uint32_t clipIndex, daw_midi_clip *out,
                      uint32_t noteOffset, daw_midi_note *notes, uint32_t capacity,
                      uint32_t *written) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_midi_clip))
            throw daw::Error("MIDI clip ABI mismatch");
        if (capacity > DAW_MIDI_NOTES_PER_CALL)
            throw daw::Error("MIDI note capacity exceeds the per-call limit");
        if (capacity && !notes)
            throw daw::Error("MIDI note buffer is missing");
        for (const auto &track : s->model.state().tracks)
            if (track.id == trackID) {
                if (clipIndex >= track.midiClips.size())
                    throw daw::Error("MIDI clip index out of range");
                const auto &clip = track.midiClips[clipIndex];
                if (noteOffset > clip.notes.size())
                    throw daw::Error("MIDI note offset is past the clip");
                *out = {};
                out->struct_size = sizeof(daw_midi_clip);
                out->version = DAW_MIDI_CLIP_VERSION;
                out->start = clip.start;
                out->length = clip.length;
                out->lane = clip.track;
                out->note_count = static_cast<uint32_t>(clip.notes.size());
                out->color = clip.color;
                const uint32_t available = static_cast<uint32_t>(clip.notes.size()) - noteOffset;
                const uint32_t copied = std::min(available, capacity);
                for (uint32_t index = 0; index < copied; ++index) {
                    const auto &note = clip.notes[noteOffset + index];
                    auto &destination = notes[index];
                    destination = {};
                    destination.struct_size = sizeof(daw_midi_note);
                    destination.version = DAW_MIDI_NOTE_VERSION;
                    destination.start = note.start;
                    destination.length = note.length;
                    destination.pitch = note.pitch;
                    destination.channel = note.channel;
                    destination.velocity = note.velocity;
                }
                if (written)
                    *written = copied;
                return;
            }
        throw daw::Error("Track not found");
    });
}
/* Live MIDI capture and metronome monitoring. See the block comment in daw.h
 * for the v0 contract: one open source, drain-on-poll, commit-on-stop, and a
 * frame measured when the ring is drained rather than when the key moved. */
namespace {
#ifdef __APPLE__
// mach absolute time in nanoseconds, i.e. the clock CoreMIDI stamps packets
// with; midi_input.cpp converts packet time through the same timebase, so an
// event hostTimeNs and this reading are directly comparable.
uint64_t hostClockNs() noexcept {
    static const mach_timebase_info_data_t base = []() {
        mach_timebase_info_data_t info{1, 1};
        if (mach_timebase_info(&info) != 0 || info.denom == 0) {
            info.numer = 1;
            info.denom = 1;
        }
        return info;
    }();
    return mach_absolute_time() * base.numer / base.denom;
}
void refreshMidiDevices(daw_session *s) {
    s->midiDevices = daw::listMidiInputDevices();
}
void closeMidiCapture(daw_session *s) noexcept {
    s->midiInput.reset();
    s->midiInputID = 0;
}
#endif
// The live transport frame the capture is measured against: the audible
// position of a running graph — the same value daw_get_transport publishes and
// the UI playhead shows — otherwise the retained edit position. Reads atomics
// only; it never touches the audio device or the model.
uint64_t captureTransportFrame(daw_session *s) noexcept {
    if (s->output && s->output->renderer.playing.load(std::memory_order_acquire))
        return s->output->renderer.audiblePositionFrames();
    if (s->duplex && s->duplex->renderer.playing.load(std::memory_order_acquire))
        return s->duplex->renderer.audiblePositionFrames();
    return s->selectedFrame;
}
const daw::MidiClip *midiClipAt(const daw::State &state, uint64_t trackID, uint32_t index) {
    for (const auto &track : state.tracks)
        if (track.id == trackID)
            return index < track.midiClips.size() ? &track.midiClips[index] : nullptr;
    return nullptr;
}
#ifdef __APPLE__
// MidiCapturedEvent -> RecordedMidiEvent, oldest first (the ring's order).
// framesFromHostTime is the platform's own host-time->frame helper, so the
// nanosecond arithmetic stays in one place: it clamps an event older than the
// base — including one whose packet carried no timestamp (hostTimeNs 0) — onto
// the base frame instead of underflowing. Subtracting the clip start makes the
// result clip-relative, which is what the model stores, and a take begun
// before the clip opens therefore saturates at frame 0. kind 2 already covers
// a 0x90 with velocity 0 because the capture parser normalises it; the kind 1
// test re-checks that convention rather than trusting it, and a note-off
// carries no velocity because RecordedMidiEvent keeps it note-on-only.
void convertCaptured(daw_session *s, const std::vector<daw::MidiCapturedEvent> &events) {
    s->midiConverted.clear();
    s->midiConverted.reserve(events.size());
    for (const auto &event : events) {
        const auto absolute =
            s->midiAnchorFrame + daw::framesFromHostTime(event.hostTimeNs, s->midiAnchorNs);
        daw::RecordedMidiEvent converted;
        converted.frame = absolute > s->midiClipStart ? absolute - s->midiClipStart : 0;
        converted.pitch = event.pitch;
        converted.channel = event.channel;
        converted.noteOff = event.kind == 2 || (event.kind == 1 && event.velocity == 0);
        converted.velocity = converted.noteOff ? 0 : event.velocity;
        s->midiConverted.push_back(converted);
    }
    if (!s->midiConverted.empty())
        s->midiRecorder->feed(s->midiConverted.data(), s->midiConverted.size());
}
#else
void closeMidiCapture(daw_session *) noexcept {}
#endif
}
int daw_get_midi_input_device_count(daw_session *s, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing MIDI device count");
#ifdef __APPLE__
        refreshMidiDevices(s);
        *count = static_cast<uint32_t>(s->midiDevices.size());
#else
    *count=0;
#endif
    });
}
int daw_get_midi_input_device(daw_session *s, [[maybe_unused]] uint32_t index,
                              daw_midi_device *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_midi_device) ||
            out->version != DAW_MIDI_DEVICE_VERSION)
            throw daw::Error("MIDI device ABI mismatch");
#ifdef __APPLE__
        // The count call publishes the list; re-list here only so a caller that
        // skipped it cannot read a stale or empty cache.
        if (s->midiDevices.empty())
            refreshMidiDevices(s);
        if (index >= s->midiDevices.size())
            throw daw::Error("MIDI device index out of range");
        const auto &device = s->midiDevices[index];
        *out = {};
        out->struct_size = sizeof(daw_midi_device);
        out->version = DAW_MIDI_DEVICE_VERSION;
        out->uniqueID = device.uniqueID;
        out->online = device.online ? 1 : 0;
        copyText(out->name, device.name);
#else
    throw daw::Error("MIDI input requires macOS");
#endif
    });
}
int daw_set_midi_input(daw_session *s, uint32_t uniqueID) {
    return guard(s, [&] {
#ifdef __APPLE__
        if (s->midiRecorder && s->midiRecorder->armed())
            throw daw::Error("Stop the armed MIDI take before changing the capture input");
        if (!uniqueID) {
            closeMidiCapture(s);
            return;
        } // idempotent close
        if (s->midiInput && s->midiInputID == uniqueID)
            return; // already open
        // open() resolves against the live source list and throws before any
        // session state moves, so a rejected id leaves the current input running.
        auto input = daw::MidiInput::open(uniqueID);
        s->midiInput = std::move(input);
        s->midiInputID = uniqueID;
        s->midiCaptured.clear();
#else
    if(uniqueID)throw daw::Error("MIDI input requires macOS");
#endif
    });
}
int daw_midi_input_active(daw_session *s, uint32_t *uniqueID) {
    return guard(s, [&] {
        if (!uniqueID)
            throw daw::Error("Missing MIDI input id");
        *uniqueID = s->midiInputID;
    });
}
int daw_midi_record_arm(daw_session *s, uint64_t trackID, uint32_t clipIndex) {
    return guard(s, [&] {
#ifdef __APPLE__
        if (!s->midiInput)
            throw daw::Error("Open a MIDI input before arming a take");
        const auto *clip = midiClipAt(s->model.state(), trackID, clipIndex);
        if (!clip)
            throw daw::Error("MIDI clip not found");
        if (!clip->length)
            throw daw::Error("MIDI clip has no length");
        // Arming is pure controller state: no Session call, so no revision, and a
        // re-arm simply drops the previous take along with its counters.
        s->midiRecorder = daw::MidiRecorder();
        s->midiRecorder->arm(trackID, clipIndex);
        s->midiClipStart = clip->start;
        s->midiAnchorFrame = captureTransportFrame(s);
        s->midiAnchorNs = hostClockNs();
        // The ring counts drops for the life of the source; the take reports only
        // what it lost, so remember where it started.
        s->midiRingBase = s->midiInput->dropped();
        s->midiCaptured.clear();
        s->midiConverted.clear();
#else
    (void)trackID;(void)clipIndex;throw daw::Error("MIDI input requires macOS");
#endif
    });
}
int daw_midi_record_poll(daw_session *s) {
    return guard(s, [&] {
        if (!s->midiRecorder || !s->midiRecorder->armed())
            return; // safe idle, armed or not
#ifdef __APPLE__
        if (!s->midiInput)
            return;
        // One drain bounds the ring; the UI timer calls this at 10 Hz, so a full
        // 4096 events means the app was starved and the oldest were dropped.
        s->midiCaptured.clear();
        const auto taken = s->midiInput->poll(s->midiCaptured, daw::MidiRing::capacity);
        // Re-base on every drain, empty or not: the pair is always at most one
        // poll interval old, which is what keeps the documented error bounded.
        if (taken) {
            // The clip may have been moved or trimmed between drains; follow it so
            // the take's frames stay relative to the window the UI still shows.
            if (const auto *clip = midiClipAt(s->model.state(), s->midiRecorder->trackID(),
                                              s->midiRecorder->clipIndex()))
                s->midiClipStart = clip->start;
            convertCaptured(s, s->midiCaptured);
        }
        s->midiAnchorFrame = captureTransportFrame(s);
        s->midiAnchorNs = hostClockNs();
#else
    if(s->midiRecorder&&s->midiRecorder->armed())throw daw::Error("MIDI input requires macOS");
#endif
    });
}
int daw_midi_record_stop(daw_session *s) {
    return guard(s, [&] {
        if (!s->midiRecorder || !s->midiRecorder->armed()) {
            s->midiRecorder.reset();
            return;
        } // quiet no-op
#ifdef __APPLE__
        const auto trackID = s->midiRecorder->trackID();
        const auto clipIndex = s->midiRecorder->clipIndex();
        const auto absolute = captureTransportFrame(s);
        // Keys still held close at the transport's own clip-relative position, so
        // a note cannot outlive the take and stop() never consumes a revision.
        const auto stopFrame = absolute > s->midiClipStart ? absolute - s->midiClipStart : 0;
        auto batch = s->midiRecorder->stop(stopFrame);
        s->midiRecorder.reset();
        s->midiClipStart = 0;
        s->midiAnchorNs = 0;
        s->midiAnchorFrame = 0;
        s->midiRingBase = 0;
        if (batch.empty())
            return; // empty take: silent no-op, revision unmoved
        // One guard covers stop and commit: the revision is read here, at the same
        // serial point, so no other writer can have moved it underneath us.
        s->model.appendMidiNotes(trackID, clipIndex, batch, s->model.state().revision);
#else
    s->midiRecorder.reset();
    throw daw::Error("MIDI input requires macOS");
#endif
    });
}
int daw_midi_record_status(daw_session *s, daw_midi_record_status_t *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_midi_record_status_t) ||
            out->version != DAW_MIDI_RECORD_STATUS_VERSION)
            throw daw::Error("MIDI record status ABI mismatch");
        *out = {};
        out->struct_size = sizeof(daw_midi_record_status_t);
        out->version = DAW_MIDI_RECORD_STATUS_VERSION;
        if (!s->midiRecorder || !s->midiRecorder->armed())
            return;
        const auto &recorder = *s->midiRecorder;
        out->armed = 1;
        out->open_notes = recorder.openNotes();
        out->recorded = recorder.recordedNotes();
        out->unmatched = recorder.unmatched();
#ifdef __APPLE__
        const auto ring = s->midiInput && s->midiInput->dropped() > s->midiRingBase
                              ? s->midiInput->dropped() - s->midiRingBase
                              : 0;
        out->dropped = recorder.dropped() + ring;
#else
    out->dropped=recorder.dropped();
#endif
    });
}
int daw_set_record_preroll(daw_session *s, uint64_t frames) {
    return guard(s, [&] {
        if (frames > 48000 * 30)
            throw daw::Error("Pre-roll must be 0-30 seconds");
        s->recordPrerollFrames = frames;
    });
}
int daw_get_record_preroll(daw_session *s, uint64_t *out) {
    return guard(s, [&] {
        if (!out)
            throw daw::Error("Pre-roll output required");
        *out = s->recordPrerollFrames;
    });
}
int daw_set_record_monitor(daw_session *s, int32_t on) {
    return guard(s, [&] {
        if (on != 0 && on != 1)
            throw daw::Error("Record monitor must be 0 or 1");
        s->recordMonitor = on != 0;
    });
}
int daw_get_record_monitor(daw_session *s, int32_t *out) {
    return guard(s, [&] {
        if (!out)
            throw daw::Error("Monitor output required");
        *out = s->recordMonitor ? 1 : 0;
    });
}
int daw_set_auto_monitor_on_arm(daw_session *s, int32_t on) {
    return guard(s, [&] {
        if (on != 0 && on != 1)
            throw daw::Error("Auto-monitor must be 0 or 1");
        s->autoMonitorOnArm = on != 0;
    });
}
int daw_get_auto_monitor_on_arm(daw_session *s, int32_t *out) {
    return guard(s, [&] {
        if (!out)
            throw daw::Error("Auto-monitor output required");
        *out = s->autoMonitorOnArm ? 1 : 0;
    });
}
int daw_set_metronome(daw_session *s, int32_t on) {
    return guard(s, [&] {
        if (on != 0 && on != 1)
            throw daw::Error("Metronome must be 0 or 1");
        s->metronomeEnabled = on != 0;
        // The live graphs take it immediately (Renderer::setMetronome is an atomic
        // monitoring switch the RT callback reads per block); every graph prepared
        // later inherits it from the session field in pollPlaybackPreparation.
        if (s->output)
            s->output->renderer.setMetronome(s->metronomeEnabled);
        if (s->duplex)
            s->duplex->renderer.setMetronome(s->metronomeEnabled);
    });
}
int daw_get_metronome(daw_session *s, int32_t *on) {
    return guard(s, [&] {
        if (!on)
            throw daw::Error("Missing metronome state");
        // A live graph is authoritative when one exists, so a caller sees what the
        // engine will actually click; with none, the remembered intent is next up.
        if (s->output)
            *on = s->output->renderer.metronome() ? 1 : 0;
        else if (s->duplex)
            *on = s->duplex->renderer.metronome() ? 1 : 0;
        else
            *on = s->metronomeEnabled ? 1 : 0;
    });
}
/* Tempo and time-signature ABI sanity mirrors the MIDI block: obvious shape
 * and range rejections happen before the domain owns the command. Changing a
 * map alters the timeline, so a stale playback preparation is cancelled.
 * exactly like the automation-point commands. */
int daw_set_tempo(daw_session *s, uint64_t frame, double bpm, uint64_t rev) {
    return guard(s, [&] {
        if (!std::isfinite(bpm))
            throw daw::Error("Tempo must be finite");
        s->model.setTempoAt(frame, bpm, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_remove_tempo(daw_session *s, uint64_t frame, uint64_t rev) {
    return guard(s, [&] {
        s->model.removeTempo(frame, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_set_time_signature(daw_session *s, uint64_t frame, uint32_t numerator, uint32_t denominator,
                           uint64_t rev) {
    return guard(s, [&] {
        if (numerator < 1 || numerator > 32)
            throw daw::Error("Time signature numerator must be 1..32");
        if (denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8 &&
            denominator != 16 && denominator != 32)
            throw daw::Error("Time signature denominator must be one of 1, 2, 4, 8, 16 or 32");
        s->model.setTimeSignatureAt(frame, static_cast<uint8_t>(numerator),
                                    static_cast<uint8_t>(denominator), rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_remove_time_signature(daw_session *s, uint64_t frame, uint64_t rev) {
    return guard(s, [&] {
        s->model.removeTimeSignature(frame, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_get_tempo_count(daw_session *s, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing tempo count output");
        *count = static_cast<uint32_t>(s->model.state().tempo.size());
    });
}
int daw_get_tempo_point(daw_session *s, uint32_t index, daw_tempo_point *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_tempo_point))
            throw daw::Error("Tempo point ABI mismatch");
        const auto &tempo = s->model.state().tempo;
        if (index >= tempo.size())
            throw daw::Error("Tempo point index out of range");
        *out = {};
        out->struct_size = sizeof(daw_tempo_point);
        out->version = DAW_TEMPO_POINT_VERSION;
        out->frame = tempo[index].frame;
        out->bpm = tempo[index].bpm;
    });
}
int daw_get_time_signature_count(daw_session *s, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing time signature count output");
        *count = static_cast<uint32_t>(s->model.state().timeSignatures.size());
    });
}
int daw_get_time_signature_point(daw_session *s, uint32_t index, daw_time_signature_point *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_time_signature_point))
            throw daw::Error("Time signature ABI mismatch");
        const auto &signatures = s->model.state().timeSignatures;
        if (index >= signatures.size())
            throw daw::Error("Time signature index out of range");
        *out = {};
        out->struct_size = sizeof(daw_time_signature_point);
        out->version = DAW_TIME_SIGNATURE_POINT_VERSION;
        out->frame = signatures[index].frame;
        out->numerator = signatures[index].numerator;
        out->denominator = signatures[index].denominator;
    });
}
int daw_get_marker_count(daw_session *s, uint32_t *count) {
    return guard(s, [&] {
        if (!count)
            throw daw::Error("Missing marker count output");
        *count = static_cast<uint32_t>(s->model.state().markers.size());
    });
}
int daw_get_marker(daw_session *s, uint32_t index, daw_marker *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_marker))
            throw daw::Error("Marker ABI mismatch");
        const auto &markers = s->model.state().markers;
        if (index >= markers.size())
            throw daw::Error("Marker index out of range");
        *out = {};
        out->struct_size = sizeof(daw_marker);
        out->version = DAW_MARKER_VERSION;
        out->frame = markers[index].frame;
        std::memcpy(out->name, markers[index].name.data(), markers[index].name.size());
    });
}
int daw_add_marker(daw_session *s, uint64_t frame, const char *name, uint64_t rev) {
    return guard(s, [&] {
        s->model.addMarker(frame, required(name), rev);
    });
}
int daw_rename_marker(daw_session *s, uint64_t frame, const char *name, uint64_t rev) {
    return guard(s, [&] {
        s->model.renameMarker(frame, required(name), rev);
    });
}
int daw_remove_marker(daw_session *s, uint64_t frame, uint64_t rev) {
    return guard(s, [&] {
        s->model.removeMarker(frame, rev);
    });
}
int daw_copy_clip_to_track(daw_session *s, uint64_t src, uint32_t index, uint64_t dst,
                           uint64_t start, uint64_t rev) {
    return guard(s, [&] {
        s->model.copyClipToTrack(src, index, dst, start, rev);
        resetTransport(s);
    });
}
int daw_move_clip_to_track(daw_session *s, uint64_t src, uint32_t index, uint64_t dst,
                           uint64_t start, uint64_t rev) {
    return guard(s, [&] {
        s->model.moveClipToTrack(src, index, dst, start, rev);
        resetTransport(s);
    });
}
int daw_copy_midi_clip_to_track(daw_session *s, uint64_t src, uint32_t index, uint64_t dst,
                                uint64_t start, uint64_t rev) {
    return guard(s, [&] {
        s->model.copyMidiClipToTrack(src, index, dst, start, rev);
        resetTransport(s);
    });
}
int daw_move_midi_clip_to_track(daw_session *s, uint64_t src, uint32_t index, uint64_t dst,
                                uint64_t start, uint64_t rev) {
    return guard(s, [&] {
        s->model.moveMidiClipToTrack(src, index, dst, start, rev);
        resetTransport(s);
    });
}
int daw_undo(daw_session *s, uint64_t rev) {
    return guard(s, [&] {
        s->model.undo(rev);
        resetTransport(s);
    });
}
int daw_redo(daw_session *s, uint64_t rev) {
    return guard(s, [&] {
        s->model.redo(rev);
        resetTransport(s);
    });
}
daw_save_job *daw_begin_save(daw_session *s, const char *path) {
    daw_save_job *job = nullptr;
    guard(s, [&] {
        auto handle = std::make_unique<daw_save_job>();
        handle->result = daw::startSave(s->model.state(), required(path));
        job = handle.release();
    });
    return job;
}
int daw_poll_save(daw_save_job *job, daw_save_status *out) {
    if (!job || !out || out->struct_size != sizeof(daw_save_status))
        return 1;
    auto &result = *job->result;
    out->status = result.status.load(std::memory_order_acquire);
    out->revision = result.revision;
    std::memset(out->error, 0, sizeof(out->error));
    if (out->status == 2)
        std::memcpy(out->error, result.error, sizeof(out->error));
    return 0;
}
void daw_release_save(daw_save_job *job) {
    delete job;
}
daw_export_job *daw_begin_export(daw_session *s, const char *path, int32_t format) {
    daw_export_job *job = nullptr;
    guard(s, [&] {
        auto handle = std::make_unique<daw_export_job>();
        handle->result = daw::startExport(s->model.state(), required(path), exportFormat(format));
        job = handle.release();
    });
    return job;
}
daw_export_job *daw_begin_export_range(daw_session *s, const char *path, int32_t format,
                                       uint64_t startFrame, uint64_t endFrame) {
    daw_export_job *job = nullptr;
    guard(s, [&] {
        auto handle = std::make_unique<daw_export_job>();
        handle->result = daw::startExportRange(s->model.state(), required(path),
                                               exportFormat(format), startFrame, endFrame);
        job = handle.release();
    });
    return job;
}
int daw_get_export_tail_summary(daw_session *s, const daw_export_options *options,
                                daw_export_tail_summary *out) {
    return guard(s, [&] {
        const auto parsedOptions = exportOptions(options);
        if (!out || out->struct_size != sizeof(daw_export_tail_summary))
            throw daw::Error("Invalid export tail summary");
        const auto summary = daw::inspectExportTail(s->model.state(), parsedOptions);
        writeExportTailSummary(summary, out);
    });
}
daw_export_job *daw_begin_export_with_options(daw_session *s, const char *path, int32_t format,
                                              const daw_export_options *options) {
    daw_export_job *job = nullptr;
    guard(s, [&] {
        auto handle = std::make_unique<daw_export_job>();
        handle->result = daw::startExport(s->model.state(), required(path), exportFormat(format),
                                          exportOptions(options));
        job = handle.release();
    });
    return job;
}
daw_export_job *daw_begin_export_range_with_options(daw_session *s, const char *path,
                                                    int32_t format, uint64_t startFrame,
                                                    uint64_t endFrame,
                                                    const daw_export_options *options) {
    daw_export_job *job = nullptr;
    guard(s, [&] {
        auto handle = std::make_unique<daw_export_job>();
        handle->result =
            daw::startExportRange(s->model.state(), required(path), exportFormat(format),
                                  startFrame, endFrame, exportOptions(options));
        job = handle.release();
    });
    return job;
}
daw_export_job *daw_begin_stem_export(daw_session *s, const char *directory, int32_t format,
                                      const daw_export_options *options) {
    daw_export_job *job = nullptr;
    guard(s, [&] {
        auto handle = std::make_unique<daw_export_job>();
        handle->result = daw::startStemExport(s->model.state(), required(directory),
                                              exportFormat(format), exportOptions(options));
        job = handle.release();
    });
    return job;
}
daw_export_job *daw_begin_stem_export_tracks(daw_session *s, const char *directory, int32_t format,
                                             const daw_export_options *options,
                                             const uint64_t *trackIds, uint32_t trackCount) {
    daw_export_job *job = nullptr;
    guard(s, [&] {
        const auto &state = s->model.state();
        std::vector<uint64_t> only;
        if (trackCount && !trackIds)
            throw daw::Error("Stem track ids missing");
        if (trackIds && trackCount) {
            for (uint32_t i = 0; i < trackCount; ++i) {
                const uint64_t id = trackIds[i];
                const bool known =
                    std::any_of(state.tracks.begin(), state.tracks.end(), [id](const auto &t) {
                        return t.id == id;
                    });
                if (!known)
                    throw daw::Error("Stem track id not found");
                if (std::find(only.begin(), only.end(), id) != only.end())
                    throw daw::Error("Duplicate stem track id");
                only.push_back(id);
            }
        }
        auto handle = std::make_unique<daw_export_job>();
        handle->result = daw::startStemExport(state, required(directory), exportFormat(format),
                                              exportOptions(options), std::move(only));
        job = handle.release();
    });
    return job;
}
int daw_measure_wav(daw_session *s, const char *path, daw_loudness_report *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_loudness_report))
            throw daw::Error("Loudness report ABI mismatch");
        const auto clip = daw::readWav(required(path));
        const auto report = daw::measureLoudness(*clip);
        out->integrated_lufs = report.integratedLufs;
        out->true_peak_db = report.truePeakDb;
        out->gated_silence = report.gatedSilence ? 1 : 0;
    });
}
int daw_package_project(daw_session *s, const char *draftPath, const char *zipPath) {
    return guard(s, [&] {
        std::string error;
        if (!daw::writeProjectPackage(required(draftPath), required(zipPath), error))
            throw daw::Error(error);
    });
}
int daw_extract_package(daw_session *s, const char *zipPath, const char *targetDraftPath) {
    return guard(s, [&] {
        std::string error;
        if (!daw::extractProjectPackage(required(zipPath), required(targetDraftPath), error))
            throw daw::Error(error);
    });
}
int daw_poll_export(daw_export_job *job, daw_export_status *out) {
    if (!job || !out || out->struct_size != sizeof(daw_export_status))
        return 1;
    auto &result = *job->result;
    out->status = result.status.load(std::memory_order_acquire);
    out->revision = result.revision;
    out->rendered_frames = result.renderedFrames.load(std::memory_order_acquire);
    out->total_frames = result.totalFrames;
    std::memset(out->error, 0, sizeof(out->error));
    if (out->status == 2)
        std::memcpy(out->error, result.error, sizeof(out->error));
    return 0;
}
void daw_cancel_export(daw_export_job *job) {
    if (job)
        job->result->cancel.store(true, std::memory_order_release);
}
void daw_release_export(daw_export_job *job) {
    delete job;
}
daw_dawproject_job *daw_begin_dawproject_export(daw_session *s, const char *path, double tempo,
                                                uint32_t numerator, uint32_t denominator,
                                                const char *title) {
    daw_dawproject_job *job = nullptr;
    guard(s, [&] {
        auto handle = std::make_unique<daw_dawproject_job>();
        handle->result = daw::dawproject::startExport(s->model.state(), required(path),
                                                      {tempo, numerator, denominator},
                                                      required(title), "1.15.0");
        job = handle.release();
    });
    return job;
}
int daw_poll_dawproject_export(daw_dawproject_job *job, daw_dawproject_status *out) {
    if (!job || !out || out->struct_size != sizeof(daw_dawproject_status))
        return 1;
    auto &result = *job->result;
    out->status = result.status.load(std::memory_order_acquire);
    out->revision = result.revision;
    out->completed_entries = result.completedEntries.load(std::memory_order_acquire);
    out->total_entries = result.totalEntries;
    out->warning_count = result.warningCount;
    out->info_count = result.infoCount;
    std::memset(out->error, 0, sizeof(out->error));
    if (out->status == 2)
        std::memcpy(out->error, result.error, sizeof(out->error));
    return 0;
}
void daw_cancel_dawproject_export(daw_dawproject_job *job) {
    if (job)
        job->result->cancel.store(true, std::memory_order_release);
}
void daw_release_dawproject_export(daw_dawproject_job *job) {
    delete job;
}
int daw_save_draft(daw_session *s, const char *path) {
    return guard(s, [&] {
        daw::writeDraft(s->model.state(), required(path));
    });
}
int daw_open_draft(daw_session *s, const char *path) {
    return guard(s, [&] {
        auto loaded = daw::readDraft(required(path));
        if (s->input) {
            s->input->cancel();
            s->input.reset();
        }
        if (s->duplex) {
            s->duplex->cancel();
            s->duplex.reset();
        }
        invalidatePlaybackPreparation(s);
        s->recordTarget = 0;
        s->loopEnabled = false;
        s->loopStart = 0;
        s->loopEnd = 0;
        s->output.reset();
        s->vst3ParameterCache.reset();
        closeMidiCapture(s);
        s->midiRecorder.reset();
        s->midiClipStart = 0;
        s->model.replace(std::move(loaded));
        ++s->projectEpoch;
        s->selectedFrame = 0;
    });
}
namespace {
daw_import_job *beginImport(daw_session *s, const char *path, const char *name,
                            uint64_t baseRevision, ImportIntent intent, ImportFormat format,
                            uint64_t trackID, uint64_t startFrame) {
    daw_import_job *job = nullptr;
    guard(s, [&] {
        if (s->model.state().revision != baseRevision)
            throw daw::Error("Revision conflict: refresh the project");
        auto handle = std::make_unique<daw_import_job>();
        handle->owner = s->lifetime;
        handle->projectEpoch = s->projectEpoch;
        handle->baseRevision = baseRevision;
        handle->intent = intent;
        handle->format = format;
        handle->trackID = trackID;
        handle->startFrame = startFrame;
        handle->name = required(name);
        daw::validateName(handle->name);
        if (intent == ImportIntent::Take) {
            const auto found = std::find_if(s->model.state().tracks.begin(),
                                            s->model.state().tracks.end(), [&](const auto &track) {
                                                return track.id == trackID && track.audio;
                                            });
            if (found == s->model.state().tracks.end())
                throw daw::Error("Audio track not found");
        }
        handle->result = (format == ImportFormat::Aiff) ? daw::startAiffImport(required(path))
                                                        : daw::startWavImport(required(path));
        job = handle.release();
    });
    return job;
}
}
daw_import_job *daw_begin_import_wav(daw_session *s, const char *path, const char *name,
                                     uint64_t baseRevision) {
    return beginImport(s, path, name, baseRevision, ImportIntent::Track, ImportFormat::Wav, 0, 0);
}
daw_import_job *daw_begin_import_take_wav(daw_session *s, uint64_t trackID, const char *path,
                                          const char *name, uint64_t startFrame,
                                          uint64_t baseRevision) {
    return beginImport(s, path, name, baseRevision, ImportIntent::Take, ImportFormat::Wav, trackID,
                       startFrame);
}
daw_import_job *daw_begin_import_aiff(daw_session *s, const char *path, const char *name,
                                      uint64_t baseRevision) {
    return beginImport(s, path, name, baseRevision, ImportIntent::Track, ImportFormat::Aiff, 0, 0);
}
daw_import_job *daw_begin_import_take_aiff(daw_session *s, uint64_t trackID, const char *path,
                                           const char *name, uint64_t startFrame,
                                           uint64_t baseRevision) {
    return beginImport(s, path, name, baseRevision, ImportIntent::Take, ImportFormat::Aiff, trackID,
                       startFrame);
}
int daw_poll_import(daw_import_job *job, daw_import_status *out) {
    if (!job || !out || out->struct_size != sizeof(daw_import_status))
        return 1;
    try {
        *out = {};
        out->struct_size = sizeof(daw_import_status);
        out->version = DAW_IMPORT_STATUS_VERSION;
        out->base_revision = job->baseRevision;
        const auto status = job->result->status.load(std::memory_order_acquire);
        const auto phase = job->result->phase.load(std::memory_order_acquire);
        out->status = static_cast<int32_t>(status);
        switch (phase) {
        case daw::ImportJobPhase::Reading:
            out->phase = DAW_IMPORT_PHASE_READING;
            break;
        case daw::ImportJobPhase::Decoding:
            out->phase = DAW_IMPORT_PHASE_DECODING;
            break;
        case daw::ImportJobPhase::Converting:
            out->phase = DAW_IMPORT_PHASE_CONVERTING;
            break;
        case daw::ImportJobPhase::Ready:
            out->phase = DAW_IMPORT_PHASE_READY;
            break;
        }
        out->progress = job->result->progress.load(std::memory_order_acquire);
        out->source_sample_rate = job->result->sourceSampleRate.load(std::memory_order_acquire);
        out->source_channels = job->result->sourceChannels.load(std::memory_order_acquire);
        out->source_frames = job->result->sourceFrames.load(std::memory_order_acquire);
        out->output_frames = job->result->outputFrames.load(std::memory_order_acquire);
        if (status == daw::ImportJobStatus::Failed)
            copyText(out->error, daw::importError(*job->result));
        return 0;
    } catch (...) {
        return 1;
    }
}
void daw_cancel_import(daw_import_job *job) {
    if (job && job->result)
        daw::cancelImport(*job->result);
}
int daw_apply_import(daw_session *s, daw_import_job *job, uint64_t expectedRevision) {
    return guard(s, [&] {
        if (!job || !job->result)
            throw daw::Error("Missing import job");
        const auto owner = job->owner.lock();
        if (!owner || owner.get() != s->lifetime.get())
            throw daw::Error("Import job belongs to a different or closed session");
        if (job->projectEpoch != s->projectEpoch)
            throw daw::Error("Import job belongs to a replaced project");
        std::lock_guard lock(job->result->publication);
        if (job->result->status.load(std::memory_order_acquire) != daw::ImportJobStatus::Ready)
            throw daw::Error("Import job is not ready");
        if (s->model.state().revision != expectedRevision)
            throw daw::Error("Revision conflict: refresh the project");
        const auto clip = job->result->clip;
        if (!clip)
            throw daw::Error("Ready import job has no audio");
        if (job->intent == ImportIntent::Track)
            s->model.import(job->name, clip, expectedRevision);
        else
            s->model.addTake(job->trackID, job->name, clip, job->startFrame, expectedRevision);
        job->result->clip.reset();
        job->result->status.store(daw::ImportJobStatus::Applied, std::memory_order_release);
        resetTransport(s);
    });
}
void daw_release_import(daw_import_job *job) {
    if (!job)
        return;
    daw_cancel_import(job);
    delete job;
}
int daw_import_wav(daw_session *s, const char *path, const char *name, uint64_t rev) {
    return guard(s, [&] {
        auto clip = daw::readWav(required(path));
        s->model.import(required(name), std::move(clip), rev);
        resetTransport(s);
    });
}
int daw_import_take_wav(daw_session *s, uint64_t id, const char *path, const char *name,
                        uint64_t start, uint64_t rev) {
    return guard(s, [&] {
        auto clip = daw::readWav(required(path));
        s->model.addTake(id, required(name), std::move(clip), start, rev);
        resetTransport(s);
    });
}
int daw_import_aiff(daw_session *s, const char *path, const char *name, uint64_t rev) {
    return guard(s, [&] {
        auto clip = daw::readAiff(required(path));
        s->model.import(required(name), std::move(clip), rev);
        resetTransport(s);
    });
}
int daw_import_take_aiff(daw_session *s, uint64_t id, const char *path, const char *name,
                         uint64_t start, uint64_t rev) {
    return guard(s, [&] {
        auto clip = daw::readAiff(required(path));
        s->model.addTake(id, required(name), std::move(clip), start, rev);
        resetTransport(s);
    });
}
int daw_get_take(daw_session *s, uint64_t id, uint32_t index, daw_take *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_take))
            throw daw::Error("Take ABI mismatch");
        for (const auto &t : s->model.state().tracks)
            if (t.id == id) {
                if (!t.audio || index > t.takes.size())
                    throw daw::Error("Take index out of range");
                const auto name = index == 0 ? t.name : t.takes[index - 1].name;
                const auto start = index == 0 ? t.baseStart : t.takes[index - 1].start;
                const auto clip = index == 0 ? t.audio : t.takes[index - 1].audio;
                *out = {};
                out->struct_size = sizeof(daw_take);
                out->index = index;
                out->start = start;
                out->frames = clip->frames();
                std::memcpy(out->name, name.data(), name.size());
                return;
            }
        throw daw::Error("Track not found");
    });
}
int daw_get_take_waveform(daw_session *s, uint64_t id, uint32_t index, float *peaks,
                          uint32_t count) {
    return guard(s, [&] {
        if (!peaks || count != 512)
            throw daw::Error("Take waveform requires a 512-float buffer");
        for (const auto &t : s->model.state().tracks)
            if (t.id == id) {
                if (!t.audio || index > t.takes.size())
                    throw daw::Error("Take index out of range");
                const auto clip = index == 0 ? t.audio : t.takes[index - 1].audio;
                std::copy(clip->peaks().begin(), clip->peaks().end(), peaks);
                return;
            }
        throw daw::Error("Track not found");
    });
}
int daw_comp_range(daw_session *s, uint64_t id, uint32_t take, uint64_t start, uint64_t end,
                   uint64_t rev) {
    return guard(s, [&] {
        if (end <= start)
            throw daw::Error("Comp end must follow start");
        s->model.compRange(id, take, start, end - start, rev);
        resetTransport(s);
    });
}
int daw_get_clip(daw_session *s, uint64_t id, uint32_t index, daw_clip *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_clip))
            throw daw::Error("Clip ABI mismatch");
        for (const auto &t : s->model.state().tracks)
            if (t.id == id) {
                if (index >= t.regions.size())
                    throw daw::Error("Clip index out of range");
                const auto &r = t.regions[index];
                *out = {sizeof(daw_clip), r.start, r.sourceOffset, r.length, r.fadeIn, r.fadeOut,
                        r.take,           r.color, r.gain,         r.muted,  r.looped, r.pan};
                return;
            }
        throw daw::Error("Track not found");
    });
}
int daw_edit_clip(daw_session *s, uint64_t id, uint32_t index, uint64_t start, uint64_t offset,
                  uint64_t length, uint64_t rev) {
    return guard(s, [&] {
        s->model.editClip(id, index, start, offset, length, rev);
        resetTransport(s);
    });
}
int daw_edit_clip_full(daw_session *s, uint64_t id, uint32_t index, uint64_t start, uint64_t offset,
                       uint64_t length, uint64_t fadeIn, uint64_t fadeOut, uint64_t rev) {
    return guard(s, [&] {
        s->model.editClipFull(id, index, start, offset, length, fadeIn, fadeOut, rev);
        resetTransport(s);
    });
}
int daw_split_clip(daw_session *s, uint64_t id, uint32_t index, uint64_t frame, uint64_t rev) {
    return guard(s, [&] {
        s->model.splitClip(id, index, frame, rev);
        resetTransport(s);
    });
}
int daw_duplicate_clip(daw_session *s, uint64_t id, uint32_t index, uint64_t rev) {
    return guard(s, [&] {
        s->model.duplicateClip(id, index, rev);
        resetTransport(s);
    });
}
int daw_delete_clip(daw_session *s, uint64_t id, uint32_t index, uint64_t rev) {
    return guard(s, [&] {
        s->model.deleteClip(id, index, rev);
        resetTransport(s);
    });
}
int daw_set_clip_fades(daw_session *s, uint64_t id, uint32_t index, uint64_t fadeIn,
                       uint64_t fadeOut, uint64_t rev) {
    return guard(s, [&] {
        s->model.setClipFades(id, index, fadeIn, fadeOut, rev);
        resetTransport(s);
    });
}
int daw_set_crossfade(daw_session *s, uint64_t id, uint32_t index, uint64_t duration,
                      uint64_t rev) {
    return guard(s, [&] {
        s->model.setCrossfade(id, index, duration, rev);
        resetTransport(s);
    });
}
int daw_set_track_color(daw_session *s, uint64_t id, uint32_t color, uint64_t rev) {
    return guard(s, [&] {
        s->model.setTrackColor(id, color, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_duplicate_track(daw_session *s, uint64_t id, uint64_t *out_new_id, uint64_t rev) {
    return guard(s, [&] {
        if (!out_new_id)
            throw daw::Error("Missing duplicate track id output");
        *out_new_id = s->model.duplicateTrack(id, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_set_clip_color(daw_session *s, uint64_t id, uint32_t index, uint32_t color, uint64_t rev) {
    return guard(s, [&] {
        s->model.setClipColor(id, index, color, rev);
        cancelStalePlaybackPreparation(s);
    });
}
int daw_set_clip_gain(daw_session *s, uint64_t id, uint32_t index, double gain_db, uint64_t rev) {
    return guard(s, [&] {
        if (!std::isfinite(gain_db))
            throw daw::Error("Clip gain must be finite");
        s->model.setClipGain(id, index, gain_db, rev);
        resetTransport(s);
    });
}
int daw_set_clip_muted(daw_session *s, uint64_t id, uint32_t index, uint32_t muted, uint64_t rev) {
    return guard(s, [&] {
        if (muted > 1)
            throw daw::Error("Clip mute flag must be 0 or 1");
        s->model.setClipMuted(id, index, muted != 0, rev);
        resetTransport(s);
    });
}
int daw_set_clip_looped(daw_session *s, uint64_t id, uint32_t index, uint32_t looped,
                        uint64_t rev) {
    return guard(s, [&] {
        if (looped > 1)
            throw daw::Error("Clip loop flag must be 0 or 1");
        s->model.setClipLooped(id, index, looped != 0, rev);
        resetTransport(s);
    });
}
int daw_delete_clips(daw_session *s, uint64_t id, const uint32_t *indices, uint32_t count,
                     uint64_t rev) {
    return guard(s, [&] {
        if (count > 0 && !indices)
            throw daw::Error("Clip indices required");
        s->model.deleteClips(
            id, count ? std::vector<uint32_t>(indices, indices + count) : std::vector<uint32_t>{},
            rev);
        resetTransport(s);
    });
}
int daw_nudge_clips(daw_session *s, uint64_t id, const uint32_t *indices, uint32_t count,
                    int64_t delta, uint64_t rev) {
    return guard(s, [&] {
        if (count > 0 && !indices)
            throw daw::Error("Clip indices required");
        s->model.nudgeClips(
            id, count ? std::vector<uint32_t>(indices, indices + count) : std::vector<uint32_t>{},
            delta, rev);
        resetTransport(s);
    });
}
int daw_set_clip_pan(daw_session *s, uint64_t id, uint32_t index, double pan, uint64_t rev) {
    return guard(s, [&] {
        s->model.setClipPan(id, index, pan, rev);
        resetTransport(s);
    });
}
int daw_seek_frame(daw_session *s, uint64_t frame) {
    return guard(s, [&] {
        if (recordingActive(s))
            throw daw::Error("Stop recording before seeking");
        if (frame > duration(s))
            throw daw::Error("Position exceeds project duration");
        invalidatePlaybackPreparation(s);
        s->output.reset();
        s->selectedFrame = frame;
    });
}
int daw_set_loop(daw_session *s, int32_t enabled, uint64_t start, uint64_t end) {
    return guard(s, [&] {
        if (recordingActive(s))
            throw daw::Error("Stop recording before changing the loop");
        if (enabled != 0 && enabled != 1)
            throw daw::Error("Loop enabled must be 0 or 1");
        if (enabled && (end <= start || end > duration(s)))
            throw daw::Error("Loop range must be inside the project");
        invalidatePlaybackPreparation(s);
        if (s->output) {
            s->output->stop();
            s->selectedFrame = s->output->renderer.audiblePositionFrames();
            s->output.reset();
        }
        s->loopEnabled = enabled != 0;
        s->loopStart = s->loopEnabled ? start : 0;
        s->loopEnd = s->loopEnabled ? end : 0;
        if (s->loopEnabled && s->selectedFrame >= s->loopEnd)
            s->selectedFrame = s->loopStart;
    });
}
int daw_get_waveform(daw_session *s, uint64_t id, float *peaks, uint32_t count) {
    return guard(s, [&] {
        if (!peaks || count != 512)
            throw daw::Error("Waveform requires a 512-float buffer");
        for (const auto &t : s->model.state().tracks)
            if (t.id == id) {
                if (t.audio)
                    std::copy(t.audio->peaks().begin(), t.audio->peaks().end(), peaks);
                else
                    std::fill_n(peaks, count, 0);
                return;
            }
        throw daw::Error("Track not found");
    });
}
int daw_play(daw_session *s) {
    return guard(s, [&] {
        if (recordingActive(s))
            throw daw::Error("Stop recording before playback");
        if (s->playbackPreparation)
            return;
        if (s->output && s->output->renderer.playing.load(std::memory_order_acquire))
            return;
        beginPlaybackPreparation(s);
    });
}
int daw_record_start(daw_session *s, uint64_t startFrame, const char *recoveryPath) {
    return guard(s, [&] {
        startRecording(s, startFrame, recoveryPath, 0);
    });
}
int daw_record_start_take(daw_session *s, uint64_t id, uint64_t startFrame,
                          const char *recoveryPath) {
    return guard(s, [&] {
        startRecording(s, startFrame, recoveryPath, id);
    });
}
int daw_record_stop(daw_session *s, const char *name, uint64_t rev) {
    return guard(s, [&] {
        if (!recordingActive(s))
            throw daw::Error("Recording is not active");
        std::string takeName = required(name);
        daw::validateName(takeName);
        if (rev != s->model.state().revision)
            throw daw::Error("Revision conflict: refresh the project");
        auto input = std::move(s->input);
        auto duplex = std::move(s->duplex);
        auto clip = duplex ? duplex->stop() : input->stop();
        if (duplex) {
            auto passes = daw::splitLoopPasses(*clip, s->loopEnd - s->loopStart);
            std::vector<daw::Take> additions;
            additions.reserve(passes.size());
            for (size_t i = 0; i < passes.size(); ++i) {
                auto nameForPass = takeName + " · " + std::to_string(i + 1);
                try {
                    daw::validateName(nameForPass);
                } catch (...) {
                    nameForPass = "Loop take " + std::to_string(i + 1);
                }
                additions.push_back({std::move(nameForPass), s->loopStart, std::move(passes[i])});
            }
            s->model.addTakes(s->recordTarget, std::move(additions), rev);
            duplex->discardRecovery();
        } else {
            if (s->recordTarget)
                s->model.addTake(s->recordTarget, takeName, std::move(clip), s->recordStart, rev);
            else
                s->model.importAt(takeName, std::move(clip), s->recordStart, rev);
            input->discardRecovery();
        }
        s->selectedFrame = s->recordStart;
        s->recordTarget = 0;
        resetTransport(s);
    });
}
int daw_record_cancel(daw_session *s) {
    return guard(s, [&] {
        if (s->input) {
            s->input->cancel();
            s->input.reset();
        }
        if (s->duplex) {
            s->duplex->cancel();
            s->duplex.reset();
        }
        s->recordTarget = 0;
    });
}
int daw_get_recording(daw_session *s, daw_recording *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_recording))
            throw daw::Error("Recording ABI mismatch");
        *out = {sizeof(daw_recording), 0, 0, 0, 0, 0, 0, 0};
        if (!recordingActive(s))
            return;
        if (s->duplex) {
            s->duplex->checkDevices();
            out->overflowed = s->duplex->overflowed();
            out->frames = s->duplex->frames();
            out->callbacks = s->duplex->callbacks();
            out->loop_recording = 1;
            const auto loopFrames = s->loopEnd - s->loopStart;
            out->pass_count = static_cast<uint32_t>((out->frames + loopFrames - 1) / loopFrames);
        } else {
            s->input->checkDevice();
            out->overflowed = s->input->overflowed();
            out->frames = s->input->frames();
            out->callbacks = s->input->callbacks();
        }
        out->recording = 1;
        out->target_track_id = s->recordTarget;
        auto now = std::chrono::steady_clock::now();
        if (out->callbacks != s->recordLastCallbacks) {
            s->recordLastCallbacks = out->callbacks;
            s->recordProgress = now;
        }
        if (now - s->recordProgress > std::chrono::seconds(2)) {
            if (s->duplex)
                s->duplex->markStalled();
            else
                s->input->cancel();
            throw daw::Error("Input stalled: no audio callbacks for 2 seconds");
        }
    });
}
int daw_recover_take(daw_session *s, const char *path, const char *name, uint64_t rev) {
    return guard(s, [&] {
        auto recoveryPath = std::string(required(path)), takeName = std::string(required(name));
        auto recovered = daw::recoverTake(recoveryPath);
        s->model.importAt(takeName, std::move(recovered.clip), recovered.startFrame, rev);
        std::error_code error;
        std::filesystem::remove(recoveryPath, error);
        s->selectedFrame = recovered.startFrame;
        resetTransport(s);
    });
}
int daw_stop(daw_session *s) {
    return guard(s, [&] {
        if (s->duplex)
            throw daw::Error("Use recording stop while loop recording is active");
        invalidatePlaybackPreparation(s);
        s->transientOutputState = 0;
        if (s->output) {
            s->output->stop();
            s->selectedFrame = s->output->renderer.audiblePositionFrames();
            s->output->renderer.resetMeters();
        }
    });
}
int daw_get_transport(daw_session *s, daw_transport *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_transport))
            throw daw::Error("Transport ABI mismatch");
        pollPlaybackPreparation(s);
        *out = {sizeof(daw_transport),
                0,
                s->selectedFrame,
                duration(s),
                0,
                0,
                0,
                s->loopEnabled ? 1 : 0,
                s->loopStart,
                s->loopEnd,
                0,
                0};
        if (!s->output && !s->duplex)
            return;
        auto &r = s->duplex ? s->duplex->renderer : s->output->renderer;
        if (s->duplex)
            s->duplex->checkDevices();
        else
            s->output->checkDevice();
        auto now = std::chrono::steady_clock::now();
        auto callbacks = r.callbacks.load();
        if (callbacks != s->lastCallbacks) {
            s->lastCallbacks = callbacks;
            s->lastProgress = now;
        }
        if (r.playing.load() && now - s->lastProgress > std::chrono::seconds(2)) {
            if (s->duplex)
                s->duplex->markStalled();
            else
                s->output->markStalled();
            throw daw::Error("Output stalled: no audio callbacks for 2 seconds");
        }
        out->playing = r.playing.load();
        out->frame = r.audiblePositionFrames();
        s->selectedFrame = out->frame;
        out->duration = r.duration();
        out->callbacks = callbacks;
        out->clipped_frames = r.clipped.load();
        out->peak = r.peak.load();
        out->plugin_errors = r.pluginErrors.load();
        out->output_latency_frames = r.masterLatencyFrames();
        if (!out->playing && s->output)
            s->output->stop();
    });
}
int daw_get_channel_meter(daw_session *s, int32_t owner, uint64_t ownerID, daw_channel_meter *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_channel_meter))
            throw daw::Error("Channel meter ABI mismatch");
        *out = {sizeof(daw_channel_meter), DAW_CHANNEL_METER_VERSION, 0, 0};
        // Validate a durable owner even while no live renderer exists, so callers
        // cannot accidentally treat a stale strip ID as a stopped zero meter.
        const auto &state = s->model.state();
        validateInsertOwner(owner, ownerID);
        switch (owner) {
        case DAW_INSERT_OWNER_TRACK:
            if (std::none_of(state.tracks.begin(), state.tracks.end(), [&](const auto &item) {
                    return item.id == ownerID;
                }))
                throw daw::Error("Track not found");
            break;
        case DAW_INSERT_OWNER_BUS:
            if (std::none_of(state.buses.begin(), state.buses.end(), [&](const auto &item) {
                    return item.id == ownerID;
                }))
                throw daw::Error("Bus not found");
            break;
        case DAW_INSERT_OWNER_MASTER:
            break;
        default:
            throw daw::Error("Unsupported insert owner");
        }
        if (s->duplex && s->duplex->renderer.playing.load())
            readChannelMeter(state, s->duplex->renderer, owner, ownerID, out->left_peak,
                             out->right_peak);
        else if (s->output && s->output->renderer.playing.load())
            readChannelMeter(state, s->output->renderer, owner, ownerID, out->left_peak,
                             out->right_peak);
    });
}
int daw_get_master_loudness(daw_session *s, daw_master_loudness *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_master_loudness))
            throw daw::Error("Master loudness ABI mismatch");
        *out = {sizeof(daw_master_loudness), DAW_MASTER_LOUDNESS_VERSION, -200.0f, -200.0f, 0};
        const daw::Renderer *live = nullptr;
        if (s->duplex && s->duplex->renderer.playing.load())
            live = &s->duplex->renderer;
        else if (s->output && s->output->renderer.playing.load())
            live = &s->output->renderer;
        if (live) {
            float momentary = 0, shortTerm = 0;
            live->masterLoudness(momentary, shortTerm);
            out->momentary_lufs = momentary;
            out->short_term_lufs = shortTerm;
            out->live = 1;
        }
    });
}
int daw_get_output_status(daw_session *s, daw_output_status *out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(daw_output_status))
            throw daw::Error("Output status ABI mismatch");
        *out = {sizeof(daw_output_status), s->transientOutputState, 0, s->playbackGeneration, 0, 0};
        if (s->playbackPreparation)
            return;
        if (!s->output && !s->duplex)
            return;
        const auto status = s->duplex ? s->duplex->telemetry() : s->output->telemetry();
        out->state = static_cast<int32_t>(status.state);
        out->device_id = status.deviceID;
        out->generation = status.generation;
        out->callbacks = status.callbacks;
        out->callback_errors = status.callbackErrors;
    });
}
void daw_error(daw_session *s, char *buffer, size_t capacity) {
    if (buffer && capacity)
        std::snprintf(buffer, capacity, "%s", s ? s->error : "Missing session");
}
}
