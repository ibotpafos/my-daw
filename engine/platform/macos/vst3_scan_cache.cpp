#include "platform/macos/vst3_scan_cache.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace daw {
namespace {
constexpr size_t kMaximumCacheBytes = 1024 * 1024;
constexpr size_t kMaximumEntries = 2048;
constexpr size_t kMaximumFieldBytes = 4096;
constexpr std::string_view kHeader = "MYDAW_VST3_SCAN_CACHE\t1";

bool safeText(std::string_view value, size_t maximum, bool required = false) {
    if (required && value.empty()) return false;
    if (value.size() > maximum) return false;
    for (unsigned char byte : value) if (byte < 0x20 || byte == '\t' || byte == '\r' || byte == '\n') return false;
    return true;
}
int hexValue(char value) { if (value >= '0' && value <= '9') return value - '0'; if (value >= 'A' && value <= 'F') return value - 'A' + 10; if (value >= 'a' && value <= 'f') return value - 'a' + 10; return -1; }
bool validFuid(std::string_view value) { if (value.size() != 32) return false; for (const char byte : value) if (hexValue(byte) < 0) return false; return true; }
bool validFingerprint(std::string_view value) { if (value.size() != 64) return false; for (const char byte : value) if (hexValue(byte) < 0) return false; return true; }
std::string encode(std::string_view value) { static constexpr char hex[] = "0123456789ABCDEF"; std::string result; result.reserve(value.size()); for (const unsigned char byte : value) { if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '/') result.push_back(static_cast<char>(byte)); else { result.push_back('%'); result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); } } return result; }
bool decode(std::string_view value, std::string& result) { result.clear(); result.reserve(value.size()); for (size_t index = 0; index < value.size();) { if (value[index] != '%') { result.push_back(value[index++]); continue; } if (index + 2 >= value.size()) return false; const int left = hexValue(value[index + 1]); const int right = hexValue(value[index + 2]); if (left < 0 || right < 0) return false; result.push_back(static_cast<char>((left << 4) | right)); index += 3; } return safeText(result, kMaximumFieldBytes); }
bool parseUnsigned(std::string_view text, uint64_t& value) { if (text.empty()) return false; const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value); return error == std::errc{} && end == text.data() + text.size(); }
std::vector<std::string_view> fields(std::string_view line) { std::vector<std::string_view> result; size_t begin = 0; while (true) { const auto end = line.find('\t', begin); result.push_back(line.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin)); if (end == std::string_view::npos) return result; begin = end + 1; } }
bool validEntry(const Vst3ScanCacheEntry& entry) { return validFuid(entry.plugin.classId) && validFingerprint(entry.plugin.moduleFingerprint) && safeText(entry.plugin.modulePath, kMaximumFieldBytes, true) && safeText(entry.plugin.name, 480, true) && safeText(entry.plugin.vendor, kMaximumFieldBytes) && safeText(entry.plugin.version, kMaximumFieldBytes) && safeText(entry.quarantineReason, kMaximumFieldBytes) && (!entry.available || entry.quarantineReason.empty()); }
std::string key(const Vst3ScannedClass& plugin) { return plugin.modulePath + ":" + plugin.classId + ":" + plugin.moduleFingerprint + ":" + plugin.vendor + ":" + plugin.version; }
bool fsyncDirectory(const std::string& path) { const auto slash = path.find_last_of('/'); const std::string directory = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash); const int descriptor = open(directory.c_str(), O_RDONLY); if (descriptor < 0) return false; const int status = fsync(descriptor); close(descriptor); return status == 0; }
}

Vst3ScanCache makeVst3ScanCache(const IsolatedVst3Scan& scan, uint64_t nowUnixSeconds) {
    Vst3ScanCache cache; cache.createdAtUnixSeconds = nowUnixSeconds; cache.entries.reserve(scan.available.size() + scan.quarantined.size());
    for (const auto& plugin : scan.available) cache.entries.push_back({plugin, true, {}, nowUnixSeconds});
    for (const auto& record : scan.quarantined) cache.entries.push_back({record.plugin, false, record.reason, nowUnixSeconds});
    return cache;
}

Vst3ScanCacheFreshness freshVst3ScanCache(const Vst3ScanCache& cache, const IsolatedVst3Enumeration& current) {
    Vst3ScanCacheFreshness result; result.fresh.createdAtUnixSeconds = cache.createdAtUnixSeconds;
    if (!current.helperError.empty()) { result.invalidatedEntries = static_cast<uint32_t>(cache.entries.size()); return result; }
    std::set<std::string> present; for (const auto& plugin : current.classes) present.insert(key(plugin));
    for (const auto& entry : cache.entries) { if (present.contains(key(entry.plugin))) result.fresh.entries.push_back(entry); else ++result.invalidatedEntries; }
    return result;
}

std::optional<Vst3ScanCache> readVst3ScanCache(const std::string& path, std::string& error) {
    error.clear(); struct stat metadata{}; if (stat(path.c_str(), &metadata) != 0) { if (errno == ENOENT) return std::nullopt; error = "Read VST3 cache metadata failed: " + std::string(std::strerror(errno)); return std::nullopt; }
    if (metadata.st_size < 0 || static_cast<uintmax_t>(metadata.st_size) > kMaximumCacheBytes) { error = "VST3 cache exceeds 1 MiB"; return std::nullopt; }
    std::ifstream input(path, std::ios::binary); if (!input) { error = "Open VST3 cache failed"; return std::nullopt; }
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()); if ((!input.good() && !input.eof()) || content.size() > kMaximumCacheBytes) { error = "Read VST3 cache failed"; return std::nullopt; }
    std::istringstream lines(content); std::string line; if (!std::getline(lines, line) || line != kHeader) { error = "VST3 cache header is invalid"; return std::nullopt; }
    Vst3ScanCache cache; if (!std::getline(lines, line)) { error = "VST3 cache timestamp is missing"; return std::nullopt; } const auto timestamp = fields(line); if (timestamp.size() != 2 || timestamp[0] != "created" || !parseUnsigned(timestamp[1], cache.createdAtUnixSeconds)) { error = "VST3 cache timestamp is invalid"; return std::nullopt; }
    std::set<std::string> keys;
    while (std::getline(lines, line)) { const auto values = fields(line); if (line.empty() || values.size() != 10 || values[0] != "entry" || (values[1] != "available" && values[1] != "quarantined")) { error = "VST3 cache record is invalid"; return std::nullopt; } Vst3ScanCacheEntry entry; entry.available = values[1] == "available"; entry.plugin.classId.assign(values[2]); entry.plugin.moduleFingerprint.assign(values[3]); if (!parseUnsigned(values[4], entry.scannedAtUnixSeconds) || !decode(values[5], entry.plugin.modulePath) || !decode(values[6], entry.plugin.name) || !decode(values[7], entry.plugin.vendor) || !decode(values[8], entry.plugin.version) || !decode(values[9], entry.quarantineReason) || !validEntry(entry) || cache.entries.size() >= kMaximumEntries || !keys.insert(key(entry.plugin)).second) { error = "VST3 cache record fields are invalid"; return std::nullopt; } cache.entries.push_back(std::move(entry)); }
    return cache;
}

bool writeVst3ScanCache(const Vst3ScanCache& cache, const std::string& path, std::string& error) {
    error.clear(); if (path.empty()) { error = "VST3 cache path is empty"; return false; } if (cache.entries.size() > kMaximumEntries) { error = "VST3 cache has too many records"; return false; }
    std::set<std::string> keys; std::ostringstream output; output << kHeader << '\n' << "created\t" << cache.createdAtUnixSeconds << '\n';
    for (const auto& entry : cache.entries) { if (!validEntry(entry) || !keys.insert(key(entry.plugin)).second) { error = "VST3 cache contains invalid or duplicate records"; return false; } output << "entry\t" << (entry.available ? "available" : "quarantined") << '\t' << entry.plugin.classId << '\t' << entry.plugin.moduleFingerprint << '\t' << entry.scannedAtUnixSeconds << '\t' << encode(entry.plugin.modulePath) << '\t' << encode(entry.plugin.name) << '\t' << encode(entry.plugin.vendor) << '\t' << encode(entry.plugin.version) << '\t' << encode(entry.quarantineReason) << '\n'; }
    const auto content = output.str(); if (content.size() > kMaximumCacheBytes) { error = "VST3 cache serialization exceeds 1 MiB"; return false; }
    std::string temporary = path + ".tmp.XXXXXX";
    std::vector<char> temporaryBuffer(temporary.begin(), temporary.end());
    temporaryBuffer.push_back('\0');
    const int descriptor = mkstemp(temporaryBuffer.data());
    if (descriptor < 0) { error = "Create VST3 cache temporary file failed: " + std::string(std::strerror(errno)); return false; }
    temporary.assign(temporaryBuffer.data());
    (void)fchmod(descriptor, 0600);
    size_t written = 0;
    while (written < content.size()) {
        const auto count = write(descriptor, content.data() + written, content.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { const auto reason = std::strerror(errno); close(descriptor); unlink(temporary.c_str()); error = "Write VST3 cache failed: " + std::string(reason); return false; }
        written += static_cast<size_t>(count);
    }
    if (fsync(descriptor) != 0) { const auto reason = std::strerror(errno); close(descriptor); unlink(temporary.c_str()); error = "Sync VST3 cache failed: " + std::string(reason); return false; }
    if (close(descriptor) != 0) { unlink(temporary.c_str()); error = "Close VST3 cache failed"; return false; }
    if (rename(temporary.c_str(), path.c_str()) != 0) { const auto reason = std::strerror(errno); unlink(temporary.c_str()); error = "Replace VST3 cache failed: " + std::string(reason); return false; }
    // Publication already succeeded. A directory fsync improves crash durability
    // but may be unavailable on some volume types, so it cannot report failure.
    (void)fsyncDirectory(path);
    return true;
}

} // namespace daw
