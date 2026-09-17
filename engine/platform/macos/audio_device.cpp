#include "platform/macos/audio_device.hpp"
#include "domain/session.hpp"
#include <CoreAudio/CoreAudio.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>

namespace daw {
namespace {
void checked(OSStatus status, const char* action) {
    if (status != noErr) throw Error(std::string(action) + " (Core Audio " + std::to_string(status) + ")");
}
template<class T>
T read(AudioObjectID device, AudioObjectPropertySelector selector,
       AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    T value{};
    UInt32 size = sizeof(value);
    AudioObjectPropertyAddress property{selector, scope, kAudioObjectPropertyElementMain};
    checked(AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &value), "Read audio device property");
    if (size != sizeof(value)) throw Error("Audio device returned an invalid property size");
    return value;
}
std::string text(AudioDeviceID device, AudioObjectPropertySelector selector) {
    auto value = read<CFStringRef>(device, selector);
    if (!value) throw Error("Audio device returned an empty identity");
    std::array<char, audioDeviceUIDBytes + 1> bytes{};
    const bool ok = CFStringGetCString(value, bytes.data(), bytes.size(), kCFStringEncodingUTF8);
    CFRelease(value);
    if (!ok) throw Error("Audio device identity is too long or invalid UTF-8");
    return bytes.data();
}
uint32_t channels(AudioDeviceID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress property{kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    checked(AudioObjectGetPropertyDataSize(device, &property, 0, nullptr, &size), "Read channel configuration size");
    if (size < offsetof(AudioBufferList, mBuffers) || size > 65536)
        throw Error("Invalid audio channel configuration size");
    const auto capacity = size;
    auto storage = std::unique_ptr<AudioBufferList, decltype(&std::free)>(
        static_cast<AudioBufferList*>(std::calloc(1, capacity)), &std::free);
    if (!storage) throw std::bad_alloc();
    checked(AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, storage.get()), "Read channel configuration");
    if (size > capacity || size < offsetof(AudioBufferList, mBuffers) ||
        storage->mNumberBuffers > (size - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer))
        throw Error("Invalid audio channel configuration");
    uint32_t total = 0;
    for (UInt32 i = 0; i < storage->mNumberBuffers; ++i) {
        const auto count = storage->mBuffers[i].mNumberChannels;
        if (count > audioDeviceChannelLimit - total) throw Error("Audio device exceeds 128 channels");
        total += count;
    }
    return total;
}
AudioDeviceID defaultDevice(AudioDeviceDirection direction) {
    return read<AudioDeviceID>(kAudioObjectSystemObject, direction == AudioDeviceDirection::Input
        ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice);
}
AudioDeviceInfo describe(AudioDeviceID id) {
    if (!id || !read<UInt32>(id, kAudioDevicePropertyDeviceIsAlive)) throw Error("Audio device disconnected");
    AudioDeviceInfo result;
    result.id = id;
    result.uid = text(id, kAudioDevicePropertyDeviceUID);
    result.name = text(id, kAudioObjectPropertyName);
    result.inputChannels = channels(id, kAudioDevicePropertyScopeInput);
    result.outputChannels = channels(id, kAudioDevicePropertyScopeOutput);
    result.sampleRate = read<Float64>(id, kAudioDevicePropertyNominalSampleRate);
    result.bufferFrames = read<UInt32>(id, kAudioDevicePropertyBufferFrameSize);
    return result;
}
}
std::vector<AudioDeviceInfo> enumerateAudioDevices() {
    AudioObjectPropertyAddress property{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    checked(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property, 0, nullptr, &size), "Read audio devices");
    if (size % sizeof(AudioDeviceID) || size / sizeof(AudioDeviceID) > 256)
        throw Error("Invalid or oversized audio device catalog");
    if (!size) return {};
    std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
    const auto capacity = size;
    checked(AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr, &size, ids.data()), "Enumerate audio devices");
    if (size > capacity || size % sizeof(AudioDeviceID)) throw Error("Audio device catalog changed; refresh it");
    ids.resize(size / sizeof(AudioDeviceID));
    const auto input = defaultDevice(AudioDeviceDirection::Input);
    const auto output = defaultDevice(AudioDeviceDirection::Output);
    std::vector<AudioDeviceInfo> result;
    for (auto id : ids) {
        try {
            auto device = describe(id);
            if (!device.inputChannels && !device.outputChannels) continue;
            device.defaultInput = id == input;
            device.defaultOutput = id == output;
            result.push_back(std::move(device));
        } catch (const Error&) {
            // A removed/unreadable device must not turn into a usable fallback.
            // One broken driver also must not hide every other working device.
        }
    }
    return result;
}
AudioDeviceInfo openAudioDevice(const AudioDeviceConfiguration& config, AudioDeviceDirection direction) {
    const auto devices = enumerateAudioDevices();
    return resolveAudioDevice(config, devices, direction);
}
void checkAudioDevice(const AudioDeviceInfo& original, bool followsDefault, AudioDeviceDirection direction) {
    const auto now = describe(original.id);
    if (now.uid != original.uid || !std::isfinite(now.sampleRate) ||
        std::abs(now.sampleRate - original.sampleRate) > 0.5 || now.bufferFrames != original.bufferFrames ||
        now.inputChannels != original.inputChannels || now.outputChannels != original.outputChannels ||
        (followsDefault && defaultDevice(direction) != original.id))
        throw Error("Audio device disconnected or its format changed. Check Audio Settings before restarting.");
}
void mapAudioInput(AudioUnit unit, uint32_t channel) {
    if (channel >= audioDeviceChannelLimit) throw Error("Invalid input channel");
    const SInt32 map = static_cast<SInt32>(channel);
    checked(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output,
                                1, &map, sizeof(map)), "Map mono input channel");
}
void mapAudioOutput(AudioUnit unit, const AudioDeviceInfo& device, uint32_t left, uint32_t right) {
    const auto map = audioOutputChannelMap(device.outputChannels, left, right);
    checked(AudioUnitSetProperty(unit, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output,
                                0, map.data(), static_cast<UInt32>(map.size() * sizeof(SInt32))), "Map stereo output channels");
}
}
