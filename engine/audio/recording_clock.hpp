#pragma once
#include <cmath>
#include <cstdint>

namespace daw {
// Only representations marked valid by the hardware adapter may be inspected.
// This is the callback/request clock, not an acoustic latency measurement.
struct CaptureTimestamp {
    double sampleTime = 0;
    uint64_t hostTime = 0;
    bool sampleTimeValid = false;
    bool hostTimeValid = false;
};
enum class CaptureClockError : uint32_t { none, sampleTimeUnavailable, sampleTimeInvalid, discontinuity, hostTimeReversed, timestampPairInvalid, latencyChanged };

inline const char* captureClockErrorMessage(CaptureClockError error) noexcept {
    switch (error) {
    case CaptureClockError::sampleTimeUnavailable:
        return "Recording stopped: the audio device supplied no valid sample timestamp. Confirmed audio remains recoverable.";
    case CaptureClockError::sampleTimeInvalid:
        return "Recording stopped: the audio device supplied an invalid sample timestamp. Confirmed audio remains recoverable.";
    case CaptureClockError::discontinuity:
        return "Recording stopped: the audio sample clock skipped or repeated frames. Confirmed audio remains recoverable.";
    case CaptureClockError::hostTimeReversed:
        return "Recording stopped: the audio host clock moved backwards or repeated. Confirmed audio remains recoverable.";
    case CaptureClockError::timestampPairInvalid:
        return "Recording stopped: input/output timestamps cannot be aligned. Confirmed audio remains recoverable.";
    case CaptureClockError::latencyChanged:
        return "Recording stopped: input/output timing changed during the take. Confirmed audio remains recoverable.";
    case CaptureClockError::none: return "";
    }
    return "Recording clock error";
}

// Single callback producer; no allocation, locks, wall-clock reads or I/O.
// Anchor at the first absolute sampleTime (which may be negative/fractional).
// Comparing to that anchor, rather than the previous timestamp, prevents small
// tolerated rounding errors accumulating into unnoticed clock drift.
class CaptureClock {
    double anchor_ = 0;
    uint64_t elapsed_ = 0, lastHost_ = 0;
    bool anchored_ = false, hasHost_ = false;
    CaptureClockError error_ = CaptureClockError::none;
public:
    static constexpr double sampleTolerance = 0.001; // strictly below one sample
    static constexpr double maximumSampleTime = 0x1p52;
    CaptureClockError observe(CaptureTimestamp time, uint32_t frames) noexcept {
        if (error_ != CaptureClockError::none || frames == 0) return error_;
        if (!time.sampleTimeValid) return error_ = CaptureClockError::sampleTimeUnavailable;
        if (!std::isfinite(time.sampleTime) || std::abs(time.sampleTime) > maximumSampleTime - frames ||
            elapsed_ > static_cast<uint64_t>(maximumSampleTime) - frames)
            return error_ = CaptureClockError::sampleTimeInvalid;
        if (anchored_ && std::abs(time.sampleTime - (anchor_ + static_cast<double>(elapsed_))) > sampleTolerance)
            return error_ = CaptureClockError::discontinuity;
        // Host time is optional. Never invent it from callback arrival time or
        // compare unflagged bytes. Resume comparison with the last VALID host.
        if (time.hostTimeValid && hasHost_ && time.hostTime <= lastHost_)
            return error_ = CaptureClockError::hostTimeReversed;
        if (!anchored_) { anchor_ = time.sampleTime; anchored_ = true; }
        if (time.hostTimeValid) { lastHost_ = time.hostTime; hasHost_ = true; }
        elapsed_ += frames;
        return error_;
    }
};
}
