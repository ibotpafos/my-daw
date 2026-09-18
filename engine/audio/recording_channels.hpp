#pragma once
#include <cstdint>
#include <span>

namespace daw {
// Prepared channel selection, independent of HAL buffer-list representation.
// Supports one interleaved stream or multiple (including planar) buffers.
struct RecordingChannel {
    uint32_t buffer = 0, channel = 0, stride = 0;
    bool operator==(const RecordingChannel&) const = default;
};
inline bool locateRecordingChannel(std::span<const uint32_t> layout, uint32_t channel,
                                   RecordingChannel& out) noexcept {
    if (layout.empty() || layout.size() > 128 || channel >= 128) return false;
    uint32_t first = 0;
    for (uint32_t i = 0; i < layout.size(); ++i) {
        if (!layout[i] || layout[i] > 128 - first) return false;
        if (channel >= first && channel - first < layout[i]) out = {i, channel - first, layout[i]};
        first += layout[i];
    }
    return channel < first;
}
inline bool recordingBufferFrames(uint32_t bytes, uint32_t channels, uint32_t& frames) noexcept {
    if (!channels || channels > 128 || bytes % (channels * sizeof(float))) return false;
    frames = bytes / (channels * sizeof(float));
    return frames > 0 && frames <= 4096;
}
}
