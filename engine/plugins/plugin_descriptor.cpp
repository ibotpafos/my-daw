#include "plugins/plugin_descriptor.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace daw {
namespace {
constexpr std::array<uint8_t, 4> magic{'M', 'D', 'V', 'S'};
constexpr uint16_t envelopeVersion = 1;
constexpr size_t maxPathBytes = 4096;
constexpr size_t maxVendorBytes = 512;
constexpr size_t maxVersionBytes = 256;
constexpr size_t maxFingerprintBytes = 512;
constexpr size_t maxStateBlobBytes = 8 * 1024 * 1024;
constexpr size_t maxEnvelopeBytes = 16 * 1024 * 1024;

bool validUtf8(std::string_view value) {
    for (size_t index = 0; index < value.size();) {
        const auto lead = static_cast<uint8_t>(value[index]);
        if (lead <= 0x7f) { ++index; continue; }
        size_t continuation = 0;
        uint32_t codepoint = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { continuation = 1; codepoint = lead & 0x1f; }
        else if (lead >= 0xe0 && lead <= 0xef) { continuation = 2; codepoint = lead & 0x0f; }
        else if (lead >= 0xf0 && lead <= 0xf4) { continuation = 3; codepoint = lead & 0x07; }
        else return false;
        if (index + continuation >= value.size()) return false;
        for (size_t offset = 1; offset <= continuation; ++offset) {
            const auto byte = static_cast<uint8_t>(value[index + offset]);
            if ((byte & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (byte & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) return false;
        ++index;
        index += continuation;
    }
    return true;
}

bool validString(std::string_view value, size_t maximum, bool path) {
    return value.size() <= maximum && value.find('\0') == std::string_view::npos && validUtf8(value) &&
           (!path || !value.empty());
}

bool nonzero(const std::array<uint8_t, 16>& bytes) {
    return std::any_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value != 0; });
}

void append16(std::vector<uint8_t>& output, uint16_t value) {
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
}
void append32(std::vector<uint8_t>& output, uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) output.push_back(static_cast<uint8_t>(value >> shift));
}
void append64(std::vector<uint8_t>& output, uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8) output.push_back(static_cast<uint8_t>(value >> shift));
}
bool read16(std::span<const uint8_t> bytes, size_t& cursor, uint16_t& output) {
    if (bytes.size() - cursor < 2) return false;
    output = static_cast<uint16_t>(bytes[cursor]) | (static_cast<uint16_t>(bytes[cursor + 1]) << 8);
    cursor += 2;
    return true;
}
bool read32(std::span<const uint8_t> bytes, size_t& cursor, uint32_t& output) {
    if (bytes.size() - cursor < 4) return false;
    output = 0;
    for (unsigned shift = 0; shift != 32; shift += 8) output |= static_cast<uint32_t>(bytes[cursor++]) << shift;
    return true;
}
bool readSlice(std::span<const uint8_t> bytes, size_t& cursor, size_t length, std::span<const uint8_t>& output) {
    if (length > bytes.size() - cursor) return false;
    output = bytes.subspan(cursor, length);
    cursor += length;
    return true;
}
void fail(std::string* error, std::string_view message) { if (error) *error = message; }
bool validDescriptor(const PluginDescriptor& descriptor) {
    return descriptor.format == PluginFormat::VST3 && nonzero(descriptor.vst3ClassFuid) &&
           validString(descriptor.modulePath, maxPathBytes, true) &&
           validString(descriptor.vendor, maxVendorBytes, false) &&
           validString(descriptor.version, maxVersionBytes, false) &&
           validString(descriptor.fingerprint, maxFingerprintBytes, false);
}
} // namespace

std::string textualVst3Fuid(const std::array<uint8_t, 16>& fuid) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string output(32, '0');
    for (size_t index = 0; index < fuid.size(); ++index) {
        output[index * 2] = digits[fuid[index] >> 4];
        output[index * 2 + 1] = digits[fuid[index] & 0x0f];
    }
    return output;
}

std::optional<std::array<uint8_t, 16>> parseTextualVst3Fuid(std::string_view text) {
    if (text.size() != 32) return std::nullopt;
    std::array<uint8_t, 16> result{};
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        return -1;
    };
    for (size_t index = 0; index < result.size(); ++index) {
        const int high = nibble(text[index * 2]);
        const int low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return result;
}

std::vector<uint8_t> encodeVst3StateEnvelope(const Vst3StateEnvelope& envelope) {
    if (!validDescriptor(envelope.descriptor) || envelope.componentState.size() > maxStateBlobBytes ||
        envelope.controllerState.size() > maxStateBlobBytes) {
        throw std::invalid_argument("Invalid VST3 state envelope");
    }
    const size_t total = 4 + 2 + 16 + 4 * 2 + 2 * 4 + envelope.descriptor.modulePath.size() + envelope.descriptor.vendor.size() +
                         envelope.descriptor.version.size() + envelope.descriptor.fingerprint.size() +
                         envelope.componentState.size() + envelope.controllerState.size();
    if (total > maxEnvelopeBytes) throw std::invalid_argument("VST3 state envelope is too large");
    std::vector<uint8_t> output;
    output.reserve(total);
    output.insert(output.end(), magic.begin(), magic.end());
    append16(output, envelopeVersion);
    output.insert(output.end(), envelope.descriptor.vst3ClassFuid.begin(), envelope.descriptor.vst3ClassFuid.end());
    append16(output, static_cast<uint16_t>(envelope.descriptor.modulePath.size()));
    append16(output, static_cast<uint16_t>(envelope.descriptor.vendor.size()));
    append16(output, static_cast<uint16_t>(envelope.descriptor.version.size()));
    append16(output, static_cast<uint16_t>(envelope.descriptor.fingerprint.size()));
    append32(output, static_cast<uint32_t>(envelope.componentState.size()));
    append32(output, static_cast<uint32_t>(envelope.controllerState.size()));
    const auto append = [&output](const auto& bytes) { output.insert(output.end(), bytes.begin(), bytes.end()); };
    append(envelope.descriptor.modulePath); append(envelope.descriptor.vendor); append(envelope.descriptor.version);
    append(envelope.descriptor.fingerprint); append(envelope.componentState); append(envelope.controllerState);
    return output;
}

std::optional<Vst3StateEnvelope> decodeVst3StateEnvelope(std::span<const uint8_t> bytes, std::string* error) {
    if (bytes.size() > maxEnvelopeBytes || bytes.size() < 38 ||
        !std::equal(magic.begin(), magic.end(), bytes.begin())) { fail(error, "Not a VST3 state envelope"); return std::nullopt; }
    size_t cursor = 4;
    uint16_t version = 0, pathLength = 0, vendorLength = 0, versionLength = 0, fingerprintLength = 0;
    uint32_t componentLength = 0, controllerLength = 0;
    if (!read16(bytes, cursor, version) || version != envelopeVersion) { fail(error, "Unsupported VST3 state envelope version"); return std::nullopt; }
    Vst3StateEnvelope result;
    result.descriptor.format = PluginFormat::VST3;
    std::span<const uint8_t> fuid;
    if (!readSlice(bytes, cursor, result.descriptor.vst3ClassFuid.size(), fuid)) { fail(error, "Truncated VST3 class FUID"); return std::nullopt; }
    std::copy(fuid.begin(), fuid.end(), result.descriptor.vst3ClassFuid.begin());
    if (!read16(bytes, cursor, pathLength) || !read16(bytes, cursor, vendorLength) || !read16(bytes, cursor, versionLength) ||
        !read16(bytes, cursor, fingerprintLength) || !read32(bytes, cursor, componentLength) || !read32(bytes, cursor, controllerLength) ||
        pathLength > maxPathBytes || vendorLength > maxVendorBytes || versionLength > maxVersionBytes ||
        fingerprintLength > maxFingerprintBytes || componentLength > maxStateBlobBytes || controllerLength > maxStateBlobBytes) {
        fail(error, "Invalid VST3 state envelope lengths"); return std::nullopt;
    }
    const auto readString = [&bytes, &cursor](size_t length, std::string& destination) {
        std::span<const uint8_t> source;
        if (!readSlice(bytes, cursor, length, source)) return false;
        destination.assign(reinterpret_cast<const char*>(source.data()), source.size());
        return true;
    };
    if (!readString(pathLength, result.descriptor.modulePath) || !readString(vendorLength, result.descriptor.vendor) ||
        !readString(versionLength, result.descriptor.version) || !readString(fingerprintLength, result.descriptor.fingerprint)) {
        fail(error, "Truncated VST3 state descriptor"); return std::nullopt;
    }
    std::span<const uint8_t> component, controller;
    if (!readSlice(bytes, cursor, componentLength, component) || !readSlice(bytes, cursor, controllerLength, controller) || cursor != bytes.size()) {
        fail(error, "Truncated or trailing VST3 state data"); return std::nullopt;
    }
    result.componentState.assign(component.begin(), component.end());
    result.controllerState.assign(controller.begin(), controller.end());
    if (!validDescriptor(result.descriptor)) { fail(error, "Invalid VST3 state descriptor"); return std::nullopt; }
    return result;
}

std::vector<uint8_t> encodeVst3PresetFile(const Vst3StateEnvelope& envelope) {
    // Steinberg's preset layout is a 48-byte header followed by raw chunks,
    // then a `List` table of (id, offset, size) records. All integer fields
    // are little endian and the class FUID is its 32-char ASCII spelling.
    if (!validDescriptor(envelope.descriptor) || envelope.componentState.size() > maxStateBlobBytes ||
        envelope.controllerState.size() > maxStateBlobBytes) {
        throw std::invalid_argument("Invalid VST3 state envelope");
    }
    const bool controller = !envelope.controllerState.empty();
    constexpr size_t headerSize = 48;
    constexpr size_t entrySize = 20;
    const uint64_t componentOffset = headerSize;
    const uint64_t controllerOffset = componentOffset + envelope.componentState.size();
    const uint64_t listOffset = controllerOffset + (controller ? envelope.controllerState.size() : 0);
    const uint64_t total = listOffset + 8 + entrySize * (controller ? 2 : 1);
    if (total > maxEnvelopeBytes + 64 || total > std::numeric_limits<size_t>::max())
        throw std::invalid_argument("VST3 preset is too large");
    std::vector<uint8_t> output;
    output.reserve(static_cast<size_t>(total));
    output.insert(output.end(), {'V','S','T','3'});
    append32(output, 1);
    const auto fuid = textualVst3Fuid(envelope.descriptor.vst3ClassFuid);
    output.insert(output.end(), fuid.begin(), fuid.end());
    append64(output, listOffset);
    output.insert(output.end(), envelope.componentState.begin(), envelope.componentState.end());
    if (controller) output.insert(output.end(), envelope.controllerState.begin(), envelope.controllerState.end());
    output.insert(output.end(), {'L','i','s','t'});
    append32(output, controller ? 2 : 1);
    output.insert(output.end(), {'C','o','m','p'}); append64(output, componentOffset); append64(output, envelope.componentState.size());
    if (controller) { output.insert(output.end(), {'C','o','n','t'}); append64(output, controllerOffset); append64(output, envelope.controllerState.size()); }
    return output;
}

std::vector<uint8_t> encodePluginState(const Vst3StateEnvelope& state) {
    if (state.descriptor.format == PluginFormat::AudioUnit) return state.componentState;
    return encodeVst3StateEnvelope(state);
}

} // namespace daw
