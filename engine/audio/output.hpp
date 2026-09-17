#pragma once
#include "audio/device.hpp"
#include "audio/renderer.hpp"
#include <memory>
namespace daw {
enum class OutputState:uint32_t { idle=0, running=1, stopped=2, deviceLost=3, stalled=4, callbackError=5 };
struct OutputTelemetry { OutputState state=OutputState::idle; uint32_t deviceID=0; uint64_t generation=0, callbacks=0, callbackErrors=0; };
class Output {
public:
    Renderer renderer;
    virtual ~Output() = default;
    // Build the complete render graph without opening or starting a hardware
    // device. This is safe to run on a bounded background control worker.
    virtual void prepare(const State&, uint64_t startFrame = 0, uint64_t loopStart = 0, uint64_t loopEnd = 0) = 0;
    // Claim a previously prepared graph and start Core Audio. No plug-in is
    // instantiated from this call.
    virtual void startPrepared() = 0;
    virtual void start(const State&, uint64_t startFrame = 0, uint64_t loopStart = 0, uint64_t loopEnd = 0) = 0;
    virtual void stop() noexcept = 0;
    virtual void checkDevice() = 0; // non-RT, throws after stopping on lost/changed device
    virtual void markStalled() noexcept = 0;
    virtual OutputTelemetry telemetry() const noexcept = 0;
};
std::unique_ptr<Output> makeOutput(const AudioDeviceConfiguration& configuration = {});
}
