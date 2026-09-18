#pragma once
#include "audio/recording.hpp"
#include "audio/renderer.hpp"

namespace daw {
// Preparation is control-thread only. No model mutation or invented silent track.
// For a linear capture the renderer continues through the take budget, including
// in an empty project and beyond the end of the backing arrangement.
std::unique_ptr<RecordingWriter> prepareDuplexCapture(Renderer &, const State &,
                                                      uint64_t capacityFrames,
                                                      const std::string &recoveryPath,
                                                      uint64_t startFrame, uint64_t loopStart,
                                                      uint64_t loopEnd, uint64_t prerollFrames);
// AUHAL validates and supplies planar buffers. No allocation, locks or file I/O:
// the existing SPSC writer captures raw mono; the existing renderer owns backing
// and metronome; optional dry unity monitoring is added only to output.
void renderDuplexBlock(Renderer &, RecordingWriter &, bool monitorInput, const float *input,
                       float *left, float *right, uint32_t frames) noexcept;
}
