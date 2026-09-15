#include "audio/effect.hpp"
#include "platform/macos/vst3_runtime.hpp"
#include "plugins/plugin_descriptor.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace daw;
using namespace daw::vst3runtime;

std::string hexDigest(const unsigned char *digest, size_t count) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string output; output.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) { output.push_back(digits[digest[i] >> 4]); output.push_back(digits[digest[i] & 15]); }
  return output;
}

std::string executableFingerprint(const std::string &modulePath) {
  namespace fs = std::filesystem;
  std::error_code error;
  const fs::path directory = fs::path(modulePath) / "Contents" / "MacOS";
  std::vector<fs::path> files;
  for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
       !error && it != end; it.increment(error)) if (it->is_regular_file(error) && !error) files.push_back(it->path());
  if (error || files.empty()) return {};
  std::sort(files.begin(), files.end());
  const auto size = fs::file_size(files.front(), error);
  if (error || size == 0 || size > 256U * 1024U * 1024U) return {};
  std::ifstream input(files.front(), std::ios::binary); if (!input) return {};
  CC_SHA256_CTX context{}; CC_SHA256_Init(&context);
  std::array<char, 64 * 1024> buffer{};
  while (input) { input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())); const auto count = input.gcount(); if (count > 0) CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(count)); }
  if (!input.eof()) return {};
  std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{}; CC_SHA256_Final(digest.data(), &context);
  return hexDigest(digest.data(), digest.size());
}

int run(const char *name) {
  const int fd = shm_open(name, O_RDWR, 0);
  if (fd < 0) return 10;
  struct stat info{}; if (fstat(fd, &info) != 0 || info.st_size < static_cast<off_t>(sizeof(SharedMapping))) { close(fd); return 11; }
  const size_t mapBytes = static_cast<size_t>(info.st_size);
  auto *mapping = static_cast<SharedMapping *>(mmap(nullptr, mapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (mapping == MAP_FAILED) { close(fd); return 11; }
  const auto cleanup = [&] { munmap(mapping, mapBytes); close(fd); };
  if (mapping->magic != kProtocolMagic || mapping->version != kProtocolVersion || mapping->stateBytes > kMaximumStateBytes || mapBytes != mappingBytes(mapping->stateBytes)) { cleanup(); return 12; }
  std::string stateError;
  const auto envelope = decodeVst3StateEnvelope(std::span<const uint8_t>(statePayload(mapping), mapping->stateBytes), &stateError);
  if (!envelope || envelope->descriptor.fingerprint.empty() ||
      executableFingerprint(envelope->descriptor.modulePath) != envelope->descriptor.fingerprint) {
    mapping->helperState.store(2, std::memory_order_release); cleanup(); return 18;
  }
  PluginInsert insert{};
  insert.hostingMode = PluginHostingMode::InProcess; // worker never nests itself
  insert.type = kVst3PluginComponentSentinel;
  insert.subtype = kVst3PluginComponentSentinel;
  insert.manufacturer = kVst3PluginComponentSentinel;
  insert.state.assign(statePayload(mapping), statePayload(mapping) + mapping->stateBytes);
  std::unique_ptr<PreparedEffect> effect;
  try { effect = prepareVst3Effect(insert); }
  catch (...) { mapping->helperState.store(2, std::memory_order_release); cleanup(); return 13; }
  mapping->pluginLatencyFrames = effect->latencyFrames();
  mapping->pluginTailFrames = effect->tailFrames();
  mapping->helperState.store(1, std::memory_order_release);
  std::fputs("READY\n", stdout); std::fflush(stdout);
  uint64_t last = 0;
  for (;;) {
    if (mapping->shutdown.load(std::memory_order_acquire) != 0) { cleanup(); return 0; }
    const uint64_t request = mapping->requestSequence.load(std::memory_order_acquire);
    if (request == last) { std::this_thread::sleep_for(std::chrono::microseconds(250)); continue; }
    if (request != last + 1 || mapping->request.sequence != request ||
        mapping->request.frames > kMaximumFrames || mapping->request.eventCount > kMaximumParameterEvents) {
      mapping->helperState.store(2, std::memory_order_release); cleanup(); return 14;
    }
    const auto &input = mapping->request;
    auto &output = mapping->reply;
    output.sequence = input.sequence; output.sampleTime = input.sampleTime; output.frames = input.frames;
    std::array<PreparedParameterEvent, kMaximumParameterEvents> events{};
    for (uint32_t i = 0; i < input.eventCount; ++i) {
      const auto &event = input.events[i];
      if (event.sampleOffset >= input.frames || event.normalizedValue < 0 || event.normalizedValue > 1) { mapping->helperState.store(2, std::memory_order_release); cleanup(); return 15; }
      events[i] = {event.parameterID, event.sampleOffset, event.normalizedValue};
    }
    std::copy_n(input.left.data(), input.frames, output.left.data());
    std::copy_n(input.right.data(), input.frames, output.right.data());
    if (!effect->process(output.left.data(), output.right.data(), output.frames, output.sampleTime,
                         std::span<const PreparedParameterEvent>(events.data(), input.eventCount))) {
      mapping->helperState.store(2, std::memory_order_release); cleanup(); return 16;
    }
    mapping->helperHeartbeat.fetch_add(1, std::memory_order_release);
    mapping->replySequence.store(request, std::memory_order_release);
    last = request;
  }
}
}

int main(int argc, char **argv) {
  return argc == 3 && std::strcmp(argv[1], "--shared-memory") == 0 ? run(argv[2]) : 2;
}
