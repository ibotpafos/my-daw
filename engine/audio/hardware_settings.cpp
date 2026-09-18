#include "audio/hardware_settings.hpp"
#include "domain/session.hpp"
#include "jobs/limiter.hpp"
#include <cmath>
#include <limits>
#include <thread>

namespace daw {
namespace {
std::atomic<int> hardwareUsers{0}; // -1: exclusive settings worker; >=0: IO owners
constexpr int readbackAttempts = 100;
bool sameRate(double a, double b) { return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 0.5; }
bool sameValues(const AudioHardwareSettings& a, double rate, uint32_t buffer) {
    return sameRate(a.sampleRate, rate) && a.bufferFrames == buffer;
}
void identity(const AudioHardwareSettings& a, const AudioHardwareSettings& expected) {
    if (a.deviceID != expected.deviceID || a.uid != expected.uid)
        throw Error("Audio device identity changed; refresh Audio Settings");
}
void canceled(const std::atomic<bool>& cancel) {
    if (cancel.load(std::memory_order_acquire)) throw Error("Audio hardware change canceled");
}
template<class Match>
AudioHardwareSettings await(AudioHardwareBackend& backend, const AudioHardwareSettings& expected,
                            Match match, const std::atomic<bool>* cancel) {
    int stable = 0;
    for (int i = 0; i < readbackAttempts; ++i) {
        if (cancel) canceled(*cancel);
        auto actual = backend.read();
        identity(actual, expected);
        stable = match(actual) ? stable + 1 : 0;
        if (stable >= 3) return actual;
        backend.wait();
    }
    throw Error("Timed out waiting for the audio driver to confirm the requested format");
}
}
AudioIOLease::AudioIOLease() {
    auto n = hardwareUsers.load(std::memory_order_acquire);
    do {
        if (n < 0) throw Error("Audio hardware configuration is in progress; wait before Play / Record");
        if (n == std::numeric_limits<int>::max()) throw Error("Too many audio device owners");
    } while (!hardwareUsers.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel));
}
AudioIOLease::~AudioIOLease() { hardwareUsers.fetch_sub(1, std::memory_order_release); }
AudioHardwareLease::AudioHardwareLease() {
    int empty = 0;
    if (!hardwareUsers.compare_exchange_strong(empty, -1, std::memory_order_acq_rel))
        throw Error("Stop all audio playback/recording before changing hardware format");
}
AudioHardwareLease::~AudioHardwareLease() { hardwareUsers.store(0, std::memory_order_release); }
bool audioHardwareChangeActive() noexcept { return hardwareUsers.load(std::memory_order_acquire) < 0; }
void validateAudioHardwareRequest(const AudioHardwareSettings& expected, uint32_t buffer) {
    if (!expected.deviceID || expected.uid.empty() || expected.uid.size() > audioDeviceUIDBytes ||
        expected.uid.find('\0') != std::string::npos)
        throw Error("An explicit available audio device UID is required");
    if (!std::isfinite(expected.sampleRate) || expected.sampleRate <= 0 ||
        expected.sampleRate > 768000 || !expected.bufferFrames || expected.bufferFrames > 1048576)
        throw Error("Invalid current hardware format");
    if (!expected.minimumBuffer || expected.minimumBuffer > expected.maximumBuffer ||
        expected.maximumBuffer > 1048576 || !buffer || buffer > audioDeviceMaximumFrames ||
        buffer < expected.minimumBuffer || buffer > expected.maximumBuffer)
        throw Error("Requested buffer is outside the supported device/engine range");
    if (!sameRate(expected.sampleRate, 48000) && (!expected.supports48k || !expected.rateWritable))
        throw Error("The selected device cannot be changed to the project's 48 kHz");
    if (expected.bufferFrames != buffer && !expected.bufferWritable)
        throw Error("The audio driver does not allow changing its buffer size");
}
AudioHardwareOutcome changeAudioHardware(AudioHardwareBackend& backend, AudioHardwareSettings expected,
                                        uint32_t buffer, const std::atomic<bool>& cancel) {
    AudioHardwareOutcome result;
    bool rateAttempted = false, bufferAttempted = false;
    uint32_t beforeBuffer = expected.bufferFrames;
    const auto remember = [&](const AudioHardwareSettings& value) {
        result.actualKnown = true; result.actualRate = value.sampleRate; result.actualBuffer = value.bufferFrames;
    };
    try {
        validateAudioHardwareRequest(expected, buffer);
        canceled(cancel);
        auto live = backend.read(); identity(live, expected); remember(live);
        if (!sameValues(live, expected.sampleRate, expected.bufferFrames))
            throw Error("Audio format changed since the panel was opened; refresh before applying");
        validateAudioHardwareRequest(live, buffer);
        if (!sameRate(live.sampleRate, 48000)) {
            canceled(cancel); rateAttempted = true;
            backend.setRate(48000);
            live = await(backend, expected, [](const auto& a) { return sameRate(a.sampleRate, 48000); }, &cancel);
        }
        // Rate changes can change buffer bounds/writability; re-read rather than
        // assuming the old driver capabilities still apply.
        live = backend.read(); identity(live, expected); remember(live);
        if (!sameRate(live.sampleRate, 48000)) throw Error("Audio sample rate changed during configuration");
        validateAudioHardwareRequest(live, buffer);
        beforeBuffer = live.bufferFrames;
        if (live.bufferFrames != buffer) {
            canceled(cancel); bufferAttempted = true;
            backend.setBuffer(buffer);
        }
        live = await(backend, expected, [&](const auto& a) { return sameValues(a, 48000, buffer); }, &cancel);
        canceled(cancel); remember(live); result.success = true;
        return result;
    } catch (const std::exception& error) { result.error = error.what(); }
      catch (...) { result.error = "Unknown audio hardware configuration failure"; }
    try {
        auto live = backend.read(); identity(live, expected); remember(live);
        if (rateAttempted || bufferAttempted) {
            // Never blindly overwrite an unrelated external application's edit.
            if ((!sameRate(live.sampleRate, expected.sampleRate) && !sameRate(live.sampleRate, 48000)) ||
                (live.bufferFrames != expected.bufferFrames && live.bufferFrames != buffer && live.bufferFrames != beforeBuffer))
                throw Error("Driver returned an unexpected format; automatic rollback was not safe");
            if (rateAttempted) {
                // Even a failed/late setter may still be pending. Explicitly
                // request the old value and require fresh readback.
                backend.setRate(expected.sampleRate);
                await(backend, expected, [&](const auto& a) { return sameRate(a.sampleRate, expected.sampleRate); }, nullptr);
            }
            live = backend.read(); identity(live, expected);
            if (bufferAttempted || live.bufferFrames != expected.bufferFrames) backend.setBuffer(expected.bufferFrames);
            live = await(backend, expected, [&](const auto& a) {
                return sameValues(a, expected.sampleRate, expected.bufferFrames);
            }, nullptr);
            remember(live);
        }
        result.restored = sameValues(live, expected.sampleRate, expected.bufferFrames);
        result.error += result.restored ? "; original format confirmed" : "; hardware state changed; refresh required";
    } catch (const std::exception& error) {
        result.actualKnown = false;
        result.error += std::string("; rollback not confirmed: ") + error.what();
    } catch (...) { result.actualKnown = false; result.error += "; rollback not confirmed"; }
    return result;
}
std::shared_ptr<AudioHardwareJob> beginAudioHardwareChange(const AudioHardwareSettings& expected, uint32_t buffer) {
    validateAudioHardwareRequest(expected, buffer);
    auto lease = std::make_shared<AudioHardwareLease>();
    auto permit = tryAcquireBackgroundJob();
    if (!permit) throw Error("Too many background jobs are active");
    auto result = std::make_shared<AudioHardwareJob>();
    std::thread worker([result, expected, buffer, lease = std::move(lease), permit = std::move(permit)]() mutable {
        try {
            auto backend = makeAudioHardwareBackend(expected);
            result->outcome = changeAudioHardware(*backend, expected, buffer, result->cancel);
        } catch (const std::exception& error) { result->outcome.error = error.what(); }
          catch (...) { result->outcome.error = "Cannot configure audio hardware"; }
        // Release exclusion before publishing done; after done the UI may start
        // a new operation. Captured leases remain owned even if the UI closes.
        permit.reset(); lease.reset();
        result->done.store(true, std::memory_order_release);
    });
    try { worker.detach(); } catch (...) { result->cancel.store(true); if (worker.joinable()) worker.join(); throw; }
    return result;
}
#ifndef __APPLE__
AudioHardwareSettings readAudioHardwareSettings(const std::string&) { throw Error("Audio hardware settings require macOS"); }
std::unique_ptr<AudioHardwareBackend> makeAudioHardwareBackend(const AudioHardwareSettings&) {
    throw Error("Audio hardware settings require macOS");
}
#endif
}
