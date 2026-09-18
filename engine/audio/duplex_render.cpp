#include "audio/duplex_render.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
std::unique_ptr<RecordingWriter> prepareDuplexCapture(Renderer &renderer, const State &state,
                                                      uint64_t capacity, const std::string &path,
                                                      uint64_t start, uint64_t loopStart,
                                                      uint64_t loopEnd, uint64_t preroll) {
    constexpr uint64_t timelineLimit = 48000 * 600;
    const bool looping = loopStart != 0 || loopEnd != 0;
    if (start >= timelineLimit || capacity == 0 || capacity > 48000 * 60 || preroll > 48000 * 30 ||
        (!looping && capacity > timelineLimit - start) ||
        (looping && (loopEnd <= loopStart || start != loopStart)))
        throw Error("Invalid duplex recording range");
    const auto lead = std::min(preroll, start);
    auto capture = std::make_unique<RecordingWriter>(path, start, capacity, 48000 * 2, lead);
    renderer.prepare(state, start - lead, loopStart, loopEnd, looping ? 0 : start + capacity);
    return capture;
}
void renderDuplexBlock(Renderer &renderer, RecordingWriter &capture, bool monitor,
                       const float *input, float *left, float *right, uint32_t frames) noexcept {
    capture.writeMono(input, frames);
    renderer.render(left, right, frames);
    if (!monitor)
        return;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // Match capture's invalid-sample policy; never send NaN/Inf to headphones.
        const float value =
            std::isfinite(input[frame]) ? std::clamp(input[frame], -16.0f, 16.0f) : 0.0f;
        left[frame] += value;
        right[frame] += value;
    }
}
}
