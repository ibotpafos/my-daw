#pragma once
#include "audio/device.hpp"
#include <chrono>
#include <memory>

namespace daw {
struct AudioValueRange {
    double minimum = 0, maximum = 0;
};
struct AudioDeviceCapabilities {
    AudioDeviceInfo device;
    std::vector<AudioValueRange> sampleRates;
    AudioValueRange bufferRange;
    bool rateWritable = false, bufferWritable = false, running = false;
};
// Thin HAL adapter. The state machine is identical in production and fixtures.
// All methods run on the owning control thread, never in an audio callback.
class AudioDeviceControl {
  public:
    virtual ~AudioDeviceControl() = default;
    virtual AudioDeviceCapabilities inspect() = 0;
    virtual void setSampleRate(double) = 0;
    virtual void setBufferFrames(uint32_t) = 0;
    virtual bool sampleRateAcknowledged() const = 0;
    virtual bool bufferAcknowledged() const = 0;
};
std::unique_ptr<AudioDeviceControl> makeAudioDeviceControl(const std::string &uid);
void validateAudioDeviceUID(const std::string &uid);
void validateAudioDeviceRequest(double sampleRate, uint32_t frames);
void validateAudioDeviceCapabilities(const AudioDeviceCapabilities &);

enum class AudioDeviceChangeState : uint32_t { Idle, Pending, Applied, Failed };
class AudioDeviceChange {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto timeout = std::chrono::seconds(2);
    AudioDeviceChange(std::unique_ptr<AudioDeviceControl>, double rate, uint32_t frames,
                      Clock::time_point now);
    void poll(Clock::time_point now);
    bool pending() const {
        return state == AudioDeviceChangeState::Pending;
    }
    AudioDeviceChangeState state = AudioDeviceChangeState::Pending;
    AudioDeviceInfo actual;
    bool actualKnown = true, mayHaveChanged = false;
    std::string error;

  private:
    enum class Phase { SubmitRate, AwaitRate, AwaitBuffer };
    std::unique_ptr<AudioDeviceControl> control;
    double targetRate;
    uint32_t targetFrames;
    Clock::time_point deadline;
    Phase phase = Phase::SubmitRate;
};
}
