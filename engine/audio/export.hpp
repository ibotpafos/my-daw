#pragma once
#include "domain/session.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace daw {
enum class WavFormat : int { PCM24 = 1, Float32 = 2 };
struct ExportResult {
  const uint64_t revision, totalFrames;
  std::atomic<uint64_t> renderedFrames{0};
  std::atomic<int> status{0}; // 0 running, 1 success, 2 failure, 3 canceled
  std::atomic<bool> cancel{false};
  char error[512]{};
  ExportResult(uint64_t rev, uint64_t total)
      : revision(rev), totalFrames(total) {}
};
// Writes the requested timeline range followed by Renderer’s bounded finite
// declared tail. WAV header and ExportResult::totalFrames include both.
void writeWav(const State &snapshot, const std::string &path, WavFormat format,
              ExportResult *progress = nullptr);
void writeWavRange(const State &snapshot, const std::string &path,
                   WavFormat format, uint64_t startFrame, uint64_t endFrame,
                   ExportResult *progress = nullptr);
std::shared_ptr<ExportResult> startExport(State snapshot, std::string path,
                                          WavFormat format);
std::shared_ptr<ExportResult> startExportRange(State snapshot, std::string path,
                                               WavFormat format,
                                               uint64_t startFrame,
                                               uint64_t endFrame);
} // namespace daw
