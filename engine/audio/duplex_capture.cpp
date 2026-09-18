#include "audio/duplex_capture.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<CaptureClockError>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
DuplexCapture::DuplexCapture(Renderer& renderer, const State& state, uint64_t capacity,
                             const std::string& path, uint64_t start, uint64_t loopStart,
                             uint64_t loopEnd, uint64_t preroll, bool monitor, RecordingLatency latency,
                             uint32_t recordingChannels)
    : renderer_(renderer), start_(start), capacity_(capacity), lead_(std::min(preroll, start)),
      loopStart_(loopStart), loopEnd_(loopEnd), channels_(recordingChannels),
      monitor_(monitor), latency_(latency), alignment_(latency) {
    constexpr uint64_t timelineLimit = 48000ULL * 600;
    const bool looping = loopStart != 0 || loopEnd != 0;
    if (!capacity || capacity > 48000ULL * 60 || (channels_ != 1 && channels_ != 2) ||
        start >= timelineLimit || preroll > 48000ULL * 30 ||
        (looping ? (loopEnd <= loopStart || loopEnd > timelineLimit || start != loopStart)
                 : capacity > timelineLimit - start))
        throw Error("Invalid duplex recording range");
    // Prepare before opening a recovery file. No dummy/silent track is added.
    renderer_.prepare(state, start - lead_, loopStart, loopEnd, looping ? loopEnd : start + capacity);
    graphFrames_ = latency_.enabled ? renderer_.masterLatencyFrames() : 0;
    writer_ = std::make_unique<RecordingWriter>(path, start, capacity, 48000 * 2);
}
void DuplexCapture::processStereo(const float* inputLeft, const float* inputRight,
                                  float* left, float* right, uint32_t frames,
                                  CaptureTimestamp time, CaptureTimestamp inputTime) noexcept {
    if (!left || !right) return;
    std::fill_n(left, frames, 0.0f);
    std::fill_n(right, frames, 0.0f);
    if (!inputLeft || !inputRight || !frames || frames > maximumSlice || clockError() != CaptureClockError::none) return;
    const auto elapsed = elapsed_.load(std::memory_order_relaxed);
    uint64_t delay = compensation_.load(std::memory_order_relaxed);
    const auto naturalEnd = lead_ + capacity_;
    auto end = std::min(stopClock_, naturalEnd);
    if (timingReady_.load(std::memory_order_relaxed) && elapsed >= end + delay) return;
    // Keep the full-block validation from recording-clock integrity, including
    // the final callback from which only a prefix is accepted.
    auto error = clock_.observe(time, frames);
    if (error == CaptureClockError::none) error = alignment_.observe(inputTime, time, frames);
    if (error != CaptureClockError::none) {
        clockError_.store(error, std::memory_order_release);
        renderer_.playing.store(false, std::memory_order_release);
        return;
    }
    if (!timingReady_.load(std::memory_order_relaxed)) {
        delay = latency_.enabled ? alignment_.separation() + latency_.presentationFrames() + graphFrames_ : 0;
        separation_.store(alignment_.separation(), std::memory_order_relaxed);
        compensation_.store(delay, std::memory_order_relaxed);
        timingReady_.store(true, std::memory_order_release);
    }
    if (latency_.enabled && stopRequested_.load(std::memory_order_acquire) && stopClock_ == UINT64_MAX) {
        // Cut at the last AUDIBLE graph frame, not the graph's input cursor.
        // No further backing/MON is emitted; only already-emitted input drains.
        stopClock_ = std::min(naturalEnd, elapsed > graphFrames_ ? elapsed - graphFrames_ : 0);
        publishedStopClock_.store(stopClock_, std::memory_order_release);
    }
    end = std::min(stopClock_, naturalEnd);
    const auto captureEnd = end <= lead_ && stopClock_ != UINT64_MAX ? elapsed : end + delay;
    const auto count = static_cast<uint32_t>(std::min<uint64_t>(frames, captureEnd > elapsed ? captureEnd - elapsed : 0));
    // Map to the requested recording interval. Audio earlier than Record In
    // (including pre-zero input) is discarded, never clamped onto frame zero.
    const auto first = std::max(elapsed, lead_ + delay);
    const auto last = std::min(elapsed + count, captureEnd);
    if (last > first) {
        const auto offset = first - elapsed;
        if (channels_ == 2)
            writer_->writeStereo(inputLeft + offset, inputRight + offset, static_cast<uint32_t>(last - first));
        else
            writer_->writeMono(inputLeft + offset, static_cast<uint32_t>(last - first));
    }
    uint32_t audible = 0;
    if (stopClock_ == UINT64_MAX) {
        const auto renderCount = static_cast<uint32_t>(std::min<uint64_t>(count, naturalEnd > elapsed ? naturalEnd - elapsed : 0));
        if (renderCount) renderer_.render(left, right, renderCount);
        const auto tailStart = elapsed + renderCount;
        const auto tailCount = static_cast<uint32_t>(std::min<uint64_t>(count - renderCount,
            naturalEnd + graphFrames_ > tailStart ? naturalEnd + graphFrames_ - tailStart : 0));
        if (tailCount) renderer_.renderTail(left + renderCount, right + renderCount, tailCount, true);
        audible = renderCount + tailCount;
    }
    const float target = monitor_.load(std::memory_order_acquire) ? 1.0f : 0.0f;
    float peak = renderer_.peak.load(std::memory_order_relaxed);
    uint64_t clipped = 0;
    for (uint32_t frame = 0; frame < audible; ++frame) {
        // Direct monitoring bypasses the project FX/PDC path. A 5 ms ramp
        // avoids a gain discontinuity when MON changes during a take.
        constexpr float step = 1.0f / 240.0f;
        monitorGain_ += std::clamp(target - monitorGain_, -step, step);
        const float dryL = std::isfinite(inputLeft[frame]) ? std::clamp(inputLeft[frame], -16.0f, 16.0f) : 0.0f;
        const float dryRSource = channels_ == 2 ? inputRight[frame] : inputLeft[frame];
        const float dryR = std::isfinite(dryRSource) ? std::clamp(dryRSource, -16.0f, 16.0f) : 0.0f;
        const float l = left[frame] + dryL * monitorGain_;
        const float r = right[frame] + dryR * monitorGain_;
        peak = std::max({peak, std::abs(l), std::abs(r)});
        if (std::abs(l) > 1.0f || std::abs(r) > 1.0f) ++clipped;
        left[frame] = std::clamp(l, -1.0f, 1.0f);
        right[frame] = std::clamp(r, -1.0f, 1.0f);
    }
    renderer_.peak.store(peak, std::memory_order_relaxed);
    renderer_.clipped.fetch_add(clipped, std::memory_order_relaxed);
    elapsed_.store(elapsed + count, std::memory_order_release);
    // Hitting a deliberate capture limit is not a dropped callback/ring xrun.
    if (elapsed + count >= captureEnd || stopClock_ != UINT64_MAX)
        renderer_.playing.store(false, std::memory_order_release);
}
DuplexCaptureProgress DuplexCapture::progress() const noexcept {
    const auto elapsed = elapsed_.load(std::memory_order_acquire);
    const auto naturalEnd = lead_ + capacity_;
    const auto end = std::min(publishedStopClock_.load(std::memory_order_acquire), naturalEnd);
    const auto audible = std::min(end, elapsed > graphFrames_ ? elapsed - graphFrames_ : 0);
    const auto delay = compensation_.load(std::memory_order_acquire);
    const bool complete = timingReady_.load(std::memory_order_acquire) && elapsed >= naturalEnd + delay;
    DuplexCaptureProgress result{capacity_, audible < lead_ ? lead_ - audible : 0, start_, complete};
    if (audible < lead_) result.timelineFrame = start_ - lead_ + audible;
    else if (loopEnd_ > loopStart_) result.timelineFrame = loopStart_ + (audible - lead_) % (loopEnd_ - loopStart_);
    else result.timelineFrame = start_ + audible - lead_;
    return result;
}
RecordingTimingInfo DuplexCapture::timing() const noexcept {
    RecordingTimingInfo info;
    info.hardware = latency_;
    info.graphFrames = graphFrames_;
    info.ready = timingReady_.load(std::memory_order_acquire);
    info.timestampSeparation = separation_.load(std::memory_order_acquire);
    info.compensationFrames = compensation_.load(std::memory_order_acquire);
    info.stopRequested = stopRequested_.load(std::memory_order_acquire);
    const auto elapsed = elapsed_.load(std::memory_order_acquire);
    const auto stopClock = publishedStopClock_.load(std::memory_order_acquire);
    const auto end = std::min(stopClock, lead_ + capacity_);
    const auto captureEnd = end + info.compensationFrames;
    info.discardedLeadingFrames = std::min(elapsed, lead_ + info.compensationFrames);
    const bool noAudio = stopClock != UINT64_MAX && end <= lead_;
    info.canFinish = !latency_.enabled || clockError() != CaptureClockError::none || noAudio ||
        (info.ready && elapsed >= captureEnd && (info.stopRequested || progress().complete));
    if (info.ready && !info.canFinish && (stopClock != UINT64_MAX || elapsed >= end + graphFrames_))
        info.drainRemainingFrames = captureEnd > elapsed ? captureEnd - elapsed : 0;
    return info;
}
std::shared_ptr<const Clip> DuplexCapture::finish() {
    if (latency_.enabled && !timing().canFinish)
        throw Error("Recording input has not finished draining; request Stop and keep polling");
    writer_->stopPreserving();
    if (const auto error = clockError(); error != CaptureClockError::none)
        throw Error(captureClockErrorMessage(error));
    if (!writer_->frames()) return {};
    return writer_->finish();
}
}
