#include "platform/macos/vst3_scanner.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
#include <vector>
#include <string_view>

extern char** environ;

namespace daw {
namespace {
constexpr size_t kMaximumHelperOutput = 1024 * 1024;
constexpr size_t kMaximumFieldBytes = 4096;

struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string output;
    std::string error;
};

ProcessResult runHelper(const std::string& path, const std::vector<std::string>& arguments,
                        std::chrono::milliseconds timeout) {
    ProcessResult result;
    int outPipe[2]{-1, -1};
    if (pipe(outPipe) != 0) {
        result.error = "Create VST3 scanner pipe failed: " + std::string(std::strerror(errno));
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, outPipe[0]);
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);
    std::vector<std::string> owned;
    owned.reserve(arguments.size() + 1);
    owned.push_back(path);
    owned.insert(owned.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& value : owned) argv.push_back(value.data());
    argv.push_back(nullptr);
    pid_t child = -1;
    const int spawnResult = posix_spawn(&child, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(outPipe[1]);
    if (spawnResult != 0) {
        close(outPipe[0]);
        result.error = "Start VST3 scanner helper failed: " + std::string(std::strerror(spawnResult));
        return result;
    }
    result.started = true;
    const int oldFlags = fcntl(outPipe[0], F_GETFL, 0);
    if (oldFlags >= 0) fcntl(outPipe[0], F_SETFL, oldFlags | O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool pipeOpen = true;
    bool exited = false;
    int status = 0;
    while (pipeOpen || !exited) {
        if (!exited && waitpid(child, &status, WNOHANG) == child) exited = true;
        if (!exited && std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            result.timedOut = true;
            waitpid(child, &status, 0);
            exited = true;
        }
        if (!pipeOpen) continue;
        pollfd descriptor{outPipe[0], POLLIN | POLLHUP, 0};
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int waitMilliseconds = exited ? 0 : static_cast<int>(std::clamp<long long>(remaining, 0, 25));
        if (poll(&descriptor, 1, waitMilliseconds) < 0 || !(descriptor.revents & (POLLIN | POLLHUP))) continue;
        std::array<char, 4096> buffer{};
        const auto count = read(outPipe[0], buffer.data(), buffer.size());
        if (count > 0) {
            if (result.output.size() + static_cast<size_t>(count) > kMaximumHelperOutput) {
                kill(child, SIGKILL);
                result.error = "VST3 scanner helper output exceeded 1 MiB";
                if (!exited) waitpid(child, &status, 0);
                exited = true;
            } else {
                result.output.append(buffer.data(), static_cast<size_t>(count));
            }
        } else if (count == 0) {
            pipeOpen = false;
        } else if (errno != EAGAIN && errno != EINTR) {
            pipeOpen = false;
            result.error = "Read VST3 scanner helper failed: " + std::string(std::strerror(errno));
        }
    }
    close(outPipe[0]);
    if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
    else if (!result.timedOut && result.error.empty()) result.error = "VST3 scanner helper terminated unexpectedly";
    return result;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

bool validText(const std::string& value, size_t maximum, bool required = false) {
    return (!required || !value.empty()) && value.size() <= maximum &&
           value.find_first_of("\r\n\t") == std::string::npos;
}

bool decode(std::string_view encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (size_t index = 0; index < encoded.size();) {
        if (encoded[index] != '%') {
            decoded.push_back(encoded[index++]);
            continue;
        }
        if (index + 2 >= encoded.size()) return false;
        const int left = hexValue(encoded[index + 1]);
        const int right = hexValue(encoded[index + 2]);
        if (left < 0 || right < 0) return false;
        decoded.push_back(static_cast<char>((left << 4) | right));
        index += 3;
    }
    return validText(decoded, kMaximumFieldBytes);
}

bool validFuid(std::string_view value) {
    if (value.size() != 32) return false;
    for (char byte : value) if (hexValue(byte) < 0) return false;
    return true;
}
bool validFingerprint(std::string_view value) {
    if (value.size() != 64) return false;
    for (char byte : value) if (hexValue(byte) < 0) return false;
    return true;
}

bool parseClass(std::string_view line, Vst3ScannedClass& value) {
    std::array<std::string_view, 6> fields{};
    size_t begin = 0;
    for (size_t index = 0; index < fields.size() - 1; ++index) {
        const auto end = line.find('\t', begin);
        if (end == std::string_view::npos) return false;
        fields[index] = line.substr(begin, end - begin);
        begin = end + 1;
    }
    fields.back() = line.substr(begin);
    if (!validFuid(fields[0]) || !decode(fields[1], value.modulePath) ||
        !decode(fields[2], value.moduleFingerprint) || !decode(fields[3], value.name) ||
        !decode(fields[4], value.vendor) || !decode(fields[5], value.version) ||
        !validFingerprint(value.moduleFingerprint) || !validText(value.modulePath, kMaximumFieldBytes, true) ||
        !validText(value.name, 480, true)) return false;
    value.classId.assign(fields[0]);
    std::transform(value.classId.begin(), value.classId.end(), value.classId.begin(),
                   [](unsigned char byte) { return static_cast<char>(std::toupper(byte)); });
    return true;
}

std::vector<Vst3ScannedClass> parseList(const std::string& output, bool& valid) {
    valid = true;
    std::vector<Vst3ScannedClass> result;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        Vst3ScannedClass item;
        if (!parseClass(line, item)) {
            valid = false;
            return {};
        }
        result.push_back(std::move(item));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return std::tie(left.modulePath, left.classId, left.moduleFingerprint, left.vendor, left.version, left.name) <
               std::tie(right.modulePath, right.classId, right.moduleFingerprint, right.vendor, right.version, right.name);
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.modulePath == right.modulePath && left.classId == right.classId;
    }), result.end());
    return result;
}

std::vector<std::string> parsePaths(const std::string& output, bool& valid) {
    valid = true;
    std::vector<std::string> result;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) continue;
        std::string path;
        if (!decode(line, path) || !validText(path, kMaximumFieldBytes, true)) { valid = false; return {}; }
        result.push_back(std::move(path));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool probeSucceeded(const ProcessResult& result) {
    return result.started && !result.timedOut && result.exitCode == 0 && result.output == "ok\n";
}

std::string probeFailure(const ProcessResult& result) {
    if (!result.error.empty()) return result.error;
    if (result.timedOut) return "Probe exceeded timeout";
    if (result.exitCode >= 0) return "Probe exited with status " + std::to_string(result.exitCode);
    return "Probe returned an invalid result";
}
} // namespace

IsolatedVst3Enumeration enumerateVst3PluginsIsolated(const std::string& helperPath,
                                                       std::chrono::milliseconds timeout) {
    IsolatedVst3Enumeration result;
    if (helperPath.empty()) {
        result.helperError = "VST3 scanner helper path is empty";
        return result;
    }
    if (timeout.count() <= 0) {
        result.helperError = "VST3 scanner timeout must be positive";
        return result;
    }
    const auto listed = runHelper(helperPath, {"--paths"}, timeout);
    if (!listed.started || listed.timedOut || listed.exitCode != 0 || !listed.error.empty()) {
        result.helperError = "VST3 enumeration failed: " + probeFailure(listed);
        return result;
    }
    bool valid = false;
    const auto paths = parsePaths(listed.output, valid);
    if (!valid) {
        result.helperError = "VST3 scanner returned malformed module list";
        return result;
    }
    for (const auto& path : paths) {
        const auto module = runHelper(helperPath, {"--list-module", path}, timeout);
        if (!module.started || module.timedOut || module.exitCode != 0 || !module.error.empty()) {
            result.quarantined.push_back({{ {}, path, {}, {}, {}, {} }, "Module enumeration failed: " + probeFailure(module)});
            continue;
        }
        bool classesValid = false;
        auto classes = parseList(module.output, classesValid);
        if (!classesValid) {
            result.quarantined.push_back({{ {}, path, {}, {}, {}, {} }, "Module returned malformed class list"});
            continue;
        }
        result.classes.insert(result.classes.end(), std::make_move_iterator(classes.begin()), std::make_move_iterator(classes.end()));
    }
    std::sort(result.classes.begin(), result.classes.end(), [](const auto& left, const auto& right) { return std::tie(left.modulePath, left.classId) < std::tie(right.modulePath, right.classId); });
    result.classes.erase(std::unique(result.classes.begin(), result.classes.end(), [](const auto& left, const auto& right) { return left.modulePath == right.modulePath && left.classId == right.classId; }), result.classes.end());
    return result;
}

IsolatedVst3Scan scanVst3PluginsIsolated(const std::string& helperPath, std::chrono::milliseconds timeout) {
    IsolatedVst3Scan result;
    const auto enumeration = enumerateVst3PluginsIsolated(helperPath, timeout);
    if (!enumeration.helperError.empty()) {
        result.helperError = enumeration.helperError;
        return result;
    }
    result.quarantined = enumeration.quarantined;
    for (const auto& candidate : enumeration.classes) {
        const auto probe = runHelper(helperPath, {"--probe", candidate.modulePath, candidate.classId}, timeout);
        if (probeSucceeded(probe)) result.available.push_back(candidate);
        else result.quarantined.push_back({candidate, probeFailure(probe)});
    }
    return result;
}

} // namespace daw
