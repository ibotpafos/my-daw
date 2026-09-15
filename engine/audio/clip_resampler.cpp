#include "audio/clip_resampler.hpp"
#include "domain/session.hpp"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

namespace daw {
namespace {
constexpr uint32_t kProjectRate = 48000;
struct Input { const float* samples; uint32_t frames; uint32_t position = 0; };

OSStatus provideInput(AudioConverterRef, UInt32* requestedPackets, AudioBufferList* buffers,
                      AudioStreamPacketDescription**, void* userData) {
    auto& input = *static_cast<Input*>(userData);
    const uint32_t count = std::min(*requestedPackets, input.frames - input.position);
    *requestedPackets = count;
    buffers->mNumberBuffers = 1;
    buffers->mBuffers[0].mNumberChannels = 2;
    buffers->mBuffers[0].mData = count == 0 ? nullptr : const_cast<float*>(input.samples + size_t(input.position) * 2);
    buffers->mBuffers[0].mDataByteSize = count * 2 * sizeof(float);
    if (count == 0) return noErr;
    input.position += count;
    return noErr;
}

AudioStreamBasicDescription pcmDescription(double rate) {
    AudioStreamBasicDescription description{};
    description.mSampleRate = rate;
    description.mFormatID = kAudioFormatLinearPCM;
    description.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    description.mBytesPerPacket = 2 * sizeof(float);
    description.mFramesPerPacket = 1;
    description.mBytesPerFrame = 2 * sizeof(float);
    description.mChannelsPerFrame = 2;
    description.mBitsPerChannel = 32;
    return description;
}

void requireNoErr(OSStatus status, const char* action) {
    if (status != noErr) throw Error(std::string("Core Audio sample-rate conversion failed while ") + action);
}
}

std::vector<float> resampleStereoTo48k(const std::vector<float>& source, uint32_t sourceRate) {
    if (sourceRate == kProjectRate) return source;
    if (source.empty() || source.size() % 2 != 0) throw Error("Invalid stereo audio for sample-rate conversion");
    const uint64_t sourceFrames = source.size() / 2;
    const uint64_t outputFrames64 = std::max<uint64_t>(1, (sourceFrames * kProjectRate + sourceRate / 2) / sourceRate);
    if (outputFrames64 > kProjectRate * 60 || outputFrames64 > std::numeric_limits<uint32_t>::max()) {
        throw Error("Resampled WAV must be at most 60 seconds");
    }
    // AudioConverter may legitimately emit no packet for a source shorter than
    // one destination frame. Preserve a valid non-empty Clip deterministically.
    if (outputFrames64 == 1) return {source[0], source[1]};
    AudioConverterRef rawConverter = nullptr;
    const auto sourceDescription = pcmDescription(sourceRate);
    const auto destinationDescription = pcmDescription(kProjectRate);
    requireNoErr(AudioConverterNew(&sourceDescription, &destinationDescription, &rawConverter), "creating converter");
    std::unique_ptr<std::remove_pointer_t<AudioConverterRef>, decltype(&AudioConverterDispose)> converter(rawConverter, &AudioConverterDispose);
    UInt32 quality = kAudioConverterQuality_Max;
    requireNoErr(AudioConverterSetProperty(rawConverter, kAudioConverterSampleRateConverterQuality, sizeof(quality), &quality), "setting quality");
    UInt32 complexity = kAudioConverterSampleRateConverterComplexity_Mastering;
    requireNoErr(AudioConverterSetProperty(rawConverter, kAudioConverterSampleRateConverterComplexity, sizeof(complexity), &complexity), "setting mastering complexity");
    UInt32 prime = kConverterPrimeMethod_Normal;
    requireNoErr(AudioConverterSetProperty(rawConverter, kAudioConverterPrimeMethod, sizeof(prime), &prime), "setting normal priming");
    std::vector<float> output(static_cast<size_t>(outputFrames64) * 2);
    Input input{source.data(), static_cast<uint32_t>(sourceFrames)};
    uint64_t producedFrames = 0;
    while (producedFrames < outputFrames64) {
        UInt32 outputPackets = static_cast<UInt32>(std::min<uint64_t>(16384, outputFrames64 - producedFrames));
        AudioBufferList outputBuffers{};
        outputBuffers.mNumberBuffers = 1;
        outputBuffers.mBuffers[0].mNumberChannels = 2;
        outputBuffers.mBuffers[0].mData = output.data() + producedFrames * 2;
        outputBuffers.mBuffers[0].mDataByteSize = outputPackets * 2 * sizeof(float);
        requireNoErr(AudioConverterFillComplexBuffer(rawConverter, provideInput, &input, &outputPackets, &outputBuffers, nullptr), "converting PCM");
        if (outputPackets == 0) break;
        producedFrames += outputPackets;
    }
    if (producedFrames != outputFrames64) {
        throw Error("Core Audio returned " + std::to_string(producedFrames) + " of " +
                    std::to_string(outputFrames64) + " expected frames while importing " +
                    std::to_string(sourceRate) + " Hz PCM");
    }
    return output;
}
}
