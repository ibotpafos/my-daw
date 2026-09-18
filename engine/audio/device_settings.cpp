#include "audio/device_settings.hpp"
#include "domain/session.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
namespace {
bool sameRate(double a, double b) {
    return std::isfinite(a) && std::abs(a - b) <= 0.5;
}
bool inRange(double value, AudioValueRange range) {
    return value >= range.minimum && value <= range.maximum;
}
bool validRange(AudioValueRange range) {
    return std::isfinite(range.minimum) && std::isfinite(range.maximum) && range.minimum > 0 &&
           range.maximum >= range.minimum;
}
void validateTarget(const AudioDeviceCapabilities &caps, double rate, uint32_t frames) {
    validateAudioDeviceCapabilities(caps);
    if (caps.running)
        throw Error("Stop all audio applications using this device before changing its format");
    if (!sameRate(caps.device.sampleRate, rate) &&
        (!caps.rateWritable ||
         !std::any_of(caps.sampleRates.begin(), caps.sampleRates.end(), [=](auto range) {
             return inRange(rate, range);
         })))
        throw Error("Selected device cannot be set to the project's 48 kHz sample rate");
    if (caps.device.bufferFrames != frames &&
        (!caps.bufferWritable || !inRange(frames, caps.bufferRange)))
        throw Error("Requested buffer is outside the writable hardware range");
}
}
void validateAudioDeviceUID(const std::string &uid) {
    if (uid.empty() || uid.size() > audioDeviceUIDBytes || uid.find('\0') != std::string::npos)
        throw Error("An explicit, valid audio device UID is required");
}
void validateAudioDeviceRequest(double rate, uint32_t frames) {
    // Hardware settings do not introduce a new project rate or silent resampling.
    if (rate != 48000.0 || frames == 0 || frames > audioDeviceMaximumFrames)
        throw Error("This project requires 48000 Hz and a buffer of 1 to 4096 frames");
}
void validateAudioDeviceCapabilities(const AudioDeviceCapabilities &caps) {
    validateAudioDeviceUID(caps.device.uid);
    if (!caps.device.id || !std::isfinite(caps.device.sampleRate) || caps.device.sampleRate <= 0 ||
        !caps.device.bufferFrames || !validRange(caps.bufferRange) ||
        caps.sampleRates.size() > 64 ||
        !std::all_of(caps.sampleRates.begin(), caps.sampleRates.end(), validRange))
        throw Error("Audio device returned invalid format capabilities");
}
AudioDeviceChange::AudioDeviceChange(std::unique_ptr<AudioDeviceControl> backend, double rate,
                                     uint32_t frames, Clock::time_point now)
    : control(std::move(backend)), targetRate(rate), targetFrames(frames), deadline(now + timeout) {
    validateAudioDeviceRequest(rate, frames);
    if (!control)
        throw Error("Missing audio device control");
    const auto caps = control->inspect();
    validateTarget(caps, rate, frames); // Both targets validated before the first hardware write.
    actual = caps.device;
    if (sameRate(actual.sampleRate, rate) && actual.bufferFrames == frames)
        state = AudioDeviceChangeState::Applied;
}
void AudioDeviceChange::poll(Clock::time_point now) {
    if (!pending())
        return;
    try {
        // A read failure must not present a cached format as current hardware.
        actualKnown = false;
        auto caps = control->inspect();
        validateAudioDeviceCapabilities(caps);
        if (caps.device.id != actual.id || caps.device.uid != actual.uid)
            throw Error("Audio device identity changed; refresh Audio Settings");
        actual = caps.device;
        actualKnown = true;
        // After the deadline, a late acknowledgement is not reported as success.
        if (now >= deadline)
            throw Error("Timed out waiting for Core Audio to confirm rate/buffer");
        if (caps.running)
            throw Error("Audio device started running while its format was changing");
        if (phase == Phase::SubmitRate) {
            validateTarget(caps, targetRate, targetFrames);
            phase = Phase::AwaitRate;
            if (!sameRate(actual.sampleRate, targetRate)) {
                mayHaveChanged = true; // Even a rejected driver call can have side effects.
                actualKnown = false;
                control->setSampleRate(targetRate);
                return;
            }
        }
        if (phase == Phase::AwaitRate) {
            if (!sameRate(actual.sampleRate, targetRate) || !control->sampleRateAcknowledged())
                return;
            // Drivers may change their buffer/range when the rate changes.
            validateTarget(caps, targetRate, targetFrames);
            phase = Phase::AwaitBuffer;
            if (actual.bufferFrames != targetFrames) {
                mayHaveChanged = true;
                actualKnown = false;
                control->setBufferFrames(targetFrames);
                return;
            }
        }
        if (!sameRate(actual.sampleRate, targetRate))
            throw Error("Audio device sample rate changed before buffer acknowledgement");
        if (actual.bufferFrames == targetFrames && control->bufferAcknowledged())
            state = AudioDeviceChangeState::Applied;
    } catch (const std::exception &problem) {
        state = AudioDeviceChangeState::Failed;
        error = problem.what();
        if (mayHaveChanged)
            error +=
                ". Hardware may be partially changed; refresh before retrying. No automatic rollback.";
    }
    if (!pending())
        control.reset(); // Unregister HAL listeners on the control thread.
}
#ifndef __APPLE__
std::unique_ptr<AudioDeviceControl> makeAudioDeviceControl(const std::string &uid) {
    validateAudioDeviceUID(uid);
    throw Error("Hardware rate/buffer settings require macOS Core Audio");
}
#endif
}
