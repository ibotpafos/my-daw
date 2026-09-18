#include "recording_device_fixture.h"
#include "audio/duplex.hpp"
#include <algorithm>
#include <memory>

namespace {
class DeviceFixture;
DeviceFixture* current = nullptr;
int failure = 0;
class DeviceFixture final : public daw::Duplex {
    daw::State state_;
    uint64_t capacity_, start_, loopStart_, loopEnd_, preroll_, callbacks_ = 0;
    std::string path_;
    bool monitor_, active_ = false;
    double nextSample_ = 0;
    uint64_t nextHost_ = 1;
    std::unique_ptr<daw::DuplexCapture> capture_;
public:
    DeviceFixture(const daw::State& state, uint64_t capacity, const std::string& path,
        uint64_t start, uint64_t loopStart, uint64_t loopEnd, uint64_t preroll, bool monitor)
        : state_(state), capacity_(capacity), start_(start), loopStart_(loopStart),
          loopEnd_(loopEnd), preroll_(preroll), path_(path), monitor_(monitor) {}
    ~DeviceFixture() override { cancel(); if (current == this) current = nullptr; }
    void start() override {
        if (failure == 1) throw daw::Error("Test device refused to start");
        if (current) throw daw::Error("Test already has an active input device");
        capture_ = std::make_unique<daw::DuplexCapture>(renderer, state_, capacity_, path_, start_, loopStart_, loopEnd_, preroll_, monitor_);
        renderer.playing = true; active_ = true; current = this;
    }
    void pump(const float* input, uint32_t frames, float* left, float* right) {
        if (!active_) throw daw::Error("Fixture is not running");
        pumpTimed({nextSample_, nextHost_, 3}, input, frames, left, right);
    }
    bool pumpTimed(const daw::CaptureTimestamp& stamp, const float* input, uint32_t frames, float* left, float* right) {
        if (!active_) throw daw::Error("Fixture is not running");
        const bool accepted = capture_->processTimed(stamp, input, left, right, frames);
        nextSample_ += frames; nextHost_ += frames; ++callbacks_;
        return accepted;
    }
    std::shared_ptr<const daw::Clip> stop() override {
        active_ = false; renderer.playing = false;
        return capture_->finish();
    }
    void cancel() noexcept override { active_ = false; renderer.playing = false; if (capture_) capture_->cancel(); }
    void markStalled() noexcept override { cancel(); }
    void checkDevices() override {
        if (capture_ && capture_->clockReport().fault != daw::CaptureClockFault::none) {
            cancel(); throw daw::Error("Recording clock discontinuity; captured prefix remains recoverable");
        }
        if (failure == 2) { cancel(); throw daw::Error("Test audio device disconnected"); } }
    uint64_t frames() const noexcept override { return capture_ ? capture_->frames() : 0; }
    uint64_t callbacks() const noexcept override { return callbacks_; }
    bool overflowed() const noexcept override { return capture_ && capture_->overflowed(); }
    daw::CaptureClockReport clockReport() const noexcept override { return capture_ ? capture_->clockReport() : daw::CaptureClockReport{}; }
    daw::DuplexCaptureProgress progress() const noexcept override { return capture_ ? capture_->progress() : daw::DuplexCaptureProgress{}; }
    void setMonitor(bool on) noexcept override { monitor_ = on; if (capture_) capture_->setMonitor(on); }
    void discardRecovery() noexcept override { if (capture_) capture_->discard(); }
    daw::OutputTelemetry telemetry() const noexcept override {
        return {active_ ? daw::OutputState::running : daw::OutputState::stopped, 0, 1, callbacks_, 0};
    }
};
}
namespace daw {
// Link-time replacement of ONLY the hardware factory in these executables.
// Production has no environment switch, hook or fake-device C API. The real
// bridge, renderer, capture processor, writer, storage and export are linked.
std::unique_ptr<Duplex> makeDuplex(const State& state, uint64_t capacity, const std::string& path,
    uint64_t start, uint64_t loopStart, uint64_t loopEnd, uint64_t preroll, bool monitor,
    const AudioDeviceConfiguration&) {
    return std::make_unique<DeviceFixture>(state, capacity, path, start, loopStart, loopEnd, preroll, monitor);
}
}
extern "C" int recording_fixture_pump(const float* input, uint32_t frames, float* left, float* right) {
    if (!current || !input || !left || !right || !frames || frames > daw::DuplexCapture::maximumSlice) return 1;
    try { current->pump(input, frames, left, right); return 0; } catch (...) { return 1; }
}
extern "C" void recording_fixture_failure(int mode) { failure = mode; }
extern "C" int recording_fixture_active() { return current != nullptr; }

extern "C" int recording_fixture_pump_timestamped(const float* input, uint32_t frames, float* left,
    float* right, double sampleTime, uint64_t hostTime, uint32_t flags) {
    if (!current || !input || !left || !right || !frames || frames > daw::DuplexCapture::maximumSlice) return 1;
    try { return current->pumpTimed({sampleTime, hostTime, flags}, input, frames, left, right) ? 0 : 1; }
    catch (...) { return 1; }
}
