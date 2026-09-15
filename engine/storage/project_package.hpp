#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {
// `.mydawzip` project archive: a ZIP (stored entries, the format produced by
// writeZipPackage) that bundles the self-contained SQLite draft together with
// a small manifest. Drafts embed all audio, so no media folder is needed.
bool writeProjectPackage(const std::string& draftPath, const std::string& destination,
                         std::string& error, const std::atomic<bool>* cancel = nullptr);
// Extracts the bundled draft to `targetDraftPath` after validating the
// manifest and the embedded SQLite magic. The target is written atomically;
// the archive is never modified.
bool extractProjectPackage(const std::string& zipPath, const std::string& targetDraftPath,
                           std::string& error, const std::atomic<bool>* cancel = nullptr);
// Reads one stored entry out of a writeZipPackage-produced archive.
bool readZipEntry(const std::string& zipPath, const std::string& name,
                  std::vector<uint8_t>& out, std::string& error);
} // namespace daw
