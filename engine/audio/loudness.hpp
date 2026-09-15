#pragma once
#include "audio/clip.hpp"

namespace daw {
// Loudness/peak report for a finished signal (BS.1770-4 metering model).
struct LoudnessReport {
  double integratedLufs = -100.0; // K-weighted integrated loudness
  double truePeakDb = -400.0;    // dBTP from 4x oversampled polyphase FIR
  bool gatedSilence = false;     // nothing survived the -70 LUFS gate
};
// Integrated loudness over an interleaved stereo clip at 48 kHz: K-weighting
// biquads (shelf + RLB high-pass), 400 ms gate blocks with 75% overlap, the
// absolute -70 LUFS and relative -10 LU gates, channel weights L=R=1.0.
// Clips shorter than one gate block are measured as a single block.
LoudnessReport measureLoudness(const Clip &clip);
} // namespace daw
