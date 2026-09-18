#include "audio/hardware_settings.hpp"
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace daw;
namespace {
struct Driver final : AudioHardwareBackend {
    AudioHardwareSettings value{7, "studio", 44100, 256, 32, 4096, true, true, true};
    std::optional<double> pendingRate;
    std::optional<uint32_t> pendingBuffer;
    int ticks = 0, reads = 0, writes = 0, delay = 0, rateAt = 0, bufferAt = 0;
    bool ignoreBuffer = false, failBufferOnce = false, disconnected = false, resetBufferOnRate = false;
    std::function<void()> onWait;
    AudioHardwareSettings read() override {
        ++reads; if (disconnected) throw std::runtime_error("device lost"); return value;
    }
    void setRate(double v) override {
        ++writes;
        if (!value.rateWritable) throw std::runtime_error("rate is read-only");
        pendingRate = v; rateAt = ticks + delay; deliver();
    }
    void setBuffer(uint32_t v) override {
        ++writes;
        if (failBufferOnce) { failBufferOnce = false; throw std::runtime_error("driver failure"); }
        if (!value.bufferWritable) throw std::runtime_error("buffer is read-only");
        if (!ignoreBuffer) { pendingBuffer = v; bufferAt = ticks + delay; deliver(); }
    }
    void deliver() {
        if (pendingRate && ticks >= rateAt) {
            value.sampleRate = *pendingRate; pendingRate.reset();
            if (resetBufferOnRate) value.bufferFrames = 512;
        }
        if (pendingBuffer && ticks >= bufferAt) { value.bufferFrames = *pendingBuffer; pendingBuffer.reset(); }
    }
    void wait() override { ++ticks; deliver(); if (onWait) onWait(); }
};
}
int main() {
    size_t checks = 0;
    auto check = [&](bool value, const char* message) {
        ++checks; if (!value) throw std::runtime_error(message);
    };
    auto reject = [&](auto fn) { bool did = false; try { fn(); } catch (...) { did = true; } check(did, "expected rejection"); };
    try {
        const std::atomic<bool> noCancel{false};
        for (const double rate : {44100.0, 48000.0, 96000.0}) {
            for (uint32_t oldBuffer : {64u, 128u, 192u, 256u, 512u}) {
                for (uint32_t target : {32u, 64u, 128u, 256u, 512u, 1024u, 4096u}) {
                    Driver driver; driver.value.sampleRate = rate; driver.value.bufferFrames = oldBuffer;
                    driver.delay = 3;
                    const auto result = changeAudioHardware(driver, driver.value, target, noCancel);
                    check(result.success && result.actualKnown, "confirmed outcome");
                    check(result.actualRate == 48000 && result.actualBuffer == target, "confirmed exact values");
                    check(driver.value.bufferFrames == target && driver.value.sampleRate == 48000, "actual driver changed");
                    check(driver.ticks < 100, "bounded ordinary change");
                }
            }
        }
        {
            Driver d; d.value.sampleRate = 48000; d.value.rateWritable = d.value.bufferWritable = false;
            auto result = changeAudioHardware(d, d.value, 256, noCancel);
            check(result.success && d.writes == 0, "read-only no-op requires no writes");
        }
        {
            Driver d; auto expected = d.value; expected.bufferFrames = 512;
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(!result.success && !result.restored && d.writes == 0, "stale snapshot never writes");
        }
        {
            Driver d; auto expected = d.value; expected.deviceID = 9;
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(!result.success && !result.actualKnown && d.writes == 0, "recycled id never writes");
        }
        {
            Driver d; auto expected = d.value; d.value.uid = "other";
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(!result.success && d.writes == 0, "changed UID never writes");
        }
        {
            Driver d; d.value.supports48k = false;
            check(!changeAudioHardware(d, d.value, 128, noCancel).success && d.writes == 0, "unsupported 48k rejected");
        }
        {
            Driver d; d.value.bufferWritable = false;
            check(!changeAudioHardware(d, d.value, 128, noCancel).success && d.writes == 0, "readonly buffer preflight");
        }
        {
            Driver d; d.value.rateWritable = false;
            check(!changeAudioHardware(d, d.value, 128, noCancel).success && d.writes == 0, "readonly rate preflight");
        }
        {
            Driver d; const auto expected = d.value; d.ignoreBuffer = true;
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(!result.success && result.restored, "ignored buffer request rolls rate back");
            check(d.value.sampleRate == expected.sampleRate && d.value.bufferFrames == expected.bufferFrames, "rollback values");
            check(d.ticks >= 100 && d.ticks <= 300, "timeout bounded");
        }
        {
            Driver d; auto expected = d.value; d.failBufferOnce = true;
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(!result.success && result.restored && d.value.sampleRate == 44100, "partial failure restores rate");
        }
        {
            Driver d; std::atomic<bool> cancel{true};
            auto result = changeAudioHardware(d, d.value, 128, cancel);
            check(!result.success && result.restored && d.writes == 0, "cancel before mutation");
        }
        {
            Driver d; const auto expected = d.value; std::atomic<bool> cancel{false}; d.delay = 3;
            d.onWait = [&] { if (d.ticks == 2) cancel.store(true); };
            auto result = changeAudioHardware(d, expected, 128, cancel);
            check(!result.success && result.restored && d.value.sampleRate == 44100, "cancel pending setter restores");
        }
        {
            Driver d; const auto expected = d.value;
            d.onWait = [&] { if (d.ticks == 1) d.disconnected = true; };
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(!result.success && !result.restored && !result.actualKnown, "disconnect not mislabeled restored");
        }
        {
            Driver d; const auto expected = d.value;
            d.onWait = [&] { if (d.ticks == 1) d.value.sampleRate = 88200; };
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(!result.success && !result.restored && d.writes == 1 && d.value.sampleRate == 88200, "don't overwrite external format");
        }
        {
            Driver d; d.resetBufferOnRate = true; const auto expected = d.value;
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(result.success && result.actualBuffer == 128, "rate-induced buffer change revalidated");
        }
        {
            Driver d; const auto expected = d.value;
            d.onWait = [&] { if (d.ticks == 1) { d.value.maximumBuffer = 64; d.value.rateWritable = false; } };
            auto result = changeAudioHardware(d, expected, 128, noCancel);
            check(!result.success && !result.restored && !result.actualKnown, "changed capabilities plus rollback failure is explicit");
        }
        Driver d;
        for (uint32_t buffer : {0u, 1u, 31u, 4097u, UINT32_MAX}) reject([&] { validateAudioHardwareRequest(d.value, buffer); });
        for (double rate : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            auto bad = d.value; bad.sampleRate = rate; reject([&] { validateAudioHardwareRequest(bad, 128); });
        }
        for (auto invalidUID : {std::string(), std::string(481, 'a'), std::string("a\0b", 3)}) {
            auto bad = d.value; bad.uid = invalidUID; reject([&] { validateAudioHardwareRequest(bad, 128); });
        }
        {
            AudioIOLease first, second;
            reject([&] { AudioHardwareLease exclusive; });
            check(!audioHardwareChangeActive(), "IO count preserved after rejected edit");
        }
        {
            AudioHardwareLease exclusive;
            check(audioHardwareChangeActive(), "exclusive visible");
            reject([&] { AudioIOLease owner; });
            reject([&] { AudioHardwareLease other; });
        }
        check(!audioHardwareChangeActive(), "exclusive released");
        std::atomic<int> inside{0}, violations{0};
        auto contention = [&] {
            for (int i = 0; i < 500; ++i) {
                try {
                    AudioHardwareLease lease;
                    if (inside.fetch_add(1) != 0) ++violations;
                    std::this_thread::yield(); inside.fetch_sub(1);
                } catch (...) {}
            }
        };
        std::thread a(contention), b(contention); a.join(); b.join();
        check(violations.load() == 0 && !audioHardwareChangeActive(), "concurrent control exclusion");
        std::cout << "audio hardware transaction: " << checks << " checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
