#include "platform/macos/audio_device.hpp"
#include "audio/device_settings.hpp"
#include "domain/session.hpp"
#include <CoreAudio/CoreAudio.h>
#include <array>
#include <atomic>
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
namespace {
bool writable(AudioDeviceID id, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress property{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    Boolean value = false;
    checked(AudioObjectIsPropertySettable(id, &property, &value), "Read audio property access");
    return value;
}
std::vector<AudioValueRange> availableRates(AudioDeviceID id) {
    AudioObjectPropertyAddress property{kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    checked(AudioObjectGetPropertyDataSize(id, &property, 0, nullptr, &size), "Read supported sample rates");
    if (size % sizeof(::AudioValueRange) || size > 64 * sizeof(::AudioValueRange))
        throw Error("Invalid or oversized sample rate catalog");
    if (!size) return {};
    std::array<::AudioValueRange, 64> ranges{};
    const auto capacity = size;
    checked(AudioObjectGetPropertyData(id, &property, 0, nullptr, &size, ranges.data()), "Read supported sample rates");
    if (size > capacity || size % sizeof(::AudioValueRange)) throw Error("Sample rate catalog changed; refresh");
    std::vector<AudioValueRange> result;
    for (size_t i = 0; i < size / sizeof(::AudioValueRange); ++i)
        result.push_back({ranges[i].mMinimum, ranges[i].mMaximum});
    return result;
}
// The C callback never dereferences a destroyed controller. This trivial state
// has process lifetime, including notifications queued before listener removal.
// Production hardware writes are serialized by the bridge's single coordinator;
// read-only capability adapters do not register listeners or change this state.
struct FormatNotifications {
    std::atomic<AudioObjectID> device{0};
    std::atomic<uint64_t> rate{0}, buffer{0};
};
FormatNotifications formatNotifications;
OSStatus formatChanged(AudioObjectID device, UInt32 count,
                       const AudioObjectPropertyAddress* addresses, void*) {
    if (device != formatNotifications.device.load(std::memory_order_acquire)) return noErr;
    for (UInt32 i = 0; i < count; ++i) {
        if (addresses[i].mSelector == kAudioDevicePropertyNominalSampleRate)
            formatNotifications.rate.fetch_add(1, std::memory_order_release);
        if (addresses[i].mSelector == kAudioDevicePropertyBufferFrameSize)
            formatNotifications.buffer.fetch_add(1, std::memory_order_release);
    }
    return noErr;
}
class HALDeviceControl final : public AudioDeviceControl {
    AudioDeviceID id;
    std::string uid;
    bool rateListener = false, bufferListener = false;
    uint64_t rateTicket = 0, bufferTicket = 0;
    void listen(AudioObjectPropertySelector selector, bool& registered) {
        formatNotifications.device.store(id, std::memory_order_release);
        if (registered) return;
        AudioObjectPropertyAddress property{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        checked(AudioObjectAddPropertyListener(id, &property, formatChanged, nullptr), "Observe audio format acknowledgement");
        registered = true;
    }
    void removeListener(AudioObjectPropertySelector selector, bool registered) noexcept {
        if (!registered) return;
        AudioObjectPropertyAddress property{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        // Device removal can make deregistration fail; callback storage still lives.
        (void)AudioObjectRemovePropertyListener(id, &property, formatChanged, nullptr);
    }
    void checkIdentity() {
        if (!read<UInt32>(id, kAudioDevicePropertyDeviceIsAlive) || text(id, kAudioDevicePropertyDeviceUID) != uid)
            throw Error("Selected audio device disconnected or was replaced");
    }
    template<class T> void write(AudioObjectPropertySelector selector, T value) {
        checkIdentity();
        if (read<UInt32>(id, kAudioDevicePropertyDeviceIsRunningSomewhere))
            throw Error("Stop all audio applications using this device first");
        if (!writable(id, selector)) throw Error("Audio device property is read-only");
        AudioObjectPropertyAddress property{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        checked(AudioObjectSetPropertyData(id, &property, 0, nullptr, sizeof(value), &value), "Set audio device format");
        // A successful setter is only submission. AudioDeviceChange reads back
        // both properties on later owner-thread polls before publishing success.
    }
public:
    explicit HALDeviceControl(const AudioDeviceInfo& device) : id(device.id), uid(device.uid) {}
    ~HALDeviceControl() override {
        removeListener(kAudioDevicePropertyNominalSampleRate, rateListener);
        removeListener(kAudioDevicePropertyBufferFrameSize, bufferListener);
    }
    AudioDeviceCapabilities inspect() override {
        checkIdentity();
        AudioDeviceCapabilities caps;
        caps.device = describe(id);
        caps.sampleRates = availableRates(id);
        const auto range = read<::AudioValueRange>(id, kAudioDevicePropertyBufferFrameSizeRange);
        caps.bufferRange = {range.mMinimum, range.mMaximum};
        caps.rateWritable = writable(id, kAudioDevicePropertyNominalSampleRate);
        caps.bufferWritable = writable(id, kAudioDevicePropertyBufferFrameSize);
        caps.running = read<UInt32>(id, kAudioDevicePropertyDeviceIsRunningSomewhere) != 0;
        checkIdentity();
        validateAudioDeviceCapabilities(caps);
        return caps;
    }
    void setSampleRate(double rate) override {
        listen(kAudioDevicePropertyNominalSampleRate, rateListener);
        rateTicket = formatNotifications.rate.load(std::memory_order_acquire);
        write<Float64>(kAudioDevicePropertyNominalSampleRate, rate);
    }
    void setBufferFrames(uint32_t frames) override {
        listen(kAudioDevicePropertyBufferFrameSize, bufferListener);
        bufferTicket = formatNotifications.buffer.load(std::memory_order_acquire);
        write<UInt32>(kAudioDevicePropertyBufferFrameSize, frames);
    }
    bool sampleRateAcknowledged() const override {
        return !rateListener || formatNotifications.rate.load(std::memory_order_acquire) != rateTicket;
    }
    bool bufferAcknowledged() const override {
        return !bufferListener || formatNotifications.buffer.load(std::memory_order_acquire) != bufferTicket;
    }
};
}
std::unique_ptr<AudioDeviceControl> makeAudioDeviceControl(const std::string& uid) {
    validateAudioDeviceUID(uid);
    for (const auto& device : enumerateAudioDevices())
        if (device.uid == uid) return std::make_unique<HALDeviceControl>(device);
    throw Error("Selected audio device is unavailable; no default-device fallback");
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
