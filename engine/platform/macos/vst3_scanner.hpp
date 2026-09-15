#pragma once

#include <chrono>
#include <string>
#include <vector>

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
};

struct Vst3QuarantineRecord {
    Vst3ScannedClass plugin;
    std::string reason;
};

struct IsolatedVst3Enumeration {
    std::vector<Vst3ScannedClass> classes;
    std::vector<Vst3QuarantineRecord> quarantined;
    std::string helperError;
};

struct IsolatedVst3Scan {
    std::vector<Vst3ScannedClass> available;
    std::vector<Vst3QuarantineRecord> quarantined;
    std::string helperError;
};

// The helper owns all VST3 SDK module loading. The host process receives only
// bounded metadata and creates a fresh process for every module/class probe.
IsolatedVst3Enumeration enumerateVst3PluginsIsolated(
    const std::string& helperPath,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

IsolatedVst3Scan scanVst3PluginsIsolated(
    const std::string& helperPath,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

} // namespace daw
