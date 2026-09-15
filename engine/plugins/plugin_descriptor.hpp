#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace daw {

// The descriptor deliberately contains no host-specific handles. It can be
// persisted, indexed, or passed to an isolated plug-in worker unchanged.
enum class PluginFormat : uint8_t {
    Unknown = 0,
    AudioUnit = 1,
    VST3 = 2,
};

struct PluginDescriptor {
    PluginFormat format = PluginFormat::Unknown;
    std::array<uint8_t, 16> vst3ClassFuid{};
    std::string modulePath;
    std::string vendor;
    std::string version;
    std::string fingerprint;
    bool operator==(const PluginDescriptor&) const = default;
};

struct Vst3StateEnvelope {
    PluginDescriptor descriptor;
    std::vector<uint8_t> componentState;
    std::vector<uint8_t> controllerState;
    bool operator==(const Vst3StateEnvelope&) const = default;
};

// Canonical upper-case, 32 hexadecimal digit spelling of a 16-byte VST3 FUID.
std::string textualVst3Fuid(const std::array<uint8_t, 16>& fuid);
std::optional<std::array<uint8_t, 16>> parseTextualVst3Fuid(std::string_view text);

// A self-describing, endian-stable envelope for PluginInsert::state when the
// insert represents a VST3. Audio Units keep using their existing raw state.
// These functions never accept trailing bytes or malformed UTF-8. Encoding
// throws std::invalid_argument when a caller supplies an invalid descriptor.
std::vector<uint8_t> encodeVst3StateEnvelope(const Vst3StateEnvelope& envelope);
std::optional<Vst3StateEnvelope> decodeVst3StateEnvelope(std::span<const uint8_t> bytes,
                                                          std::string* error = nullptr);

// Encode the interoperable VST3 .vstpreset layout from a validated internal
// envelope.  MDVS stays an internal persistence format and is never emitted
// to interchange packages.
std::vector<uint8_t> encodeVst3PresetFile(const Vst3StateEnvelope& envelope);

// A small adapter boundary used by hosts while PluginInsert remains legacy.
// AudioUnit intentionally returns componentState byte-for-byte, preserving
// already-saved AU presets. VST3 always uses the v1 envelope above.
std::vector<uint8_t> encodePluginState(const Vst3StateEnvelope& state);

} // namespace daw
