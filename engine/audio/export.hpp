#pragma once
#include "domain/session.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace daw {
struct GraphTailSummary;
enum class WavFormat : int { PCM24 = 1, Float32 = 2 };
enum class ExportTailMode : uint32_t { Automatic = 1, None = 2, ManualLimit = 3 };
// ManualLimit applies only to an infinite graph component; finite tails are
// always retained. The caller supplies frames at the fixed 48 kHz rate.
struct ExportOptions {
  ExportTailMode tailMode = ExportTailMode::Automatic;
  uint32_t manualTailFrames = 0;
};
struct ExportTailSummary {
  uint32_t finiteTailFrames = 0;
  uint32_t selectedTailFrames = 0;
  bool infiniteTailDetected = false;
};
struct ExportResult {
  const uint64_t revision, totalFrames;
  std::atomic<uint64_t> renderedFrames{0};
  std::atomic<int> status{0}; // 0 running, 1 success, 2 failure, 3 canceled
  std::atomic<bool> cancel{false};
  char error[512]{};
  ExportResult(uint64_t rev, uint64_t total)
      : revision(rev), totalFrames(total) {}
};
// Inspects a prepared snapshot without starting a background job. `startFrame`
// is accepted so callers validating an export range use the same renderer
// preparation path as the writer.
ExportTailSummary inspectExportTail(const State &, ExportOptions = {},
                                    uint64_t startFrame = 0);
// Resolves the public policy against an already compiled graph summary. Kept
// separate for deterministic tests and callers that have prepared a Renderer.
ExportTailSummary resolveExportTail(const GraphTailSummary &, ExportOptions = {});
// Legacy entry points keep automatic bounded behavior. New overloads let a
// caller select how an infinite VST3 declaration contributes to the tail.
void writeWav(const State &snapshot, const std::string &path, WavFormat format,
              ExportResult *progress = nullptr);
void writeWav(const State &snapshot, const std::string &path, WavFormat format,
              ExportOptions, ExportResult *progress = nullptr);
void writeWavRange(const State &snapshot, const std::string &path,
                   WavFormat format, uint64_t startFrame, uint64_t endFrame,
                   ExportResult *progress = nullptr);
void writeWavRange(const State &snapshot, const std::string &path,
                   WavFormat format, uint64_t startFrame, uint64_t endFrame,
                   ExportOptions, ExportResult *progress = nullptr);
std::shared_ptr<ExportResult> startExport(State snapshot, std::string path,
                                          WavFormat format);
std::shared_ptr<ExportResult> startExport(State snapshot, std::string path,
                                          WavFormat format, ExportOptions);
std::shared_ptr<ExportResult> startExportRange(State snapshot, std::string path,
                                               WavFormat format,
                                               uint64_t startFrame,
                                               uint64_t endFrame);
std::shared_ptr<ExportResult> startExportRange(State snapshot, std::string path,
                                               WavFormat format,
                                               uint64_t startFrame,
                                               uint64_t endFrame,
                                               ExportOptions);
} // namespace daw
