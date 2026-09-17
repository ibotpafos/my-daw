#pragma once

#include <cstdint>
#include <string>

namespace daw {

// A VST3 class identifier is represented in its canonical 32-character,
// upper-case hexadecimal form. Keeping it textual prevents host byte-order
// assumptions from leaking into the project format and persistent cache.
struct Vst3ScannedClass {
    std::string classId;
    std::string modulePath;
    // SHA-256 of the module executable. It is produced by the isolated helper
    // with a fixed size cap and invalidates cached verdicts after replacement.
    std::string moduleFingerprint;
    std::string name;
    std::string vendor;
    std::string version;
    // True when the VST3 sub-category declares "Instrument". Hosting stays
    // audio-only until the note-timeline arc lands on the renderer side; the
    // flag is what lets the browser separate note sinks from effects.
    bool instrument = false;
};

struct Vst3ScanCacheEntry {
    Vst3ScannedClass plugin;
    bool available = false;
    std::string quarantineReason;
    uint64_t scannedAtUnixSeconds = 0;
};

} // namespace daw
