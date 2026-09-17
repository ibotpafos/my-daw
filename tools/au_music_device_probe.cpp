#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

bool parseHex(const char* text, std::uint32_t& value) {
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 16);
    if (end == nullptr || *end != '\0' || parsed > 0xffffffffUL) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

std::string fourCC(std::uint32_t value) {
    std::array<char, 5> text{
        static_cast<char>((value >> 24U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>(value & 0xffU),
        '\0'};
    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte < 32U || byte > 126U) text[i] = '?';
    }
    return text.data();
}

std::string componentName(AudioComponent component) {
    CFStringRef name = nullptr;
    if (AudioComponentCopyName(component, &name) != noErr || name == nullptr) {
        return "Unnamed Audio Unit";
    }
    std::array<char, 1024> bytes{};
    const bool converted = CFStringGetCString(
        name, bytes.data(), static_cast<CFIndex>(bytes.size()), kCFStringEncodingUTF8);
    CFRelease(name);
    return converted && bytes[0] != '\0' ? std::string(bytes.data()) : "Unnamed Audio Unit";
}

AudioStreamBasicDescription stereoFloatFormat(double sampleRate) {
    AudioStreamBasicDescription value{};
    value.mSampleRate = sampleRate;
    value.mFormatID = kAudioFormatLinearPCM;
    value.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                         kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian;
    value.mBytesPerPacket = sizeof(float);
    value.mFramesPerPacket = 1;
    value.mBytesPerFrame = sizeof(float);
    value.mChannelsPerFrame = 2;
    value.mBitsPerChannel = 32;
    return value;
}

struct StereoBufferList {
    UInt32 count = 2;
    AudioBuffer buffers[2]{};
};

struct Unit {
    AudioUnit value = nullptr;
    bool initialized = false;

    ~Unit() {
        if (value != nullptr) {
            if (initialized) AudioUnitUninitialize(value);
            AudioComponentInstanceDispose(value);
        }
    }
};

void printOSStatus(const char* operation, OSStatus status) {
    std::cerr << operation << " failed (OSStatus " << status << ")\n";
}

int listMusicDevices() {
    AudioComponentDescription query{};
    query.componentType = kAudioUnitType_MusicDevice;

    AudioComponent component = nullptr;
    std::size_t count = 0;
    while ((component = AudioComponentFindNext(component, &query)) != nullptr) {
        AudioComponentDescription description{};
        if (AudioComponentGetDescription(component, &description) != noErr) continue;
        std::cout << fourCC(description.componentSubType) << '\t'
                  << fourCC(description.componentManufacturer) << '\t'
                  << std::hex << description.componentSubType << '\t'
                  << description.componentManufacturer << std::dec << '\t'
                  << componentName(component) << '\n';
        ++count;
    }
    std::cerr << "MusicDevice components: " << count << '\n';
    return count == 0 ? 10 : 0;
}

std::optional<AudioComponent> findMusicDevice(std::uint32_t subtype, std::uint32_t manufacturer) {
    AudioComponentDescription query{};
    query.componentType = kAudioUnitType_MusicDevice;
    query.componentSubType = subtype;
    query.componentManufacturer = manufacturer;
    const AudioComponent component = AudioComponentFindNext(nullptr, &query);
    if (component == nullptr) return std::nullopt;
    return component;
}

int probeMusicDevice(std::uint32_t subtype, std::uint32_t manufacturer, double seconds) {
    const auto component = findMusicDevice(subtype, manufacturer);
    if (!component) {
        std::cerr << "MusicDevice not found: subtype=" << fourCC(subtype)
                  << " manufacturer=" << fourCC(manufacturer) << '\n';
        return 11;
    }

    std::cout << "Probing " << componentName(*component) << " ["
              << fourCC(subtype) << "/" << fourCC(manufacturer) << "]\n";

    Unit unit;
    OSStatus status = AudioComponentInstanceNew(*component, &unit.value);
    if (status != noErr || unit.value == nullptr) {
        printOSStatus("AudioComponentInstanceNew", status);
        return 12;
    }

    constexpr double sampleRate = 48000.0;
    constexpr UInt32 maximumFrames = 512;
    const auto format = stereoFloatFormat(sampleRate);

    status = AudioUnitSetProperty(unit.value, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output, 0, &format, sizeof(format));
    if (status != noErr) {
        printOSStatus("Set MusicDevice output stream format", status);
        return 13;
    }

    UInt32 maximum = maximumFrames;
    status = AudioUnitSetProperty(unit.value, kAudioUnitProperty_MaximumFramesPerSlice,
                                  kAudioUnitScope_Global, 0, &maximum, sizeof(maximum));
    if (status != noErr) {
        printOSStatus("Set maximum frames per slice", status);
        return 14;
    }

    status = AudioUnitInitialize(unit.value);
    if (status != noErr) {
        printOSStatus("AudioUnitInitialize", status);
        return 15;
    }
    unit.initialized = true;

    Float64 latencySeconds = 0.0;
    UInt32 latencySize = sizeof(latencySeconds);
    status = AudioUnitGetProperty(unit.value, kAudioUnitProperty_Latency,
                                  kAudioUnitScope_Global, 0, &latencySeconds, &latencySize);
    if (status != noErr || !std::isfinite(latencySeconds) || latencySeconds < 0.0 ||
        latencySeconds > 10.0) {
        if (status != noErr) printOSStatus("Read MusicDevice latency", status);
        else std::cerr << "MusicDevice returned invalid latency\n";
        return 16;
    }

    std::vector<float> left(maximumFrames, 0.0F);
    std::vector<float> right(maximumFrames, 0.0F);
    StereoBufferList list;
    list.buffers[0].mNumberChannels = 1;
    list.buffers[0].mData = left.data();
    list.buffers[0].mDataByteSize = maximumFrames * sizeof(float);
    list.buffers[1].mNumberChannels = 1;
    list.buffers[1].mData = right.data();
    list.buffers[1].mDataByteSize = maximumFrames * sizeof(float);

    const std::uint64_t totalFrames = static_cast<std::uint64_t>(std::llround(seconds * sampleRate));
    const std::uint64_t noteOffFrame = std::min<std::uint64_t>(totalFrames / 2U, 24000U);
    std::uint64_t rendered = 0;
    bool noteOffSent = false;
    float peak = 0.0F;
    std::uint64_t nonSilentSamples = 0;

    status = MusicDeviceMIDIEvent(unit.value, 0x90U, 60U, 100U, 0U);
    if (status != noErr) {
        printOSStatus("MusicDevice Note On", status);
        return 17;
    }

    while (rendered < totalFrames) {
        const UInt32 frames = static_cast<UInt32>(
            std::min<std::uint64_t>(maximumFrames, totalFrames - rendered));

        if (!noteOffSent && rendered >= noteOffFrame) {
            status = MusicDeviceMIDIEvent(unit.value, 0x80U, 60U, 0U, 0U);
            if (status != noErr) {
                printOSStatus("MusicDevice Note Off", status);
                return 18;
            }
            noteOffSent = true;
        }

        std::fill(left.begin(), left.end(), 0.0F);
        std::fill(right.begin(), right.end(), 0.0F);
        list.buffers[0].mDataByteSize = frames * sizeof(float);
        list.buffers[1].mDataByteSize = frames * sizeof(float);

        AudioTimeStamp timestamp{};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = static_cast<Float64>(rendered);
        AudioUnitRenderActionFlags flags = 0;
        status = AudioUnitRender(unit.value, &flags, &timestamp, 0, frames,
                                 reinterpret_cast<AudioBufferList*>(&list));
        if (status != noErr) {
            printOSStatus("AudioUnitRender", status);
            return 19;
        }

        for (UInt32 index = 0; index < frames; ++index) {
            const float absoluteLeft = std::fabs(left[index]);
            const float absoluteRight = std::fabs(right[index]);
            peak = std::max({peak, absoluteLeft, absoluteRight});
            if (absoluteLeft > 1.0e-6F) ++nonSilentSamples;
            if (absoluteRight > 1.0e-6F) ++nonSilentSamples;
        }
        rendered += frames;
    }

    if (!noteOffSent) {
        status = MusicDeviceMIDIEvent(unit.value, 0x80U, 60U, 0U, 0U);
        if (status != noErr) {
            printOSStatus("MusicDevice final Note Off", status);
            return 20;
        }
    }
    (void)MusicDeviceMIDIEvent(unit.value, 0xB0U, 123U, 0U, 0U); // All Notes Off.

    std::cout << "rendered_frames=" << rendered
              << " peak=" << peak
              << " non_silent_samples=" << nonSilentSamples
              << " latency_frames=" << std::llround(latencySeconds * sampleRate) << '\n';

    if (peak <= 1.0e-6F || nonSilentSamples == 0U) {
        std::cerr << "MusicDevice initialized and accepted MIDI, but rendered silence. "
                     "The plug-in may require a preset or may not support headless rendering.\n";
        return 21;
    }

    std::cout << "ok\n";
    return 0;
}

void usage(const char* argv0) {
    std::cerr << "Usage:\n"
              << "  " << argv0 << " --list\n"
              << "  " << argv0 << " --probe <subtype-hex> <manufacturer-hex> [seconds]\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--list") == 0) {
        return listMusicDevices();
    }

    if ((argc == 4 || argc == 5) && std::strcmp(argv[1], "--probe") == 0) {
        std::uint32_t subtype = 0;
        std::uint32_t manufacturer = 0;
        if (!parseHex(argv[2], subtype) || !parseHex(argv[3], manufacturer) ||
            subtype == 0U || manufacturer == 0U) {
            usage(argv[0]);
            return 2;
        }
        double seconds = 2.0;
        if (argc == 5) {
            char* end = nullptr;
            seconds = std::strtod(argv[4], &end);
            if (end == nullptr || *end != '\0' || !std::isfinite(seconds) ||
                seconds < 0.25 || seconds > 30.0) {
                usage(argv[0]);
                return 2;
            }
        }
        return probeMusicDevice(subtype, manufacturer, seconds);
    }

    usage(argv[0]);
    return 2;
}
