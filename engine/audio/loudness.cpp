#include "audio/loudness.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace daw {
namespace {
double lufsOf(double power) {
  return power > 0 ? -0.691 + 10.0 * std::log10(power)
                   : -std::numeric_limits<double>::infinity();
}
} // namespace

void MasterLoudnessTracker::process(const float *left, const float *right,
                                    uint32_t frames) noexcept {
  for (uint32_t i = 0; i < frames; ++i) {
    const double l = highpassL_.process(shelfL_.process(left[i]));
    const double r = highpassR_.process(shelfR_.process(right[i]));
    blockSum_ += l * l + r * r;
    if (++sinceBlock_ < 4800)
      continue;
    blocks_[next_] = static_cast<float>(blockSum_ / 4800.0);
    next_ = (next_ + 1) % 30;
    if (filled_ < 30)
      ++filled_;
    blockSum_ = 0;
    sinceBlock_ = 0;
    double momentarySum = 0, shortSum = 0;
    for (size_t k = 0; k < filled_; ++k) {
      const size_t index = (next_ + 30 - filled_ + k) % 30;
      shortSum += blocks_[index];
      if (k + 4 >= filled_)
        momentarySum += blocks_[index];
    }
    const size_t momentaryCount = filled_ < 4 ? filled_ : 4;
    const double momentaryPower = momentaryCount ? momentarySum / static_cast<double>(momentaryCount) : 0.0;
    const double shortPower = filled_ ? shortSum / static_cast<double>(filled_) : 0.0;
    momentary_.store(momentaryPower > 1e-12 ? float(lufsOf(momentaryPower)) : -200.0f,
                     std::memory_order_release);
    short_.store(shortPower > 1e-12 ? float(lufsOf(shortPower)) : -200.0f,
                 std::memory_order_release);
  }
}
void MasterLoudnessTracker::reset() noexcept {
  LoudnessBiquad shelf{1.53512485958697, -2.69169618940638,
                       1.19839281085285, -1.69065929318241, 0.73248077421585};
  shelfL_ = shelf; shelfR_ = shelf;
  LoudnessBiquad highpass{1.0, -2.0, 1.0, -1.99004715666020, 0.99007225734356};
  highpassL_ = highpass; highpassR_ = highpass;
  blockSum_ = 0; sinceBlock_ = 0; filled_ = 0; next_ = 0;
  momentary_.store(-200.0f, std::memory_order_release);
  short_.store(-200.0f, std::memory_order_release);
}

LoudnessReport measureLoudness(const Clip &clip) {
  LoudnessReport report;
  const auto &samples = clip.samples();
  const size_t frames = samples.size() / 2;
  if (frames == 0) {
    report.gatedSilence = true;
    return report;
  }
  // K-weighting: high shelf then RLB high-pass, per channel.
  LoudnessBiquad shelfL{1.53512485958697, -2.69169618940638, 1.19839281085285,
                -1.69065929318241, 0.73248077421585};
  LoudnessBiquad shelfR = shelfL;
  LoudnessBiquad highpassL{1.0, -2.0, 1.0, -1.99004715666020, 0.99007225734356};
  LoudnessBiquad highpassR = highpassL;
  std::vector<double> prefix(frames + 1, 0.0);
  for (size_t i = 0; i < frames; ++i) {
    const double l = highpassL.process(shelfL.process(samples[i * 2]));
    const double r = highpassR.process(shelfR.process(samples[i * 2 + 1]));
    prefix[i + 1] = prefix[i] + l * l + r * r;
  }
  // Gate-block powers, 400 ms at 75% overlap; short files measure whole.
  constexpr size_t kBlock = 19200, kStep = 4800;
  std::vector<double> powers;
  if (frames < kBlock)
    powers.push_back(prefix[frames] / static_cast<double>(frames));
  else
    for (size_t start = 0; start + kBlock <= frames; start += kStep)
      powers.push_back((prefix[start + kBlock] - prefix[start]) /
                       static_cast<double>(kBlock));
  std::vector<double> kept;
  for (const double power : powers)
    if (lufsOf(power) > -70.0)
      kept.push_back(power);
  if (kept.empty()) {
    report.gatedSilence = true;
    return report;
  }
  double ungated = 0.0;
  for (const double power : kept)
    ungated += power;
  ungated /= static_cast<double>(kept.size());
  const double relative = lufsOf(ungated) - 10.0;
  double gated = 0.0;
  size_t above = 0;
  for (const double power : kept)
    if (lufsOf(power) > relative) {
      gated += power;
      ++above;
    }
  if (above == 0) {
    report.gatedSilence = true;
    return report;
  }
  report.integratedLufs = lufsOf(gated / static_cast<double>(above));
  // True peak: 4x polyphase lowpass (48-tap windowed sinc, cutoff at the
  // original Nyquist, DC gain 4). Peak alignment is irrelevant for meters.
  constexpr size_t kPhaseTaps = 12;
  double prototype[4 * kPhaseTaps];
  constexpr int kProtoLength = static_cast<int>(4 * kPhaseTaps);
  const double center = (kProtoLength - 1) / 2.0;
  for (int k = 0; k < kProtoLength; ++k) {
    const double x = k - center;
    const double sinc =
        x == 0.0 ? 1.0 : std::sin(3.14159265358979 * 0.25 * x) /
                            (3.14159265358979 * 0.25 * x);
    const double window =
        0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * k /
                             (kProtoLength - 1));
    prototype[k] = sinc * window;
  }
  double peak = 0.0;
  for (size_t channel = 0; channel < 2; ++channel)
    for (size_t i = 0; i < frames; ++i)
      for (size_t phase = 0; phase < 4; ++phase) {
        double accumulator = 0.0;
        for (size_t tap = 0; tap < kPhaseTaps; ++tap) {
          const size_t index = i >= tap ? i - tap : frames;
          if (index != frames)
            accumulator += samples[index * 2 + channel] *
                           prototype[tap * 4 + phase];
        }
        const double magnitude = std::abs(accumulator);
        if (magnitude > peak)
          peak = magnitude;
      }
  report.truePeakDb =
      peak > 0.0 ? 20.0 * std::log10(peak) : -400.0;
  return report;
}
} // namespace daw
