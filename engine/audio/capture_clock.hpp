#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace daw {
// AUHAL callback time, not the wall-clock time at which the UI polls. Host ticks
// remain in the platform timebase: never label them nanoseconds or audio frames.
struct CaptureTimestamp {
    enum : uint32_t { sampleValid = 1, hostValid = 2 };
    double sampleTime = 0;
    uint64_t hostTime = 0;
    uint32_t flags = 0;
};
enum class CaptureClockFault : uint32_t {
    none = 0, invalidTimestamp = 1, sampleDiscontinuity = 2, hostDiscontinuity = 3
};
struct CaptureClockReport {
    uint64_t validatedFrames = 0, firstHostTime = 0;
    double firstSampleTime = 0, expectedSampleTime = 0, observedSampleTime = 0;
    uint32_t initialFlags = 0;
    CaptureClockFault fault = CaptureClockFault::none;
};
// One audio-thread writer; arbitrary control-thread readers. Initial anchors
// and failure details are each written once and release-published. No retry
// loops, locks, allocation, host-clock queries or logging on the audio thread.
class CaptureClock final {
    bool started_ = false, haveHost_ = false;
    double expected_ = 0, firstSample_ = 0, faultExpected_ = 0, faultObserved_ = 0;
    uint64_t lastHost_ = 0, firstHost_ = 0, validated_ = 0;
    uint32_t firstFlags_ = 0;
    std::atomic<bool> anchored_{false};
    std::atomic<uint64_t> publishedFrames_{0};
    std::atomic<CaptureClockFault> fault_{CaptureClockFault::none};

    bool fail(CaptureClockFault code, double observed) noexcept {
        faultExpected_ = started_ ? expected_ : 0;
        faultObserved_ = std::isfinite(observed) ? observed : 0;
        fault_.store(code, std::memory_order_release);
        return false;
    }
public:
    bool accept(const CaptureTimestamp& time, uint32_t frames) noexcept {
        if (fault_.load(std::memory_order_relaxed) != CaptureClockFault::none) return false;
        // Retain sub-frame precision, including legal negative start times.
        constexpr double bound = 0x1p50;
        if (!frames || frames > 4096 || !(time.flags & CaptureTimestamp::sampleValid) ||
            (time.flags & ~(CaptureTimestamp::sampleValid | CaptureTimestamp::hostValid)) ||
            !std::isfinite(time.sampleTime) || std::abs(time.sampleTime) > bound ||
            validated_ > UINT64_MAX - frames)
            return fail(CaptureClockFault::invalidTimestamp, time.sampleTime);
        // Different callback sizes are legal. A missing or repeated frame is
        // not: do not concatenate later audio onto an earlier timeline position.
        if (started_ && std::abs(time.sampleTime - expected_) > 0.25)
            return fail(CaptureClockFault::sampleDiscontinuity, time.sampleTime);
        if ((time.flags & CaptureTimestamp::hostValid) && haveHost_ && time.hostTime <= lastHost_)
            return fail(CaptureClockFault::hostDiscontinuity, time.sampleTime);
        if (!started_) {
            firstSample_ = time.sampleTime;
            firstHost_ = time.flags & CaptureTimestamp::hostValid ? time.hostTime : 0;
            firstFlags_ = time.flags;
            started_ = true;
            anchored_.store(true, std::memory_order_release);
        }
        if (time.flags & CaptureTimestamp::hostValid) {
            lastHost_ = time.hostTime;
            haveHost_ = true;
        }
        validated_ += frames;
        expected_ = firstSample_ + static_cast<double>(validated_);
        publishedFrames_.store(validated_, std::memory_order_release);
        return true;
    }
    CaptureClockFault fault() const noexcept { return fault_.load(std::memory_order_acquire); }
    CaptureClockReport report() const noexcept {
        CaptureClockReport result;
        // Observe failure first: an acquired fault also orders all preceding
        // anchor/counter publications, so fault reports cannot lack that prefix.
        result.fault = fault();
        if (anchored_.load(std::memory_order_acquire)) {
            result.firstSampleTime = firstSample_;
            result.firstHostTime = firstHost_;
            result.initialFlags = firstFlags_;
            result.validatedFrames = publishedFrames_.load(std::memory_order_acquire);
        }
        if (result.fault != CaptureClockFault::none) {
            result.expectedSampleTime = faultExpected_;
            result.observedSampleTime = faultObserved_;
        }
        return result;
    }
};
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<CaptureClockFault>::is_always_lock_free);
} // namespace daw
