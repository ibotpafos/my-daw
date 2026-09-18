#pragma once
#include "audio/recording.hpp"
#include "audio/recording_clock.hpp"
#include "audio/renderer.hpp"

namespace daw {
struct DuplexCaptureProgress {
    uint64_t capacity = 0, prerollRemaining = 0, timelineFrame = 0;
    bool complete = false;
};
// Shared production callback body, independent of HAL. The I/O adapter must
// stop/quiesce its callback before finish/cancel/destruction. One producer;
// only the monitor target and published progress are accessed concurrently.
class DuplexCapture {
    Renderer& renderer_;
    std::unique_ptr<RecordingWriter> writer_;
    uint64_t start_ = 0, capacity_ = 0, lead_ = 0, loopStart_ = 0, loopEnd_ = 0;
    std::atomic<uint64_t> elapsed_{0};
    std::atomic<bool> monitor_{false};
    float monitorGain_ = 0;
    CaptureClock clock_;
    std::atomic<CaptureClockError> clockError_{CaptureClockError::none};
public:
    static constexpr uint32_t maximumSlice = 4096;
    DuplexCapture(Renderer&, const State&, uint64_t capacity, const std::string& path,
                  uint64_t start, uint64_t loopStart, uint64_t loopEnd,
                  uint64_t preroll, bool monitor);
    // Valid buffers, frames <= maximumSlice. Allocation/file I/O-free.
    void process(const float* input, float* left, float* right, uint32_t frames, CaptureTimestamp time) noexcept;
    CaptureClockError clockError() const noexcept { return clockError_.load(std::memory_order_acquire); }
    void setMonitor(bool on) noexcept { monitor_.store(on, std::memory_order_release); }
    DuplexCaptureProgress progress() const noexcept;
    uint64_t frames() const noexcept { return writer_->frames(); }
    bool overflowed() const noexcept { return writer_->overflowed(); }
    // A stopped pre-roll returns nullptr: no empty clip and no project edit.
    std::shared_ptr<const Clip> finish();
    void cancel() noexcept { writer_->stopPreserving(); }
    void discard() noexcept { writer_->discard(); }
};
}
