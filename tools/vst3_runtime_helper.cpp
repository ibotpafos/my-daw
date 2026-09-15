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

void append16(std::vector<uint8_t> &bytes, uint16_t value) { bytes.push_back(static_cast<uint8_t>(value)); bytes.push_back(static_cast<uint8_t>(value >> 8)); }
void append32(std::vector<uint8_t> &bytes, uint32_t value) { for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(value >> shift)); }
void appendFloat(std::vector<uint8_t> &bytes, float value) { uint32_t raw = 0; std::memcpy(&raw, &value, sizeof(raw)); append32(bytes, raw); }
std::vector<uint8_t> encodeParameters(const std::vector<Vst3Parameter> &parameters) {
  if (parameters.size() > kMaximumControlParameters) throw Error("VST3 parameter count exceeds control limit");
  std::vector<uint8_t> bytes; bytes.reserve(parameters.size() * 64 + 4); append32(bytes, static_cast<uint32_t>(parameters.size()));
  for (const auto &parameter : parameters) {
    if (parameter.title.size() > 512 || parameter.shortTitle.size() > 512 || parameter.units.size() > 512 ||
        !std::isfinite(parameter.normalizedValue) || !std::isfinite(parameter.defaultNormalizedValue) ||
        parameter.normalizedValue < 0 || parameter.normalizedValue > 1 || parameter.defaultNormalizedValue < 0 || parameter.defaultNormalizedValue > 1)
      throw Error("VST3 parameter control value is invalid");
    append32(bytes, parameter.id); append32(bytes, static_cast<uint32_t>(parameter.stepCount)); append32(bytes, parameter.flags);
    appendFloat(bytes, parameter.normalizedValue); appendFloat(bytes, parameter.defaultNormalizedValue);
    append16(bytes, static_cast<uint16_t>(parameter.title.size())); append16(bytes, static_cast<uint16_t>(parameter.shortTitle.size())); append16(bytes, static_cast<uint16_t>(parameter.units.size()));
    bytes.insert(bytes.end(), parameter.title.begin(), parameter.title.end()); bytes.insert(bytes.end(), parameter.shortTitle.begin(), parameter.shortTitle.end()); bytes.insert(bytes.end(), parameter.units.begin(), parameter.units.end());
    if (bytes.size() > kMaximumControlPayloadBytes) throw Error("VST3 parameter control payload exceeds limit");
  }
  return bytes;
}

PluginInsert controlInsert(const uint8_t *state, uint32_t stateBytes) {
  std::string error;
  const auto envelope = decodeVst3StateEnvelope(std::span<const uint8_t>(state, stateBytes), &error);
  if (!envelope || envelope->descriptor.fingerprint.empty() || executableFingerprint(envelope->descriptor.modulePath) != envelope->descriptor.fingerprint)
    throw Error("VST3 control state or fingerprint is invalid");
  PluginInsert insert{}; insert.hostingMode = PluginHostingMode::InProcess;
  insert.type = kVst3PluginComponentSentinel; insert.subtype = kVst3PluginComponentSentinel; insert.manufacturer = kVst3PluginComponentSentinel;
  insert.state.assign(state, state + stateBytes); return insert;
}

int runControl(const char *name) {
  const int fd = shm_open(name, O_RDWR, 0); if (fd < 0) return 30;
  struct stat info{}; if (fstat(fd, &info) != 0 || info.st_size < static_cast<off_t>(sizeof(ControlMapping))) { close(fd); return 31; }
  const size_t mapBytes = static_cast<size_t>(info.st_size);
  auto *mapping = static_cast<ControlMapping *>(mmap(nullptr, mapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (mapping == MAP_FAILED) { close(fd); return 32; }
  const auto cleanup = [&] { munmap(mapping, mapBytes); close(fd); };
  const uint64_t capacity = mapping->responseCapacityBytes;
  const size_t logicalBytes = controlMappingBytes(mapping->requestStateBytes, mapping->responseCapacityBytes);
  const size_t pageBytes = static_cast<size_t>(getpagesize());
  if (mapping->magic != kControlMagic || mapping->version != kControlVersion || mapping->requestStateBytes > kMaximumStateBytes ||
      capacity > static_cast<uint64_t>(kMaximumStateBytes) + kMaximumControlPayloadBytes ||
      mapBytes < logicalBytes || mapBytes - logicalBytes >= pageBytes) { cleanup(); return 33; }
  try {
    const auto operation = static_cast<ControlOperation>(mapping->operation);
    if (operation != ControlOperation::ListParameters && operation != ControlOperation::Snapshot && operation != ControlOperation::SetNormalized)
      throw Error("Unsupported VST3 control operation");
    if (operation == ControlOperation::SetNormalized && (!std::isfinite(mapping->normalizedValue) || mapping->normalizedValue < 0 || mapping->normalizedValue > 1))
      throw Error("Invalid VST3 normalized control value");
    const auto insert = controlInsert(controlRequestPayload(mapping), mapping->requestStateBytes);
    Vst3EffectSnapshot snapshot{};
    if (operation == ControlOperation::ListParameters) snapshot.parameters = vst3Parameters(insert);
    else if (operation == ControlOperation::Snapshot) snapshot = snapshotVst3Effect(insert);
    else snapshot = setVst3Parameter(insert, mapping->parameterID, mapping->normalizedValue);
    const auto parameters = encodeParameters(snapshot.parameters);
    if (snapshot.state.size() > kMaximumStateBytes || parameters.size() > kMaximumControlPayloadBytes ||
        snapshot.state.size() + parameters.size() > mapping->responseCapacityBytes)
      throw Error("VST3 control response exceeds mapping");
    mapping->pluginLatencyFrames = snapshot.latencyFrames; mapping->pluginTailFrames = snapshot.tailFrames;
    mapping->responseStateBytes = static_cast<uint32_t>(snapshot.state.size()); mapping->responsePayloadBytes = static_cast<uint32_t>(parameters.size());
    auto *response = controlResponsePayload(mapping);
    std::copy(snapshot.state.begin(), snapshot.state.end(), response);
    std::copy(parameters.begin(), parameters.end(), response + snapshot.state.size());
    mapping->completion.store(1, std::memory_order_release); cleanup(); return 0;
  } catch (...) { mapping->completion.store(2, std::memory_order_release); cleanup(); return 34; }
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
  PluginInsert insert{};
  try { insert = controlInsert(statePayload(mapping), mapping->stateBytes); }
  catch (...) {
    mapping->helperState.store(2, std::memory_order_release); cleanup(); return 18;
  }
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
        mapping->request.frames > kMaximumFrames || mapping->request.eventCount > kMaximumParameterEvents ||
        mapping->request.midiEventCount > kMaximumMidiEvents) {
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
    std::array<PreparedMidiEvent, kMaximumMidiEvents> midi{};
    for (uint32_t i = 0; i < input.midiEventCount; ++i) {
      const auto &event = input.midiEvents[i];
      if (event.sampleOffset >= input.frames || event.channel > 15 || event.pitch > 127 ||
          event.velocity > 127 || event.noteOff > 1) { mapping->helperState.store(2, std::memory_order_release); cleanup(); return 15; }
      midi[i] = {event.sampleOffset, event.channel, event.pitch, event.velocity, event.noteOff != 0};
    }
    std::copy_n(input.left.data(), input.frames, output.left.data());
    std::copy_n(input.right.data(), input.frames, output.right.data());
    if (!effect->process(output.left.data(), output.right.data(), output.frames, output.sampleTime,
                         std::span<const PreparedParameterEvent>(events.data(), input.eventCount),
                         std::span<const PreparedMidiEvent>(midi.data(), input.midiEventCount))) {
      mapping->helperState.store(2, std::memory_order_release); cleanup(); return 16;
    }
    mapping->helperHeartbeat.fetch_add(1, std::memory_order_release);
    mapping->replySequence.store(request, std::memory_order_release);
    last = request;
  }
}
}

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  if (std::strcmp(argv[1], "--shared-memory") == 0) return run(argv[2]);
  if (std::strcmp(argv[1], "--control-shared-memory") == 0) return runControl(argv[2]);
  return 2;
}
