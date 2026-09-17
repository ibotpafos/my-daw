// My DAW end-to-end scenario harness.
//
// Scenarios here are black-box user journeys: they drive the product through
// the SAME public C ABI the macOS app uses (daw.h) and assert on durable
// artifacts (draft files, exported WAVs) with an INDEPENDENT reader/writer of
// our own. Engine internals are deliberately not included: if the ABI lies,
// the scenario must fail even though every white-box CTest stays green.
//
// Conventions:
//  - CHECK / CHECK_OK / CHECK_REJ from this header; scenario main() catches
//    and reports, matching the existing CTest runner style.
//  - Fixture audio is generated with fixed math (sine/DC), never randomness.
//  - Every background job wait has a named deadline; CI hangs are traceable.
#ifndef MY_DAW_E2E_HPP
#define MY_DAW_E2E_HPP

#include "daw.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <unistd.h>

namespace e2e {

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("Failed: ") + #x); } while (false)

// The project frame rate is fixed by design; every assertion assumes it.
constexpr uint32_t kProjectRate = 48000;

inline std::string bridgeError(daw_session* session) {
    char buffer[512] = {0};
    if (session) daw_error(session, buffer, sizeof(buffer));
    return std::string(buffer);
}

// CHECK_OK fails with the bridge's own error text so a scenario log reads like
// a user-visible failure instead of a bare return code.
#define CHECK_OK(session, call) do { \
    int rc_ = (call); \
    if (rc_ != 0) { \
        const std::string reason_ = e2e::bridgeError(session); \
        throw std::runtime_error(std::string("Rejected (rc=") + std::to_string(rc_) + \
                                 "): " #call + (reason_.empty() ? "" : " — " + reason_)); \
    } \
} while (false)

// Every rejected mutation must leave a human-readable error behind as well.
#define CHECK_REJ(session, call) do { \
    int rc_ = (call); \
    if (rc_ == 0) throw std::runtime_error(std::string("Accepted, expected rejection: ") + #call); \
    const std::string reason_ = e2e::bridgeError(session); \
    if (reason_.empty()) throw std::runtime_error(std::string("Rejected without error text: ") + #call); \
} while (false)

class TempRoot {
public:
    std::filesystem::path path;
    explicit TempRoot(const std::string& tag) {
        const auto pattern = (std::filesystem::temp_directory_path() / ("mydaw-e2e-" + tag + "-XXXXXX")).string();
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back(0);
        const char* made = ::mkdtemp(bytes.data());
        if (!made) throw std::runtime_error("mkdtemp failed for " + pattern);
        path = made;
    }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;
    ~TempRoot() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::filesystem::path operator/(const std::string& leaf) const { return path / leaf; }
    // Act as a path wherever std::filesystem wants one, and expose the string
    // form, so scenario code reads naturally (root.string(), iterate root/path).
    operator const std::filesystem::path&() const { return path; }
    std::string string() const { return path.string(); }
};

// RAII wrapper for the opaque session handle.
class Bridge {
public:
    Bridge() : session(daw_create(), daw_destroy) {
        if (!session) throw std::runtime_error("daw_create failed");
    }
    daw_session* get() const { return session.get(); }

private:
    std::unique_ptr<daw_session, decltype(&daw_destroy)> session;
};

// Versioned-struct factory: zeroed storage with the correct struct_size gate.
template <class T> T abi() {
    T value{};
    value.struct_size = sizeof(T);
    return value;
}

inline daw_snapshot snapshotOf(daw_session* session) {
    auto snap = abi<daw_snapshot>();
    CHECK(daw_get_snapshot(session, &snap) == 0);
    return snap;
}

inline uint64_t rev(daw_session* session) { return snapshotOf(session).revision; }

// Commands that mint no id: read the freshly appended entity back by position.
inline uint64_t addTrack(daw_session* session, const std::string& name) {
    const auto before = snapshotOf(session);
    CHECK_OK(session, daw_add_track(session, name.c_str(), before.revision));
    const auto after = snapshotOf(session);
    CHECK(after.track_count == before.track_count + 1);
    auto track = abi<daw_track>();
    CHECK_OK(session, daw_get_track(session, after.track_count - 1, &track));
    return track.id;
}

inline uint64_t addBus(daw_session* session, const std::string& name) {
    uint64_t busId = 0;
    CHECK_OK(session, daw_create_bus(session, name.c_str(), &busId, rev(session)));
    CHECK(busId != 0);
    return busId;
}

inline daw_track trackById(daw_session* session, uint64_t id) {
    const auto snap = snapshotOf(session);
    for (uint32_t index = 0; index < snap.track_count; ++index) {
        auto track = abi<daw_track>();
        CHECK_OK(session, daw_get_track(session, index, &track));
        if (track.id == id) return track;
    }
    throw std::runtime_error("track " + std::to_string(id) + " not found");
}

inline daw_bus busById(daw_session* session, uint64_t id) {
    const auto snap = snapshotOf(session);
    for (uint32_t index = 0; index < snap.bus_count; ++index) {
        auto bus = abi<daw_bus>();
        CHECK_OK(session, daw_get_bus(session, index, &bus));
        if (bus.id == id) return bus;
    }
    throw std::runtime_error("bus " + std::to_string(id) + " not found");
}

// ---------------------------------------------------------------------------
// Independent WAV fixture I/O (never daw::readWav/writeWav: e2e must be able
// to disagree with the engine's own codecs).
// ---------------------------------------------------------------------------
struct Wav {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    bool floating = false;
    std::vector<float> samples; // interleaved
    uint64_t frames() const { return channels ? samples.size() / channels : 0; }
};

inline void putLE(std::vector<unsigned char>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<unsigned char>(value >> shift));
}
inline void putLE16(std::vector<unsigned char>& bytes, uint16_t value) {
    bytes.push_back(static_cast<unsigned char>(value));
    bytes.push_back(static_cast<unsigned char>(value >> 8));
}
inline uint32_t getLE(const std::vector<unsigned char>& bytes, size_t at) {
    if (at + 4 > bytes.size()) throw std::runtime_error("wav chunk overrun");
    return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}
inline uint16_t getLE16(const std::vector<unsigned char>& bytes, size_t at) {
    if (at + 2 > bytes.size()) throw std::runtime_error("wav chunk overrun");
    return static_cast<uint16_t>(bytes[at]) | (static_cast<uint16_t>(bytes[at + 1]) << 8);
}

// Writes interleaved float [-1,1] as PCM16 / PCM24 / IEEE float32 WAV.
inline void writeWavFixture(const std::filesystem::path& path, const std::vector<float>& interleaved,
                            uint32_t sampleRate, uint16_t channels, const std::string& format) {
    const bool floating = format == "f32";
    const uint16_t bits = floating ? 32 : static_cast<uint16_t>(format == "pcm24" ? 24 : 16);
    const uint16_t code = floating ? 3 : 1;
    const uint32_t bytesPerSample = bits / 8;
    CHECK(channels > 0 && interleaved.size() % channels == 0);
    const uint32_t frames = static_cast<uint32_t>(interleaved.size() / channels);
    const uint32_t dataBytes = frames * channels * bytesPerSample;
    std::vector<unsigned char> bytes;
    bytes.reserve(44 + dataBytes);
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    putLE(bytes, 36 + dataBytes);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E'});
    bytes.insert(bytes.end(), {'f', 'm', 't', ' '});
    putLE(bytes, 16);
    putLE16(bytes, code);
    putLE16(bytes, channels);
    putLE(bytes, sampleRate);
    putLE(bytes, sampleRate * channels * bytesPerSample);
    putLE16(bytes, static_cast<uint16_t>(channels * bytesPerSample));
    putLE16(bytes, bits);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    putLE(bytes, dataBytes);
    for (const float raw : interleaved) {
        const float v = raw < -1.0f ? -1.0f : (raw > 1.0f ? 1.0f : raw);
        if (floating) {
            uint32_t encoded = 0;
            std::memcpy(&encoded, &v, sizeof(encoded));
            putLE(bytes, encoded);
        } else if (bits == 16) {
            putLE16(bytes, static_cast<uint16_t>(static_cast<int16_t>(std::lround(v * 32767.0f))));
        } else {
            const int32_t q = static_cast<int32_t>(std::lround(v * 8388607.0f));
            bytes.push_back(static_cast<unsigned char>(q));
            bytes.push_back(static_cast<unsigned char>(q >> 8));
            bytes.push_back(static_cast<unsigned char>(q >> 16));
        }
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream.good()) throw std::runtime_error("cannot write fixture " + path.string());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(stream.good());
}

// Reads PCM16/24/32 + IEEE float32 WAV by walking chunks; unknown chunks are
// skipped the same way a browser-style reader would.
inline Wav readWavFixture(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) throw std::runtime_error("cannot open wav " + path.string());
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error("not a RIFF/WAVE file: " + path.string());
    Wav wav;
    size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const uint32_t size = getLE(bytes, at + 4);
        if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && at + 8 + 16 <= bytes.size()) {
            wav.floating = getLE16(bytes, at + 8) == 3;
            wav.channels = getLE16(bytes, at + 10);
            wav.sampleRate = getLE(bytes, at + 12);
            wav.bits = getLE16(bytes, at + 22);
        } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
            const uint32_t bytesPerSample = wav.bits / 8;
            const size_t stride = wav.channels * bytesPerSample;
            const size_t start = at + 8;
            const size_t end = std::min(start + static_cast<size_t>(size), bytes.size());
            for (size_t cursor = start; cursor + stride <= end; cursor += stride) {
                for (uint16_t channel = 0; channel < wav.channels; ++channel) {
                    const size_t sampleAt = cursor + static_cast<size_t>(channel) * bytesPerSample;
                    float value = 0.0f;
                    if (wav.floating && wav.bits == 32) {
                        const uint32_t raw = getLE(bytes, sampleAt);
                        std::memcpy(&value, &raw, sizeof(value));
                    } else if (wav.bits == 16) {
                        value = static_cast<float>(static_cast<int16_t>(getLE16(bytes, sampleAt))) / 32768.0f;
                    } else if (wav.bits == 24) {
                        int32_t raw = static_cast<int32_t>(bytes[sampleAt]) |
                                      (static_cast<int32_t>(bytes[sampleAt + 1]) << 8) |
                                      (static_cast<int32_t>(bytes[sampleAt + 2]) << 16);
                        if (raw & 0x800000) raw |= static_cast<int32_t>(0xFF000000u);
                        value = static_cast<float>(raw) / 8388608.0f;
                    } else if (wav.bits == 32) {
                        value = static_cast<float>(static_cast<int32_t>(getLE(bytes, sampleAt))) / 2147483648.0f;
                    } else {
                        throw std::runtime_error("unsupported wav bit depth " + std::to_string(wav.bits));
                    }
                    wav.samples.push_back(value);
                }
            }
        }
        at += 8 + static_cast<size_t>(size) + (size & 1u);
    }
    if (wav.channels == 0) throw std::runtime_error("no fmt chunk in wav " + path.string());
    return wav;
}

// Deterministic fixture material.
inline std::vector<float> sineInterleaved(uint32_t frames, uint32_t rate, double hz, double amplitude,
                                          uint16_t channels = 2) {
    std::vector<float> out(static_cast<size_t>(frames) * channels, 0.0f);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const double angle = 2.0 * 3.14159265358979319 * hz * static_cast<double>(frame) / static_cast<double>(rate);
        for (uint16_t channel = 0; channel < channels; ++channel)
            out[static_cast<size_t>(frame) * channels + channel] =
                static_cast<float>(amplitude * std::sin(angle));
    }
    return out;
}

inline std::vector<float> constantInterleaved(uint32_t frames, uint16_t channels, float left, float right) {
    std::vector<float> out(static_cast<size_t>(frames) * channels, 0.0f);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        out[static_cast<size_t>(frame) * channels] = left;
        if (channels > 1) out[static_cast<size_t>(frame) * channels + 1] = right;
    }
    return out;
}

// ---------------------------------------------------------------------------
// AIFF fixture writer (big-endian PCM / AIFC fl32), independent of the engine.
// ---------------------------------------------------------------------------
namespace detail {
inline void putBE(std::vector<unsigned char>& bytes, size_t at, uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes[at + i] = static_cast<unsigned char>((value >> (8 * (width - 1 - i))) & 0xFF);
}
inline void putExt80(std::vector<unsigned char>& bytes, size_t at, double v) {
    int exponent = 0;
    double mantissa = std::frexp(v, &exponent);
    mantissa *= 2.0;
    exponent -= 1;
    const int expField = exponent + 16383;
    const uint64_t mant = static_cast<uint64_t>(std::round(mantissa * 9223372036854775808.0));
    bytes[at] = static_cast<unsigned char>((expField >> 8) & 0x7F);
    bytes[at + 1] = static_cast<unsigned char>(expField & 0xFF);
    for (int i = 0; i < 8; ++i) bytes[at + 2 + i] = static_cast<unsigned char>((mant >> (56 - 8 * i)) & 0xFF);
}
} // namespace detail

// format: "pcm16" | "pcm24" | "f32" (f32 emits an AIFC/fl32 chunk layout).
inline void writeAiffFixture(const std::filesystem::path& path, const std::vector<float>& interleaved,
                             uint32_t sampleRate, uint16_t channels, const std::string& format) {
    const bool aifc = format == "f32";
    const uint16_t bits = aifc ? 32 : static_cast<uint16_t>(format == "pcm24" ? 24 : 16);
    const unsigned width = bits / 8;
    CHECK(channels > 0 && interleaved.size() % channels == 0);
    const uint32_t frames = static_cast<uint32_t>(interleaved.size() / channels);
    const uint32_t dataBytes = frames * channels * width;
    unsigned commLogical = 18; // channels(2)+samples(4)+bits(2)+rate(10)
    if (aifc) commLogical += 4 + 1 + 4;
    const unsigned commPadded = commLogical + (commLogical & 1);
    const unsigned ssndLogical = 8 + dataBytes;
    const unsigned ssndPadded = ssndLogical + (ssndLogical & 1);
    const unsigned formBody = 4 + (8 + commPadded) + (8 + ssndPadded);
    std::vector<unsigned char> bytes(8 + formBody, 0);
    std::memcpy(bytes.data(), "FORM", 4);
    detail::putBE(bytes, 4, formBody, 4);
    std::memcpy(bytes.data() + 8, aifc ? "AIFC" : "AIFF", 4);
    size_t p = 12;
    std::memcpy(bytes.data() + p, "COMM", 4);
    detail::putBE(bytes, p + 4, commLogical, 4);
    bytes[p + 8] = static_cast<unsigned char>(channels >> 8);
    bytes[p + 9] = static_cast<unsigned char>(channels);
    detail::putBE(bytes, p + 10, frames, 4);
    bytes[p + 14] = static_cast<unsigned char>(bits >> 8);
    bytes[p + 15] = static_cast<unsigned char>(bits);
    detail::putExt80(bytes, p + 16, static_cast<double>(sampleRate));
    if (aifc) {
        std::memcpy(bytes.data() + p + 26, "fl32", 4);
        bytes[p + 30] = 4;
        std::memcpy(bytes.data() + p + 31, "fl32", 4);
    }
    p += 8 + commPadded;
    std::memcpy(bytes.data() + p, "SSND", 4);
    detail::putBE(bytes, p + 4, ssndLogical, 4);
    detail::putBE(bytes, p + 8, 0, 4);
    detail::putBE(bytes, p + 12, 0, 4);
    size_t d = p + 16;
    const float scale = static_cast<float>(1u << (bits - 1));
    const int32_t limit = static_cast<int32_t>(1) << (bits - 1);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint16_t channel = 0; channel < channels; ++channel) {
            const float raw = interleaved[static_cast<size_t>(frame) * channels + channel];
            const float v = raw < -1.0f ? -1.0f : (raw > 1.0f ? 1.0f : raw);
            if (aifc) {
                uint32_t encoded = 0;
                std::memcpy(&encoded, &v, sizeof(encoded));
                detail::putBE(bytes, d, encoded, 4);
                d += 4;
            } else {
                int32_t q = static_cast<int32_t>(std::lround(v * scale));
                if (q > limit - 1) q = limit - 1;
                if (q < -limit) q = -limit;
                detail::putBE(bytes, d, static_cast<uint32_t>(q), width);
                d += width;
            }
        }
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream.good()) throw std::runtime_error("cannot write aiff " + path.string());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(stream.good());
}

// ---------------------------------------------------------------------------
// Bounded job polling; each helper names the wait it could not finish.
// ---------------------------------------------------------------------------
inline daw_save_status waitSave(daw_save_job* job, int timeoutMs = 30000) {
    daw_save_status status = abi<daw_save_status>();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        CHECK(daw_poll_save(job, &status) == 0);
        if (status.status != 0) break;
        if (std::chrono::steady_clock::now() > end) throw std::runtime_error("timeout waiting for save job");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (status.status != 1) throw std::runtime_error("save job failed: " + std::string(status.error));
    return status;
}

inline daw_export_status waitExport(daw_export_job* job, int32_t expected = 1, int timeoutMs = 120000) {
    daw_export_status status = abi<daw_export_status>();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        CHECK(daw_poll_export(job, &status) == 0);
        if (status.status != 0) break;
        if (std::chrono::steady_clock::now() > end) throw std::runtime_error("timeout waiting for export job");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (status.status != expected)
        throw std::runtime_error("export job status " + std::to_string(status.status) + " (want " +
                                 std::to_string(expected) + "): " + status.error);
    return status;
}

inline daw_import_status waitImport(daw_import_job* job, int32_t expected = DAW_IMPORT_READY, int timeoutMs = 30000) {
    daw_import_status status = abi<daw_import_status>();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        CHECK(daw_poll_import(job, &status) == 0);
        if (status.status != DAW_IMPORT_RUNNING) break;
        if (std::chrono::steady_clock::now() > end) throw std::runtime_error("timeout waiting for import job");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (status.status != expected)
        throw std::runtime_error("import job status " + std::to_string(status.status) + " (want " +
                                 std::to_string(expected) + "): " + status.error);
    return status;
}

inline daw_dawproject_status waitDawproject(daw_dawproject_job* job, int32_t expected = 1, int timeoutMs = 60000) {
    daw_dawproject_status status = abi<daw_dawproject_status>();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        CHECK(daw_poll_dawproject_export(job, &status) == 0);
        if (status.status != 0) break;
        if (std::chrono::steady_clock::now() > end) throw std::runtime_error("timeout waiting for dawproject job");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (status.status != expected)
        throw std::runtime_error("dawproject job status " + std::to_string(status.status) + " (want " +
                                 std::to_string(expected) + "): " + status.error);
    return status;
}

// ---------------------------------------------------------------------------
// Audio assertions.
// ---------------------------------------------------------------------------
inline void expectNear(double actual, double expected, double tolerance, const std::string& what) {
    if (!(std::abs(actual - expected) <= tolerance))
        throw std::runtime_error("value out of tolerance: " + what + " actual=" + std::to_string(actual) +
                                 " expected=" + std::to_string(expected) + " tol=" + std::to_string(tolerance));
}

inline double channelPeak(const Wav& wav, uint32_t fromFrame, uint32_t toFrame, uint16_t channel) {
    double peak = 0.0;
    for (uint32_t frame = fromFrame; frame < toFrame && frame < static_cast<uint32_t>(wav.frames()); ++frame)
        peak = std::max(peak, static_cast<double>(std::abs(wav.samples[static_cast<size_t>(frame) * wav.channels + channel])));
    return peak;
}

inline double channelRms(const Wav& wav, uint32_t fromFrame, uint32_t toFrame, uint16_t channel) {
    double sum = 0.0;
    uint32_t counted = 0;
    for (uint32_t frame = fromFrame; frame < toFrame && frame < static_cast<uint32_t>(wav.frames()); ++frame, ++counted) {
        const double v = wav.samples[static_cast<size_t>(frame) * wav.channels + channel];
        sum += v * v;
    }
    return counted ? std::sqrt(sum / static_cast<double>(counted)) : 0.0;
}

// Goertzel magnitude: mean |component| at one frequency over a window.
inline double toneMagnitude(const Wav& wav, uint32_t fromFrame, uint32_t toFrame, uint16_t channel, double hz) {
    const double omega = 2.0 * 3.14159265358979319 * hz / static_cast<double>(wav.sampleRate);
    const double coeff = 2.0 * std::cos(omega);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    uint32_t counted = 0;
    for (uint32_t frame = fromFrame; frame < toFrame && frame < static_cast<uint32_t>(wav.frames()); ++frame, ++counted) {
        s0 = static_cast<double>(wav.samples[static_cast<size_t>(frame) * wav.channels + channel]) + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    if (!counted) return 0.0;
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / static_cast<double>(counted);
}

inline bool fileNonEmpty(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return !ec && size > 0;
}

// ---------------------------------------------------------------------------
// Save/export conveniences built on the same public calls the app uses.
// ---------------------------------------------------------------------------
inline daw_save_status saveDraftAndWait(daw_session* session, const std::filesystem::path& path) {
    auto* job = daw_begin_save(session, path.c_str());
    CHECK(job != nullptr);
    auto status = waitSave(job);
    daw_release_save(job);
    return status;
}

inline Wav exportProject(daw_session* session, const std::filesystem::path& path, int32_t format = 2) {
    auto* job = daw_begin_export(session, path.c_str(), format);
    CHECK(job != nullptr);
    waitExport(job);
    daw_release_export(job);
    return readWavFixture(path);
}

// ---------------------------------------------------------------------------
// Model dump: read the entire durable project back through the C ABI so
// save/reload/package scenarios can assert full fidelity. Plain pairs/tuples
// keep comparisons total and the diff readable.
// ---------------------------------------------------------------------------
using Points = std::vector<std::pair<uint64_t, double>>;

struct DumpClip {
    uint64_t start = 0, sourceOffset = 0, length = 0, fadeIn = 0, fadeOut = 0, takeIndex = 0;
    uint32_t color = 0;
    double gainDb = 0.0;
    int muted = 0, looped = 0;
    double pan = 0.0;
    bool operator==(const DumpClip& other) const {
        return std::tie(start, sourceOffset, length, fadeIn, fadeOut, takeIndex, color, gainDb, muted, looped, pan) ==
               std::tie(other.start, other.sourceOffset, other.length, other.fadeIn, other.fadeOut, other.takeIndex,
                        other.color, other.gainDb, other.muted, other.looped, other.pan);
    }
};
struct DumpTake {
    uint32_t index = 0;
    uint64_t start = 0, frames = 0;
    std::string name;
    bool operator==(const DumpTake& other) const {
        return std::tie(index, start, frames, name) == std::tie(other.index, other.start, other.frames, other.name);
    }
};
struct DumpSend {
    uint64_t busId = 0;
    double gainDb = 0.0;
    int preFader = 0;
    double pan = 0;
    int muted = 0, independentPan = 0;
    bool operator==(const DumpSend& other) const {
        return std::tie(busId, gainDb, preFader, pan, muted, independentPan) ==
               std::tie(other.busId, other.gainDb, other.preFader, other.pan, other.muted, other.independentPan);
    }
};
struct DumpNote {
    uint64_t start = 0, length = 0;
    uint8_t pitch = 0, channel = 0, velocity = 0;
    bool operator==(const DumpNote& other) const {
        return std::tie(start, length, pitch, channel, velocity) ==
               std::tie(other.start, other.length, other.pitch, other.channel, other.velocity);
    }
};
struct DumpMidi {
    uint64_t start = 0, length = 0;
    int32_t lane = 0;
    uint32_t color = 0;
    std::vector<DumpNote> notes;
    bool operator==(const DumpMidi& other) const {
        return std::tie(start, length, lane, color) == std::tie(other.start, other.length, other.lane, other.color) &&
               notes == other.notes;
    }
};
struct DumpInsert {
    uint64_t id = 0;
    uint32_t type = 0, subtype = 0, manufacturer = 0;
    int bypassed = 0;
    uint32_t latencyFrames = 0, available = 0;
    std::string name;
    bool operator==(const DumpInsert& other) const {
        return std::tie(id, type, subtype, manufacturer, bypassed, latencyFrames, available, name) ==
               std::tie(other.id, other.type, other.subtype, other.manufacturer, other.bypassed, other.latencyFrames,
                        other.available, other.name);
    }
};
struct DumpTrack {
    uint64_t id = 0, outputBusId = 0;
    double gainDb = 0.0, pan = 0.0;
    std::string name;
    uint64_t audioFrames = 0;
    int muted = 0, solo = 0;
    uint32_t color = 0;
    std::vector<DumpClip> clips;
    std::vector<DumpTake> takes;
    std::vector<DumpSend> sends;
    Points volumeAutomation, panAutomation;
    std::vector<DumpMidi> midi;
    std::vector<DumpInsert> inserts;
    bool operator==(const DumpTrack& other) const {
        return std::tie(id, outputBusId, gainDb, pan, name, audioFrames, muted, solo, color, clips, takes, sends,
                        volumeAutomation, panAutomation, midi, inserts) ==
               std::tie(other.id, other.outputBusId, other.gainDb, other.pan, other.name, other.audioFrames,
                        other.muted, other.solo, other.color, other.clips, other.takes, other.sends,
                        other.volumeAutomation, other.panAutomation, other.midi, other.inserts);
    }
};
struct DumpBus {
    uint64_t id = 0, outputBusId = 0;
    double gainDb = 0.0, pan = 0.0;
    int muted = 0;
    std::string name;
    Points gainAutomation;
    std::vector<DumpInsert> inserts;
    bool operator==(const DumpBus& other) const {
        return std::tie(id, outputBusId, gainDb, pan, muted, name, gainAutomation, inserts) ==
               std::tie(other.id, other.outputBusId, other.gainDb, other.pan, other.muted, other.name,
                        other.gainAutomation, other.inserts);
    }
};
struct Dump {
    uint64_t revision = 0;
    double masterGain = 0.0;
    std::vector<DumpTrack> tracks;
    std::vector<DumpBus> buses;
    std::vector<DumpInsert> masterInserts;
    Points masterAutomation;
    std::vector<std::pair<uint64_t, double>> tempo;
    std::vector<std::tuple<uint64_t, int, int>> signatures;
    std::vector<std::pair<uint64_t, std::string>> markers;
    bool operator==(const Dump& other) const {
        return revision == other.revision && masterGain == other.masterGain && tracks == other.tracks &&
               buses == other.buses && masterInserts == other.masterInserts && masterAutomation == other.masterAutomation &&
               tempo == other.tempo && signatures == other.signatures && markers == other.markers;
    }
};

inline Points volumePoints(daw_session* session, uint64_t trackId) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_track_volume_automation_count(session, trackId, &count));
    Points points;
    for (uint32_t index = 0; index < count; ++index) {
        auto point = abi<daw_automation_point>();
        CHECK_OK(session, daw_get_track_volume_automation_point(session, trackId, index, &point));
        points.emplace_back(point.frame, point.gain_db);
    }
    return points;
}
inline Points panPoints(daw_session* session, uint64_t trackId) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_track_pan_automation_count(session, trackId, &count));
    Points points;
    for (uint32_t index = 0; index < count; ++index) {
        auto point = abi<daw_automation_point>();
        CHECK_OK(session, daw_get_track_pan_automation_point(session, trackId, index, &point));
        points.emplace_back(point.frame, point.gain_db);
    }
    return points;
}
inline Points busGainPoints(daw_session* session, uint64_t busId) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_bus_gain_automation_count(session, busId, &count));
    Points points;
    for (uint32_t index = 0; index < count; ++index) {
        auto point = abi<daw_automation_point>();
        CHECK_OK(session, daw_get_bus_gain_automation_point(session, busId, index, &point));
        points.emplace_back(point.frame, point.gain_db);
    }
    return points;
}
inline Points masterGainPoints(daw_session* session) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_master_gain_automation_count(session, &count));
    Points points;
    for (uint32_t index = 0; index < count; ++index) {
        auto point = abi<daw_automation_point>();
        CHECK_OK(session, daw_get_master_gain_automation_point(session, index, &point));
        points.emplace_back(point.frame, point.gain_db);
    }
    return points;
}

inline std::vector<DumpInsert> insertsOf(daw_session* session, int32_t owner, uint64_t ownerId) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_insert_count(session, owner, ownerId, &count));
    std::vector<DumpInsert> inserts;
    for (uint32_t index = 0; index < count; ++index) {
        auto plugin = abi<daw_plugin>();
        CHECK_OK(session, daw_get_insert(session, owner, ownerId, index, &plugin));
        DumpInsert entry;
        entry.id = plugin.id;
        entry.type = plugin.type;
        entry.subtype = plugin.subtype;
        entry.manufacturer = plugin.manufacturer;
        entry.bypassed = plugin.bypassed;
        entry.latencyFrames = plugin.latency_frames;
        entry.available = plugin.available;
        entry.name = plugin.name;
        inserts.push_back(entry);
    }
    return inserts;
}

inline std::vector<DumpMidi> midiOf(daw_session* session, uint64_t trackId) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_midi_clip_count(session, trackId, &count));
    std::vector<DumpMidi> clips;
    for (uint32_t index = 0; index < count; ++index) {
        auto clip = abi<daw_midi_clip>();
        clip.version = DAW_MIDI_CLIP_VERSION;
        std::vector<daw_midi_note> page(DAW_MIDI_NOTES_PER_CALL);
        std::vector<DumpNote> notes;
        uint32_t written = 0;
        uint32_t offset = 0;
        CHECK_OK(session, daw_get_midi_clip(session, trackId, index, &clip, 0, nullptr, 0, &written));
        for (;;) {
            CHECK_OK(session, daw_get_midi_clip(session, trackId, index, &clip, offset, page.data(),
                                                 static_cast<uint32_t>(page.size()), &written));
            CHECK(written > 0 || offset == 0);
            for (uint32_t note = 0; note < written; ++note) {
                DumpNote dump;
                dump.start = page[note].start;
                dump.length = page[note].length;
                dump.pitch = page[note].pitch;
                dump.channel = page[note].channel;
                dump.velocity = page[note].velocity;
                notes.push_back(dump);
            }
            offset += written;
            if (offset >= clip.note_count) break;
        }
        DumpMidi entry;
        entry.start = clip.start;
        entry.length = clip.length;
        entry.lane = clip.lane;
        entry.color = clip.color;
        entry.notes = std::move(notes);
        clips.push_back(entry);
    }
    return clips;
}

inline Dump dumpOf(daw_session* session) {
    Dump dump;
    const auto snap = snapshotOf(session);
    dump.revision = snap.revision;
    dump.masterGain = snap.master_gain_db;
    for (uint32_t index = 0; index < snap.track_count; ++index) {
        auto raw = abi<daw_track>();
        CHECK_OK(session, daw_get_track(session, index, &raw));
        DumpTrack track;
        track.id = raw.id;
        track.gainDb = raw.gain_db;
        track.name = raw.name;
        track.audioFrames = raw.audio_frames;
        track.pan = raw.pan;
        track.muted = raw.muted;
        track.solo = raw.solo;
        track.outputBusId = raw.output_bus_id;
        track.color = raw.color;
        for (uint32_t clip = 0; clip < raw.clip_count; ++clip) {
            auto region = abi<daw_clip>();
            CHECK_OK(session, daw_get_clip(session, raw.id, clip, &region));
            DumpClip entry;
            entry.start = region.start;
            entry.sourceOffset = region.source_offset;
            entry.length = region.length;
            entry.fadeIn = region.fade_in;
            entry.fadeOut = region.fade_out;
            entry.takeIndex = region.take_index;
            entry.color = region.color;
            entry.gainDb = region.gain_db;
            entry.muted = region.muted;
            entry.looped = region.looped;
            entry.pan = region.pan;
            track.clips.push_back(entry);
        }
        for (uint32_t take = 0; take < raw.take_count; ++take) {
            auto source = abi<daw_take>();
            CHECK_OK(session, daw_get_take(session, raw.id, take, &source));
            DumpTake entry;
            entry.index = source.index;
            entry.start = source.start;
            entry.frames = source.frames;
            entry.name = source.name;
            track.takes.push_back(entry);
        }
        for (uint32_t send = 0; send < raw.send_count; ++send) {
            auto route = abi<daw_send>();
            CHECK_OK(session, daw_get_send(session, raw.id, send, &route));
            DumpSend entry;
            entry.busId = route.bus_id;
            entry.gainDb = route.gain_db;
            entry.preFader = route.pre_fader;
            auto controls = abi<daw_send_controls>();
            CHECK_OK(session, daw_get_send_controls(session, raw.id, route.bus_id, &controls));
            entry.pan = controls.pan;
            entry.muted = controls.muted;
            entry.independentPan = controls.independent_pan;
            track.sends.push_back(entry);
        }
        track.volumeAutomation = volumePoints(session, raw.id);
        track.panAutomation = panPoints(session, raw.id);
        track.midi = midiOf(session, raw.id);
        track.inserts = insertsOf(session, DAW_INSERT_OWNER_TRACK, raw.id);
        dump.tracks.push_back(track);
    }
    for (uint32_t index = 0; index < snap.bus_count; ++index) {
        auto raw = abi<daw_bus>();
        CHECK_OK(session, daw_get_bus(session, index, &raw));
        DumpBus bus;
        bus.id = raw.id;
        bus.gainDb = raw.gain_db;
        bus.pan = raw.pan;
        bus.muted = raw.muted;
        bus.outputBusId = raw.output_bus_id;
        bus.name = raw.name;
        bus.gainAutomation = busGainPoints(session, raw.id);
        bus.inserts = insertsOf(session, DAW_INSERT_OWNER_BUS, raw.id);
        dump.buses.push_back(bus);
    }
    for (uint32_t index = 0; index < snap.master_insert_count; ++index) {
        auto plugin = abi<daw_plugin>();
        CHECK_OK(session, daw_get_master_insert(session, index, &plugin));
        DumpInsert entry;
        entry.id = plugin.id;
        entry.type = plugin.type;
        entry.subtype = plugin.subtype;
        entry.manufacturer = plugin.manufacturer;
        entry.bypassed = plugin.bypassed;
        entry.latencyFrames = plugin.latency_frames;
        entry.available = plugin.available;
        entry.name = plugin.name;
        dump.masterInserts.push_back(entry);
    }
    dump.masterAutomation = masterGainPoints(session);
    uint32_t tempoCount = 0;
    CHECK_OK(session, daw_get_tempo_count(session, &tempoCount));
    for (uint32_t index = 0; index < tempoCount; ++index) {
        auto point = abi<daw_tempo_point>();
        point.version = DAW_TEMPO_POINT_VERSION;
        CHECK_OK(session, daw_get_tempo_point(session, index, &point));
        dump.tempo.emplace_back(point.frame, point.bpm);
    }
    uint32_t signatureCount = 0;
    CHECK_OK(session, daw_get_time_signature_count(session, &signatureCount));
    for (uint32_t index = 0; index < signatureCount; ++index) {
        auto point = abi<daw_time_signature_point>();
        point.version = DAW_TIME_SIGNATURE_POINT_VERSION;
        CHECK_OK(session, daw_get_time_signature_point(session, index, &point));
        dump.signatures.emplace_back(point.frame, static_cast<int>(point.numerator), static_cast<int>(point.denominator));
    }
    uint32_t markerCount = 0;
    CHECK_OK(session, daw_get_marker_count(session, &markerCount));
    for (uint32_t index = 0; index < markerCount; ++index) {
        auto marker = abi<daw_marker>();
        marker.version = DAW_MARKER_VERSION;
        CHECK_OK(session, daw_get_marker(session, index, &marker));
        dump.markers.emplace_back(marker.frame, std::string(marker.name));
    }
    return dump;
}

inline std::string describe(const Dump& dump) {
    std::string text = "rev=" + std::to_string(dump.revision) + " master=" + std::to_string(dump.masterGain);
    for (const auto& track : dump.tracks)
        text += "\n  track id=" + std::to_string(track.id) + " \"" + track.name + "\" clips=" +
                std::to_string(track.clips.size()) + " takes=" + std::to_string(track.takes.size()) +
                " sends=" + std::to_string(track.sends.size()) + " midi=" + std::to_string(track.midi.size()) +
                " inserts=" + std::to_string(track.inserts.size()) + " volPts=" +
                std::to_string(track.volumeAutomation.size()) + " panPts=" + std::to_string(track.panAutomation.size());
    for (const auto& bus : dump.buses)
        text += "\n  bus id=" + std::to_string(bus.id) + " \"" + bus.name + "\" inserts=" +
                std::to_string(bus.inserts.size());
    text += "\n  masterInserts=" + std::to_string(dump.masterInserts.size()) +
            " tempoPts=" + std::to_string(dump.tempo.size()) + " sigPts=" + std::to_string(dump.signatures.size()) +
            " markers=" + std::to_string(dump.markers.size());
    return text;
}

} // namespace e2e

#endif // MY_DAW_E2E_HPP
