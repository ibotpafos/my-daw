#include "domain/session.hpp"
#include "plugins/plugin_descriptor.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
namespace daw {
bool isVst3PluginInsert(const PluginInsert &plugin) noexcept {
    return plugin.type == kVst3PluginComponentSentinel &&
           plugin.subtype == kVst3PluginComponentSentinel &&
           plugin.manufacturer == kVst3PluginComponentSentinel;
}
void validateName(const std::string &name) {
    // Strict UTF-8 scalar validation, rejecting NUL/control characters and overlong encodings.
    size_t count = 0;
    for (size_t i = 0; i < name.size();) {
        auto c = static_cast<unsigned char>(name[i++]);
        uint32_t cp;
        int more;
        if (c < 0x80) {
            cp = c;
            more = 0;
        } else if (c >= 0xC2 && c <= 0xDF) {
            cp = c & 31;
            more = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            cp = c & 15;
            more = 2;
        } else if (c >= 0xF0 && c <= 0xF4) {
            cp = c & 7;
            more = 3;
        } else
            throw Error("Invalid UTF-8 name");
        for (int j = 0; j < more; ++j) {
            if (i >= name.size())
                throw Error("Truncated UTF-8 name");
            auto b = static_cast<unsigned char>(name[i++]);
            if ((b & 0xC0) != 0x80)
                throw Error("Invalid UTF-8 continuation");
            cp = (cp << 6) | (b & 63);
        }
        if ((more == 1 && cp < 0x80) || (more == 2 && cp < 0x800) || (more == 3 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp < 32 || (cp >= 127 && cp <= 159))
            throw Error("Invalid character in name");
        ++count;
    }
    if (count == 0 || count > 120)
        throw Error("Name must contain 1–120 characters");
}
namespace {
void validateAutomation(const std::vector<AutomationPoint> &points, double minimum, double maximum,
                        const char *label) {
    if (points.size() > 2048)
        throw Error(std::string(label) + " supports at most 2048 automation points");
    uint64_t previousFrame = 0;
    bool first = true;
    for (const auto &point : points) {
        if (point.frame > 48000 * 600)
            throw Error("Automation point exceeds timeline limit");
        if (!std::isfinite(point.gainDb) || point.gainDb < minimum || point.gainDb > maximum)
            throw Error(std::string(label) + " automation value is outside its range");
        if (!first && point.frame <= previousFrame)
            throw Error("Automation frames must be strictly ordered");
        previousFrame = point.frame;
        first = false;
    }
}
void upsertAutomation(std::vector<AutomationPoint> &points, uint64_t frame, double value) {
    auto point = std::lower_bound(points.begin(), points.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point != points.end() && point->frame == frame)
        point->gainDb = value;
    else
        points.insert(point, {frame, value});
}
bool removeAutomation(std::vector<AutomationPoint> &points, uint64_t frame) {
    auto point = std::lower_bound(points.begin(), points.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point == points.end() || point->frame != frame)
        return false;
    points.erase(point);
    return true;
}
std::vector<AutomationPoint> &automationLane(State &state, AutomationTarget target,
                                             uint64_t targetID) {
    switch (target) {
    case AutomationTarget::TrackVolume: {
        auto track = std::find_if(state.tracks.begin(), state.tracks.end(), [&](const auto &item) {
            return item.id == targetID;
        });
        if (track == state.tracks.end())
            throw Error("Track not found");
        return track->volumeAutomation;
    }
    case AutomationTarget::TrackPan: {
        auto track = std::find_if(state.tracks.begin(), state.tracks.end(), [&](const auto &item) {
            return item.id == targetID;
        });
        if (track == state.tracks.end())
            throw Error("Track not found");
        return track->panAutomation;
    }
    case AutomationTarget::BusGain: {
        auto bus = std::find_if(state.buses.begin(), state.buses.end(), [&](const auto &item) {
            return item.id == targetID;
        });
        if (bus == state.buses.end())
            throw Error("Bus not found");
        return bus->gainAutomation;
    }
    case AutomationTarget::MasterGain:
        if (targetID != 0)
            throw Error("Master automation target ID must be zero");
        return state.masterGainAutomation;
    }
    throw Error("Unsupported automation target");
}
void validateGestureValue(AutomationTarget target, uint64_t frame, double value) {
    if (frame > 48000 * 600)
        throw Error("Automation point exceeds timeline limit");
    const auto pan = target == AutomationTarget::TrackPan;
    if (!std::isfinite(value) || value < (pan ? -1.0 : -120.0) || value > (pan ? 1.0 : 24.0))
        throw Error("Automation value is outside its range");
}
void applyWorkflow(State &state, const std::vector<WorkflowOperation> &batch) {
    if (batch.empty() || batch.size() > 100)
        throw Error("Workflow supports 1–100 operations");
    for (const auto &operation : batch) {
        auto track = std::find_if(state.tracks.begin(), state.tracks.end(), [&](const auto &item) {
            return item.id == operation.trackID;
        });
        if (track == state.tracks.end())
            throw Error("Workflow track not found");
        switch (operation.kind) {
        case WorkflowOperationKind::RenameTrack:
            track->name = operation.name;
            break;
        case WorkflowOperationKind::SetTrackGain:
            track->gain = operation.gainDb;
            break;
        default:
            throw Error("Unsupported workflow operation");
        }
    }
}
std::vector<WorkflowTrackSummary> workflowSummary(const State &state) {
    std::vector<WorkflowTrackSummary> result;
    result.reserve(state.tracks.size());
    for (const auto &track : state.tracks)
        result.push_back({track.id, track.name, track.gain});
    return result;
}
}
void validate(const State &state) {
    if (state.revision >= static_cast<uint64_t>(INT64_MAX) || state.nextID == 0 ||
        state.nextID >= static_cast<uint64_t>(INT64_MAX))
        throw Error("Counter limit reached");
    if (state.tracks.size() > 256)
        throw Error("Draft supports at most 256 tracks");
    std::set<uint64_t> ids;
    size_t audioCount = 0, audioAssets = 0, audioBytes = 0, midiNotes = 0;
    for (const auto &t : state.tracks) {
        validateName(t.name);
        if (t.audio) {
            ++audioCount;
            ++audioAssets;
            audioBytes += t.audio->samples().size() * sizeof(float);
            if (t.baseStart > 48000 * 600 || t.audio->frames() > 48000 * 600 - t.baseStart)
                throw Error("Invalid base take bounds");
            if (t.takes.size() > 15)
                throw Error("Track supports at most 16 takes");
            for (const auto &take : t.takes) {
                validateName(take.name);
                if (!take.audio || take.start > 48000 * 600 ||
                    take.audio->frames() > 48000 * 600 - take.start)
                    throw Error("Invalid take bounds");
                ++audioAssets;
                audioBytes += take.audio->samples().size() * sizeof(float);
            }
            if (t.regions.size() > 256)
                throw Error("Audio track supports at most 256 clips");
            uint64_t previousEnd = 0;
            bool first = true;
            size_t regionIndex = 0;
            for (const auto &region : t.regions) {
                const auto source = region.take == 0 ? t.audio
                                                     : (region.take <= t.takes.size()
                                                            ? t.takes[region.take - 1].audio
                                                            : nullptr);
                // Looped regions may outlive their slice: reads wrap inside
                // [sourceOffset, frames); an unlooped one stays strictly inside.
                if (!source || region.sourceOffset >= source->frames() || !region.length ||
                    (!region.looped && region.length > source->frames() - region.sourceOffset) ||
                    region.start > 48000 * 600 || region.length > 48000 * 600 - region.start)
                    throw Error("Invalid clip bounds (timeline limit: 10 minutes)");
                if (region.fadeIn > region.length || region.fadeOut > region.length ||
                    region.fadeIn > region.length - region.fadeOut)
                    throw Error("Clip fades exceed its duration");
                if (!first && region.start < previousEnd) {
                    const auto overlap = previousEnd - region.start;
                    const auto &previous = t.regions[regionIndex - 1];
                    if (overlap >= previous.length || overlap >= region.length)
                        throw Error("Crossfade must leave audible material in both clips");
                    if (previous.fadeOut != overlap || region.fadeIn != overlap)
                        throw Error("Clip overlap requires a matching crossfade");
                    if (regionIndex > 1 && region.start < t.regions[regionIndex - 2].start +
                                                              t.regions[regionIndex - 2].length)
                        throw Error("Three clips cannot overlap");
                }
                previousEnd = region.start + region.length;
                first = false;
                ++regionIndex;
            }
        } else if (!t.regions.empty() || !t.takes.empty())
            throw Error("Empty track cannot contain clips or takes");
        if (t.id == 0 || t.id >= state.nextID || !ids.insert(t.id).second)
            throw Error("Invalid track ID");
        if (!std::isfinite(t.gain) || t.gain < -120 || t.gain > 24)
            throw Error("Gain outside -120…24 dB");
        if (!std::isfinite(t.pan) || t.pan < -1 || t.pan > 1)
            throw Error("Pan outside -1…1");
        validateAutomation(t.volumeAutomation, -120, 24, "Track volume");
        validateAutomation(t.panAutomation, -1, 1, "Track pan");
        if (t.midiClips.size() > kMaxMidiClipsPerTrack)
            throw Error("Track supports at most 64 MIDI clips");
        for (size_t clipIndex = 0; clipIndex < t.midiClips.size(); ++clipIndex) {
            const auto &clip = t.midiClips[clipIndex];
            if (clip.track < 0)
                throw Error("MIDI clip lane must be non-negative");
            if (!clip.length || clip.start > kMaxMidiFrame || clip.length > kMaxMidiFrame)
                throw Error("MIDI clip bounds exceed the project timeline limit");
            const auto clipEnd = clip.start + clip.length;
            for (size_t other = 0; other < clipIndex; ++other) {
                const auto &previous = t.midiClips[other];
                if (clip.start < previous.start + previous.length && previous.start < clipEnd)
                    throw Error("MIDI clip overlap is not allowed");
            }
            uint64_t noteEnd = 0;
            for (const auto &note : clip.notes) {
                if (midiNotes == kMaxMidiNotesPerProject)
                    throw Error("Project supports at most 65536 MIDI notes");
                ++midiNotes;
                if (note.pitch > 127)
                    throw Error("MIDI pitch outside 0…127");
                if (note.channel > 15)
                    throw Error("MIDI channel outside 0…15");
                if (note.velocity < 1 || note.velocity > 127)
                    throw Error("MIDI velocity outside 1…127");
                if (!note.length || note.length > kMaxMidiNoteLength)
                    throw Error("MIDI note length outside 1…480000 frames");
                if (note.start > clip.length || note.length > clip.length - note.start)
                    throw Error("MIDI notes must fit inside their clip");
                noteEnd = std::max(noteEnd, note.start + note.length);
            }
            if (noteEnd > clip.length)
                throw Error("MIDI clip length is shorter than its notes");
        }
    }
    if (!std::isfinite(state.masterGain) || state.masterGain < -120 || state.masterGain > 24)
        throw Error("Master gain outside -120…24 dB");
    validateAutomation(state.masterGainAutomation, -120, 24, "Master gain");
    // Tempo and time-signature maps: sorted unique frames below the shared
    // project timeline limit, bounded size, and an anchored primary point.
    if (state.tempo.size() > kMaxTempoPointsPerProject)
        throw Error("Too many tempo points");
    if (state.timeSignatures.size() > kMaxTimeSignaturePointsPerProject)
        throw Error("Too many time signature points");
    if (state.tempo.empty() || state.tempo.front().frame != 0)
        throw Error("The tempo map must start at frame 0");
    if (state.timeSignatures.empty() || state.timeSignatures.front().frame != 0)
        throw Error("The time signature map must start at frame 0");
    {
        uint64_t tempoPrevious = 0, signaturePrevious = 0;
        bool tempoFirst = true, signatureFirst = true;
        for (const auto &point : state.tempo) {
            if (point.frame >= kMaxMidiFrame)
                throw Error("Tempo point exceeds the project timeline limit");
            if (!std::isfinite(point.bpm) || point.bpm <= kMinTempoBpm || point.bpm > kMaxTempoBpm)
                throw Error("Tempo outside 20…999 BPM");
            if (!tempoFirst && point.frame <= tempoPrevious)
                throw Error("Tempo frames must be strictly ordered");
            tempoPrevious = point.frame;
            tempoFirst = false;
        }
        for (const auto &point : state.timeSignatures) {
            if (point.frame >= kMaxMidiFrame)
                throw Error("Time signature point exceeds the project timeline limit");
            if (point.numerator < 1 || point.numerator > 32)
                throw Error("Time signature numerator outside 1…32");
            if (!isTimeSignatureDenominator(point.denominator))
                throw Error("Time signature denominator must be one of 1, 2, 4, 8, 16 or 32");
            if (!signatureFirst && point.frame <= signaturePrevious)
                throw Error("Time signature frames must be strictly ordered");
            signaturePrevious = point.frame;
            signatureFirst = false;
        }
    }
    // Marker locators share the project frame rules but not the anchor: an
    // empty lane is legal, so a fresh or pre-v18 project validates unchanged.
    if (state.markers.size() > kMaxMarkersPerProject)
        throw Error("Too many markers");
    {
        uint64_t markerPrevious = 0;
        bool markerFirst = true;
        for (const auto &marker : state.markers) {
            if (marker.frame >= kMaxMidiFrame)
                throw Error("Marker exceeds the project timeline limit");
            validateName(marker.name);
            if (!markerFirst && marker.frame <= markerPrevious)
                throw Error("Marker frames must be strictly ordered");
            markerPrevious = marker.frame;
            markerFirst = false;
        }
    }
    if (audioCount > 8 || audioAssets > 32 || audioBytes > 64 * 1024 * 1024)
        throw Error("Prototype supports 8 audio tracks, 32 takes and 64 MiB decoded audio");
    if (state.buses.size() > 16)
        throw Error("Project supports at most 16 buses");
    std::set<uint64_t> busIDs;
    for (const auto &bus : state.buses) {
        validateName(bus.name);
        if (bus.id == 0 || bus.id >= state.nextID || !ids.insert(bus.id).second ||
            !busIDs.insert(bus.id).second)
            throw Error("Invalid bus ID");
        if (!std::isfinite(bus.gain) || bus.gain < -120 || bus.gain > 24)
            throw Error("Bus gain outside -120…24 dB");
        if (!std::isfinite(bus.pan) || bus.pan < -1 || bus.pan > 1)
            throw Error("Bus pan outside -1…1");
        validateAutomation(bus.gainAutomation, -120, 24, "Bus gain");
    }
    auto hasBus = [&](uint64_t id) {
        return id == 0 || busIDs.contains(id);
    };
    for (const auto &track : state.tracks) {
        if (!hasBus(track.outputBus))
            throw Error("Track output bus not found");
        if (track.sends.size() > 8)
            throw Error("Track supports at most 8 sends");
        std::set<uint64_t> targets;
        for (const auto &send : track.sends) {
            if (!send.bus || !busIDs.contains(send.bus))
                throw Error("Send bus not found");
            if (!targets.insert(send.bus).second)
                throw Error("Duplicate send target");
            if (!std::isfinite(send.gain) || send.gain < -120 || send.gain > 24)
                throw Error("Send gain outside -120…24 dB");
            if (!std::isfinite(send.pan) || send.pan < -1 || send.pan > 1)
                throw Error("Send balance outside -1…1");
        }
    }
    for (const auto &bus : state.buses)
        if (bus.outputBus == bus.id || !hasBus(bus.outputBus))
            throw Error("Invalid bus output");
    std::vector<uint8_t> colours(state.buses.size());
    auto busIndex = [&](uint64_t id) {
        return static_cast<size_t>(std::find_if(state.buses.begin(), state.buses.end(),
                                                [&](const auto &bus) {
                                                    return bus.id == id;
                                                }) -
                                   state.buses.begin());
    };
    auto visit = [&](auto &&self, size_t index) -> void {
        if (colours[index] == 1)
            throw Error("Bus routing cycle");
        if (colours[index] == 2)
            return;
        colours[index] = 1;
        const auto output = state.buses[index].outputBus;
        if (output)
            self(self, busIndex(output));
        colours[index] = 2;
    };
    for (size_t i = 0; i < state.buses.size(); ++i)
        visit(visit, i);
    size_t pluginStateBytes = 0, pluginCount = 0, pluginAutomationPoints = 0;
    const auto validateInserts = [&](const std::vector<PluginInsert> &inserts, const char *owner) {
        if (inserts.size() > kMaxPluginInsertsPerOwner)
            throw Error(std::string(owner) + " supports at most 4 inserts");
        if (pluginCount > kMaxProjectPluginInserts - inserts.size())
            throw Error("Project supports at most 64 plug-in inserts");
        pluginCount += inserts.size();
        for (const auto &plugin : inserts) {
            validateName(plugin.name);
            if (plugin.id == 0 || plugin.id >= state.nextID || !ids.insert(plugin.id).second)
                throw Error("Invalid plugin ID");
            if (plugin.hostingMode != PluginHostingMode::InProcess &&
                plugin.hostingMode != PluginHostingMode::OutOfProcess)
                throw Error("Invalid plug-in hosting mode");
            const auto vst3 = isVst3PluginInsert(plugin);
            if (!vst3 && (!plugin.type || !plugin.subtype || !plugin.manufacturer))
                throw Error("Invalid Audio Unit component ID");
            if (vst3) {
                std::string envelopeError;
                if (!decodeVst3StateEnvelope(plugin.state, &envelopeError))
                    throw Error("Invalid VST3 state envelope: " + envelopeError);
            }
            if (plugin.latencyFrames > 48000 * 10)
                throw Error("Plugin latency exceeds 10 seconds");
            const auto perPluginLimit =
                vst3 ? kMaxVst3PluginStateBytes : kMaxAudioUnitPluginStateBytes;
            if (plugin.state.size() > perPluginLimit ||
                pluginStateBytes > kMaxProjectPluginStateBytes - plugin.state.size())
                throw Error("Plugin state exceeds project limit");
            pluginStateBytes += plugin.state.size();
            if (plugin.parameterAutomation.size() > kMaxPluginParameterAutomationLanes)
                throw Error("Plug-in supports at most 128 parameter automation lanes");
            std::set<uint32_t> parameterIDs;
            for (const auto &lane : plugin.parameterAutomation) {
                if (!parameterIDs.insert(lane.parameterID).second)
                    throw Error("Duplicate plug-in parameter automation lane");
                if (!lane.name.empty())
                    validateName(lane.name);
                if (lane.points.size() > kMaxPluginParameterAutomationPoints ||
                    pluginAutomationPoints >
                        kMaxProjectPluginParameterAutomationPoints - lane.points.size())
                    throw Error("Plug-in parameter automation exceeds project limit");
                pluginAutomationPoints += lane.points.size();
                uint64_t previousFrame = 0;
                bool first = true;
                for (const auto &point : lane.points) {
                    if (point.frame > 48000 * 600 || !std::isfinite(point.normalizedValue) ||
                        point.normalizedValue < 0 || point.normalizedValue > 1)
                        throw Error("Invalid plug-in parameter automation point");
                    if (!first && point.frame <= previousFrame)
                        throw Error("Plug-in parameter automation frames must be strictly ordered");
                    previousFrame = point.frame;
                    first = false;
                }
            }
        }
    };
    for (const auto &track : state.tracks)
        validateInserts(track.inserts, "Track");
    for (const auto &bus : state.buses)
        validateInserts(bus.inserts, "Bus");
    validateInserts(state.masterInserts, "Master");
}
std::vector<WorkflowOperation>
prepareVocalTracks(const State &state, const std::vector<uint64_t> &selectedTrackIDs,
                   const std::string &namingBase,
                   const std::vector<VocalGainSuggestion> &suggestions) {
    if (selectedTrackIDs.empty() || selectedTrackIDs.size() > 32)
        throw Error("Vocal workflow supports 1–32 selected tracks");
    validateName(namingBase);
    std::set<uint64_t> selected;
    for (const auto id : selectedTrackIDs) {
        if (!selected.insert(id).second)
            throw Error("Vocal workflow selection contains duplicate tracks");
        if (std::find_if(state.tracks.begin(), state.tracks.end(), [&](const auto &track) {
                return track.id == id;
            }) == state.tracks.end())
            throw Error("Vocal workflow track not found");
    }
    if (suggestions.size() > selectedTrackIDs.size())
        throw Error("Too many vocal gain suggestions");
    std::vector<VocalGainSuggestion> normalizedSuggestions;
    normalizedSuggestions.reserve(suggestions.size());
    std::set<uint64_t> suggested;
    for (const auto &suggestion : suggestions) {
        if (!selected.contains(suggestion.trackID))
            throw Error("Vocal gain suggestion track is not selected");
        if (!suggested.insert(suggestion.trackID).second)
            throw Error("Duplicate vocal gain suggestion");
        if (!std::isfinite(suggestion.suggestedGainDb))
            throw Error("Vocal gain suggestion must be finite");
        normalizedSuggestions.push_back(
            {suggestion.trackID, std::clamp(suggestion.suggestedGainDb, -120.0, 24.0)});
    }
    std::vector<WorkflowOperation> batch;
    batch.reserve(selectedTrackIDs.size() + normalizedSuggestions.size());
    for (size_t index = 0; index < selectedTrackIDs.size(); ++index) {
        const auto suffix =
            index == 0 ? std::string(" Lead") : std::string(" Double ") + std::to_string(index);
        const auto name = namingBase + suffix;
        validateName(name);
        batch.push_back({WorkflowOperationKind::RenameTrack, selectedTrackIDs[index], name, 0});
        const auto suggestion = std::find_if(normalizedSuggestions.begin(),
                                             normalizedSuggestions.end(), [&](const auto &item) {
                                                 return item.trackID == selectedTrackIDs[index];
                                             });
        if (suggestion != normalizedSuggestions.end())
            batch.push_back({WorkflowOperationKind::SetTrackGain,
                             selectedTrackIDs[index],
                             {},
                             suggestion->suggestedGainDb});
    }
    return batch;
}
void Session::check(uint64_t expected) const {
    if (mixerGesture)
        throw Error("Mixer gesture is active");
    if (gesture || pluginParameterGesture)
        throw Error("Automation gesture is active");
    if (expected != current.revision)
        throw Error("Revision conflict: refresh the project");
}
void Session::commit(State next) {
    next.revision = current.revision + 1;
    validate(next);
    // Allocate before changing the authoritative state, retaining the strong exception guarantee.
    auto history = past;
    if (history.size() == 128)
        history.erase(history.begin());
    history.push_back(current);
    // Bound unique media retained by undo; snapshots share immutable clips.
    auto retainedBytes = [&] {
        std::set<const Clip *> seen;
        size_t bytes = 0;
        auto count = [&](const State &state) {
            for (const auto &t : state.tracks) {
                if (t.audio && seen.insert(t.audio.get()).second)
                    bytes += t.audio->samples().size() * sizeof(float);
                for (const auto &take : t.takes)
                    if (take.audio && seen.insert(take.audio.get()).second)
                        bytes += take.audio->samples().size() * sizeof(float);
            }
        };
        count(next);
        for (const auto &state : history)
            count(state);
        return bytes;
    };
    while (!history.empty() && retainedBytes() > 128 * 1024 * 1024)
        history.erase(history.begin());
    past.swap(history);
    future.clear();
    current = std::move(next);
}
void Session::add(const std::string &name, uint64_t expected) {
    check(expected);
    State next = current;
    next.tracks.push_back(
        {next.nextID++, name, 0.0, 0u, {}, {}, 0.0, false, false, 0, {}, 0, {}, {}, {}, {}, {}});
    commit(std::move(next));
}
void Session::removeTrack(uint64_t id, uint64_t expected) {
    check(expected);
    State next = current;
    const auto track = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &item) {
        return item.id == id;
    });
    if (track == next.tracks.end())
        throw Error("Track not found");
    // Erasing the owning Track intentionally removes all track-scoped media,
    // routing, sends, automation and inserts in the same commit. Buses and
    // master state have no ownership edge back to a track and remain intact.
    next.tracks.erase(track);
    commit(std::move(next));
}
void Session::moveTrack(uint64_t id, uint32_t newIndex, uint64_t expected) {
    check(expected);
    State next = current;
    const auto track = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &item) {
        return item.id == id;
    });
    if (track == next.tracks.end())
        throw Error("Track not found");
    if (newIndex >= next.tracks.size())
        throw Error("Track index out of range");
    const auto currentIndex = static_cast<uint32_t>(std::distance(next.tracks.begin(), track));
    if (currentIndex == newIndex)
        return;
    // Move the complete Track value so all owned media, routing, automation,
    // inserts and IDs remain exactly intact in the one history commit.
    Track moved = std::move(*track);
    next.tracks.erase(track);
    next.tracks.insert(next.tracks.begin() + newIndex, std::move(moved));
    commit(std::move(next));
}
void Session::import(const std::string &name, std::shared_ptr<const Clip> clip, uint64_t expected) {
    importAt(name, std::move(clip), 0, expected);
}
void Session::importAt(const std::string &name, std::shared_ptr<const Clip> clip, uint64_t start,
                       uint64_t expected) {
    check(expected);
    if (!clip)
        throw Error("Missing clip");
    auto frames = clip->frames();
    State next = current;
    next.tracks.push_back({next.nextID++,
                           name,
                           0.0,
                           0u,
                           std::move(clip),
                           {{start, 0, frames, 0, 0}},
                           0.0,
                           false,
                           false,
                           start,
                           {},
                           0,
                           {},
                           {},
                           {},
                           {},
                           {}});
    commit(std::move(next));
}
void Session::rename(uint64_t id, const std::string &name, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->name == name)
        return;
    it->name = name;
    commit(std::move(next));
}
void Session::gain(uint64_t id, double value, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->gain == value)
        return;
    it->gain = value;
    commit(std::move(next));
}
void Session::pan(uint64_t id, double value, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->pan == value)
        return;
    it->pan = value;
    commit(std::move(next));
}
void Session::mute(uint64_t id, bool value, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->muted == value)
        return;
    it->muted = value;
    commit(std::move(next));
}
void Session::solo(uint64_t id, bool value, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->solo == value)
        return;
    it->solo = value;
    commit(std::move(next));
}
void Session::setTrackMuted(uint64_t id, bool muted, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->muted == muted)
        return;
    it->muted = muted;
    commit(std::move(next));
}
void Session::setTrackSolo(uint64_t id, bool solo, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->solo == solo)
        return;
    it->solo = solo;
    commit(std::move(next));
}
void Session::setTrackColor(uint64_t id, uint32_t color, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->color == color)
        return;
    it->color = color;
    commit(std::move(next));
}
void Session::setTrackGain(uint64_t id, double gainDb, uint64_t expected) {
    check(expected);
    if (!std::isfinite(gainDb) || gainDb < -60.0 || gainDb > 12.0)
        throw Error("Track gain outside -60..12 dB");
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->gain == gainDb)
        return;
    it->gain = gainDb;
    commit(std::move(next));
}
uint64_t Session::duplicateTrack(uint64_t id, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (next.tracks.size() >= 256)
        throw Error("Project supports at most 256 tracks");
    Track copy = *it;
    copy.id = next.nextID++;
    copy.name = it->name + " copy";
    const uint64_t newId = copy.id;
    next.tracks.push_back(std::move(copy));
    commit(std::move(next));
    return newId;
}
void Session::masterGain(double value, uint64_t expected) {
    check(expected);
    if (current.masterGain == value)
        return;
    State next = current;
    next.masterGain = value;
    commit(std::move(next));
}
void Session::addBus(const std::string &name, uint64_t expected) {
    check(expected);
    State next = current;
    next.buses.push_back({next.nextID++, name, 0, 0, false, 0, {}, {}});
    commit(std::move(next));
}
void Session::deleteBus(uint64_t id, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.buses.begin(), next.buses.end(), [id](const auto &bus) {
        return bus.id == id;
    });
    if (it == next.buses.end())
        throw Error("Bus not found");
    // Reroute tracks outputting to this bus to master.
    for (auto &track : next.tracks) {
        if (track.outputBus == id)
            track.outputBus = 0;
        // Remove sends targeting this bus.
        auto newEnd =
            std::remove_if(track.sends.begin(), track.sends.end(), [id](const auto &send) {
                return send.bus == id;
            });
        track.sends.erase(newEnd, track.sends.end());
    }
    // Reroute buses outputting to this bus to master.
    for (auto &bus : next.buses) {
        if (bus.outputBus == id)
            bus.outputBus = 0;
    }
    next.buses.erase(it);
    commit(std::move(next));
}
void Session::renameBus(uint64_t id, const std::string &name, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.buses.begin(), next.buses.end(), [&](const auto &bus) {
        return bus.id == id;
    });
    if (it == next.buses.end())
        throw Error("Bus not found");
    if (it->name == name)
        return;
    it->name = name;
    commit(std::move(next));
}
void Session::busGain(uint64_t id, double value, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.buses.begin(), next.buses.end(), [&](const auto &bus) {
        return bus.id == id;
    });
    if (it == next.buses.end())
        throw Error("Bus not found");
    if (it->gain == value)
        return;
    it->gain = value;
    commit(std::move(next));
}
void Session::busPan(uint64_t id, double value, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.buses.begin(), next.buses.end(), [&](const auto &bus) {
        return bus.id == id;
    });
    if (it == next.buses.end())
        throw Error("Bus not found");
    if (it->pan == value)
        return;
    it->pan = value;
    commit(std::move(next));
}
void Session::busMute(uint64_t id, bool value, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.buses.begin(), next.buses.end(), [&](const auto &bus) {
        return bus.id == id;
    });
    if (it == next.buses.end())
        throw Error("Bus not found");
    if (it->muted == value)
        return;
    it->muted = value;
    commit(std::move(next));
}
void Session::routeTrack(uint64_t trackID, uint64_t busID, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &track) {
        return track.id == trackID;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    if (it->outputBus == busID)
        return;
    it->outputBus = busID;
    commit(std::move(next));
}
void Session::routeBus(uint64_t busID, uint64_t outputBusID, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.buses.begin(), next.buses.end(), [&](const auto &bus) {
        return bus.id == busID;
    });
    if (it == next.buses.end())
        throw Error("Bus not found");
    if (it->outputBus == outputBusID)
        return;
    it->outputBus = outputBusID;
    commit(std::move(next));
}
void Session::upsertSend(uint64_t trackID, uint64_t busID, double value, bool preFader,
                         uint64_t expected) {
    check(expected);
    State next = current;
    auto track = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &item) {
        return item.id == trackID;
    });
    if (track == next.tracks.end())
        throw Error("Track not found");
    auto send = std::find_if(track->sends.begin(), track->sends.end(), [&](const auto &item) {
        return item.bus == busID;
    });
    if (send == track->sends.end())
        track->sends.push_back({busID, value, preFader});
    else {
        if (send->gain == value && send->preFader == preFader)
            return;
        send->gain = value;
        send->preFader = preFader;
    }
    commit(std::move(next));
}
void Session::removeSend(uint64_t trackID, uint64_t busID, uint64_t expected) {
    check(expected);
    State next = current;
    auto track = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &item) {
        return item.id == trackID;
    });
    if (track == next.tracks.end())
        throw Error("Track not found");
    auto send = std::find_if(track->sends.begin(), track->sends.end(), [&](const auto &item) {
        return item.bus == busID;
    });
    if (send == track->sends.end())
        throw Error("Send not found");
    track->sends.erase(send);
    commit(std::move(next));
}
void Session::upsertTrackVolumeAutomation(uint64_t trackID, uint64_t frame, double gainDb,
                                          uint64_t expected) {
    check(expected);
    State next = current;
    auto track = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &item) {
        return item.id == trackID;
    });
    if (track == next.tracks.end())
        throw Error("Track not found");
    auto point = std::lower_bound(track->volumeAutomation.begin(), track->volumeAutomation.end(),
                                  frame, [](const auto &item, uint64_t value) {
                                      return item.frame < value;
                                  });
    if (point != track->volumeAutomation.end() && point->frame == frame) {
        if (point->gainDb == gainDb)
            return;
        point->gainDb = gainDb;
    } else
        track->volumeAutomation.insert(point, {frame, gainDb});
    commit(std::move(next));
}
void Session::removeTrackVolumeAutomation(uint64_t trackID, uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    auto track = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &item) {
        return item.id == trackID;
    });
    if (track == next.tracks.end())
        throw Error("Track not found");
    auto point = std::lower_bound(track->volumeAutomation.begin(), track->volumeAutomation.end(),
                                  frame, [](const auto &item, uint64_t value) {
                                      return item.frame < value;
                                  });
    if (point == track->volumeAutomation.end() || point->frame != frame)
        throw Error("Automation point not found");
    track->volumeAutomation.erase(point);
    commit(std::move(next));
}
void Session::upsertTrackPanAutomation(uint64_t trackID, uint64_t frame, double pan,
                                       uint64_t expected) {
    check(expected);
    State next = current;
    auto track = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &item) {
        return item.id == trackID;
    });
    if (track == next.tracks.end())
        throw Error("Track not found");
    const auto before = track->panAutomation;
    upsertAutomation(track->panAutomation, frame, pan);
    if (track->panAutomation == before)
        return;
    commit(std::move(next));
}
void Session::removeTrackPanAutomation(uint64_t trackID, uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    auto track = std::find_if(next.tracks.begin(), next.tracks.end(), [&](const auto &item) {
        return item.id == trackID;
    });
    if (track == next.tracks.end())
        throw Error("Track not found");
    if (!removeAutomation(track->panAutomation, frame))
        throw Error("Automation point not found");
    commit(std::move(next));
}
void Session::upsertBusGainAutomation(uint64_t busID, uint64_t frame, double gainDb,
                                      uint64_t expected) {
    check(expected);
    State next = current;
    auto bus = std::find_if(next.buses.begin(), next.buses.end(), [&](const auto &item) {
        return item.id == busID;
    });
    if (bus == next.buses.end())
        throw Error("Bus not found");
    const auto before = bus->gainAutomation;
    upsertAutomation(bus->gainAutomation, frame, gainDb);
    if (bus->gainAutomation == before)
        return;
    commit(std::move(next));
}
void Session::removeBusGainAutomation(uint64_t busID, uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    auto bus = std::find_if(next.buses.begin(), next.buses.end(), [&](const auto &item) {
        return item.id == busID;
    });
    if (bus == next.buses.end())
        throw Error("Bus not found");
    if (!removeAutomation(bus->gainAutomation, frame))
        throw Error("Automation point not found");
    commit(std::move(next));
}
void Session::upsertMasterGainAutomation(uint64_t frame, double gainDb, uint64_t expected) {
    check(expected);
    State next = current;
    const auto before = next.masterGainAutomation;
    upsertAutomation(next.masterGainAutomation, frame, gainDb);
    if (next.masterGainAutomation == before)
        return;
    commit(std::move(next));
}
void Session::removeMasterGainAutomation(uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    if (!removeAutomation(next.masterGainAutomation, frame))
        throw Error("Automation point not found");
    commit(std::move(next));
}
void Session::beginAutomationGesture(AutomationTarget target, uint64_t targetID,
                                     AutomationWriteMode mode, uint64_t expected) {
    check(expected);
    if (mode != AutomationWriteMode::Touch && mode != AutomationWriteMode::Latch)
        throw Error("Unsupported automation write mode");
    State working = current;
    // Resolve the target before publishing a gesture, so an invalid track/bus
    // can never leave the session locked in a half-open control operation.
    (void)automationLane(working, target, targetID);
    gesture = AutomationGesture{target, targetID, mode, current.revision, std::move(working)};
}
void Session::writeAutomationGesture(uint64_t frame, double value) {
    if (!gesture)
        throw Error("No active automation gesture");
    if (gesture->hasWritten && frame < gesture->lastFrame)
        throw Error("Automation gesture frames must be ordered");
    validateGestureValue(gesture->target, frame, value);
    State next = gesture->working;
    upsertAutomation(automationLane(next, gesture->target, gesture->targetID), frame, value);
    validate(next);
    gesture->working = std::move(next);
    gesture->hasWritten = true;
    gesture->lastFrame = frame;
    gesture->lastValue = value;
}
void Session::endAutomationGesture(uint64_t endFrame, uint64_t expected) {
    if (!gesture)
        throw Error("No active automation gesture");
    if (expected != gesture->baseRevision || current.revision != gesture->baseRevision)
        throw Error("Revision conflict: refresh the project");
    if (endFrame > 48000 * 600)
        throw Error("Automation point exceeds timeline limit");
    if (gesture->hasWritten && endFrame < gesture->lastFrame)
        throw Error("Automation gesture end precedes its final sample");
    auto finished = std::move(*gesture);
    gesture.reset();
    if (!finished.hasWritten)
        return;
    if (finished.mode == AutomationWriteMode::Latch && endFrame > finished.lastFrame) {
        upsertAutomation(automationLane(finished.working, finished.target, finished.targetID),
                         endFrame, finished.lastValue);
    }
    validate(finished.working);
    commit(std::move(finished.working));
}
void Session::cancelAutomationGesture() noexcept {
    gesture.reset();
}
bool Session::automationGestureActive() const noexcept {
    return gesture.has_value();
}
WorkflowPreview Session::previewWorkflow(const std::vector<WorkflowOperation> &batch,
                                         uint64_t expected) const {
    check(expected);
    State next = current;
    applyWorkflow(next, batch);
    validate(next);
    return {current.revision, current.revision + (next.tracks == current.tracks ? 0 : 1),
            workflowSummary(current), workflowSummary(next)};
}
void Session::commitWorkflow(const std::vector<WorkflowOperation> &batch, uint64_t expected) {
    check(expected);
    State next = current;
    applyWorkflow(next, batch);
    if (next.tracks == current.tracks)
        return;
    commit(std::move(next));
}
void Session::addMasterInsert(PluginInsert plugin, uint64_t expected) {
    check(expected);
    State next = current;
    if (next.masterInserts.size() >= 4)
        throw Error("Master supports at most 4 inserts");
    plugin.id = next.nextID++;
    next.masterInserts.push_back(std::move(plugin));
    commit(std::move(next));
}
void Session::moveMasterInsert(uint64_t id, uint32_t newIndex, uint64_t expected) {
    check(expected);
    State next = current;
    auto current =
        std::find_if(next.masterInserts.begin(), next.masterInserts.end(), [&](const auto &plugin) {
            return plugin.id == id;
        });
    if (current == next.masterInserts.end())
        throw Error("Master insert not found");
    if (newIndex >= next.masterInserts.size())
        throw Error("Master insert index out of range");
    const auto oldIndex = static_cast<uint32_t>(current - next.masterInserts.begin());
    if (oldIndex == newIndex)
        return;
    if (oldIndex < newIndex)
        std::rotate(current, current + 1, next.masterInserts.begin() + newIndex + 1);
    else
        std::rotate(next.masterInserts.begin() + newIndex, current, current + 1);
    commit(std::move(next));
}
void Session::removeMasterInsert(uint64_t id, uint64_t expected) {
    check(expected);
    State next = current;
    auto it =
        std::find_if(next.masterInserts.begin(), next.masterInserts.end(), [&](const auto &plugin) {
            return plugin.id == id;
        });
    if (it == next.masterInserts.end())
        throw Error("Master insert not found");
    next.masterInserts.erase(it);
    commit(std::move(next));
}
void Session::bypassMasterInsert(uint64_t id, bool bypassed, uint64_t expected) {
    check(expected);
    State next = current;
    auto it =
        std::find_if(next.masterInserts.begin(), next.masterInserts.end(), [&](const auto &plugin) {
            return plugin.id == id;
        });
    if (it == next.masterInserts.end())
        throw Error("Master insert not found");
    if (it->bypassed == bypassed)
        return;
    it->bypassed = bypassed;
    commit(std::move(next));
}
void Session::setMasterInsertHostingMode(uint64_t id, PluginHostingMode mode, uint64_t expected) {
    check(expected);
    State next = current;
    auto plugin =
        std::find_if(next.masterInserts.begin(), next.masterInserts.end(), [&](const auto &item) {
            return item.id == id;
        });
    if (plugin == next.masterInserts.end())
        throw Error("Master insert not found");
    if (plugin->hostingMode == mode)
        return;
    plugin->hostingMode = mode;
    validate(next);
    commit(std::move(next));
}
void Session::updateMasterInsertState(uint64_t id, std::vector<uint8_t> state,
                                      uint32_t latencyFrames, uint64_t expected) {
    check(expected);
    State next = current;
    auto it =
        std::find_if(next.masterInserts.begin(), next.masterInserts.end(), [&](const auto &plugin) {
            return plugin.id == id;
        });
    if (it == next.masterInserts.end())
        throw Error("Master insert not found");
    // State updates cannot change the persisted plug-in format.  AU state is
    // left byte-for-byte opaque while VST3 is always a complete MDVS envelope.
    if (isVst3PluginInsert(*it)) {
        std::string envelopeError;
        if (!decodeVst3StateEnvelope(state, &envelopeError))
            throw Error("Invalid VST3 state envelope: " + envelopeError);
    } else if (state.size() >= 4 && state[0] == 'M' && state[1] == 'D' && state[2] == 'V' &&
               state[3] == 'S')
        throw Error("Audio Unit state cannot be replaced with a VST3 envelope");
    if (it->state == state && it->latencyFrames == latencyFrames)
        return;
    it->state = std::move(state);
    it->latencyFrames = latencyFrames;
    commit(std::move(next));
}
namespace {
std::vector<PluginInsert> &trackInserts(State &state, uint64_t trackID) {
    auto track = std::find_if(state.tracks.begin(), state.tracks.end(), [&](const auto &item) {
        return item.id == trackID;
    });
    if (track == state.tracks.end())
        throw Error("Track not found");
    return track->inserts;
}
std::vector<PluginInsert> &busInserts(State &state, uint64_t busID) {
    auto bus = std::find_if(state.buses.begin(), state.buses.end(), [&](const auto &item) {
        return item.id == busID;
    });
    if (bus == state.buses.end())
        throw Error("Bus not found");
    return bus->inserts;
}
std::vector<PluginInsert> &ownerInserts(State &state, PluginOwner owner, uint64_t ownerID) {
    switch (owner) {
    case PluginOwner::Track:
        return trackInserts(state, ownerID);
    case PluginOwner::Bus:
        return busInserts(state, ownerID);
    case PluginOwner::Master:
        if (ownerID)
            throw Error("Master plug-in owner ID must be zero");
        return state.masterInserts;
    }
    throw Error("Unsupported plug-in owner");
}
PluginInsert &ownerPlugin(State &state, PluginOwner owner, uint64_t ownerID, uint64_t pluginID) {
    auto &inserts = ownerInserts(state, owner, ownerID);
    auto plugin = std::find_if(inserts.begin(), inserts.end(), [&](const auto &item) {
        return item.id == pluginID;
    });
    if (plugin == inserts.end())
        throw Error("Plug-in insert not found");
    return *plugin;
}
bool moveInsert(std::vector<PluginInsert> &inserts, uint64_t id, uint32_t newIndex) {
    auto current = std::find_if(inserts.begin(), inserts.end(), [&](const auto &plugin) {
        return plugin.id == id;
    });
    if (current == inserts.end())
        throw Error("Insert not found");
    if (newIndex >= inserts.size())
        throw Error("Insert index out of range");
    const auto oldIndex = static_cast<uint32_t>(current - inserts.begin());
    if (oldIndex == newIndex)
        return false;
    if (oldIndex < newIndex)
        std::rotate(current, current + 1, inserts.begin() + newIndex + 1);
    else
        std::rotate(inserts.begin() + newIndex, current, current + 1);
    return true;
}
bool updateInsertState(std::vector<PluginInsert> &inserts, uint64_t id, std::vector<uint8_t> state,
                       uint32_t latencyFrames) {
    auto it = std::find_if(inserts.begin(), inserts.end(), [&](const auto &plugin) {
        return plugin.id == id;
    });
    if (it == inserts.end())
        throw Error("Insert not found");
    if (isVst3PluginInsert(*it)) {
        std::string envelopeError;
        if (!decodeVst3StateEnvelope(state, &envelopeError))
            throw Error("Invalid VST3 state envelope: " + envelopeError);
    } else if (state.size() >= 4 && state[0] == 'M' && state[1] == 'D' && state[2] == 'V' &&
               state[3] == 'S')
        throw Error("Audio Unit state cannot be replaced with a VST3 envelope");
    if (it->state == state && it->latencyFrames == latencyFrames)
        return false;
    it->state = std::move(state);
    it->latencyFrames = latencyFrames;
    return true;
}
bool bypassInsert(std::vector<PluginInsert> &inserts, uint64_t id, bool bypassed) {
    auto it = std::find_if(inserts.begin(), inserts.end(), [&](const auto &plugin) {
        return plugin.id == id;
    });
    if (it == inserts.end())
        throw Error("Insert not found");
    if (it->bypassed == bypassed)
        return false;
    it->bypassed = bypassed;
    return true;
}
bool setInsertHostingMode(std::vector<PluginInsert> &inserts, uint64_t id, PluginHostingMode mode) {
    auto it = std::find_if(inserts.begin(), inserts.end(), [&](const auto &plugin) {
        return plugin.id == id;
    });
    if (it == inserts.end())
        throw Error("Insert not found");
    if (it->hostingMode == mode)
        return false;
    it->hostingMode = mode;
    return true;
}
}
void Session::addTrackInsert(uint64_t trackID, PluginInsert plugin, uint64_t expected) {
    check(expected);
    State next = current;
    auto &inserts = trackInserts(next, trackID);
    if (inserts.size() >= kMaxPluginInsertsPerOwner)
        throw Error("Track supports at most 4 inserts");
    plugin.id = next.nextID++;
    inserts.push_back(std::move(plugin));
    commit(std::move(next));
}
void Session::moveTrackInsert(uint64_t trackID, uint64_t id, uint32_t newIndex, uint64_t expected) {
    check(expected);
    State next = current;
    if (!moveInsert(trackInserts(next, trackID), id, newIndex))
        return;
    commit(std::move(next));
}
void Session::removeTrackInsert(uint64_t trackID, uint64_t id, uint64_t expected) {
    check(expected);
    State next = current;
    auto &inserts = trackInserts(next, trackID);
    auto it = std::find_if(inserts.begin(), inserts.end(), [&](const auto &plugin) {
        return plugin.id == id;
    });
    if (it == inserts.end())
        throw Error("Track insert not found");
    inserts.erase(it);
    commit(std::move(next));
}
void Session::bypassTrackInsert(uint64_t trackID, uint64_t id, bool bypassed, uint64_t expected) {
    check(expected);
    State next = current;
    if (!bypassInsert(trackInserts(next, trackID), id, bypassed))
        return;
    commit(std::move(next));
}
void Session::setTrackInsertHostingMode(uint64_t trackID, uint64_t id, PluginHostingMode mode,
                                        uint64_t expected) {
    check(expected);
    State next = current;
    if (!setInsertHostingMode(trackInserts(next, trackID), id, mode))
        return;
    validate(next);
    commit(std::move(next));
}
void Session::updateTrackInsertState(uint64_t trackID, uint64_t id, std::vector<uint8_t> state,
                                     uint32_t latencyFrames, uint64_t expected) {
    check(expected);
    State next = current;
    if (!updateInsertState(trackInserts(next, trackID), id, std::move(state), latencyFrames))
        return;
    commit(std::move(next));
}
void Session::addBusInsert(uint64_t busID, PluginInsert plugin, uint64_t expected) {
    check(expected);
    State next = current;
    auto &inserts = busInserts(next, busID);
    if (inserts.size() >= kMaxPluginInsertsPerOwner)
        throw Error("Bus supports at most 4 inserts");
    plugin.id = next.nextID++;
    inserts.push_back(std::move(plugin));
    commit(std::move(next));
}
void Session::moveBusInsert(uint64_t busID, uint64_t id, uint32_t newIndex, uint64_t expected) {
    check(expected);
    State next = current;
    if (!moveInsert(busInserts(next, busID), id, newIndex))
        return;
    commit(std::move(next));
}
void Session::removeBusInsert(uint64_t busID, uint64_t id, uint64_t expected) {
    check(expected);
    State next = current;
    auto &inserts = busInserts(next, busID);
    auto it = std::find_if(inserts.begin(), inserts.end(), [&](const auto &plugin) {
        return plugin.id == id;
    });
    if (it == inserts.end())
        throw Error("Bus insert not found");
    inserts.erase(it);
    commit(std::move(next));
}
void Session::bypassBusInsert(uint64_t busID, uint64_t id, bool bypassed, uint64_t expected) {
    check(expected);
    State next = current;
    if (!bypassInsert(busInserts(next, busID), id, bypassed))
        return;
    commit(std::move(next));
}
void Session::setBusInsertHostingMode(uint64_t busID, uint64_t id, PluginHostingMode mode,
                                      uint64_t expected) {
    check(expected);
    State next = current;
    if (!setInsertHostingMode(busInserts(next, busID), id, mode))
        return;
    validate(next);
    commit(std::move(next));
}
void Session::updateBusInsertState(uint64_t busID, uint64_t id, std::vector<uint8_t> state,
                                   uint32_t latencyFrames, uint64_t expected) {
    check(expected);
    State next = current;
    if (!updateInsertState(busInserts(next, busID), id, std::move(state), latencyFrames))
        return;
    commit(std::move(next));
}
void Session::upsertPluginParameterAutomation(PluginOwner owner, uint64_t ownerID,
                                              uint64_t pluginID, uint32_t parameterID,
                                              const std::string &name, uint64_t frame,
                                              double normalizedValue, uint64_t expected) {
    check(expected);
    if (frame > 48000 * 600 || !std::isfinite(normalizedValue) || normalizedValue < 0 ||
        normalizedValue > 1)
        throw Error("Invalid plug-in parameter automation point");
    if (!name.empty())
        validateName(name);
    State next = current;
    auto &plugin = ownerPlugin(next, owner, ownerID, pluginID);
    auto lane = std::find_if(plugin.parameterAutomation.begin(), plugin.parameterAutomation.end(),
                             [&](const auto &item) {
                                 return item.parameterID == parameterID;
                             });
    bool changed = false;
    if (lane == plugin.parameterAutomation.end()) {
        if (plugin.parameterAutomation.size() >= kMaxPluginParameterAutomationLanes)
            throw Error("Plug-in supports at most 128 parameter automation lanes");
        plugin.parameterAutomation.push_back({parameterID, name, {}});
        lane = std::prev(plugin.parameterAutomation.end());
        changed = true;
    } else if (!name.empty() && lane->name != name) {
        lane->name = name;
        changed = true;
    }
    auto point = std::lower_bound(lane->points.begin(), lane->points.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point != lane->points.end() && point->frame == frame) {
        if (point->normalizedValue != normalizedValue) {
            point->normalizedValue = normalizedValue;
            changed = true;
        }
    } else {
        if (lane->points.size() >= kMaxPluginParameterAutomationPoints)
            throw Error("Plug-in parameter automation lane supports at most 2048 points");
        lane->points.insert(point, {frame, normalizedValue});
        changed = true;
    }
    if (!changed)
        return;
    validate(next);
    commit(std::move(next));
}
void Session::removePluginParameterAutomation(PluginOwner owner, uint64_t ownerID,
                                              uint64_t pluginID, uint32_t parameterID,
                                              uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    auto &plugin = ownerPlugin(next, owner, ownerID, pluginID);
    auto lane = std::find_if(plugin.parameterAutomation.begin(), plugin.parameterAutomation.end(),
                             [&](const auto &item) {
                                 return item.parameterID == parameterID;
                             });
    if (lane == plugin.parameterAutomation.end())
        throw Error("Plug-in parameter automation lane not found");
    auto point = std::lower_bound(lane->points.begin(), lane->points.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point == lane->points.end() || point->frame != frame)
        throw Error("Plug-in parameter automation point not found");
    lane->points.erase(point);
    if (lane->points.empty())
        plugin.parameterAutomation.erase(lane);
    commit(std::move(next));
}
void Session::beginPluginParameterAutomationGesture(PluginOwner owner, uint64_t ownerID,
                                                    uint64_t pluginID, uint32_t parameterID,
                                                    const std::string &name,
                                                    AutomationWriteMode mode, uint64_t expected) {
    check(expected);
    if (mode != AutomationWriteMode::Touch && mode != AutomationWriteMode::Latch)
        throw Error("Unsupported automation write mode");
    if (!name.empty())
        validateName(name);
    State working = current;
    (void)ownerPlugin(working, owner, ownerID, pluginID);
    pluginParameterGesture = PluginParameterAutomationGesture{
        owner, ownerID, pluginID, parameterID, name, mode, current.revision, std::move(working)};
}
void Session::writePluginParameterAutomationGesture(uint64_t frame, double normalizedValue) {
    if (!pluginParameterGesture)
        throw Error("No active plug-in parameter automation gesture");
    auto &gesture = *pluginParameterGesture;
    if (frame > 48000 * 600 || !std::isfinite(normalizedValue) || normalizedValue < 0 ||
        normalizedValue > 1)
        throw Error("Invalid plug-in parameter automation point");
    if (gesture.hasWritten && frame < gesture.lastFrame)
        throw Error("Plug-in parameter automation gesture frames must be ordered");
    State next = gesture.working;
    auto &plugin = ownerPlugin(next, gesture.owner, gesture.ownerID, gesture.pluginID);
    auto lane = std::find_if(plugin.parameterAutomation.begin(), plugin.parameterAutomation.end(),
                             [&](const auto &item) {
                                 return item.parameterID == gesture.parameterID;
                             });
    bool changed = false;
    if (lane == plugin.parameterAutomation.end()) {
        if (plugin.parameterAutomation.size() >= kMaxPluginParameterAutomationLanes)
            throw Error("Plug-in supports at most 128 parameter automation lanes");
        plugin.parameterAutomation.push_back({gesture.parameterID, gesture.name, {}});
        lane = std::prev(plugin.parameterAutomation.end());
        changed = true;
    } else if (!gesture.name.empty() && lane->name != gesture.name) {
        lane->name = gesture.name;
        changed = true;
    }
    auto point = std::lower_bound(lane->points.begin(), lane->points.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point != lane->points.end() && point->frame == frame) {
        if (point->normalizedValue != normalizedValue) {
            point->normalizedValue = normalizedValue;
            changed = true;
        }
    } else {
        if (lane->points.size() >= kMaxPluginParameterAutomationPoints)
            throw Error("Plug-in parameter automation lane supports at most 2048 points");
        lane->points.insert(point, {frame, normalizedValue});
        changed = true;
    }
    if (changed) {
        validate(next);
        gesture.working = std::move(next);
        gesture.changed = true;
    }
    gesture.hasWritten = true;
    gesture.lastFrame = frame;
    gesture.lastValue = normalizedValue;
}
void Session::endPluginParameterAutomationGesture(uint64_t endFrame, uint64_t expected) {
    if (!pluginParameterGesture)
        throw Error("No active plug-in parameter automation gesture");
    if (expected != pluginParameterGesture->baseRevision ||
        current.revision != pluginParameterGesture->baseRevision)
        throw Error("Revision conflict: refresh the project");
    if (endFrame > 48000 * 600)
        throw Error("Automation point exceeds timeline limit");
    if (pluginParameterGesture->hasWritten && endFrame < pluginParameterGesture->lastFrame)
        throw Error("Automation gesture end precedes its final sample");
    auto finished = std::move(*pluginParameterGesture);
    pluginParameterGesture.reset();
    if (!finished.hasWritten)
        return;
    if (finished.mode == AutomationWriteMode::Latch && endFrame > finished.lastFrame) {
        auto &plugin =
            ownerPlugin(finished.working, finished.owner, finished.ownerID, finished.pluginID);
        auto lane = std::find_if(plugin.parameterAutomation.begin(),
                                 plugin.parameterAutomation.end(), [&](const auto &item) {
                                     return item.parameterID == finished.parameterID;
                                 });
        if (lane == plugin.parameterAutomation.end())
            throw Error("Plug-in parameter automation lane not found");
        auto point = std::lower_bound(lane->points.begin(), lane->points.end(), endFrame,
                                      [](const auto &item, uint64_t target) {
                                          return item.frame < target;
                                      });
        if (point == lane->points.end() || point->frame != endFrame) {
            if (lane->points.size() >= kMaxPluginParameterAutomationPoints)
                throw Error("Plug-in parameter automation lane supports at most 2048 points");
            lane->points.insert(point, {endFrame, finished.lastValue});
            finished.changed = true;
        } else if (point->normalizedValue != finished.lastValue) {
            point->normalizedValue = finished.lastValue;
            finished.changed = true;
        }
    }
    if (!finished.changed)
        return;
    validate(finished.working);
    commit(std::move(finished.working));
}
void Session::cancelPluginParameterAutomationGesture() noexcept {
    pluginParameterGesture.reset();
}
bool Session::pluginParameterAutomationGestureActive() const noexcept {
    return pluginParameterGesture.has_value();
}
void Session::editClip(uint64_t id, uint32_t index, uint64_t start, uint64_t offset,
                       uint64_t length, uint64_t expected) {
    const auto track =
        std::find_if(current.tracks.begin(), current.tracks.end(), [id](const auto &t) {
            return t.id == id;
        });
    if (track == current.tracks.end() || index >= track->regions.size())
        throw Error("Audio clip not found");
    const auto fadeIn = std::min(track->regions[index].fadeIn, length);
    const auto fadeOut = std::min(track->regions[index].fadeOut, length - fadeIn);
    editClipFull(id, index, start, offset, length, fadeIn, fadeOut, expected);
}
void Session::editClipFull(uint64_t id, uint32_t index, uint64_t start, uint64_t offset,
                           uint64_t length, uint64_t fadeIn, uint64_t fadeOut, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || index >= it->regions.size())
        throw Error("Audio clip not found");
    auto &region = it->regions[index];
    const auto carried =
        Region{start,       offset,       length,       fadeIn,        fadeOut,   region.take,
               region.gain, region.color, region.muted, region.looped, region.pan};
    if (region == carried)
        return;
    region = carried;
    std::stable_sort(it->regions.begin(), it->regions.end(), [](const auto &a, const auto &b) {
        return a.start < b.start;
    });
    commit(std::move(next));
}
void Session::splitClip(uint64_t id, uint32_t index, uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || index >= it->regions.size())
        throw Error("Audio clip not found");
    const auto original = it->regions[index];
    if (frame <= original.start || frame >= original.start + original.length)
        throw Error("Split position must be inside the clip");
    if (original.looped)
        throw Error("Unloop the clip before splitting");
    const auto left = frame - original.start;
    it->regions[index] = {original.start,
                          original.sourceOffset,
                          left,
                          std::min(original.fadeIn, left),
                          0,
                          original.take,
                          original.gain,
                          original.color,
                          original.muted,
                          original.looped,
                          original.pan};
    it->regions.insert(it->regions.begin() + index + 1,
                       {frame, original.sourceOffset + left, original.length - left, 0,
                        std::min(original.fadeOut, original.length - left), original.take,
                        original.gain, original.color, original.muted, original.looped,
                        original.pan});
    commit(std::move(next));
}
void Session::duplicateClip(uint64_t id, uint32_t index, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || index >= it->regions.size())
        throw Error("Audio clip not found");
    if (it->regions.size() >= 256)
        throw Error("Track supports at most 256 clips");
    auto copy = it->regions[index];
    uint64_t end = 0;
    for (const auto &region : it->regions)
        end = std::max(end, region.start + region.length);
    copy.start = end;
    if (copy.length > 48000 * 600 - copy.start)
        throw Error("No timeline space for duplicate");
    it->regions.push_back(copy);
    commit(std::move(next));
}
void Session::copyClipToTrack(uint64_t sourceTrack, uint32_t index, uint64_t targetTrack,
                              uint64_t start, uint64_t expected) {
    check(expected);
    const auto source = std::find_if(current.tracks.begin(), current.tracks.end(),
                                     [sourceTrack](const Track &track) {
                                         return track.id == sourceTrack;
                                     });
    if (source == current.tracks.end() || index >= source->regions.size())
        throw Error("Audio clip not found");
    transferClips(sourceTrack, {index}, false, targetTrack, start, true, expected);
}
void Session::moveClipToTrack(uint64_t sourceTrack, uint32_t index, uint64_t targetTrack,
                              uint64_t start, uint64_t expected) {
    check(expected);
    const auto source = std::find_if(current.tracks.begin(), current.tracks.end(),
                                     [sourceTrack](const Track &track) {
                                         return track.id == sourceTrack;
                                     });
    if (source == current.tracks.end() || index >= source->regions.size())
        throw Error("Audio clip not found");
    transferClips(sourceTrack, {index}, false, targetTrack, start, false, expected);
}
void Session::copyMidiClipToTrack(uint64_t sourceTrack, uint32_t index, uint64_t targetTrack,
                                  uint64_t start, uint64_t expected) {
    check(expected);
    State next = current;
    auto src = std::find_if(next.tracks.begin(), next.tracks.end(), [sourceTrack](const auto &t) {
        return t.id == sourceTrack;
    });
    if (src == next.tracks.end() || index >= src->midiClips.size())
        throw Error("MIDI clip not found");
    auto dst = std::find_if(next.tracks.begin(), next.tracks.end(), [targetTrack](const auto &t) {
        return t.id == targetTrack;
    });
    if (dst == next.tracks.end())
        throw Error("Track not found");
    auto copy = src->midiClips[index];
    copy.start = start;
    dst->midiClips.push_back(std::move(copy));
    commit(std::move(next));
}
void Session::moveMidiClipToTrack(uint64_t sourceTrack, uint32_t index, uint64_t targetTrack,
                                  uint64_t start, uint64_t expected) {
    check(expected);
    State next = current;
    auto src = std::find_if(next.tracks.begin(), next.tracks.end(), [sourceTrack](const auto &t) {
        return t.id == sourceTrack;
    });
    if (src == next.tracks.end() || index >= src->midiClips.size())
        throw Error("MIDI clip not found");
    auto dst = std::find_if(next.tracks.begin(), next.tracks.end(), [targetTrack](const auto &t) {
        return t.id == targetTrack;
    });
    if (dst == next.tracks.end())
        throw Error("Track not found");
    auto copy = src->midiClips[index];
    copy.start = start;
    src->midiClips.erase(src->midiClips.begin() + index);
    dst->midiClips.push_back(std::move(copy));
    commit(std::move(next));
}
void Session::deleteClip(uint64_t id, uint32_t index, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || index >= it->regions.size())
        throw Error("Audio clip not found");
    it->regions.erase(it->regions.begin() + index);
    commit(std::move(next));
}
void Session::setClipFades(uint64_t id, uint32_t index, uint64_t fadeIn, uint64_t fadeOut,
                           uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || index >= it->regions.size())
        throw Error("Audio clip not found");
    auto &region = it->regions[index];
    if (region.fadeIn == fadeIn && region.fadeOut == fadeOut)
        return;
    region.fadeIn = fadeIn;
    region.fadeOut = fadeOut;
    commit(std::move(next));
}
void Session::setCrossfade(uint64_t id, uint32_t leftIndex, uint64_t duration, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || leftIndex + 1 >= it->regions.size())
        throw Error("Crossfade requires two adjacent clips");
    auto &left = it->regions[leftIndex];
    auto &right = it->regions[leftIndex + 1];
    const auto leftEnd = left.start + left.length;
    const auto oldOverlap = leftEnd > right.start ? leftEnd - right.start : 0;
    if (oldOverlap && (left.fadeOut != oldOverlap || right.fadeIn != oldOverlap))
        throw Error("Existing overlap is not a crossfade");
    if (oldOverlap == duration)
        return;
    if (duration == 1)
        throw Error("Crossfade requires at least two frames");
    if (oldOverlap) {
        right.start += oldOverlap;
        right.sourceOffset += oldOverlap;
        right.length -= oldOverlap;
        left.fadeOut = 0;
        right.fadeIn = 0;
    }
    if (left.start + left.length != right.start)
        throw Error("Crossfade clips must touch");
    if (duration) {
        if (duration >= left.length || duration >= right.length || right.sourceOffset < duration)
            throw Error("Not enough audio handle for crossfade");
        if (left.fadeIn + duration > left.length ||
            right.fadeOut + duration > right.length + duration)
            throw Error("Crossfade exceeds existing fades");
        right.start -= duration;
        right.sourceOffset -= duration;
        right.length += duration;
        left.fadeOut = duration;
        right.fadeIn = duration;
    }
    commit(std::move(next));
}
namespace {
struct TrackScope {
    std::vector<Track>::iterator track;
};
TrackScope findMidiTrack(State &next, uint64_t trackID, uint32_t index) {
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [trackID](const auto &t) {
        return t.id == trackID;
    });
    if (it == next.tracks.end() || index >= it->midiClips.size())
        throw Error("MIDI clip not found");
    return {it};
}
}
void Session::addMidiClip(uint64_t trackID, MidiClip clip, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [trackID](const auto &t) {
        return t.id == trackID;
    });
    if (it == next.tracks.end())
        throw Error("Track not found");
    it->midiClips.push_back(std::move(clip));
    commit(std::move(next));
}
void Session::removeMidiClip(uint64_t trackID, uint32_t index, uint64_t expected) {
    check(expected);
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    scope.track->midiClips.erase(scope.track->midiClips.begin() + index);
    commit(std::move(next));
}
void Session::setMidiNotes(uint64_t trackID, uint32_t index, std::vector<MidiNote> notes,
                           uint64_t expected) {
    check(expected);
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    auto &clip = scope.track->midiClips[index];
    if (clip.notes == notes)
        return;
    clip.notes = std::move(notes);
    commit(std::move(next));
}
void Session::appendMidiNotes(uint64_t trackID, uint32_t index, const std::vector<MidiNote> &batch,
                              uint64_t expected) {
    check(expected);
    if (batch.empty())
        return;
    // Refuse an oversized batch before the whole State is copied. validate()
    // still owns the project-wide note budget for every accepted append.
    if (batch.size() > kMaxMidiNotesPerProject)
        throw Error("A single MIDI append supports at most 65536 notes");
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    // The clip window never grows and capture order is preserved verbatim: a
    // note that does not fit whole inside the clip fails validate(), and an
    // append that pushes the project over its note budget fails there too,
    // before the authoritative state or its revision changes.
    auto &notes = scope.track->midiClips[index].notes;
    notes.insert(notes.end(), batch.begin(), batch.end());
    commit(std::move(next));
}
void Session::moveMidiClip(uint64_t trackID, uint32_t index, uint64_t newStart, uint64_t expected) {
    check(expected);
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    auto &clip = scope.track->midiClips[index];
    if (clip.start == newStart)
        return;
    clip.start = newStart;
    commit(std::move(next));
}
void Session::trimMidiClip(uint64_t trackID, uint32_t index, uint64_t newStart, uint64_t newLength,
                           uint64_t expected) {
    check(expected);
    if (!newLength || newStart > kMaxMidiFrame || newLength > kMaxMidiFrame)
        throw Error("MIDI clip bounds exceed the project timeline limit");
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    auto &clip = scope.track->midiClips[index];
    const auto oldStart = clip.start;
    if (oldStart == newStart && clip.length == newLength)
        return;
    // Notes are clip-relative: growing the window keeps every note but shifts
    // its start; shrinking deletes only notes that no longer fit whole.
    std::vector<MidiNote> kept;
    kept.reserve(clip.notes.size());
    for (const auto &note : clip.notes) {
        const auto absolute = oldStart + note.start, absoluteEnd = absolute + note.length;
        if (absolute < newStart || absoluteEnd > newStart + newLength)
            continue;
        auto moved = note;
        moved.start = absolute - newStart;
        kept.push_back(moved);
    }
    clip = {newStart, newLength, std::move(kept), clip.track, clip.color};
    commit(std::move(next));
}
void Session::splitMidiClip(uint64_t trackID, uint32_t index, uint64_t atFrame, uint64_t expected) {
    check(expected);
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    const auto original = scope.track->midiClips[index];
    if (atFrame <= original.start || atFrame >= original.start + original.length)
        throw Error("Split position must be inside the clip");
    const auto leftLength = atFrame - original.start,
               rightLength = original.start + original.length - atFrame;
    std::vector<MidiNote> left, right;
    left.reserve(original.notes.size());
    right.reserve(original.notes.size());
    for (const auto &note : original.notes) {
        if (note.start + note.length <= leftLength)
            left.push_back(note);
        else if (note.start >= leftLength) {
            auto moved = note;
            moved.start -= leftLength;
            right.push_back(moved);
        } else {
            // Scissors produce two positive-length notes. The right part
            // retriggers at the cut; Undo restores the original sustained note.
            auto leftPart = note, rightPart = note;
            leftPart.length = leftLength - note.start;
            rightPart.start = 0;
            rightPart.length = note.start + note.length - leftLength;
            left.push_back(leftPart);
            right.push_back(rightPart);
        }
    }
    scope.track->midiClips[index] = {original.start, leftLength, std::move(left), original.track,
                                     original.color};
    scope.track->midiClips.insert(
        scope.track->midiClips.begin() + index + 1,
        {atFrame, rightLength, std::move(right), original.track, original.color});
    commit(std::move(next));
}
void Session::setClipColor(uint64_t id, uint32_t clipIndex, uint32_t color, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || clipIndex >= it->regions.size())
        throw Error("Audio clip not found");
    auto &region = it->regions[clipIndex];
    if (region.color == color)
        return;
    region.color = color;
    commit(std::move(next));
}
void Session::setClipGain(uint64_t id, uint32_t clipIndex, double gainDb, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || clipIndex >= it->regions.size())
        throw Error("Audio clip not found");
    if (!std::isfinite(gainDb) || gainDb < -60.0 || gainDb > 12.0)
        throw Error("Clip gain outside -60..12 dB");
    auto &region = it->regions[clipIndex];
    if (region.gain == gainDb)
        return;
    region.gain = gainDb;
    commit(std::move(next));
}
void Session::setClipMuted(uint64_t id, uint32_t clipIndex, bool muted, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || clipIndex >= it->regions.size())
        throw Error("Audio clip not found");
    auto &region = it->regions[clipIndex];
    if (region.muted == muted)
        return;
    region.muted = muted;
    commit(std::move(next));
}
void Session::setClipLooped(uint64_t id, uint32_t clipIndex, bool looped, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || clipIndex >= it->regions.size())
        throw Error("Audio clip not found");
    auto &region = it->regions[clipIndex];
    if (region.looped == looped)
        return;
    region.looped = looped;
    commit(std::move(next));
}
void Session::setClipPan(uint64_t id, uint32_t clipIndex, double pan, uint64_t expected) {
    check(expected);
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio || clipIndex >= it->regions.size())
        throw Error("Audio clip not found");
    if (!std::isfinite(pan) || pan < -1.0 || pan > 1.0)
        throw Error("Clip pan outside -1..1");
    auto &region = it->regions[clipIndex];
    if (region.pan == pan)
        return;
    region.pan = pan;
    commit(std::move(next));
}
void Session::deleteClips(uint64_t id, std::vector<uint32_t> indices, uint64_t expected) {
    check(expected);
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty())
        return;
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio)
        throw Error("Audio clip not found");
    for (const auto index : indices)
        if (index >= it->regions.size())
            throw Error("Audio clip not found");
    auto regions = std::move(it->regions);
    std::vector<Region> kept;
    for (size_t position = 0; position < regions.size(); ++position)
        if (!std::binary_search(indices.begin(), indices.end(), static_cast<uint32_t>(position)))
            kept.push_back(std::move(regions[position]));
    it->regions = std::move(kept);
    commit(std::move(next));
}
void Session::nudgeClips(uint64_t id, std::vector<uint32_t> indices, int64_t deltaFrames,
                         uint64_t expected) {
    check(expected);
    if (indices.empty() || deltaFrames == 0)
        return;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio)
        throw Error("Audio clip not found");
    for (const auto index : indices)
        if (index >= it->regions.size())
            throw Error("Audio clip not found");
    for (const auto index : indices) {
        const auto &region = it->regions[index];
        const auto target = static_cast<int64_t>(region.start) + deltaFrames;
        if (target < 0 ||
            target > static_cast<int64_t>(48000ull * 600) - static_cast<int64_t>(region.length))
            throw Error("No timeline space for the clip");
    }
    for (const auto index : indices)
        it->regions[index].start =
            static_cast<uint64_t>(static_cast<int64_t>(it->regions[index].start) + deltaFrames);
    std::stable_sort(it->regions.begin(), it->regions.end(), [](const auto &a, const auto &b) {
        return a.start < b.start;
    });
    commit(std::move(next));
}
void Session::setMidiClipColor(uint64_t trackID, uint32_t index, uint32_t color,
                               uint64_t expected) {
    check(expected);
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    auto &clip = scope.track->midiClips[index];
    if (clip.color == color)
        return;
    clip.color = color;
    commit(std::move(next));
}
void Session::transposeMidiClip(uint64_t trackID, uint32_t index, int8_t semitones,
                                uint64_t expected) {
    check(expected);
    if (semitones == 0)
        return;
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    auto &clip = scope.track->midiClips[index];
    bool changed = false;
    for (auto &note : clip.notes) {
        const int v = static_cast<int>(note.pitch) + semitones;
        const uint8_t p = v < 0 ? 0 : (v > 127 ? 127 : static_cast<uint8_t>(v));
        if (p != note.pitch)
            changed = true;
        note.pitch = p;
    }
    if (!changed)
        return;
    commit(std::move(next));
}
void Session::quantizeMidiClip(uint64_t trackID, uint32_t index, double gridBeats,
                               uint64_t expected) {
    check(expected);
    if (gridBeats <= 0)
        return;
    State next = current;
    auto scope = findMidiTrack(next, trackID, index);
    auto &clip = scope.track->midiClips[index];
    bool changed = false;
    for (auto &note : clip.notes) {
        const double beats = next.beatsAtFrame(note.start);
        const double snapped = std::round(beats / gridBeats) * gridBeats;
        const uint64_t frame = next.frameAtBeats(snapped);
        if (frame != note.start)
            changed = true;
        note.start = frame;
    }
    if (!changed)
        return;
    commit(std::move(next));
}
namespace {
// Frames per beat at a tempo: 60/bpm seconds at the fixed 48 kHz project rate.
constexpr long double framesPerBeat(double bpm) noexcept {
    return 2880000.0L / static_cast<long double>(bpm);
}
// Upsert helpers mirror upsertAutomation: given a sorted lane they keep it
// sorted by inserting at the lower_bound slot.
bool upsertTempo(std::vector<TempoPoint> &points, uint64_t frame, double bpm) {
    auto point = std::lower_bound(points.begin(), points.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point != points.end() && point->frame == frame) {
        if (point->bpm == bpm)
            return false;
        point->bpm = bpm;
        return true;
    }
    points.insert(point, {frame, bpm});
    return true;
}
bool upsertTimeSignature(std::vector<TimeSignaturePoint> &points, uint64_t frame, uint8_t numerator,
                         uint8_t denominator) {
    auto point = std::lower_bound(points.begin(), points.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point != points.end() && point->frame == frame) {
        if (point->numerator == numerator && point->denominator == denominator)
            return false;
        point->numerator = numerator;
        point->denominator = denominator;
        return true;
    }
    points.insert(point, {frame, numerator, denominator});
    return true;
}
}
double State::bpmAtFrame(uint64_t frame) const {
    if (tempo.empty())
        return kDefaultTempoBpm; // defensive; validated states start at frame 0
    const auto point = std::upper_bound(tempo.begin(), tempo.end(), frame,
                                        [](uint64_t target, const TempoPoint &item) {
                                            return target < item.frame;
                                        });
    if (point == tempo.begin())
        return tempo.front().bpm;
    return std::prev(point)->bpm;
}
double State::beatsAtFrame(uint64_t frame) const {
    static const std::vector<TempoPoint> fallback{{0, kDefaultTempoBpm}};
    const auto &lane = tempo.empty() ? fallback : tempo;
    long double beats = 0;
    size_t index = 0;
    for (; index + 1 < lane.size() && lane[index + 1].frame <= frame; ++index)
        beats += static_cast<long double>(lane[index + 1].frame - lane[index].frame) *
                 lane[index].bpm / 2880000.0L;
    beats += static_cast<long double>(frame - lane[index].frame) * lane[index].bpm / 2880000.0L;
    return static_cast<double>(beats);
}
uint64_t State::frameAtBeats(double beats) const {
    if (!std::isfinite(beats) || beats < 0)
        throw Error("Beat position must be finite and non-negative");
    static const std::vector<TempoPoint> fallback{{0, kDefaultTempoBpm}};
    const auto &lane = tempo.empty() ? fallback : tempo;
    long double remaining = static_cast<long double>(beats);
    for (size_t index = 0;; ++index) {
        const auto perBeat = framesPerBeat(lane[index].bpm);
        if (index + 1 < lane.size()) {
            const auto segment =
                static_cast<long double>(lane[index + 1].frame - lane[index].frame) / perBeat;
            if (remaining == segment)
                return lane[index + 1].frame; // an exact boundary lands on the next point
            if (remaining > segment) {
                remaining -= segment;
                continue;
            }
        }
        const auto frame = static_cast<long double>(lane[index].frame) + remaining * perBeat;
        if (frame >= static_cast<long double>(kMaxMidiFrame))
            throw Error("Beat position exceeds the project timeline limit");
        return static_cast<uint64_t>(std::llroundl(frame));
    }
}
void Session::setTempoAt(uint64_t frame, double bpm, uint64_t expected) {
    check(expected);
    if (frame >= kMaxMidiFrame)
        throw Error("Tempo point exceeds the project timeline limit");
    if (!std::isfinite(bpm) || bpm <= kMinTempoBpm || bpm > kMaxTempoBpm)
        throw Error("Tempo outside 20…999 BPM");
    State next = current;
    if (!upsertTempo(next.tempo, frame, bpm))
        return;
    commit(std::move(next));
}
void Session::removeTempo(uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    auto point = std::lower_bound(next.tempo.begin(), next.tempo.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point == next.tempo.end() || point->frame != frame)
        throw Error("Tempo point not found");
    if (point == next.tempo.begin())
        throw Error("The first tempo point cannot be removed");
    next.tempo.erase(point);
    commit(std::move(next));
}
void Session::setTimeSignatureAt(uint64_t frame, uint8_t numerator, uint8_t denominator,
                                 uint64_t expected) {
    check(expected);
    if (frame >= kMaxMidiFrame)
        throw Error("Time signature point exceeds the project timeline limit");
    if (numerator < 1 || numerator > 32)
        throw Error("Time signature numerator outside 1…32");
    if (!isTimeSignatureDenominator(denominator))
        throw Error("Time signature denominator must be one of 1, 2, 4, 8, 16 or 32");
    State next = current;
    if (!upsertTimeSignature(next.timeSignatures, frame, numerator, denominator))
        return;
    commit(std::move(next));
}
void Session::removeTimeSignature(uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    auto point = std::lower_bound(next.timeSignatures.begin(), next.timeSignatures.end(), frame,
                                  [](const auto &item, uint64_t target) {
                                      return item.frame < target;
                                  });
    if (point == next.timeSignatures.end() || point->frame != frame)
        throw Error("Time signature point not found");
    if (point == next.timeSignatures.begin())
        throw Error("The first time signature point cannot be removed");
    next.timeSignatures.erase(point);
    commit(std::move(next));
}
namespace {
// Marker lane lookup mirrors the tempo upsert helpers: on a sorted lane
// lower_bound yields both the exact match and the sorted insertion slot.
std::vector<Marker>::iterator markerLanePoint(State &state, uint64_t frame) {
    return std::lower_bound(state.markers.begin(), state.markers.end(), frame,
                            [](const auto &marker, uint64_t target) {
                                return marker.frame < target;
                            });
}
}
void Session::addMarker(uint64_t frame, const std::string &name, uint64_t expected) {
    check(expected);
    if (frame >= kMaxMidiFrame)
        throw Error("Marker exceeds the project timeline limit");
    validateName(name);
    State next = current;
    const auto point = markerLanePoint(next, frame);
    if (point != next.markers.end() && point->frame == frame)
        throw Error("A marker already exists at this position");
    // Refuse the 257th locator before copying the whole State; validate()
    // still owns the same cap for every other entry path into the lane.
    if (next.markers.size() >= kMaxMarkersPerProject)
        throw Error("Too many markers");
    next.markers.insert(point, {frame, name});
    commit(std::move(next));
}
void Session::removeMarker(uint64_t frame, uint64_t expected) {
    check(expected);
    State next = current;
    const auto point = markerLanePoint(next, frame);
    if (point == next.markers.end() || point->frame != frame)
        throw Error("Marker not found");
    next.markers.erase(point);
    commit(std::move(next));
}
void Session::renameMarker(uint64_t frame, const std::string &name, uint64_t expected) {
    check(expected);
    validateName(name);
    State next = current;
    const auto point = markerLanePoint(next, frame);
    if (point == next.markers.end() || point->frame != frame)
        throw Error("Marker not found");
    if (point->name == name)
        return;
    point->name = name;
    commit(std::move(next));
}
void Session::addTake(uint64_t id, const std::string &name, std::shared_ptr<const Clip> clip,
                      uint64_t start, uint64_t expected) {
    std::vector<Take> additions;
    additions.push_back({name, start, std::move(clip)});
    addTakes(id, std::move(additions), expected);
}
void Session::addTakes(uint64_t id, std::vector<Take> additions, uint64_t expected) {
    check(expected);
    if (additions.empty())
        throw Error("No takes to add");
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio)
        throw Error("Audio track not found");
    if (additions.size() > 15 - it->takes.size())
        throw Error("Track supports at most 16 takes");
    for (auto &take : additions) {
        validateName(take.name);
        if (!take.audio)
            throw Error("Missing take audio");
        it->takes.push_back(std::move(take));
    }
    commit(std::move(next));
}
void Session::compRange(uint64_t id, uint32_t takeIndex, uint64_t start, uint64_t length,
                        uint64_t expected) {
    check(expected);
    if (!length || start > 48000 * 600 || length > 48000 * 600 - start)
        throw Error("Invalid comp range");
    State next = current;
    auto it = std::find_if(next.tracks.begin(), next.tracks.end(), [id](const auto &t) {
        return t.id == id;
    });
    if (it == next.tracks.end() || !it->audio)
        throw Error("Audio track not found");
    if (takeIndex > it->takes.size())
        throw Error("Take index out of range");
    const auto source = takeIndex == 0 ? it->audio : it->takes[takeIndex - 1].audio;
    const auto takeStart = takeIndex == 0 ? it->baseStart : it->takes[takeIndex - 1].start;
    if (!source || start < takeStart || start - takeStart >= source->frames() ||
        length > source->frames() - (start - takeStart))
        throw Error("Comp range is outside the selected take");
    for (size_t i = 1; i < it->regions.size(); ++i)
        if (it->regions[i].start < it->regions[i - 1].start + it->regions[i - 1].length)
            throw Error("Remove active crossfades before changing the comp");
    const auto end = start + length;
    std::vector<Region> regions;
    regions.reserve(it->regions.size() + 2);
    for (const auto &region : it->regions) {
        const auto regionEnd = region.start + region.length;
        if (regionEnd <= start || region.start >= end) {
            regions.push_back(region);
            continue;
        }
        if (region.start < start) {
            auto left = region;
            left.length = start - region.start;
            left.fadeOut = 0;
            left.fadeIn = std::min(left.fadeIn, left.length);
            regions.push_back(left);
        }
        if (regionEnd > end) {
            auto right = region;
            const auto removed = end - region.start;
            right.start = end;
            right.sourceOffset += removed;
            right.length = regionEnd - end;
            right.fadeIn = 0;
            right.fadeOut = std::min(right.fadeOut, right.length);
            regions.push_back(right);
        }
    }
    regions.push_back({start, start - takeStart, length, 0, 0, takeIndex});
    std::stable_sort(regions.begin(), regions.end(), [](const auto &a, const auto &b) {
        return a.start < b.start;
    });
    if (regions == it->regions)
        return;
    it->regions = std::move(regions);
    commit(std::move(next));
}
void Session::undo(uint64_t expected) {
    check(expected);
    if (past.empty())
        throw Error("Nothing to undo");
    State next = past.back();
    next.revision = current.revision + 1;
    next.nextID = current.nextID;
    validate(next);
    future.push_back(current);
    past.pop_back();
    current = std::move(next);
}
void Session::redo(uint64_t expected) {
    check(expected);
    if (future.empty())
        throw Error("Nothing to redo");
    State next = future.back();
    next.revision = current.revision + 1;
    next.nextID = current.nextID;
    validate(next);
    past.push_back(current);
    future.pop_back();
    current = std::move(next);
}
void Session::replace(State state) {
    if (mixerGesture)
        throw Error("Mixer gesture is active");
    if (gesture || pluginParameterGesture)
        throw Error("Automation gesture is active");
    validate(state);
    current = std::move(state);
    past.clear();
    future.clear();
}
}
