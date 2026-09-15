#pragma once
#include "platform/macos/au_scanner.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace daw {
struct AudioUnitScanCacheEntry {
    AudioUnitDescriptor descriptor;
    // Empty bundle metadata is valid for helpers that cannot expose it yet.
    std::string bundlePath;
    std::string bundleVersion;
    bool available=false;
    std::string quarantineReason;
    uint64_t scannedAtUnixSeconds=0;
};

struct AudioUnitScanCache {
    uint64_t createdAtUnixSeconds=0;
    std::vector<AudioUnitScanCacheEntry> entries;
};

AudioUnitScanCache makeAudioUnitScanCache(const IsolatedAudioUnitScan& scan,uint64_t nowUnixSeconds);

struct AudioUnitScanCacheFreshness {
    AudioUnitScanCache fresh;
    uint32_t invalidatedEntries=0;
};

// Retains only entries whose component tuple and bundle path/version still
// occur in a fresh helper enumeration. This does not instantiate any plugin.
AudioUnitScanCacheFreshness freshAudioUnitScanCache(
    const AudioUnitScanCache& cache,
    const IsolatedAudioUnitEnumeration& current);

// Returns nullopt for a missing cache. Any present malformed cache is rejected
// and described in error, so callers can rescan instead of trusting stale data.
std::optional<AudioUnitScanCache> readAudioUnitScanCache(const std::string& path,std::string& error);

// Uses a same-directory temporary file, fsync, rename and directory fsync.
// Cache writes are intentionally bounded to prevent a bad scanner result from
// consuming Application Support storage.
bool writeAudioUnitScanCache(const AudioUnitScanCache& cache,const std::string& path,std::string& error);
}
