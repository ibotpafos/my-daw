#include "audio/loudness.hpp"

#include <cmath>
#include <limits>
#include <vector>

namespace daw {
namespace {
struct Biquad {
  double b0, b1, b2, a1, a2;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  double process(double x) {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    return y;
  }
};
double lufsOf(double power) {
  return power > 0 ? -0.691 + 10.0 * std::log10(power)
                   : -std::numeric_limits<double>::infinity();
}
} // namespace

LoudnessReport measureLoudness(const Clip &clip) {
  LoudnessReport report;
  const auto &samples = clip.samples();
  const size_t frames = samples.size() / 2;
  if (frames == 0) {
    report.gatedSilence = true;
    return report;
  }
  // K-weighting: high shelf then RLB high-pass, per channel.
  Biquad shelfL{1.53512485958697, -2.69169618940638, 1.19839281085285,
                -1.69065929318241, 0.73248077421585};
  Biquad shelfR = shelfL;
  Biquad highpassL{1.0, -2.0, 1.0, -1.99004715666020, 0.99007225734356};
  Biquad highpassR = highpassL;
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
