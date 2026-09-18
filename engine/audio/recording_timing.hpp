#pragma once
#include "audio/recording_clock.hpp"
#include "domain/session.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace daw {
// Immutable for one take. Buffer/safety are diagnostics: the paired native
// timestamps ALREADY include this scheduling separation. Never add them twice.
struct RecordingLatency {
    bool enabled = false;
    uint32_t deviceID = 0, bufferFrames = 0;
    uint32_t inputDevice = 0, inputStream = 0, inputSafety = 0;
    uint32_t outputDevice = 0, outputStream = 0, outputSafety = 0;
    uint32_t inputStreamID = 0, outputLeftStreamID = 0, outputRightStreamID = 0;
    double hostTicksPerSecond = 0;
    bool operator==(const RecordingLatency&) const = default;
    uint64_t presentationFrames() const noexcept {
        return uint64_t(inputDevice) + inputStream + outputDevice + outputStream;
    }
    void validate() const {
        if (!enabled) return;
        if (!deviceID || !inputStreamID || !outputLeftStreamID || !outputRightStreamID ||
            !bufferFrames || bufferFrames > 4096 || presentationFrames() > 48000 * 2 ||
            inputSafety > 48000 || outputSafety > 48000 ||
            !std::isfinite(hostTicksPerSecond) || hostTicksPerSecond <= 0 || hostTicksPerSecond > 1e12)
            throw Error("Invalid recording latency report; no unverified zero-latency fallback was used");
    }
};
struct RecordingTimingInfo {
    RecordingLatency hardware;
    uint64_t timestampSeparation = 0, compensationFrames = 0, graphFrames = 0;
    uint64_t discardedLeadingFrames = 0, drainRemainingFrames = 0;
    bool ready = false, stopRequested = false, canFinish = true;
};

// Both stamps belong to ONE AudioDeviceIOProc cycle and hence one device sample
// clock (including an explicitly configured aggregate). The host delta is an
// optional consistency check, not callback-arrival time and not a second offset.
class RecordingAlignment {
    RecordingLatency hardware_;
    CaptureClock inputClock_;
    bool ready_ = false;
    uint64_t separation_ = 0;
public:
    explicit RecordingAlignment(RecordingLatency hardware) : hardware_(hardware) { hardware_.validate(); }
    CaptureClockError observe(CaptureTimestamp input, CaptureTimestamp output, uint32_t frames) noexcept {
        if (!hardware_.enabled) return CaptureClockError::none;
        const auto inputError = inputClock_.observe(input, frames);
        if (inputError != CaptureClockError::none) return inputError;
        // Output was independently checked by DuplexCapture's existing clock.
        const double delta = output.sampleTime - input.sampleTime;
        if (!std::isfinite(delta) || delta < 0 || delta > 48000 * 2 ||
            std::abs(delta - std::round(delta)) > CaptureClock::sampleTolerance)
            return CaptureClockError::timestampPairInvalid;
        const auto separation = static_cast<uint64_t>(std::round(delta));
        if (input.hostTimeValid && output.hostTimeValid) {
            if (output.hostTime < input.hostTime) return CaptureClockError::timestampPairInvalid;
            const double hostFrames = double(output.hostTime - input.hostTime) / hardware_.hostTicksPerSecond * 48000.0;
            if (!std::isfinite(hostFrames) || std::abs(hostFrames - delta) > 2.0)
                return CaptureClockError::timestampPairInvalid;
        }
        if (ready_ && separation != separation_) return CaptureClockError::latencyChanged;
        separation_ = separation;
        ready_ = true;
        return CaptureClockError::none;
    }
    uint64_t separation() const noexcept { return separation_; }
};
}
