#include "audio/import_job.hpp"
#include "domain/session.hpp"
#include "jobs/limiter.hpp"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while (false)
template <class Fn> void rejects(Fn&& fn) { bool rejected = false; try { fn(); } catch (...) { rejected = true; } CHECK(rejected); }

std::vector<unsigned char> wav441() {
    constexpr uint32_t rate = 44100, frames = 44100, channels = 2, align = channels * 2;
    std::vector<unsigned char> bytes(44 + size_t(frames) * align);
    std::memcpy(bytes.data(), "RIFF", 4); std::memcpy(bytes.data() + 8, "WAVEfmt ", 8); std::memcpy(bytes.data() + 36, "data", 4);
    auto put = [&](size_t at, uint32_t value, unsigned width) { for (unsigned i = 0; i < width; ++i) bytes[at + i] = static_cast<unsigned char>(value >> (i * 8)); };
    put(4, static_cast<uint32_t>(bytes.size() - 8), 4); put(16, 16, 4); put(20, 1, 2); put(22, channels, 2);
    put(24, rate, 4); put(28, rate * align, 4); put(32, align, 2); put(34, 16, 2); put(40, frames * align, 4);
    for (uint32_t frame = 0; frame < frames; ++frame) { put(44 + size_t(frame) * align, 8192, 2); put(46 + size_t(frame) * align, static_cast<uint16_t>(-4096), 2); }
    return bytes;
}

struct PhaseCancel { std::atomic<bool>* cancel; uint8_t target; };
void cancelAtPhase(void* opaque, uint8_t phase) {
    auto& trigger = *static_cast<PhaseCancel*>(opaque);
    if (phase == trigger.target) trigger.cancel->store(true, std::memory_order_release);
}

int main() { try {
    CHECK(static_cast<uint8_t>(daw::ImportJobPhase::Reading) == 1);
    CHECK(static_cast<uint8_t>(daw::ImportJobPhase::Decoding) == 2);
    CHECK(static_cast<uint8_t>(daw::ImportJobPhase::Converting) == 3);
    CHECK(static_cast<uint8_t>(daw::ImportJobPhase::Ready) == 4);
    const auto path = std::filesystem::temp_directory_path() / ("mydaw-import-job-" + std::to_string(getpid()) + ".wav");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); } } cleanup{path};
    const auto bytes = wav441();
    { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }

    // Every expensive stage observes cancellation before it starts work.
    for (uint8_t phase : {uint8_t{1}, uint8_t{2}, uint8_t{3}}) {
        std::atomic<bool> cancel{false}; std::atomic<uint32_t> progress{0}; PhaseCancel trigger{&cancel, phase};
        daw::ImportControl control{&cancel, &progress, 0, 100, &cancelAtPhase, &trigger};
        rejects([&] { (void)daw::readWav(path.string(), control); });
        CHECK(cancel.load(std::memory_order_acquire));
    }

    const auto baseline = daw::backgroundJobsInFlight();
    auto ready = daw::startWavImport(path.string());
    for (int i = 0; i < 1000 && ready->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(ready->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Ready);
    CHECK(ready->phase.load(std::memory_order_acquire) == daw::ImportJobPhase::Ready);
    CHECK(ready->progress.load(std::memory_order_acquire) == 100);
    CHECK(ready->sourceSampleRate.load(std::memory_order_acquire) == 44100 && ready->sourceChannels.load(std::memory_order_acquire) == 2);
    CHECK(ready->sourceFrames.load(std::memory_order_acquire) == 44100 && ready->outputFrames.load(std::memory_order_acquire) == 48000);
    CHECK(daw::importClip(*ready)->frames() == 48000);
    daw::cancelImport(*ready);
    CHECK(ready->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Canceled && !daw::importClip(*ready));
    for (int i = 0; i < 1000 && daw::backgroundJobsInFlight() != baseline; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(daw::backgroundJobsInFlight() == baseline);

    // The zero-copy project-rate path is still a completed import from the
    // caller's perspective: Ready always means 100% progress.
    auto bytes48 = bytes;
    auto put32 = [&](size_t at, uint32_t value) { for (unsigned i = 0; i < 4; ++i) bytes48[at + i] = static_cast<unsigned char>(value >> (i * 8)); };
    put32(24, 48000); put32(28, 48000 * 4);
    { std::ofstream file(path, std::ios::binary | std::ios::trunc); file.write(reinterpret_cast<const char*>(bytes48.data()), static_cast<std::streamsize>(bytes48.size())); }
    auto projectRate = daw::startWavImport(path.string());
    for (int i = 0; i < 1000 && projectRate->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(projectRate->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Ready);
    CHECK(projectRate->sourceSampleRate.load(std::memory_order_acquire) == 48000 &&
          projectRate->outputFrames.load(std::memory_order_acquire) == 44100 &&
          projectRate->progress.load(std::memory_order_acquire) == 100);
    daw::cancelImport(*projectRate);
    for (int i = 0; i < 1000 && daw::backgroundJobsInFlight() != baseline; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(daw::backgroundJobsInFlight() == baseline);

    auto applied = daw::startWavImport(path.string());
    for (int i = 0; i < 1000 && applied->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(daw::markImportApplied(*applied));
    CHECK(applied->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Applied && !daw::importClip(*applied));
    for (int i = 0; i < 1000 && daw::backgroundJobsInFlight() != baseline; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(daw::backgroundJobsInFlight() == baseline);

    auto canceled = daw::startWavImport(path.string());
    daw::cancelImport(*canceled);
    CHECK(canceled->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Canceled);
    for (int i = 0; i < 1000 && daw::backgroundJobsInFlight() != baseline; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(daw::backgroundJobsInFlight() == baseline);
    std::cout << "PASS: background WAV import metadata, cancellation stages, publication, and permit release\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
