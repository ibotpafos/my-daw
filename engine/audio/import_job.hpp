#pragma once

#include "audio/clip.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace daw {
enum class ImportJobStatus : uint8_t { Running, Ready, Failed, Canceled, Applied };
// Values intentionally match ImportControl's phase callback and the public C
// ABI. Keeping them explicit prevents a decoding/converting status from being
// presented as the following phase in the macOS UI.
enum class ImportJobPhase : uint8_t { Reading = 1, Decoding = 2, Converting = 3, Ready = 4 };

// This is deliberately independent of Session.  The bridge is the sole owner
// of applying a ready immutable Clip to domain state, after its revision check.
struct ImportJobResult {
    std::atomic<ImportJobStatus> status{ImportJobStatus::Running};
    std::atomic<ImportJobPhase> phase{ImportJobPhase::Reading};
    std::atomic<bool> cancel{false};
    std::atomic<uint32_t> progress{0};
    std::atomic<uint32_t> sourceSampleRate{0};
    std::atomic<uint32_t> sourceChannels{0};
    std::atomic<uint64_t> sourceFrames{0};
    std::atomic<uint64_t> outputFrames{0};
    mutable std::mutex publication;
    std::shared_ptr<const Clip> clip;
    std::string error;
};

std::shared_ptr<ImportJobResult> startWavImport(std::string path);
std::shared_ptr<ImportJobResult> startAiffImport(std::string path);
// Locks publication and cancels a job that has not been applied.  A bridge
// applying a Ready Clip must hold publication across its revision check,
// domain mutation, and Applied transition so cancel cannot race that commit.
void cancelImport(ImportJobResult& result) noexcept;
std::shared_ptr<const Clip> importClip(const ImportJobResult& result);
std::string importError(const ImportJobResult& result);
// Convenience for non-domain users.  A bridge that commits to Session holds
// publication itself and writes Applied under that same lock.
bool markImportApplied(ImportJobResult& result) noexcept;
}
