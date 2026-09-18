#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace daw {
// User/machine preference, not durable musical project state. Empty UID opts in
// to following the corresponding system default. A nonempty UID never falls back.
constexpr uint32_t audioDeviceChannelLimit = 128;
constexpr uint32_t audioDeviceUIDBytes = 480;
constexpr uint32_t audioDeviceMaximumFrames = 4096;
struct AudioDeviceConfiguration {
    std::string inputUID, outputUID;
    uint32_t inputChannel = 0, inputRight = 1, inputChannels = 1, outputLeft = 0, outputRight = 1;
    bool operator==(const AudioDeviceConfiguration&) const = default;
};
struct AudioDeviceInfo {
    uint32_t id = 0;
    std::string uid, name;
    uint32_t inputChannels = 0, outputChannels = 0, bufferFrames = 0;
    double sampleRate = 0;
    bool defaultInput = false, defaultOutput = false;
};
enum class AudioDeviceDirection { Input, Output };
void validateAudioDeviceConfiguration(const AudioDeviceConfiguration&);
// Selection/validation and channel-map construction are shared by all three
// HAL paths and tested without opening hardware. These are control-thread APIs.
const AudioDeviceInfo& resolveAudioDevice(const AudioDeviceConfiguration&,
                                          std::span<const AudioDeviceInfo>, AudioDeviceDirection);
std::vector<int32_t> audioOutputChannelMap(uint32_t channels, uint32_t left, uint32_t right);
// Enumeration is read-only: no microphone permission, IO start or hardware edits.
std::vector<AudioDeviceInfo> enumerateAudioDevices();
}
