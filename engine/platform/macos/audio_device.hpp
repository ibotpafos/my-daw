#pragma once
#include "audio/device.hpp"
#include <AudioToolbox/AudioToolbox.h>
namespace daw {
AudioDeviceInfo openAudioDevice(const AudioDeviceConfiguration&, AudioDeviceDirection);
void checkAudioDevice(const AudioDeviceInfo&, bool followsDefault, AudioDeviceDirection);
void mapAudioInput(AudioUnit, uint32_t channel);
void mapAudioOutput(AudioUnit, const AudioDeviceInfo&, uint32_t left, uint32_t right);
}
