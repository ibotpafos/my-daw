#pragma once
#include "audio/device.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace daw {
// Process-local exclusion. These leases are created/destroyed on control paths,
// never in render callbacks. A hardware edit cannot overlap any HAL owner,
// including a graph being prepared for another session.
class AudioIOLease {
public:
    AudioIOLease();
    ~AudioIOLease();
    AudioIOLease(const AudioIOLease&) = delete;
    AudioIOLease& operator=(const AudioIOLease&) = delete;
};
class AudioHardwareLease {
public:
    AudioHardwareLease();
    ~AudioHardwareLease();
    AudioHardwareLease(const AudioHardwareLease&) = delete;
    AudioHardwareLease& operator=(const AudioHardwareLease&) = delete;
};
bool audioHardwareChangeActive() noexcept;

struct AudioHardwareSettings {
    uint32_t deviceID = 0;
    std::string uid;
    double sampleRate = 0;
    uint32_t bufferFrames = 0, minimumBuffer = 0, maximumBuffer = 0;
    bool supports48k = false, rateWritable = false, bufferWritable = false;
};
// Narrow adapter, shared transaction tests use an explicit fake driver. There
// is no fixture mode or arbitrary backend injection in the public C interface.
class AudioHardwareBackend {
public:
    virtual ~AudioHardwareBackend() = default;
    virtual AudioHardwareSettings read() = 0;
    virtual void setRate(double) = 0;
    virtual void setBuffer(uint32_t) = 0;
    virtual void wait() = 0; // 10 ms on production worker; virtual time in tests
};
AudioHardwareSettings readAudioHardwareSettings(const std::string& uid);
std::unique_ptr<AudioHardwareBackend> makeAudioHardwareBackend(const AudioHardwareSettings& expected);
void validateAudioHardwareRequest(const AudioHardwareSettings&, uint32_t buffer);
struct AudioHardwareOutcome {
    bool success = false, restored = false, actualKnown = false;
    double actualRate = 0;
    uint32_t actualBuffer = 0;
    std::string error;
};
// Bounded polling, rate first then buffer, exact readback, and best-effort
// rollback. No project, device-selection, MON or transport mutation here.
AudioHardwareOutcome changeAudioHardware(AudioHardwareBackend&, AudioHardwareSettings,
                                        uint32_t buffer, const std::atomic<bool>& cancel);
struct AudioHardwareJob {
    std::atomic<bool> done{false}, cancel{false};
    AudioHardwareOutcome outcome; // only read after done acquire
};
std::shared_ptr<AudioHardwareJob> beginAudioHardwareChange(const AudioHardwareSettings&, uint32_t buffer);
}
