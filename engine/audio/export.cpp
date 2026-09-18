#include "audio/export.hpp"
#include "audio/renderer.hpp"
#include "jobs/limiter.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <vector>

namespace daw {
#ifdef __APPLE__
std::string replacementDirectory(const std::string &target);
#endif
namespace {
struct ExportCanceled final {};
constexpr uint32_t kMaximumManualTailFrames = 48000 * 30;
void validateExportOptions(const ExportOptions &options) {
  switch (options.tailMode) {
  case ExportTailMode::Automatic:
  case ExportTailMode::None:
    return;
  case ExportTailMode::ManualLimit:
    if (options.manualTailFrames <= kMaximumManualTailFrames)
      return;
    throw Error("Manual tail limit exceeds 30 seconds");
  }
  throw Error("Unknown export tail mode");
}
ExportTailSummary resolveExportTailImpl(const GraphTailSummary &tail,
                                        ExportOptions options) {
  validateExportOptions(options);
  uint32_t infiniteFrames = 0;
  if (tail.hasInfiniteTail) {
    switch (options.tailMode) {
    case ExportTailMode::Automatic:
      infiniteFrames = kMaximumManualTailFrames;
      break;
    case ExportTailMode::None:
      break;
    case ExportTailMode::ManualLimit:
      infiniteFrames = options.manualTailFrames;
      break;
    }
  }
  return {tail.finiteFrames, std::max(tail.finiteFrames, infiniteFrames),
          tail.hasInfiniteTail};
}
void put16(unsigned char *p, uint16_t value) {
  p[0] = static_cast<unsigned char>(value);
  p[1] = static_cast<unsigned char>(value >> 8);
}
void put32(unsigned char *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = static_cast<unsigned char>(value >> (i * 8));
}
bool writeAll(int fd, const void *data, size_t size) {
  auto p = static_cast<const unsigned char *>(data);
  while (size) {
    auto n = write(fd, p, size);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}
std::vector<unsigned char> wavHeader(WavFormat format, uint64_t frames) {
  const bool floating = format == WavFormat::Float32;
  const uint16_t bits = floating ? 32 : 24,
                 align = static_cast<uint16_t>(bits / 8 * 2);
  const uint32_t dataSize = static_cast<uint32_t>(frames * align),
                 headerSize = floating ? 58 : 44;
  std::vector<unsigned char> h(headerSize);
  std::copy_n("RIFF", 4, h.data());
  put32(h.data() + 4, headerSize + dataSize - 8);
  std::copy_n("WAVE", 4, h.data() + 8);
  std::copy_n("fmt ", 4, h.data() + 12);
  put32(h.data() + 16, floating ? 18 : 16);
  put16(h.data() + 20, floating ? 3 : 1);
  put16(h.data() + 22, 2);
  put32(h.data() + 24, 48000);
  put32(h.data() + 28, 48000 * align);
  put16(h.data() + 32, align);
  put16(h.data() + 34, bits);
  if (floating) {
    put16(h.data() + 36, 0);
    std::copy_n("fact", 4, h.data() + 38);
    put32(h.data() + 42, 4);
    put32(h.data() + 46, static_cast<uint32_t>(frames));
    std::copy_n("data", 4, h.data() + 50);
    put32(h.data() + 54, dataSize);
  } else {
    std::copy_n("data", 4, h.data() + 36);
    put32(h.data() + 40, dataSize);
  }
  return h;
}
uint32_t random32(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}
float uniform(uint32_t &state) {
  return float(random32(state) >> 8) * (1.0f / 16777216.0f);
}
int32_t pcm24(float sample, uint32_t &random) {
  constexpr float scale = 8388608.0f;
  sample += (uniform(random) - uniform(random)) / scale;
  auto value = static_cast<int64_t>(std::llround(double(sample) * scale));
  return static_cast<int32_t>(std::clamp<int64_t>(value, -8388608, 8388607));
}
} // namespace

ExportTailSummary resolveExportTail(const GraphTailSummary &tail,
                                    ExportOptions options) {
  return resolveExportTailImpl(tail, options);
}

ExportTailSummary inspectExportTail(const State &snapshot, ExportOptions options,
                                    uint64_t startFrame) {
  Renderer renderer;
  renderer.prepare(snapshot, startFrame);
  return resolveExportTail(renderer.tailSummary(), options);
}

void writeWavRange(const State &snapshot, const std::string &path,
                   WavFormat format, uint64_t startFrame, uint64_t endFrame,
                   ExportResult *progress) {
  writeWavRange(snapshot, path, format, startFrame, endFrame, {}, progress);
}

void writeWavRange(const State &snapshot, const std::string &path,
                   WavFormat format, uint64_t startFrame, uint64_t endFrame,
                   ExportOptions options, ExportResult *progress) {
  if (path.empty())
    throw Error("Choose an export path");
  if (format != WavFormat::PCM24 && format != WavFormat::Float32)
    throw Error("Unsupported WAV export format");
  validateExportOptions(options);
  Renderer renderer;
  renderer.prepare(snapshot, startFrame);
  auto duration = renderer.duration();
  if (startFrame >= endFrame || endFrame > duration)
    throw Error("Invalid WAV export range");
  const auto requested = endFrame - startFrame;
  auto tail = resolveExportTail(renderer.tailSummary(), options);
  auto total = requested + tail.selectedTailFrames;
  if (requested > 48000 * 600)
    throw Error("Export exceeds 10 minutes");
  // A delayed master chain must see the project from frame zero. Discard its
  // initial delay, then retain the requested range followed by the bounded
  // declared effect/PDC tail. Header and progress use the extended duration.
  auto latency = renderer.masterLatencyFrames();
  const bool compensateLatency = latency != 0;
  if (compensateLatency) {
    renderer.prepare(snapshot, 0);
    latency = renderer.masterLatencyFrames();
    tail = resolveExportTail(renderer.tailSummary(), options);
    total = requested + tail.selectedTailFrames;
  }
  auto parent = std::filesystem::path(path).parent_path();
  if (parent.empty())
    parent = ".";
  struct DirectoryCleanup {
    std::filesystem::path path;
    ~DirectoryCleanup() {
      if (!path.empty()) {
        std::error_code error;
        std::filesystem::remove(path, error);
      }
    }
  } directoryCleanup;
#ifdef __APPLE__
  parent = replacementDirectory(path);
  directoryCleanup.path = parent;
#endif
  std::string pattern = (parent / ".mydaw-export-XXXXXX").string();
  std::vector<char> name(pattern.begin(), pattern.end());
  name.push_back(0);
  int fd = mkstemp(name.data());
  if (fd < 0)
    throw Error("Cannot create temporary WAV export");
  struct Descriptor {
    int value;
    ~Descriptor() {
      if (value >= 0)
        close(value);
    }
  } descriptor{fd};
  std::string temporary(name.data());
  struct Cleanup {
    std::string path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } cleanup{temporary};
  auto header = wavHeader(format, total);
  if (!writeAll(fd, header.data(), header.size()))
    throw Error("Cannot write WAV header");
  constexpr uint32_t block = 4096;
  std::vector<float> left(block), right(block);
  std::vector<unsigned char> encoded(static_cast<size_t>(block) *
                                     (format == WavFormat::PCM24 ? 6 : 8));
  uint32_t random = 0x4d594441;
  uint64_t done = 0, streamed = 0;
  const uint64_t discard = compensateLatency ? startFrame + latency : 0,
                 inputFrames = compensateLatency ? endFrame : requested;
  auto writeRendered = [&](uint32_t offset, uint32_t count) {
    if (format == WavFormat::Float32) {
      for (uint32_t i = 0; i < count; ++i) {
        auto l = std::bit_cast<uint32_t>(left[offset + i]),
             r = std::bit_cast<uint32_t>(right[offset + i]);
        put32(encoded.data() + i * 8, l);
        put32(encoded.data() + i * 8 + 4, r);
      }
    } else {
      for (uint32_t i = 0; i < count; ++i) {
        int32_t values[2] = {pcm24(left[offset + i], random),
                             pcm24(right[offset + i], random)};
        for (unsigned channel = 0; channel < 2; ++channel) {
          auto value = static_cast<uint32_t>(values[channel]);
          auto p = encoded.data() + i * 6 + channel * 3;
          p[0] = static_cast<unsigned char>(value);
          p[1] = static_cast<unsigned char>(value >> 8);
          p[2] = static_cast<unsigned char>(value >> 16);
        }
      }
    }
    auto bytes =
        static_cast<size_t>(count) * (format == WavFormat::PCM24 ? 6 : 8);
    if (!writeAll(fd, encoded.data(), bytes))
      throw Error("Cannot write WAV audio");
    done += count;
    if (progress)
      progress->renderedFrames.store(done, std::memory_order_release);
  };
  auto consume = [&](uint32_t count) {
    const auto chunkBegin = streamed, chunkEnd = streamed + count,
               requiredEnd = discard + total;
    const auto begin = std::max(chunkBegin, discard),
               end = std::min(chunkEnd, requiredEnd);
    if (begin < end)
      writeRendered(static_cast<uint32_t>(begin - chunkBegin),
                    static_cast<uint32_t>(end - begin));
    streamed = chunkEnd;
  };
  renderer.playing.store(true);
  uint64_t inputDone = 0;
  while (inputDone < inputFrames) {
    if (progress && progress->cancel.load(std::memory_order_acquire))
      throw ExportCanceled{};
    auto count = static_cast<uint32_t>(
        std::min<uint64_t>(block, inputFrames - inputDone));
    renderer.renderExport(left.data(), right.data(), count); // bounced files never carry the monitoring click
    consume(count);
    inputDone += count;
  }
  const uint64_t tailFrames = latency + tail.selectedTailFrames;
  uint64_t tailDone = 0;
  while (tailDone < tailFrames) {
    if (progress && progress->cancel.load(std::memory_order_acquire))
      throw ExportCanceled{};
    auto count =
        static_cast<uint32_t>(std::min<uint64_t>(block, tailFrames - tailDone));
    renderer.renderTail(left.data(), right.data(), count);
    consume(count);
    tailDone += count;
  }
  if (done != total)
    throw Error("Tail-aware latency compensation did not produce requested range");
  if (progress && progress->cancel.load(std::memory_order_acquire))
    throw ExportCanceled{};
  if (fsync(fd) != 0)
    throw Error("Cannot sync WAV export");
  if (close(fd) != 0)
    throw Error("Cannot close WAV export");
  descriptor.value = -1;
  std::filesystem::rename(temporary, path);
  cleanup.path.clear();
}

void writeWav(const State &snapshot, const std::string &path, WavFormat format,
              ExportResult *progress) {
  writeWav(snapshot, path, format, {}, progress);
}

void writeWav(const State &snapshot, const std::string &path, WavFormat format,
              ExportOptions options, ExportResult *progress) {
  uint64_t duration = 0;
  {
    Renderer probe;
    probe.prepare(snapshot);
    duration = probe.duration();
  }
  writeWavRange(snapshot, path, format, 0, duration, options, progress);
}

std::shared_ptr<ExportResult> startExportRange(State snapshot, std::string path,
                                               WavFormat format,
                                               uint64_t startFrame,
                                               uint64_t endFrame) {
  return startExportRange(std::move(snapshot), std::move(path), format,
                          startFrame, endFrame, {});
}

std::shared_ptr<ExportResult> startExportRange(State snapshot, std::string path,
                                               WavFormat format,
                                               uint64_t startFrame,
                                               uint64_t endFrame,
                                               ExportOptions options) {
  uint64_t totalFrames = 0;
  {
    // Complete probe teardown BEFORE the worker can instantiate its graph.
    // Otherwise AU initialization on the worker races uninitialization of
    // these temporary instances on the caller (observed with Apple DLS).
    Renderer probe;
    probe.prepare(snapshot, startFrame);
    if (startFrame >= endFrame || endFrame > probe.duration())
      throw Error("Invalid WAV export range");
    const auto tail = resolveExportTail(probe.tailSummary(), options);
    totalFrames = endFrame - startFrame + tail.selectedTailFrames;
  }
  auto permit = tryAcquireBackgroundJob();
  if (!permit)
    throw Error("Background job capacity reached");
  auto result = std::make_shared<ExportResult>(snapshot.revision, totalFrames);
  std::thread([snapshot = std::move(snapshot), path = std::move(path), format,
               startFrame, endFrame, options, result, permit = std::move(permit)] {
    (void)permit;
    try {
      writeWavRange(snapshot, path, format, startFrame, endFrame, options,
                    result.get());
      result->status.store(1, std::memory_order_release);
    } catch (const ExportCanceled &) {
      result->status.store(3, std::memory_order_release);
    } catch (const std::exception &error) {
      std::snprintf(result->error, sizeof(result->error), "%s", error.what());
      result->status.store(2, std::memory_order_release);
    } catch (...) {
      std::snprintf(result->error, sizeof(result->error),
                    "Unknown export error");
      result->status.store(2, std::memory_order_release);
    }
  }).detach();
  return result;
}

std::shared_ptr<ExportResult> startExport(State snapshot, std::string path,
                                          WavFormat format) {
  return startExport(std::move(snapshot), std::move(path), format, {});
}

std::shared_ptr<ExportResult> startExport(State snapshot, std::string path,
                                          WavFormat format,
                                          ExportOptions options) {
  uint64_t duration = 0;
  {
    // The full-project wrapper must not keep another probe alive across the
    // asynchronous start either. Only immutable scalar metadata crosses it.
    Renderer probe;
    probe.prepare(snapshot);
    duration = probe.duration();
  }
  return startExportRange(std::move(snapshot), std::move(path), format, 0,
                          duration, options);
}

namespace {
bool stemSafeName(std::size_t index, const std::string &name,
                  std::string &out) {
  std::string clean;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
                    c == '_' || c == '.';
    clean.push_back(ok ? c : '_');
  }
  while (!clean.empty() && clean.back() == ' ') clean.pop_back();
  while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
  if (clean.empty()) clean = "Track";
  char prefix[8];
  std::snprintf(prefix, sizeof(prefix), "%02zu", index + 1);
  out = std::string(prefix) + " - " + clean + ".wav";
  return true;
}
State stemSnapshot(const State &source, std::size_t track) {
  State copy = source;
  for (std::size_t i = 0; i < copy.tracks.size(); ++i)
    copy.tracks[i].solo = (i == track);
  copy.masterGain = 0;                       // unity: summing stems restores gain once
  copy.masterGainAutomation.clear();         // no lane = plain smoothed fader path
  copy.masterInserts.clear();                // stems are pre-master by convention
  return copy;
}
// Probe one stem: false when the track is user-muted, holds no material,
// or every one of its region clips is muted — silent tracks produce no file.
bool stemDuration(const State &snapshot, std::size_t track,
                  uint64_t &out) {
  const auto &source = snapshot.tracks[track];
  if (source.muted || (!source.audio && source.midiClips.empty() &&
                       source.inserts.empty()))
    return false;
  if (source.audio) {
    bool anyAudible = false;
    for (const auto &region : source.regions)
      if (!region.muted)
        anyAudible = true;
    if (!anyAudible && source.midiClips.empty() && source.inserts.empty())
      return false;
  }
  try {
    Renderer probe;
    probe.prepare(stemSnapshot(snapshot, track));
    out = probe.duration();
  } catch (const Error &) {
    return false;
  }
  return out > 0;
}
} // namespace

void writeStems(const State &snapshot, const std::string &directory,
                WavFormat format, ExportOptions options,
                ExportResult *progress, const std::vector<uint64_t> *onlyTrackIds) {
  if (directory.empty())
    throw Error("Choose a stems directory");
  std::error_code fs;
  if (!std::filesystem::is_directory(directory, fs))
    throw Error("Stems directory not found");
  uint64_t written = 0;
  std::size_t count = 0;
  for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
    if (onlyTrackIds && std::find(onlyTrackIds->begin(), onlyTrackIds->end(),
                                    snapshot.tracks[i].id) == onlyTrackIds->end())
      continue;
    uint64_t duration = 0;
    if (!stemDuration(snapshot, i, duration))
      continue;
    std::string file;
    stemSafeName(i, snapshot.tracks[i].name, file);
    writeWavRange(stemSnapshot(snapshot, i),
                  (std::filesystem::path(directory) / file).string(), format, 0,
                  duration, options, nullptr);
    written += duration;
    ++count;
    if (progress) {
      progress->renderedFrames.store(written, std::memory_order_release);
      if (progress->cancel.load(std::memory_order_acquire))
        throw ExportCanceled{};
    }
  }
  if (count == 0)
    throw Error("No audible track to export");
}

std::shared_ptr<ExportResult> startStemExport(State snapshot,
                                               std::string directory,
                                               WavFormat format,
                                               ExportOptions options,
                                               std::vector<uint64_t> onlyTrackIds) {
  if (directory.empty())
    throw Error("Choose a stems directory");
  uint64_t total = 0;
  const auto *filter = onlyTrackIds.empty() ? nullptr : &onlyTrackIds;
  for (std::size_t i = 0; i < snapshot.tracks.size(); ++i) {
    if (filter && std::find(filter->begin(), filter->end(),
                            snapshot.tracks[i].id) == filter->end())
      continue;
    uint64_t duration = 0;
    if (stemDuration(snapshot, i, duration))
      total += duration;
  }
  if (total == 0)
    throw Error("No audible track to export");
  auto permit = tryAcquireBackgroundJob();
  if (!permit)
    throw Error("Background job capacity reached");
  auto result = std::make_shared<ExportResult>(snapshot.revision, total);
  std::thread([snapshot = std::move(snapshot), directory = std::move(directory),
               format, options, result, permit = std::move(permit),
               onlyTracks = std::move(onlyTrackIds)] {
    (void)permit;
    try {
      writeStems(snapshot, directory, format, options, result.get(),
                 onlyTracks.empty() ? nullptr : &onlyTracks);
      result->status.store(1, std::memory_order_release);
    } catch (const ExportCanceled &) {
      result->status.store(3, std::memory_order_release);
    } catch (const std::exception &error) {
      std::snprintf(result->error, sizeof(result->error), "%s", error.what());
      result->status.store(2, std::memory_order_release);
    } catch (...) {
      std::snprintf(result->error, sizeof(result->error),
                    "Unknown export error");
      result->status.store(2, std::memory_order_release);
    }
  }).detach();
  return result;
}
} // namespace daw
