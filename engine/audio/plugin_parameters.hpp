#pragma once

#include "domain/session.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

// A host-neutral representation of one Audio Unit parameter. Values are kept
// both in the unit's native range and in 0...1 so a UI can choose either form.
struct AudioUnitParameter {
    uint32_t id = 0;
    std::string name;
    float minimum = 0;
    float maximum = 1;
    float value = 0;
    float normalizedValue = 0;
    bool writable = false;
    bool logarithmic = false;
    bool indexed = false;
};

// Reads the current parameter values from a temporary instance restored from
// plugin.state. This has no side effects on the live audio renderer.
std::vector<AudioUnitParameter> audioUnitParameters(const PluginInsert& plugin,
                                                     uint32_t sampleRate = 48000,
                                                     uint32_t maxFrames = 4096);

// Restores plugin.state into a temporary instance, changes one writable global
// parameter by native value, and returns a fresh binary state and latency.
// The caller replaces only PluginInsert::state and ::latencyFrames.
AudioUnitSnapshot setAudioUnitParameter(const PluginInsert& plugin,
                                        uint32_t parameterID,
                                        float nativeValue,
                                        uint32_t sampleRate = 48000,
                                        uint32_t maxFrames = 4096);

}
