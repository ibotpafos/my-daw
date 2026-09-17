#pragma once
#include "audio/output.hpp"
#include <memory>
#include <string>

namespace daw {
class Duplex {
public:
    Renderer renderer;
    virtual ~Duplex() = default;
    virtual void start() = 0;
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
