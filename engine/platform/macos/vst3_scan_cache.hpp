#pragma once

#include "platform/macos/vst3_scanner.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace daw {

struct Vst3ScanCacheEntry {
    Vst3ScannedClass plugin;
    bool available = false;
    std::string quarantineReason;
    uint64_t scannedAtUnixSeconds = 0;
};

struct Vst3ScanCache {
    uint64_t createdAtUnixSeconds = 0;
    std::vector<Vst3ScanCacheEntry> entries;
};

struct Vst3ScanCacheFreshness {
    Vst3ScanCache fresh;
    uint32_t invalidatedEntries = 0;
};

Vst3ScanCache makeVst3ScanCache(const IsolatedVst3Scan& scan, uint64_t nowUnixSeconds);

// Freshness is derived from an isolated metadata-only enumeration. A changed
// module path, FUID, vendor or version invalidates its old verdict.
Vst3ScanCacheFreshness freshVst3ScanCache(
    const Vst3ScanCache& cache,
    const IsolatedVst3Enumeration& current);

std::optional<Vst3ScanCache> readVst3ScanCache(const std::string& path, std::string& error);
bool writeVst3ScanCache(const Vst3ScanCache& cache, const std::string& path, std::string& error);

} // namespace daw
