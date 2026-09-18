#include "audio/device.hpp"
#include "domain/session.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
void validateAudioDeviceConfiguration(const AudioDeviceConfiguration& config) {
    for (const auto* uid : {&config.inputUID, &config.outputUID}) {
        if (uid->size() > audioDeviceUIDBytes || uid->find('\0') != std::string::npos)
            throw Error("Invalid audio device UID");
    }
    if (config.inputChannels < 1 || config.inputChannels > 2 ||
        config.inputChannel >= audioDeviceChannelLimit || config.inputRight >= audioDeviceChannelLimit ||
        (config.inputChannels == 2 && config.inputChannel == config.inputRight) ||
        config.outputLeft >= audioDeviceChannelLimit ||
        config.outputRight >= audioDeviceChannelLimit || config.outputLeft == config.outputRight)
        throw Error("Invalid audio channel selection");
}
const AudioDeviceInfo& resolveAudioDevice(const AudioDeviceConfiguration& config,
                                          std::span<const AudioDeviceInfo> devices,
                                          AudioDeviceDirection direction) {
    validateAudioDeviceConfiguration(config);
    const bool input = direction == AudioDeviceDirection::Input;
    const auto& uid = input ? config.inputUID : config.outputUID;
    const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
        return uid.empty() ? (input ? device.defaultInput : device.defaultOutput) : device.uid == uid;
    });
    if (found == devices.end())
        throw Error(input ? "Selected input device is unavailable. Check Audio Settings."
                          : "Selected output device is unavailable. Check Audio Settings.");
    if (!found->id || found->uid.empty())
        throw Error("Audio device has no stable identity");
    if (input ? (config.inputChannel >= found->inputChannels ||
                 (config.inputChannels == 2 && config.inputRight >= found->inputChannels))
              : std::max(config.outputLeft, config.outputRight) >= found->outputChannels)
        throw Error("Selected audio channels are unavailable. Check Audio Settings.");
    if (!std::isfinite(found->sampleRate) || std::abs(found->sampleRate - 48000.0) > 0.5)
        throw Error("This project requires 48 kHz. Set the selected device to 48 kHz in Audio MIDI Setup.");
    if (!found->bufferFrames || found->bufferFrames > audioDeviceMaximumFrames)
        throw Error("Audio device buffer must be between 1 and 4096 frames");
    return *found;
}
std::vector<int32_t> audioOutputChannelMap(uint32_t channels, uint32_t left, uint32_t right) {
    if (channels < 2 || channels > audioDeviceChannelLimit || left >= channels ||
        right >= channels || left == right)
        throw Error("Invalid stereo output channel map");
    // AUHAL maps each hardware destination to a client source. Unselected
    // hardware channels are explicitly silent, never copies of the master.
    std::vector<int32_t> map(channels, -1);
    map[left] = 0;
    map[right] = 1;
    return map;
}
#ifndef __APPLE__
std::vector<AudioDeviceInfo> enumerateAudioDevices() { return {}; }
#endif
}
