#include "audio/input.hpp"
#include "domain/session.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
CaptureBuffer::CaptureBuffer(uint64_t capacityFrames) {
    if (!capacityFrames || capacityFrames > 48000 * 60) throw Error("Capture capacity must be 1–60 seconds");
    samples_.resize(static_cast<size_t>(capacityFrames) * 2);
}

void CaptureBuffer::writeMono(const float* input, uint32_t count) noexcept {
    if (!input || !count) return;
    auto begin = frames_.load(std::memory_order_relaxed);
    auto available = capacity() - std::min(begin, capacity());
    auto accepted = std::min<uint64_t>(available, count);
    for (uint64_t i = 0; i < accepted; ++i) {
        auto value = std::isfinite(input[i]) ? std::clamp(input[i],-16.0f,16.0f) : 0.0f;
        samples_[static_cast<size_t>((begin + i) * 2)] = value;
        samples_[static_cast<size_t>((begin + i) * 2 + 1)] = value;
    }
    frames_.store(begin + accepted, std::memory_order_release);
    if (accepted < count) overflow_.store(true, std::memory_order_release);
}

std::shared_ptr<const Clip> CaptureBuffer::finish() const {
    auto count = frames();
    if (!count) throw Error("Recording contains no audio frames");
    std::vector<float> result(samples_.begin(), samples_.begin() + static_cast<size_t>(count * 2));
    return std::make_shared<const Clip>(std::move(result));
}
}
