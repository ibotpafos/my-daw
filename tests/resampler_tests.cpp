#include "audio/clip_resampler.hpp"
#include "audio/clip.hpp"
#include "domain/session.hpp"
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class Function> void rejects(Function function) {
    bool rejected = false;
    try {
        function();
    } catch (const daw::Error &) {
        rejected = true;
    }
    check(rejected, "Malformed resampler input was accepted");
}
}

int main() {
    try {
        std::atomic<uint32_t> progress{0};
        daw::ImportControl control{nullptr, &progress};
        const std::vector<float> identity{0.25f, -0.125f, 0.5f, 0.0f};
        check(daw::resampleStereoTo48k(identity, 48000, control) == identity, "48 kHz identity");
        check(progress.load() == 100, "Identity progress");
        for (uint32_t rate : {8000U, 44100U, 96000U, 192000U}) {
            std::vector<float> source(size_t(rate) * 2, 0.0f);
            for (size_t frame = 0; frame < rate; ++frame)
                source[frame * 2] = 0.25f;
            progress.store(0);
            const auto output = daw::resampleStereoTo48k(source, rate, control);
            check(output.size() == 96000, "One second must contain 48000 output frames");
            check(progress.load() == 100, "Conversion progress");
            for (size_t frame = 0; frame < output.size() / 2; ++frame) {
                check(std::isfinite(output[frame * 2]), "Finite output");
                check(output[frame * 2 + 1] == 0.0f, "No cross-channel leakage");
            }
            check(std::abs(output[48000] - 0.25f) < 0.001f, "Interior DC amplitude");
        }
        for (size_t frames : {size_t{3}, size_t{11}, size_t{4097}}) {
            const auto output = daw::resampleStereoTo48k(std::vector<float>(frames * 2, 0.0f), 96000);
            check(output.size() == ((frames + 1) / 2) * 2, "Fractional duration rounds up");
            for (float sample : output)
                check(sample == 0.0f, "Silence remains silent");
        }
        check(daw::resampleStereoTo48k({0.25f, -0.125f}, 192000) ==
                  std::vector<float>({0.25f, -0.125f}), "Sub-frame clip preservation");

        // A tone above the destination Nyquist frequency must be filtered, not
        // aliased into the project. All filtering is performed by libsamplerate.
        std::vector<float> highTone(96000 * 2, 0.0f);
        for (size_t frame = 0; frame < 96000; ++frame)
            highTone[frame * 2] = static_cast<float>(
                0.5 * std::sin(2 * std::numbers::pi * 30000 * double(frame) / 96000));
        const auto filtered = daw::resampleStereoTo48k(highTone, 96000);
        double energy = 0;
        for (size_t frame = 1024; frame < 48000 - 1024; ++frame)
            energy += double(filtered[frame * 2]) * filtered[frame * 2];
        check(std::sqrt(energy / (48000 - 2048)) < 0.001, "Downsampling anti-alias filter");

        rejects([&] { (void)daw::resampleStereoTo48k(identity, 0); });
        rejects([] { (void)daw::resampleStereoTo48k({}, 44100); });
        rejects([] { (void)daw::resampleStereoTo48k({0.0f}, 44100); });
        rejects([] { (void)daw::resampleStereoTo48k({std::numeric_limits<float>::infinity(), 0}, 44100); });
        rejects([] { (void)daw::resampleStereoTo48k(std::vector<float>(8000 * 60 * 2 + 2), 8000); });
        std::atomic<bool> cancel{true};
        control.cancel = &cancel;
        bool canceled = false;
        try {
            (void)daw::resampleStereoTo48k(identity, 44100, control);
        } catch (const daw::ImportCanceled &) {
            canceled = true;
        }
        check(canceled, "Cancellation must precede conversion");
        std::cout << "PASS: libsamplerate integration, rates, duration, channels, anti-aliasing, input guards and cancellation\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
