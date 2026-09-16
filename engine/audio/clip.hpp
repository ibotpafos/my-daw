#pragma once
#include <memory>
#include <exception>
#include <array>
#include <atomic>
#include <span>
#include <string>
#include <vector>
namespace daw {
// A decode/import caller may provide this small control block to make a
// potentially expensive file import observable and cancellable.  The public
// synchronous APIs intentionally keep their original behavior by using the
// default empty control.
struct ImportControl {
    std::atomic<bool>* cancel = nullptr;
    std::atomic<uint32_t>* progress = nullptr;
    uint32_t progressBegin = 0;
    uint32_t progressEnd = 100;
    void (*setPhase)(void*, uint8_t) = nullptr;
    void* phaseContext = nullptr;
};
class ImportCanceled final : public std::exception {
public:
    const char* what() const noexcept override { return "WAV import canceled"; }
};
void checkImportCanceled(const ImportControl& control);
void updateImportProgress(const ImportControl& control, uint64_t completed, uint64_t total);
void updateImportPhase(const ImportControl& control, uint8_t phase);
// Immutable, validated, interleaved stereo float32 at 48 kHz.
class Clip {
    std::vector<float> samples_;
    std::array<float,512> peaks_{};
    // The compact cache remains the stable API for overview meters.  The
    // denser cache is built alongside it exactly once at import time so the
    // editor never has to walk PCM samples on its UI thread.
    std::array<float,2048> detailPeaks_{};
public:
    explicit Clip(std::vector<float> samples, const ImportControl& control = {});
    const std::vector<float>& samples() const { return samples_; }
    const std::array<float,512>& peaks() const { return peaks_; }
    const std::array<float,2048>& detailPeaks() const { return detailPeaks_; }
    size_t frames() const { return samples_.size()/2; }
};
std::shared_ptr<const Clip> decodeWav(std::span<const unsigned char> bytes);
std::shared_ptr<const Clip> decodeWav(std::span<const unsigned char> bytes, const ImportControl& control,
                                      uint32_t* sourceRate = nullptr, uint32_t* sourceChannels = nullptr,
                                      uint64_t* sourceFrames = nullptr);
std::shared_ptr<const Clip> readWav(const std::string& path);
std::shared_ptr<const Clip> decodeAiff(std::span<const unsigned char> bytes);
std::shared_ptr<const Clip> decodeAiff(std::span<const unsigned char> bytes, const ImportControl& control,
                                       uint32_t* sourceRate = nullptr, uint32_t* sourceChannels = nullptr,
                                       uint64_t* sourceFrames = nullptr);
std::shared_ptr<const Clip> readAiff(const std::string& path);
std::shared_ptr<const Clip> readAiff(const std::string& path, const ImportControl& control,
                                     uint32_t* sourceRate = nullptr, uint32_t* sourceChannels = nullptr,
                                     uint64_t* sourceFrames = nullptr);
std::shared_ptr<const Clip> readWav(const std::string& path, const ImportControl& control,
                                    uint32_t* sourceRate = nullptr, uint32_t* sourceChannels = nullptr,
                                    uint64_t* sourceFrames = nullptr);
std::vector<unsigned char> encodePCM(const Clip& clip);
std::shared_ptr<const Clip> decodePCM(std::span<const unsigned char> bytes);
}
