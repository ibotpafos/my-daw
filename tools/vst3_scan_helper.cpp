#include "public.sdk/source/vst/hosting/module.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {
constexpr size_t kMaximumTextBytes = 4096;
constexpr uintmax_t kMaximumModuleBinaryBytes = 256U * 1024U * 1024U;

std::string encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (unsigned char byte : value) {
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '/') {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 15]);
        }
    }
    return result;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool validFuid(std::string_view value) {
    if (value.size() != 32) return false;
    for (const char byte : value) if (hexValue(byte) < 0) return false;
    return true;
}
bool validFingerprint(std::string_view value) {
    if (value.size() != 64) return false;
    for (const char byte : value) if (hexValue(byte) < 0) return false;
    return true;
}

bool validText(std::string_view value, bool required = false) {
    if ((required && value.empty()) || value.size() > kMaximumTextBytes) return false;
    for (unsigned char byte : value) if (byte < 0x20 || byte == '\t' || byte == '\r' || byte == '\n') return false;
    return true;
}

struct Candidate {
    std::string modulePath;
    std::string moduleFingerprint;
    VST3::Hosting::ClassInfo info;
};

std::string hexDigest(const unsigned char* digest, size_t count) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(count * 2);
    for (size_t index = 0; index < count; ++index) {
        result.push_back(hex[digest[index] >> 4]);
        result.push_back(hex[digest[index] & 15]);
    }
    return result;
}

// A bundle can contain resources that do not affect DSP code. Hash the selected
// executable only, with a fixed cap so a malformed install cannot consume the
// scanner indefinitely before its per-process timeout is enforced.
std::string moduleFingerprint(const std::string& modulePath) {
    namespace fs = std::filesystem;
    std::vector<fs::path> executables;
    const fs::path directory = fs::path(modulePath) / "Contents" / "MacOS";
    std::error_code error;
    for (fs::directory_iterator iterator(directory, fs::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error) executables.push_back(iterator->path());
    }
    if (error || executables.empty()) return {};
    std::sort(executables.begin(), executables.end());
    const auto executable = executables.front();
    const auto size = fs::file_size(executable, error);
    if (error || size == 0 || size > kMaximumModuleBinaryBytes) return {};
    std::ifstream input(executable, std::ios::binary);
    if (!input) return {};
    CC_SHA256_CTX context{};
    CC_SHA256_Init(&context);
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(count));
    }
    if (!input.eof()) return {};
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256_Final(digest.data(), &context);
    return hexDigest(digest.data(), digest.size());
}

// `Module::getModulePaths` is the SDK's macOS discovery path. Since this
// helper is built for arm64, module loading can only succeed for an arm64 or
// universal executable; x86-only bundles never enter the catalogue.
int listModule(const std::string& path) {
    std::vector<Candidate> candidates;
    const auto fingerprint = moduleFingerprint(path);
    if (!validFingerprint(fingerprint)) return 10;
    std::string error;
    const auto module = VST3::Hosting::Module::create(path, error);
    if (!module) return 11;
    for (const auto& info : module->getFactory().classInfos()) {
        if (info.category() != kVstAudioEffectClass) continue;
        const auto id = info.ID().toString();
        if (!validFuid(id) || !validText(path, true) || !validText(info.name(), true) ||
            !validText(info.vendor()) || !validText(info.version())) continue;
        candidates.push_back({path, fingerprint, info});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        const auto leftId = left.info.ID().toString();
        const auto rightId = right.info.ID().toString();
        return std::tie(left.modulePath, leftId, left.moduleFingerprint, left.info.vendor(), left.info.version(), left.info.name()) <
               std::tie(right.modulePath, rightId, right.moduleFingerprint, right.info.vendor(), right.info.version(), right.info.name());
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.modulePath == right.modulePath && left.info.ID() == right.info.ID() && left.moduleFingerprint == right.moduleFingerprint;
    }), candidates.end());
    for (const auto& candidate : candidates) {
        const auto& info = candidate.info;
        std::printf("%s\t%s\t%s\t%s\t%s\t%s\n", info.ID().toString().c_str(), encode(candidate.modulePath).c_str(),
                    encode(candidate.moduleFingerprint).c_str(), encode(info.name()).c_str(), encode(info.vendor()).c_str(), encode(info.version()).c_str());
    }
    return 0;
}

int paths() {
    auto modules = VST3::Hosting::Module::getModulePaths();
    std::sort(modules.begin(), modules.end());
    modules.erase(std::unique(modules.begin(), modules.end()), modules.end());
    for (const auto& path : modules) if (validText(path, true)) std::printf("%s\n", encode(path).c_str());
    return 0;
}

int probe(std::string_view modulePath, std::string_view classId) {
    if (!validText(modulePath, true) || !validFuid(classId)) return 2;
    std::string error;
    const auto module = VST3::Hosting::Module::create(std::string(modulePath), error);
    if (!module) return 10;
    const auto wanted = VST3::UID::fromString(std::string(classId));
    if (!wanted) return 2;
    const VST3::Hosting::ClassInfo* info = nullptr;
    for (const auto& candidate : module->getFactory().classInfos()) {
        if (candidate.category() == kVstAudioEffectClass && candidate.ID() == *wanted) {
            info = &candidate;
            break;
        }
    }
    if (!info) return 11;
    auto component = module->getFactory().createInstance<Steinberg::Vst::IComponent>(info->ID());
    if (!component) return 12;
    if (component->initialize(nullptr) != Steinberg::kResultOk) return 13;
    const auto cleanup = [&] { component->terminate(); };
    const auto inputBuses = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
    const auto outputBuses = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
    if (inputBuses < 0 || outputBuses < 0) { cleanup(); return 14; }
    cleanup();
    std::fputs("ok\n", stdout);
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--paths") == 0) return paths();
    if (argc == 3 && std::strcmp(argv[1], "--list-module") == 0) return listModule(argv[2]);
    if (argc == 4 && std::strcmp(argv[1], "--probe") == 0) return probe(argv[2], argv[3]);
    return 2;
}
