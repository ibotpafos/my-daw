#pragma once
#include "audio/clip.hpp"
#include <atomic>
#include <cstdint>

namespace daw {
// One biquad (direct form I) shared by the offline and online K-weighting.
struct LoudnessBiquad {
  double b0, b1, b2, a1, a2;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  double process(double x) noexcept {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    return y;
  }
};
// Online momentary (400 ms) and short-term (3 s) master loudness per
// BS.1770-4: K-weighted per-frame power, 100 ms energy blocks on a fixed
// 30-slot ring, sliding windows over the last 4/30 blocks. process() is
// RT-safe — no allocations, only a release-store at each block boundary.
// Sentinel -200 dBFS-equivalent means "nothing measured/fully quiet".
class MasterLoudnessTracker {
public:
  void process(const float *left, const float *right,
               uint32_t frames) noexcept;
  void reset() noexcept;
  float momentaryLufs() const noexcept {
    return momentary_.load(std::memory_order_acquire);
  }
  float shortTermLufs() const noexcept {
    return short_.load(std::memory_order_acquire);
  }

private:
  LoudnessBiquad shelfL_{1.53512485958697, -2.69169618940638,
                         1.19839281085285, -1.69065929318241,
                         0.73248077421585};
  LoudnessBiquad shelfR_ = shelfL_;
  LoudnessBiquad highpassL_{1.0, -2.0, 1.0, -1.99004715666020,
                            0.99007225734356};
  LoudnessBiquad highpassR_ = highpassL_;
  double blockSum_ = 0;
  uint32_t sinceBlock_ = 0;
  float blocks_[30] = {0};
  size_t filled_ = 0, next_ = 0;
  std::atomic<float> momentary_{-200.0f};
  std::atomic<float> short_{-200.0f};
};
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
