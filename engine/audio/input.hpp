#pragma once
#include "audio/hardware_settings.hpp"
#include "audio/device.hpp"
#include "audio/clip.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace daw {

// Fixed-capacity capture storage. writeMono is safe for the Core Audio callback:
// it allocates nothing, locks nothing, and never throws.
class CaptureBuffer {
    std::vector<float> samples_;
    std::atomic<uint64_t> frames_{0};
    std::atomic<bool> overflow_{false};
public:
    explicit CaptureBuffer(uint64_t capacityFrames = 48000 * 60);
    void writeMono(const float* input, uint32_t frames) noexcept;
    uint64_t frames() const noexcept { return frames_.load(std::memory_order_acquire); }
    uint64_t capacity() const noexcept { return samples_.size() / 2; }
    bool overflowed() const noexcept { return overflow_.load(std::memory_order_acquire); }
    std::shared_ptr<const Clip> finish() const;
};

class Input {
    AudioIOLease hardwareLease_;
public:
    virtual ~Input() = default;
    virtual void start() = 0;
    virtual std::shared_ptr<const Clip> stop() = 0;
    virtual void cancel() noexcept = 0;
    virtual void checkDevice() = 0;
    virtual uint64_t frames() const noexcept = 0;
    virtual uint64_t callbacks() const noexcept = 0;
    virtual bool overflowed() const noexcept = 0;
    virtual void discardRecovery() noexcept = 0;
};

std::unique_ptr<Input> makeInput(uint64_t capacityFrames,const std::string& recoveryPath,uint64_t startFrame,const AudioDeviceConfiguration& configuration = {});
}
