#pragma once
#include "audio/effect.hpp"
#include <chrono>
#include <string>
#include <vector>

namespace daw {
// A component that could not finish its isolated probe is deliberately kept out
// of the usable catalogue. Callers may show these records in a quarantine UI.
struct AudioUnitQuarantineRecord {
    AudioUnitDescriptor descriptor;
    std::string bundlePath;
    std::string bundleVersion;
    std::string reason;
};

// Metadata is collected from a component bundle in the disposable helper. It
// describes a bundle on disk, not an instantiated Audio Unit.
struct AudioUnitScannedComponent {
    AudioUnitDescriptor descriptor;
    std::string bundlePath;
    std::string bundleVersion;
};

struct IsolatedAudioUnitEnumeration {
    std::vector<AudioUnitScannedComponent> components;
    std::string helperError;
};

struct IsolatedAudioUnitScan {
    std::vector<AudioUnitDescriptor> available;
    std::vector<AudioUnitScannedComponent> availableMetadata;
    std::vector<AudioUnitQuarantineRecord> quarantined;
    std::string helperError;
};

// Runs the helper once to enumerate components, then once per component to
// instantiate/configure it. The host process never loads a scanned component.
// helperPath must point to the daw_au_scan_helper executable installed with the
// application. A timeout or malformed result quarantines only that component.
IsolatedAudioUnitScan scanAudioUnitsIsolated(
    const std::string& helperPath,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

// Performs enumeration only. It never calls AudioComponentInstanceNew in the
// host process and is used to invalidate stale persistent cache entries.
IsolatedAudioUnitEnumeration enumerateAudioUnitsIsolated(
    const std::string& helperPath,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
}
