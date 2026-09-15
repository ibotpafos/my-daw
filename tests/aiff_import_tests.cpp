#include "audio/import_job.hpp"
#include "domain/session.hpp"
#include "jobs/limiter.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while (false)
template <class Fn> void rejects(Fn&& fn) { bool rejected = false; try { fn(); } catch (...) { rejected = true; } CHECK(rejected); }

static void putBE(std::vector<unsigned char>& b, size_t at, uint32_t v, unsigned w) {
    for (unsigned i = 0; i < w; ++i) b[at + i] = static_cast<unsigned char>((v >> (8 * (w - 1 - i))) & 0xFF);
}
static void putBE16(std::vector<unsigned char>& b, size_t at, uint16_t v) {
    b[at] = static_cast<unsigned char>((v >> 8) & 0xFF);
    b[at + 1] = static_cast<unsigned char>(v & 0xFF);
}
// AIFF stores the sample rate as an 80-bit IEEE extended float (big-endian).
static void putExt80(std::vector<unsigned char>& b, size_t at, double v) {
    int e; double m = std::frexp(v, &e); m *= 2.0; e -= 1;
    int expField = e + 16383;
    uint64_t mant = static_cast<uint64_t>(std::round(m * 9223372036854775808.0));
    b[at] = static_cast<unsigned char>((expField >> 8) & 0x7F);
    b[at + 1] = static_cast<unsigned char>(expField & 0xFF);
    for (int i = 0; i < 8; ++i) b[at + 2 + i] = static_cast<unsigned char>((mant >> (56 - 8 * i)) & 0xFF);
}

enum class Kind { Int, Float };
static std::vector<unsigned char> makeAiff(uint32_t rate, uint16_t channels, uint16_t bits, Kind kind,
                                            uint32_t frames, const std::function<float(unsigned, unsigned)>& gen) {
    const unsigned width = bits / 8;
    const unsigned frameBytes = channels * width;
    const uint32_t dataBytes = frames * frameBytes;
    unsigned commLogical = 18; // channels(2)+samples(4)+bits(2)+rate(10)
    const bool aifc = (kind == Kind::Float);
    if (aifc) commLogical += 4 + 1 + 4; // compressionType(4) + pascal count(1) + name(4)
    const unsigned commPadded = commLogical + (commLogical & 1);
    const unsigned ssndLogical = 8 + dataBytes; // offset(4)+blockSize(4)+data
    const unsigned ssndPadded = ssndLogical + (ssndLogical & 1);
    const unsigned formBody = 4 + (8 + commPadded) + (8 + ssndPadded);
    std::vector<unsigned char> b(8 + formBody, 0);
    b[0] = 'F'; b[1] = 'O'; b[2] = 'R'; b[3] = 'M';
    putBE(b, 4, static_cast<uint32_t>(formBody), 4);
    const char* ft = aifc ? "AIFC" : "AIFF";
    for (int i = 0; i < 4; ++i) b[8 + i] = static_cast<unsigned char>(ft[i]);
    size_t p = 12;
    b[p] = 'C'; b[p + 1] = 'O'; b[p + 2] = 'M'; b[p + 3] = 'M';
    putBE(b, p + 4, commLogical, 4);
    putBE16(b, p + 8, channels);
    putBE(b, p + 10, frames, 4);
    putBE16(b, p + 14, bits);
    putExt80(b, p + 16, static_cast<double>(rate));
    if (aifc) { b[p + 26] = 'f'; b[p + 27] = 'l'; b[p + 28] = '3'; b[p + 29] = '2'; b[p + 30] = 4; b[p + 31] = 'f'; b[p + 32] = 'l'; b[p + 33] = '3'; b[p + 34] = '2'; }
    p += 8 + commPadded;
    b[p] = 'S'; b[p + 1] = 'S'; b[p + 2] = 'N'; b[p + 3] = 'D';
    putBE(b, p + 4, ssndLogical, 4);
    putBE(b, p + 8, 0, 4); // offset
    putBE(b, p + 12, 0, 4); // blockSize
    size_t d = p + 16;
    for (uint32_t f = 0; f < frames; ++f) for (unsigned ch = 0; ch < channels; ++ch) {
        const float v = gen(f, ch);
        if (kind == Kind::Float) {
            uint32_t u; std::memcpy(&u, &v, 4); putBE(b, d, u, 4); d += 4;
        } else {
            int32_t iv;
            if (bits == 16) iv = static_cast<int32_t>(std::lround(v * 32767.0f));
            else if (bits == 24) iv = static_cast<int32_t>(std::lround(v * 8388607.0f));
            else iv = static_cast<int32_t>(std::lround(v * 2147483647.0f));
            const int32_t lim = static_cast<int32_t>(1) << (bits - 1);
            if (iv > lim - 1) iv = lim - 1;
            if (iv < -lim) iv = -lim;
            putBE(b, d, static_cast<uint32_t>(static_cast<uint32_t>(iv)), width); d += width;
        }
    }
    return b;
}

int main() { try {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const auto baseline = daw::backgroundJobsInFlight();
    unsigned counter = 0;
    auto runCase = [&](uint32_t rate, uint16_t channels, uint16_t bits, Kind kind) {
        const uint32_t frames = rate / 10; // 0.1 s of audio
        const std::filesystem::path path = dir / ("mydaw-aiff-" + std::to_string(getpid()) + "-" + std::to_string(counter++) + ".aif");
        struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ig; std::filesystem::remove(p, ig); } } cleanup{path};
        auto gen = [](unsigned, unsigned ch) { return ch == 0 ? 0.5f : -0.3f; };
        auto bytes = makeAiff(rate, channels, bits, kind, frames, gen);
        { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }

        auto job = daw::startAiffImport(path.string());
        for (int i = 0; i < 2000 && job->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(job->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Ready);
        CHECK(job->sourceSampleRate.load(std::memory_order_acquire) == rate);
        CHECK(job->sourceChannels.load(std::memory_order_acquire) == channels);
        const uint64_t expected = (uint64_t(frames) * 48000 + rate / 2) / rate;
        CHECK(job->sourceFrames.load(std::memory_order_acquire) == frames);
        CHECK(job->outputFrames.load(std::memory_order_acquire) == expected);
        auto clip = daw::importClip(*job);
        CHECK(clip != nullptr);
        CHECK(clip->frames() == expected);

        double lsum = 0, rsum = 0; bool bad = false;
        for (size_t i = 0; i < clip->samples().size(); ++i) {
            const float v = clip->samples()[i];
            if (!std::isfinite(v) || std::abs(v) > 1.05f) { bad = true; break; }
            if (i % 2 == 0) lsum += v; else rsum += v;
        }
        CHECK(!bad);
        const double lmean = lsum / (expected ? expected : 1), rmean = rsum / (expected ? expected : 1);
        if (channels == 1) { CHECK(std::abs(lmean - 0.5) < 0.05 && std::abs(rmean - 0.5) < 0.05); }
        else { CHECK(std::abs(lmean - 0.5) < 0.05 && std::abs(rmean + 0.3) < 0.05); }

        daw::cancelImport(*job);
        for (int i = 0; i < 2000 && daw::backgroundJobsInFlight() != baseline; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(daw::backgroundJobsInFlight() == baseline);
    };

    for (uint32_t rate : {uint32_t(44100), uint32_t(48000), uint32_t(96000)})
        for (uint16_t channels : {uint16_t(1), uint16_t(2)})
            for (auto spec : {std::make_pair(uint16_t(16), Kind::Int),
                              std::make_pair(uint16_t(24), Kind::Int),
                              std::make_pair(uint16_t(32), Kind::Int),
                              std::make_pair(uint16_t(32), Kind::Float)})
                runCase(rate, channels, spec.first, spec.second);

    // A malformed FORM/AIFF (no COMM/SSND) must be rejected by the decoder.
    {
        const auto path = dir / ("mydaw-aiff-bad-" + std::to_string(getpid()) + ".aif");
        struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ig; std::filesystem::remove(p, ig); } } cleanup{path};
        std::vector<unsigned char> bad(64, 0); std::memcpy(bad.data(), "FORM", 4); putBE(bad, 4, 40, 4); std::memcpy(bad.data() + 8, "AIFF", 4);
        { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bad.data()), static_cast<std::streamsize>(bad.size())); }
        rejects([&] { (void)daw::readAiff(path.string()); });
    }

    std::cout << "PASS: AIFF/AIFC import (16/24/32-bit int, 32-bit float; mono/stereo; 44.1/48/96 kHz) decodes, resamples to 48 kHz, no NaN, mono->stereo correct" << std::endl;
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; } }
