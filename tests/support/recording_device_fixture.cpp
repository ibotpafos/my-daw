#include "recording_device_fixture.h"
#include "audio/duplex.hpp"
#include <algorithm>
#include <memory>

namespace {
class DeviceFixture;
DeviceFixture* current = nullptr;
int failure = 0;
daw::RecordingLatency fixtureLatency;
uint32_t fixtureSeparation = 0;
class DeviceFixture final : public daw::Duplex {
    daw::State state_;
    uint64_t capacity_, start_, loopStart_, loopEnd_, preroll_, callbacks_ = 0, clockFrames_ = 0;
    std::string path_;
    bool monitor_, active_ = false;
    uint32_t recordingChannels_ = 1;
    daw::RecordingLatency latency_ = fixtureLatency;
    uint32_t separation_ = fixtureSeparation;
    std::unique_ptr<daw::DuplexCapture> capture_;
public:
    DeviceFixture(const daw::State& state, uint64_t capacity, const std::string& path,
        uint64_t start, uint64_t loopStart, uint64_t loopEnd, uint64_t preroll, bool monitor,
        uint32_t recordingChannels)
        : state_(state), capacity_(capacity), start_(start), loopStart_(loopStart),
          loopEnd_(loopEnd), preroll_(preroll), path_(path), monitor_(monitor),
          recordingChannels_(recordingChannels) {}
    ~DeviceFixture() override { cancel(); if (current == this) current = nullptr; }
    void start() override {
        if (failure == 1) throw daw::Error("Test device refused to start");
        if (current) throw daw::Error("Test already has an active input device");
        capture_ = std::make_unique<daw::DuplexCapture>(renderer, state_, capacity_, path_, start_, loopStart_, loopEnd_, preroll_, monitor_, latency_, recordingChannels_);
        renderer.playing = true; active_ = true; current = this;
    }
    void pumpStereo(const float* inputLeft, const float* inputRight, uint32_t frames, float* left, float* right) {
        if (!active_) throw daw::Error("Fixture is not running");
        daw::CaptureTimestamp time{double(clockFrames_), clockFrames_ + 1, true, true};
        if (failure == 3) time.sampleTime += 1; // one missing sample, not device loss
        if (failure == 4) time.sampleTimeValid = false;
        daw::CaptureTimestamp inputTime{};
        if (latency_.enabled) {
            inputTime = {double(clockFrames_), clockFrames_ + 100000, true, true};
            time.sampleTime += separation_;
            time.hostTime = clockFrames_ + 100000 + separation_;
            if (failure == 5) inputTime.sampleTimeValid = false;
        }
        capture_->processStereo(inputLeft, inputRight, left, right, frames, time, inputTime);
        clockFrames_ += frames; ++callbacks_;
    }
    void requestStop() noexcept override { if (capture_) capture_->requestStop(); }
    daw::RecordingTimingInfo timing() const noexcept override { return capture_ ? capture_->timing() : daw::RecordingTimingInfo{}; }
    std::shared_ptr<const daw::Clip> stop() override {
        active_ = false; renderer.playing = false;
        return capture_->finish();
    }
    void cancel() noexcept override { active_ = false; renderer.playing = false; if (capture_) capture_->cancel(); }
    void markStalled() noexcept override { cancel(); }
    void checkDevices() override { if (capture_ && capture_->clockError() != daw::CaptureClockError::none) { const auto error = capture_->clockError(); cancel(); throw daw::Error(daw::captureClockErrorMessage(error)); } if (failure == 2) { cancel(); throw daw::Error("Test audio device disconnected"); } }
    uint64_t frames() const noexcept override { return capture_ ? capture_->frames() : 0; }
    uint64_t callbacks() const noexcept override { return callbacks_; }
    bool overflowed() const noexcept override { return capture_ && capture_->overflowed(); }
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
    const AudioDeviceConfiguration& config) {
    return std::make_unique<DeviceFixture>(state, capacity, path, start, loopStart, loopEnd, preroll, monitor,
                                           config.recordingChannels);
}
}
extern "C" int recording_fixture_pump(const float* input, uint32_t frames, float* left, float* right) {
    if (!current || !input || !left || !right || !frames || frames > daw::DuplexCapture::maximumSlice) return 1;
    try { current->pumpStereo(input, input, frames, left, right); return 0; } catch (...) { return 1; }
}
extern "C" int recording_fixture_pump_stereo(const float* inputLeft, const float* inputRight,
                                               uint32_t frames, float* left, float* right) {
    if (!current || !inputLeft || !inputRight || !left || !right || !frames ||
        frames > daw::DuplexCapture::maximumSlice) return 1;
    try { current->pumpStereo(inputLeft, inputRight, frames, left, right); return 0; } catch (...) { return 1; }
}
extern "C" void recording_fixture_failure(int mode) { failure = mode; }
extern "C" int recording_fixture_active() { return current != nullptr; }

extern "C" int recording_fixture_latency(uint32_t separation, uint32_t input, uint32_t output) {
    if (current || separation > 96000 || input > 48000 || output > 48000) return 1;
    fixtureLatency = {};
    fixtureLatency.enabled = true;
    fixtureLatency.deviceID = 7; fixtureLatency.bufferFrames = 256;
    fixtureLatency.inputStreamID = 11; fixtureLatency.outputLeftStreamID = fixtureLatency.outputRightStreamID = 12;
    fixtureLatency.inputDevice = input; fixtureLatency.outputDevice = output;
    fixtureLatency.inputSafety = 17; fixtureLatency.outputSafety = 29;
    fixtureLatency.hostTicksPerSecond = 48000;
    fixtureSeparation = separation;
    return 0;
}
extern "C" int recording_fixture_no_latency() {
    if (current) return 1;
    fixtureLatency = {}; fixtureSeparation = 0; return 0;
}
