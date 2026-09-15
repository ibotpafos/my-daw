#pragma once
#include <memory>
#include <array>
#include <span>
#include <string>
#include <vector>
namespace daw {
// Immutable, validated, interleaved stereo float32 at 48 kHz.
class Clip {
    std::vector<float> samples_;
    std::array<float,512> peaks_{};
public:
    explicit Clip(std::vector<float> samples);
    const std::vector<float>& samples() const { return samples_; }
    const std::array<float,512>& peaks() const { return peaks_; }
    size_t frames() const { return samples_.size()/2; }
};
std::shared_ptr<const Clip> decodeWav(std::span<const unsigned char> bytes);
std::shared_ptr<const Clip> readWav(const std::string& path);
std::vector<unsigned char> encodePCM(const Clip& clip);
std::shared_ptr<const Clip> decodePCM(std::span<const unsigned char> bytes);
}
