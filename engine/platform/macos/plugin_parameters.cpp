#include "audio/effect.hpp"
#include "audio/plugin_parameters.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace daw {
namespace {
void checked(OSStatus status, const char* operation) {
    if (status != noErr) throw Error(std::string(operation) + " failed (OSStatus " + std::to_string(status) + ")");
}

AudioStreamBasicDescription streamFormat(uint32_t sampleRate) {
    AudioStreamBasicDescription value{};
    value.mSampleRate = sampleRate;
    value.mFormatID = kAudioFormatLinearPCM;
    value.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                         kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian;
    value.mBytesPerPacket = 4;
    value.mFramesPerPacket = 1;
    value.mBytesPerFrame = 4;
    value.mChannelsPerFrame = 2;
    value.mBitsPerChannel = 32;
    return value;
}

AudioComponent componentFor(const PluginInsert& plugin) {
    AudioComponentDescription description{plugin.type, plugin.subtype, plugin.manufacturer, 0, 0};
    auto component = AudioComponentFindNext(nullptr, &description);
    if (!component) throw Error("Audio Unit is unavailable");
    return component;
}

std::string componentName(AudioComponent component) {
    CFStringRef value = nullptr;
    checked(AudioComponentCopyName(component, &value), "Read Audio Unit name");
    if (!value) return "Audio Unit";
    std::array<char, 481> bytes{};
    const bool copied = CFStringGetCString(value, bytes.data(), bytes.size(), kCFStringEncodingUTF8);
    CFRelease(value);
    if (!copied) throw Error("Audio Unit name is too long");
    return bytes.data();
}

class Unit final {
public:
    explicit Unit(AudioComponent component) { checked(AudioComponentInstanceNew(component, &value), "Instantiate Audio Unit"); }
    ~Unit() {
        if (value) {
            if (initialized) AudioUnitUninitialize(value);
            AudioComponentInstanceDispose(value);
        }
    }
    Unit(const Unit&) = delete;
    Unit& operator=(const Unit&) = delete;

    AudioUnit value = nullptr;
    bool initialized = false;
};

void configure(Unit& unit, uint32_t sampleRate, uint32_t maxFrames, const std::vector<uint8_t>& state) {
    if (sampleRate == 0 || maxFrames == 0) throw Error("Invalid Audio Unit format");
    const auto format = streamFormat(sampleRate);
    checked(AudioUnitSetProperty(unit.value, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                 &format, sizeof(format)), "Configure Audio Unit input");
    checked(AudioUnitSetProperty(unit.value, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
                                 &format, sizeof(format)), "Configure Audio Unit output");
    const UInt32 maximum = maxFrames;
    checked(AudioUnitSetProperty(unit.value, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                 &maximum, sizeof(maximum)), "Configure Audio Unit block limit");
    if (state.empty()) return;
    if (state.size() > 1024U * 1024U) throw Error("Audio Unit state exceeds 1 MiB");
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, state.data(), static_cast<CFIndex>(state.size()));
    if (!data) throw Error("Create Audio Unit state data failed");
    CFErrorRef error = nullptr;
    auto property = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, &error);
    CFRelease(data);
    if (!property) {
        if (error) CFRelease(error);
        throw Error("Decode Audio Unit state failed");
    }
    checked(AudioUnitSetProperty(unit.value, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0,
                                 &property, sizeof(property)), "Restore Audio Unit state");
    CFRelease(property);
}

AudioUnitSnapshot capture(Unit& unit, const std::string& name, uint32_t sampleRate) {
    Float64 seconds = 0;
    UInt32 size = sizeof(seconds);
    checked(AudioUnitGetProperty(unit.value, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0,
                                 &seconds, &size), "Read Audio Unit latency");
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 10) throw Error("Invalid Audio Unit latency");
    CFPropertyListRef property = nullptr;
    size = sizeof(property);
    checked(AudioUnitGetProperty(unit.value, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0,
                                 &property, &size), "Capture Audio Unit state");
    if (!property) throw Error("Audio Unit returned no state");
    CFErrorRef error = nullptr;
    auto data = CFPropertyListCreateData(kCFAllocatorDefault, property, kCFPropertyListBinaryFormat_v1_0, 0, &error);
    CFRelease(property);
    if (!data) {
        if (error) CFRelease(error);
        throw Error("Encode Audio Unit state failed");
    }
    const auto length = CFDataGetLength(data);
    if (length < 0 || length > 1024 * 1024) {
        CFRelease(data);
        throw Error("Audio Unit state exceeds 1 MiB");
    }
    std::vector<uint8_t> state(static_cast<size_t>(length));
    if (length) std::memcpy(state.data(), CFDataGetBytePtr(data), static_cast<size_t>(length));
    CFRelease(data);
    return {name, static_cast<uint32_t>(std::llround(seconds * sampleRate)), std::move(state)};
}

float normalized(float value, float minimum, float maximum) {
    if (!std::isfinite(value) || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum) return 0;
    return std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
}

std::vector<AudioUnitParameter> readParameters(Unit& unit) {
    UInt32 size = 0;
    const auto listStatus = AudioUnitGetPropertyInfo(unit.value, kAudioUnitProperty_ParameterList,
                                                     kAudioUnitScope_Global, 0, &size, nullptr);
    if (listStatus == kAudioUnitErr_InvalidProperty) return {};
    checked(listStatus, "Read Audio Unit parameter list size");
    if (size % sizeof(AudioUnitParameterID) != 0 || size > 65536U * sizeof(AudioUnitParameterID)) {
        throw Error("Invalid Audio Unit parameter list");
    }
    std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
    if (!ids.empty()) checked(AudioUnitGetProperty(unit.value, kAudioUnitProperty_ParameterList,
                                                   kAudioUnitScope_Global, 0, ids.data(), &size),
                              "Read Audio Unit parameter list");
    std::vector<AudioUnitParameter> result;
    result.reserve(ids.size());
    for (const auto id : ids) {
        AudioUnitParameterInfo info{};
        UInt32 infoSize = sizeof(info);
        const auto status = AudioUnitGetProperty(unit.value, kAudioUnitProperty_ParameterInfo,
                                                 kAudioUnitScope_Global, id, &info, &infoSize);
        if (status != noErr) continue;
        AudioUnitParameterValue current = info.defaultValue;
        const auto getStatus = AudioUnitGetParameter(unit.value, id, kAudioUnitScope_Global, 0, &current);
        if (getStatus != noErr) current = info.defaultValue;
        if (!std::isfinite(current) || !std::isfinite(info.minValue) || !std::isfinite(info.maxValue) ||
            info.maxValue < info.minValue) continue;
        std::array<char, sizeof(info.name)> name{};
        std::memcpy(name.data(), info.name, name.size() - 1);
        const auto flags = info.flags;
        result.push_back({static_cast<uint32_t>(id), name.data(), info.minValue, info.maxValue, current,
                          normalized(current, info.minValue, info.maxValue),
                          (flags & kAudioUnitParameterFlag_IsWritable) != 0,
                          (flags & kAudioUnitParameterFlag_DisplayLogarithmic) != 0,
                          (flags & kAudioUnitParameterFlag_ValuesHaveStrings) != 0});
    }
    return result;
}

}

std::vector<AudioUnitParameter> audioUnitParameters(const PluginInsert& plugin, uint32_t sampleRate, uint32_t maxFrames) {
    Unit unit(componentFor(plugin));
    configure(unit, sampleRate, maxFrames, plugin.state);
    return readParameters(unit);
}

AudioUnitSnapshot setAudioUnitParameter(const PluginInsert& plugin, uint32_t parameterID, float nativeValue,
                                        uint32_t sampleRate, uint32_t maxFrames) {
    if (!std::isfinite(nativeValue)) throw Error("Audio Unit parameter value must be finite");
    auto component = componentFor(plugin);
    Unit unit(component);
    configure(unit, sampleRate, maxFrames, plugin.state);
    const auto parameters = readParameters(unit);
    const auto found = std::find_if(parameters.begin(), parameters.end(), [parameterID](const auto& parameter) {
        return parameter.id == parameterID;
    });
    if (found == parameters.end()) throw Error("Audio Unit parameter is unavailable");
    if (!found->writable) throw Error("Audio Unit parameter is read-only");
    const auto value = std::clamp(nativeValue, found->minimum, found->maximum);
    checked(AudioUnitSetParameter(unit.value, parameterID, kAudioUnitScope_Global, 0, value, 0),
            "Set Audio Unit parameter");
    checked(AudioUnitInitialize(unit.value), "Initialize Audio Unit");
    unit.initialized = true;
    return capture(unit, componentName(component), sampleRate);
}
}
