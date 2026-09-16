#pragma once

#include "plugins/vst3_catalog.hpp"
#include <chrono>
#include <string>
#include <vector>

namespace daw {

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
