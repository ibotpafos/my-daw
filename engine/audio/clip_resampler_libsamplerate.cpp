#include "audio/clip_resampler.hpp"
#include "audio/clip.hpp"
#include "domain/session.hpp"

#include <samplerate.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace daw {
namespace {
constexpr uint32_t kProjectRate = 48000;
constexpr uint64_t kBlockFrames = 4096;
}

std::vector<float> resampleStereoTo48k(const std::vector<float> &source, uint32_t sourceRate) {
    return resampleStereoTo48k(source, sourceRate, {});
}

std::vector<float> resampleStereoTo48k(const std::vector<float> &source, uint32_t sourceRate,
                                      const ImportControl &control) {
    checkImportCanceled(control);
    if (sourceRate == 0 || source.empty() || source.size() % 2 != 0)
        throw Error("Invalid stereo audio for sample-rate conversion");
    const uint64_t sourceFrames = source.size() / 2;
    // Bound duration before multiplication and allocation. The decoder has the
    // same 60-second import limit; this also protects direct adapter callers.
    if (sourceFrames > uint64_t(sourceRate) * 60)
        throw Error("Resampled WAV must be at most 60 seconds");
    const uint64_t expected = std::max<uint64_t>(
        1, (sourceFrames * kProjectRate + sourceRate / 2) / sourceRate);
    if (expected > uint64_t(kProjectRate) * 60)
        throw Error("Resampled WAV must be at most 60 seconds");
    const double ratio = double(kProjectRate) / sourceRate;
    if (!src_is_valid_ratio(ratio))
        throw Error("Unsupported sample-rate conversion ratio");

    for (size_t offset = 0; offset < source.size();) {
        checkImportCanceled(control);
        const size_t end = std::min(source.size(), offset + size_t(kBlockFrames) * 2);
        for (; offset < end; ++offset)
            if (!std::isfinite(source[offset]))
                throw Error("Non-finite audio for sample-rate conversion");
    }
    if (sourceRate == kProjectRate) {
        updateImportProgress(control, 1, 1);
        return source;
    }
    // Preserve the existing native contract for sub-frame clips.
    if (expected == 1) {
        updateImportProgress(control, 1, 1);
        return {source[0], source[1]};
    }

    int error = 0;
    std::unique_ptr<SRC_STATE, decltype(&src_delete)> converter(
        src_new(SRC_SINC_BEST_QUALITY, 2, &error), &src_delete);
    if (!converter)
        throw Error(std::string("Create sample-rate converter: ") + src_strerror(error));
    std::vector<float> output(static_cast<size_t>(expected) * 2, 0.0f);
    uint64_t consumed = 0, produced = 0;
    while (produced < expected) {
        checkImportCanceled(control);
        SRC_DATA block{};
        block.data_in = source.data() + consumed * 2;
        block.data_out = output.data() + produced * 2;
        block.input_frames = static_cast<long>(std::min(kBlockFrames, sourceFrames - consumed));
        block.output_frames = static_cast<long>(std::min(kBlockFrames, expected - produced));
        block.src_ratio = ratio;
        block.end_of_input = consumed + static_cast<uint64_t>(block.input_frames) == sourceFrames;
        error = src_process(converter.get(), &block);
        checkImportCanceled(control);
        if (error != 0)
            throw Error(std::string("Convert sample rate: ") + src_strerror(error));
        if (block.input_frames_used < 0 || block.input_frames_used > block.input_frames ||
            block.output_frames_gen < 0 || block.output_frames_gen > block.output_frames)
            throw Error("Sample-rate converter returned invalid frame counts");
        consumed += static_cast<uint64_t>(block.input_frames_used);
        produced += static_cast<uint64_t>(block.output_frames_gen);
        updateImportProgress(control, produced, expected);
        if (block.input_frames_used == 0 && block.output_frames_gen == 0) {
            // libsamplerate can floor the fractional final frame; the project
            // rounds duration to the nearest frame. Only that one terminal
            // frame may retain zero extension. Never hide a truncated stream.
            if (consumed != sourceFrames || expected - produced > 1)
                throw Error("Sample-rate converter stopped before the expected duration");
            break;
        }
    }
    for (size_t offset = 0; offset < output.size();) {
        checkImportCanceled(control);
        const size_t end = std::min(output.size(), offset + size_t(kBlockFrames) * 2);
        for (; offset < end; ++offset)
            if (!std::isfinite(output[offset]))
                throw Error("Sample-rate converter returned non-finite audio");
    }
    updateImportProgress(control, 1, 1);
    return output;
}
} // namespace daw
