#pragma once
#include "audio/device.hpp"
#include "audio/recording_timing.hpp"
#include "audio/recording_channels.hpp"
#include <AudioToolbox/AudioToolbox.h>
namespace daw {
struct DuplexHardwareProfile {
    RecordingLatency latency;
    std::vector<uint32_t> inputBuffers, outputBuffers;
    RecordingChannel input, inputRight, left, right;
    uint32_t inputChannels = 1;
    bool operator==(const DuplexHardwareProfile&) const = default;
};
// Read-only, control thread. Requires one shared native 48 kHz Float32 clock.
DuplexHardwareProfile readDuplexHardwareProfile(const AudioDeviceInfo&, const AudioDeviceConfiguration&);
AudioDeviceInfo openAudioDevice(const AudioDeviceConfiguration&, AudioDeviceDirection);
void checkAudioDevice(const AudioDeviceInfo&, bool followsDefault, AudioDeviceDirection);
void mapAudioInput(AudioUnit, uint32_t channel);
void mapAudioOutput(AudioUnit, const AudioDeviceInfo&, uint32_t left, uint32_t right);
}
