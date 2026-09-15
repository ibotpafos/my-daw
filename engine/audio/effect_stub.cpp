#include "audio/effect.hpp"
namespace daw {
std::vector<AudioUnitDescriptor> supportedAudioUnits(){return {};}
bool audioUnitAvailable(const AudioUnitDescriptor&) noexcept{return false;}
AudioUnitSnapshot snapshotAudioUnit(const AudioUnitDescriptor&,uint32_t,uint32_t){throw Error("Audio Unit hosting requires macOS");}
std::unique_ptr<PreparedEffect> prepareAudioUnit(const PluginInsert&,uint32_t,uint32_t){throw Error("Audio Unit hosting requires macOS");}
}
