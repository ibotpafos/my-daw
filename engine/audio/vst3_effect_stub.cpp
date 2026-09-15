#include "audio/effect.hpp"
#include "plugins/plugin_descriptor.hpp"

namespace daw {

bool isVst3Insert(const PluginInsert &plugin) noexcept {
  return isVst3PluginInsert(plugin);
}

std::unique_ptr<PreparedEffect> prepareVst3Effect(const PluginInsert &,
                                                  uint32_t,
                                                  uint32_t) {
  throw Error("VST3 hosting requires the pinned VST3 SDK");
}

std::vector<Vst3Parameter> vst3Parameters(const PluginInsert &, uint32_t,
                                          uint32_t) {
  throw Error("VST3 hosting requires the pinned VST3 SDK");
}

Vst3EffectSnapshot snapshotVst3Effect(const PluginInsert &, uint32_t,
                                      uint32_t) {
  throw Error("VST3 hosting requires the pinned VST3 SDK");
}

Vst3EffectSnapshot setVst3Parameter(const PluginInsert &, uint32_t, float,
                                    uint32_t, uint32_t) {
  throw Error("VST3 hosting requires the pinned VST3 SDK");
}

} // namespace daw
