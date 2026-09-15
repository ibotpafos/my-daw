#include "export/dawproject_model.hpp"
#include "plugins/plugin_descriptor.hpp"
#include <cmath>
#include <set>

namespace daw::dawproject {
namespace {
std::string decimalID(uint64_t id) { return std::to_string(id); }
bool validDenominator(uint32_t value) { return value && value <= 32 && (value & (value - 1)) == 0; }
void validateOptions(const ExportOptions& options) {
    if (!std::isfinite(options.tempoBpm) || options.tempoBpm < 20 || options.tempoBpm > 400)
        throw Error("Export tempo must be between 20 and 400 BPM");
    if (!options.timeSignatureNumerator || options.timeSignatureNumerator > 32 ||
        !validDenominator(options.timeSignatureDenominator))
        throw Error("Invalid export time signature");
}
std::string mediaID(uint64_t trackID, uint32_t takeIndex) {
    return decimalID(trackID) + "." + std::to_string(takeIndex);
}
}

bool LossReport::hasErrors() const {
    for (const auto& item : items) if (item.severity == LossSeverity::Error) return true;
    return false;
}

ExportModel makeExportModel(const State& state, const ExportOptions& options) {
    validate(state);
    validateOptions(options);
    LossReport losses;
    const auto exportParameterAutomation=[](const std::vector<PluginParameterAutomationLane>& source){std::vector<ExportPluginParameterAutomationLane> result;result.reserve(source.size());for(const auto& lane:source)result.push_back({lane.parameterID,lane.name,lane.points});return result;};
    const auto exportPlugins=[&](const std::vector<PluginInsert>& source,const std::string& ownerPath){
        std::vector<ExportMasterPlugin> result;result.reserve(source.size());
        for(const auto& plugin:source){const auto id=decimalID(plugin.id);const auto objectID=ownerPath+"-"+id;if(isVst3PluginInsert(plugin)){std::string envelopeError;const auto envelope=decodeVst3StateEnvelope(plugin.state,&envelopeError);if(!envelope)throw Error("Invalid VST3 state envelope: "+envelopeError);result.push_back({id,0,0,0,plugin.name,plugin.bypassed,plugin.latencyFrames,"plugins/"+objectID+".vstpreset",encodeVst3PresetFile(*envelope),true,envelope->descriptor.vst3ClassFuid,envelope->descriptor.vendor,exportParameterAutomation(plugin.parameterAutomation)});losses.items.push_back({LossSeverity::Warning,"vst3-portability",objectID,"VST3 state is embedded, but the importing host must provide the same class FUID and compatible plug-in version."});}else{result.push_back({id,plugin.type,plugin.subtype,plugin.manufacturer,plugin.name,plugin.bypassed,plugin.latencyFrames,"plugins/"+objectID+".aupreset",plugin.state,false,{},{},exportParameterAutomation(plugin.parameterAutomation)});losses.items.push_back({LossSeverity::Warning,"audio-unit-portability",objectID,"Audio Unit state is embedded, but the importing host may not provide or restore the same component."});}if(!plugin.parameterAutomation.empty())losses.items.push_back({LossSeverity::Warning,"plugin-parameter-automation-portability",objectID,"Normalized parameter lanes are exported with stable host parameter IDs, but an importing host or plug-in may not map those IDs."});if(plugin.latencyFrames)losses.items.push_back({LossSeverity::Info,"latency-metadata-not-portable",objectID,"Runtime latency is recalculated by the importing host and is not stored as project routing data."});}
        return result;
    };
    std::vector<ExportTrack> tracks;
    tracks.reserve(state.tracks.size());
    for (const auto& track : state.tracks) {
        ExportTrack item;
        item.id = decimalID(track.id);
        item.name = track.name;
        item.gainDb = track.gain;
        item.pan = track.pan;
        item.muted = track.muted;
        item.solo = track.solo;
        item.outputBusID = track.outputBus ? decimalID(track.outputBus) : "";
        item.volumeAutomation = track.volumeAutomation;
        item.panAutomation = track.panAutomation;
        item.plugins=exportPlugins(track.inserts,"track-"+item.id);

        std::set<uint32_t> usedTakes;
        for (const auto& region : track.regions) usedTakes.insert(region.take);
        for (const auto takeIndex : usedTakes) {
            const auto audio = takeIndex == 0 ? track.audio : track.takes.at(takeIndex - 1).audio;
            item.media.push_back({mediaID(track.id, takeIndex), track.id, takeIndex,
                                  audio->frames(), 48000, 2, audio});
        }
        for (size_t index = 0; index < track.regions.size(); ++index) {
            const auto& region = track.regions[index];
            item.regions.push_back({static_cast<uint32_t>(index), region.take, region.start,
                                    region.sourceOffset, region.length, region.fadeIn,
                                    region.fadeOut, mediaID(track.id, region.take)});
        }
        for (const auto& send : track.sends)
            item.sends.push_back({decimalID(send.bus), send.gain, send.preFader});
        if (!track.takes.empty())
            losses.items.push_back({LossSeverity::Warning, "comp-provenance-flattened", item.id,
                "The audible comp regions are preserved, but take-lane grouping and unused takes are omitted."});
        tracks.push_back(std::move(item));
    }

    std::vector<ExportBus> buses;
    buses.reserve(state.buses.size());
    for (const auto& bus : state.buses){ExportBus item{decimalID(bus.id),bus.name,bus.gain,bus.pan,bus.muted,bus.outputBus?decimalID(bus.outputBus):"",bus.gainAutomation,{}};item.plugins=exportPlugins(bus.inserts,"bus-"+item.id);buses.push_back(std::move(item));}

    ExportMaster master;
    master.gainDb = state.masterGain;
    master.gainAutomation = state.masterGainAutomation;
    master.plugins=exportPlugins(state.masterInserts,"master");
    losses.items.push_back({LossSeverity::Info, "audio-materialized", "project",
        "Referenced audio is embedded as 48 kHz stereo float WAV files."});
    losses.items.push_back({LossSeverity::Info, "editor-state-omitted", "project",
        "Window layout, selections, transport position, loop state and Undo history are not interchange data."});

    return ExportModel({options.tempoBpm, options.timeSignatureNumerator,
                        options.timeSignatureDenominator},
                       std::move(tracks), std::move(buses), std::move(master),
                       std::move(losses));
}
}
