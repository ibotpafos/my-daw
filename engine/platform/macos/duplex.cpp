#include "audio/duplex.hpp"
#include "platform/macos/audio_device.hpp"
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <thread>

namespace daw {
namespace {
void checkedDuplex(OSStatus status, const char* action) {
    if (status != noErr) throw Error(std::string(action) + " (Core Audio " + std::to_string(status) + ")");
}
class MacDuplex final : public Duplex {
    struct Context {
        std::atomic<MacDuplex*> owner{nullptr};
        std::atomic<uint64_t> entered{0};
    };
    static_assert(std::atomic<MacDuplex*>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    State snapshot_;
    AudioDeviceConfiguration configuration_;
    AudioDeviceInfo opened_;
    DuplexHardwareProfile profile_;
    uint64_t capacity_, start_, loopStart_, loopEnd_, preroll_;
    bool monitor_ = false, active_ = false;
    std::string path_;
    AudioDeviceIOProcID proc_ = nullptr;
    std::unique_ptr<Context> context_;
    std::unique_ptr<DuplexCapture> capture_;
    std::array<float, DuplexCapture::maximumSlice> inputLeft_{}, inputRight_{}, left_{}, right_{};
    std::atomic<uint64_t> callbacks_{0}, errors_{0};
    OutputState state_ = OutputState::idle;
    uint64_t generation_ = 0;

    static void silence(AudioBufferList* output) noexcept {
        if (!output || output->mNumberBuffers > 128) return;
        for (UInt32 b = 0; b < output->mNumberBuffers; ++b)
            if (output->mBuffers[b].mData)
                std::memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);
    }
    static CaptureTimestamp timestamp(const AudioTimeStamp* native) noexcept {
        CaptureTimestamp value;
        if (!native) return value;
        value.sampleTimeValid = (native->mFlags & kAudioTimeStampSampleTimeValid) != 0;
        value.hostTimeValid = (native->mFlags & kAudioTimeStampHostTimeValid) != 0;
        if (value.sampleTimeValid) value.sampleTime = native->mSampleTime;
        if (value.hostTimeValid) value.hostTime = native->mHostTime;
        return value;
    }
    static bool shape(const AudioBufferList* buffers, const std::vector<uint32_t>& expected) noexcept {
        if (!buffers || buffers->mNumberBuffers != expected.size()) return false;
        for (UInt32 b = 0; b < buffers->mNumberBuffers; ++b)
            if (buffers->mBuffers[b].mNumberChannels != expected[b]) return false;
        return true;
    }
    static bool selected(const AudioBufferList* buffers, RecordingChannel channel, uint32_t& frames) noexcept {
        const auto& buffer = buffers->mBuffers[channel.buffer];
        return buffer.mData && recordingBufferFrames(buffer.mDataByteSize, channel.stride, frames);
    }
    void process(const AudioBufferList* input, const AudioTimeStamp* inputTime,
                 AudioBufferList* output, const AudioTimeStamp* outputTime) noexcept {
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        if (errors_.load(std::memory_order_acquire) || !capture_ || capture_->progress().complete ||
            capture_->clockError() != CaptureClockError::none) { silence(output); return; }
        uint32_t inLeftFrames = 0, inRightFrames = 0, leftFrames = 0, rightFrames = 0;
        if (!shape(input, profile_.inputBuffers) || !shape(output, profile_.outputBuffers) ||
            !selected(input, profile_.inputLeft, inLeftFrames) ||
            !selected(input, profile_.inputRight, inRightFrames) ||
            !selected(output, profile_.left, leftFrames) ||
            !selected(output, profile_.right, rightFrames) ||
            inLeftFrames != inRightFrames || inLeftFrames != leftFrames || inLeftFrames != rightFrames) {
            silence(output);
            errors_.fetch_add(1, std::memory_order_release);
            renderer.playing.store(false, std::memory_order_release);
            return;
        }
        const auto* sourceLeft = static_cast<const float*>(input->mBuffers[profile_.inputLeft.buffer].mData);
        const auto* sourceRight = static_cast<const float*>(input->mBuffers[profile_.inputRight.buffer].mData);
        for (uint32_t f = 0; f < inLeftFrames; ++f) {
            inputLeft_[f] = sourceLeft[f * profile_.inputLeft.stride + profile_.inputLeft.channel];
            inputRight_[f] = profile_.recordingChannels == 2
                ? sourceRight[f * profile_.inputRight.stride + profile_.inputRight.channel]
                : inputLeft_[f];
        }
        silence(output);
        capture_->processStereo(inputLeft_.data(), inputRight_.data(), left_.data(), right_.data(),
                                inLeftFrames, timestamp(outputTime), timestamp(inputTime));
        auto* left = static_cast<float*>(output->mBuffers[profile_.left.buffer].mData);
        auto* right = static_cast<float*>(output->mBuffers[profile_.right.buffer].mData);
        for (uint32_t f = 0; f < inFrames; ++f) {
            left[f * profile_.left.stride + profile_.left.channel] = left_[f];
            right[f * profile_.right.stride + profile_.right.channel] = right_[f];
        }
    }
    static OSStatus callback(AudioDeviceID, const AudioTimeStamp*, const AudioBufferList* input,
                             const AudioTimeStamp* inputTime, AudioBufferList* output,
                             const AudioTimeStamp* outputTime, void* ref) noexcept {
        auto& context = *static_cast<Context*>(ref);
        context.entered.fetch_add(1, std::memory_order_seq_cst);
        if (auto* owner = context.owner.load(std::memory_order_seq_cst))
            owner->process(input, inputTime, output, outputTime);
        else silence(output);
        context.entered.fetch_sub(1, std::memory_order_seq_cst);
        return noErr; // AudioDeviceIOProc return value is unused by HAL.
    }
    bool shutdown(OutputState reason) noexcept {
        active_ = false;
        renderer.playing.store(false, std::memory_order_release);
        if (context_) context_->owner.store(nullptr, std::memory_order_seq_cst);
        bool stopped = true;
        if (proc_) {
            const auto stopStatus = AudioDeviceStop(opened_.id, proc_);
            const auto destroyStatus = AudioDeviceDestroyIOProcID(opened_.id, proc_);
            stopped = stopStatus == noErr && destroyStatus == noErr;
            // Wait only on the control thread. The RT callback never waits.
            while (context_->entered.load(std::memory_order_seq_cst)) std::this_thread::yield();
            if (destroyStatus != noErr) {
                // A defective driver may retain this registration. Leave only
                // its detached 2-atomic sentinel alive, never the DAW/session.
                (void)context_.release();
            } else context_.reset();
            proc_ = nullptr;
        }
        state_ = reason;
        return stopped;
    }
public:
    MacDuplex(const State& state, uint64_t capacity, std::string path, uint64_t start,
              uint64_t loopBegin, uint64_t loopFinish, uint64_t preroll, bool monitor,
              const AudioDeviceConfiguration& config)
        : snapshot_(state), configuration_(config), capacity_(capacity), start_(start),
          loopStart_(loopBegin), loopEnd_(loopFinish), preroll_(preroll), monitor_(monitor), path_(std::move(path)) {}
    ~MacDuplex() override { cancel(); }
    void start() override {
        if (active_) throw Error("Recording is already active");
        try {
            opened_ = openAudioDevice(configuration_, AudioDeviceDirection::Input);
            const auto output = openAudioDevice(configuration_, AudioDeviceDirection::Output);
            if (opened_.id != output.id)
                throw Error("Recording requires one input/output device or a Core Audio aggregate. Select it in Audio Settings.");
            profile_ = readDuplexHardwareProfile(opened_, configuration_);
            capture_ = std::make_unique<DuplexCapture>(renderer, snapshot_, capacity_, path_, start_,
                loopStart_, loopEnd_, preroll_, monitor_, profile_.latency, profile_.recordingChannels);
            context_ = std::make_unique<Context>();
            context_->owner.store(this, std::memory_order_release);
            checkedDuplex(AudioDeviceCreateIOProcID(opened_.id, callback, context_.get(), &proc_), "Create timestamped recording IO");
            if (readDuplexHardwareProfile(opened_, configuration_) != profile_)
                throw Error("Recording latency changed during preparation; retry recording");
            renderer.playing.store(true, std::memory_order_release);
            checkedDuplex(AudioDeviceStart(opened_.id, proc_), "Start timestamped recording IO");
            active_ = true; ++generation_; state_ = OutputState::running;
        } catch (...) { shutdown(OutputState::stopped); capture_.reset(); throw; }
    }
    void requestStop() noexcept override { if (capture_) capture_->requestStop(); }
    RecordingTimingInfo timing() const noexcept override { return capture_ ? capture_->timing() : RecordingTimingInfo{}; }
    std::shared_ptr<const Clip> stop() override {
        if (!active_) throw Error("Recording is not active");
        checkDevices();
        if (capture_->clockError() == CaptureClockError::none && !errors_.load() && !capture_->timing().canFinish)
            throw Error("Recording input is draining; poll timing before finalizing");
        const bool clean = shutdown(OutputState::stopped);
        if (!clean || errors_.load()) {
            capture_->cancel();
            throw Error("Recording IO failed; confirmed audio remains recoverable");
        }
        return capture_->finish();
    }
    void cancel() noexcept override { shutdown(OutputState::stopped); if (capture_) capture_->cancel(); }
    void markStalled() noexcept override { shutdown(OutputState::stalled); if (capture_) capture_->cancel(); }
    void checkDevices() override {
        if (!active_) return;
        if (capture_ && capture_->clockError() != CaptureClockError::none) {
            const auto error = capture_->clockError(); shutdown(OutputState::callbackError);
            throw Error(captureClockErrorMessage(error));
        }
        if (errors_.load()) { shutdown(OutputState::callbackError); throw Error("Recording callback received incompatible audio buffers"); }
        try {
            checkAudioDevice(opened_, configuration_.inputUID.empty(), AudioDeviceDirection::Input);
            checkAudioDevice(opened_, configuration_.outputUID.empty(), AudioDeviceDirection::Output);
            if (readDuplexHardwareProfile(opened_, configuration_) != profile_)
                throw Error("Recording latency or stream layout changed; confirmed audio remains recoverable");
        } catch (...) { shutdown(OutputState::deviceLost); throw; }
    }
    uint64_t frames() const noexcept override { return capture_ ? capture_->frames() : 0; }
    uint64_t callbacks() const noexcept override { return callbacks_.load(std::memory_order_relaxed); }
    bool overflowed() const noexcept override { return errors_.load() || (capture_ && capture_->overflowed()); }
    DuplexCaptureProgress progress() const noexcept override { return capture_ ? capture_->progress() : DuplexCaptureProgress{}; }
    void setMonitor(bool on) noexcept override { monitor_ = on; if (capture_) capture_->setMonitor(on); }
    void discardRecovery() noexcept override { if (capture_) capture_->discard(); }
    OutputTelemetry telemetry() const noexcept override {
        return {state_, active_ ? opened_.id : 0, generation_, renderer.callbacks.load(), errors_.load()};
    }
};
}
std::unique_ptr<Duplex> makeDuplex(const State& state, uint64_t capacity, const std::string& path,
                                  uint64_t start, uint64_t loopStart, uint64_t loopEnd,
                                  uint64_t preroll, bool monitor, const AudioDeviceConfiguration& config) {
    return std::make_unique<MacDuplex>(state, capacity, path, start, loopStart, loopEnd, preroll, monitor, config);
}
}
