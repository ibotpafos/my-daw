#pragma once
#include "audio/hardware_settings.hpp"
#include "audio/output.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace daw {
class Duplex {
    AudioIOLease hardwareLease_;
    std::atomic<bool> monitorInput{false};
public:
    Renderer renderer;
    void setMonitor(bool enabled) noexcept { monitorInput.store(enabled, std::memory_order_release); }
    bool monitoring() const noexcept { return monitorInput.load(std::memory_order_acquire); }
    virtual ~Duplex() = default;
    virtual void start() = 0;
    // Quiesces callbacks before finalizing. Null means Stop during preroll with
    // no captured frames; callers must not create an empty asset/Undo command.
    virtual std::shared_ptr<const Clip> stop() = 0;
    virtual void cancel() noexcept = 0;
    virtual void markStalled() noexcept = 0;
    virtual void checkDevices() = 0;
    virtual uint64_t frames() const noexcept = 0;
    virtual uint64_t callbacks() const noexcept = 0;
    virtual bool overflowed() const noexcept = 0;
    virtual void discardRecovery() noexcept = 0;
    virtual OutputTelemetry telemetry() const noexcept = 0;
};

std::unique_ptr<Duplex> makeDuplex(const State& state,uint64_t capacityFrames,const std::string& recoveryPath,uint64_t startFrame,uint64_t loopStart,uint64_t loopEnd,uint64_t prerollFrames=0,bool monitorInput=false,const AudioDeviceConfiguration& configuration = {});
}
