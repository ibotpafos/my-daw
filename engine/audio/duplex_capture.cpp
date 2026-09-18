#include "audio/duplex_capture.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
DuplexCapture::DuplexCapture(Renderer& renderer, const State& state, uint64_t capacity,
                             const std::string& path, uint64_t start, uint64_t loopStart,
                             uint64_t loopEnd, uint64_t preroll, bool monitor)
    : renderer_(renderer), start_(start), capacity_(capacity), lead_(std::min(preroll, start)),
      loopStart_(loopStart), loopEnd_(loopEnd), monitor_(monitor) {
    constexpr uint64_t timelineLimit = 48000ULL * 600;
    const bool looping = loopStart != 0 || loopEnd != 0;
    if (!capacity || capacity > 48000ULL * 60 || start >= timelineLimit || preroll > 48000ULL * 30 ||
        (looping ? (loopEnd <= loopStart || loopEnd > timelineLimit || start != loopStart)
                 : capacity > timelineLimit - start))
        throw Error("Invalid duplex recording range");
    // Prepare before opening a recovery file. No dummy/silent track is added.
    renderer_.prepare(state, start - lead_, loopStart, loopEnd, looping ? loopEnd : start + capacity);
    writer_ = std::make_unique<RecordingWriter>(path, start, capacity, 48000 * 2, lead_);
}
void DuplexCapture::process(const float* input, float* left, float* right, uint32_t frames) noexcept {
    if (!left || !right) return; // HAL adapter validates its actual buffer list.
    std::fill_n(left, frames, 0.0f);
    std::fill_n(right, frames, 0.0f);
    if (!input || !frames || frames > maximumSlice || clock_.fault() != CaptureClockFault::none) return;
    const auto elapsed = elapsed_.load(std::memory_order_relaxed);
    const auto remaining = lead_ + capacity_ - elapsed;
    const auto count = static_cast<uint32_t>(std::min<uint64_t>(frames, remaining));
    if (!count) return;
    // Only the dry device input reaches disk: no backing, click or monitor sum.
    writer_->writeMono(input, count);
    renderer_.render(left, right, count);
    const float target = monitor_.load(std::memory_order_acquire) ? 1.0f : 0.0f;
    float peak = renderer_.peak.load(std::memory_order_relaxed);
    uint64_t clipped = 0;
    for (uint32_t frame = 0; frame < count; ++frame) {
        // Direct monitoring bypasses the project FX/PDC path. A 5 ms ramp
        // avoids a gain discontinuity when MON changes during a take.
        constexpr float step = 1.0f / 240.0f;
        monitorGain_ += std::clamp(target - monitorGain_, -step, step);
        const float dry = std::isfinite(input[frame]) ? std::clamp(input[frame], -16.0f, 16.0f) : 0.0f;
        const float l = left[frame] + dry * monitorGain_;
        const float r = right[frame] + dry * monitorGain_;
        peak = std::max({peak, std::abs(l), std::abs(r)});
        if (std::abs(l) > 1.0f || std::abs(r) > 1.0f) ++clipped;
        left[frame] = std::clamp(l, -1.0f, 1.0f);
        right[frame] = std::clamp(r, -1.0f, 1.0f);
    }
    renderer_.peak.store(peak, std::memory_order_relaxed);
    renderer_.clipped.fetch_add(clipped, std::memory_order_relaxed);
    elapsed_.store(elapsed + count, std::memory_order_release);
    // Hitting a deliberate capture limit is not a dropped callback/ring xrun.
    if (elapsed + count == lead_ + capacity_) renderer_.playing.store(false, std::memory_order_release);
}
bool DuplexCapture::processTimed(const CaptureTimestamp& time, const float* input,
                                  float* left, float* right, uint32_t frames) noexcept {
    // Pointers and storage sizes belong to the adapter. Do not write an invalid
    // span; known valid HAL buffers are silenced by the adapter on any failure.
    if (!input || !left || !right || !frames || frames > maximumSlice) return false;
    if (progress().complete && clock_.fault() == CaptureClockFault::none) {
        std::fill_n(left, frames, 0.0f); std::fill_n(right, frames, 0.0f);
        return true;
    }
    if (!clock_.accept(time, frames)) {
        std::fill_n(left, frames, 0.0f); std::fill_n(right, frames, 0.0f);
        renderer_.playing.store(false, std::memory_order_release);
        return false;
    }
    process(input, left, right, frames);
    return true;
}
DuplexCaptureProgress DuplexCapture::progress() const noexcept {
    const auto elapsed = elapsed_.load(std::memory_order_acquire);
    DuplexCaptureProgress result{capacity_, elapsed < lead_ ? lead_ - elapsed : 0, start_, elapsed == lead_ + capacity_};
    if (elapsed < lead_) result.timelineFrame = start_ - lead_ + elapsed;
    else if (loopEnd_ > loopStart_) result.timelineFrame = loopStart_ + (elapsed - lead_) % (loopEnd_ - loopStart_);
    else result.timelineFrame = start_ + elapsed - lead_;
    return result;
}
std::shared_ptr<const Clip> DuplexCapture::finish() {
    writer_->stopPreserving();
    if (clock_.fault() != CaptureClockFault::none)
        throw Error("Recording clock discontinuity: recording stopped; the confirmed prefix remains recoverable");
    if (!writer_->frames()) return {};
    return writer_->finish();
}
}
