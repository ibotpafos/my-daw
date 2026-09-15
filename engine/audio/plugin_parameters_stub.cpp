#include "audio/effect.hpp"
#include "audio/plugin_parameters.hpp"

namespace daw {
std::vector<AudioUnitParameter> audioUnitParameters(const PluginInsert&, uint32_t, uint32_t) {
    throw Error("Audio Unit parameter hosting requires macOS");
}
AudioUnitSnapshot setAudioUnitParameter(const PluginInsert&, uint32_t, float, uint32_t, uint32_t) {
    throw Error("Audio Unit parameter hosting requires macOS");
}
}
