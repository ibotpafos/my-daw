#include "platform/macos/vst3_runtime.hpp"

#include "platform/macos/vst3_effect.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <libproc.h>
#include <memory>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace daw {
namespace {
using namespace vst3runtime;

constexpr uint32_t kPipelineLatencyFrames = kMaximumFrames;

std::string &testHelperPath() { static std::string path; return path; }

std::string helperPath() {
  if (!testHelperPath().empty()) return testHelperPath();
  std::array<char, PROC_PIDPATHINFO_MAXSIZE> executable{};
  const int count = proc_pidpath(getpid(), executable.data(),
                                 static_cast<uint32_t>(executable.size()));
  if (count <= 0)
    throw Error("Cannot resolve VST3 runtime helper beside executable");
  std::string path(executable.data(), static_cast<size_t>(count));
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos)
    throw Error("Cannot resolve VST3 runtime helper beside executable");
  return path.substr(0, slash + 1) + "daw_vst3_runtime_helper";
}

std::string uniqueMappingName() {
  static std::atomic<uint64_t> serial{0};
  return "/my-daw-vst3-" + std::to_string(static_cast<unsigned long long>(getpid())) +
         "-" + std::to_string(static_cast<unsigned long long>(serial.fetch_add(1, std::memory_order_relaxed)));
}

void closeFd(int &value) noexcept { if (value >= 0) { (void)close(value); value = -1; } }

class RemoteVst3Effect final : public PreparedEffect {
public:
  RemoteVst3Effect(const PluginInsert &plugin, uint32_t sampleRate, uint32_t maxFrames) {
    try {
    if (sampleRate != 48000 || maxFrames == 0 || maxFrames > kMaximumFrames)
      throw Error("VST3 master effects require 48 kHz and 1..4096 frame blocks");
    if (plugin.hostingMode != PluginHostingMode::OutOfProcess)
      throw Error("Remote VST3 proxy requires Out-of-Process hosting mode");
    if (plugin.state.size() > kMaximumStateBytes)
      throw Error("VST3 isolated state exceeds 16 MiB");
    name_ = uniqueMappingName();
    fd_ = shm_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd_ < 0)
      throw Error("Create VST3 isolated shared memory failed: " + std::string(std::strerror(errno)));
    mapBytes_ = mappingBytes(static_cast<uint32_t>(plugin.state.size()));
    if (ftruncate(fd_, static_cast<off_t>(mapBytes_)) != 0)
      throw Error("Size VST3 isolated shared memory failed: " + std::string(std::strerror(errno)));
    mapping_ = static_cast<SharedMapping *>(mmap(nullptr, mapBytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (mapping_ == MAP_FAILED) { mapping_ = nullptr; throw Error("Map VST3 isolated shared memory failed: " + std::string(std::strerror(errno))); }
    new (mapping_) SharedMapping();
    mapping_->stateBytes = static_cast<uint32_t>(plugin.state.size());
    std::copy(plugin.state.begin(), plugin.state.end(), statePayload(mapping_));
    spawnAndAwaitReady();
    latency_ = kPipelineLatencyFrames + mapping_->pluginLatencyFrames;
    tail_ = mapping_->pluginTailFrames;
    maximum_ = maxFrames;
    dryLeft_.fill(0);
    dryRight_.fill(0);
    } catch (...) {
      release();
      throw;
    }
  }

  ~RemoteVst3Effect() override { release(); }
  void release() noexcept {
    // Reaping is strictly lifecycle/control work, never in process().
    if (child_ > 0) {
      if (mapping_) mapping_->shutdown.store(1, std::memory_order_release);
      (void)kill(child_, SIGTERM);
      int status = 0;
      bool reaped = false;
      for (unsigned attempt = 0; attempt < 100; ++attempt) {
        if (waitpid(child_, &status, WNOHANG) == child_) { reaped = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!reaped) { (void)kill(child_, SIGKILL); (void)waitpid(child_, &status, 0); }
      child_ = -1;
    }
    if (mapping_) { mapping_->~SharedMapping(); (void)munmap(mapping_, mapBytes_); }
    mapping_ = nullptr;
    closeFd(fd_);
    if (!name_.empty()) (void)shm_unlink(name_.c_str());
    name_.clear();
  }

  bool process(float *left, float *right, uint32_t frames, uint64_t sampleTime,
               std::span<const PreparedParameterEvent> events) noexcept override {
    if (!left || !right || frames > maximum_ || events.size() > kMaximumParameterEvents) return faultOnce();
    // The fixed 4096-frame dry delay makes startup and an unhealthy worker
    // deterministic.  No branch below waits, allocates, calls the OS, or
    // invokes VST3 from the audio thread.
    if (healthy_.load(std::memory_order_relaxed) &&
        mapping_->helperState.load(std::memory_order_acquire) == 1) {
      const uint64_t reply = mapping_->replySequence.load(std::memory_order_acquire);
      if (publishedSequence_ != 0 && reply == publishedSequence_) {
        if (mapping_->reply.sequence != reply || mapping_->reply.frames > kMaximumFrames) {
          markFault(4);
        } else {
          const auto &block = mapping_->reply;
          // The reply replaces the corresponding delayed dry window. It is only
          // used when it still falls inside the 4096-frame pipeline horizon.
          for (uint32_t i = 0; i < block.frames; ++i) {
            const uint32_t index = static_cast<uint32_t>((block.sampleTime + i) % kPipelineLatencyFrames);
            dryLeft_[index] = block.left[i]; dryRight_[index] = block.right[i];
          }
        }
        publishedSequence_ = 0;
      } else if (publishedSequence_ != 0 && reply > publishedSequence_) {
        markFault(4);
      } else if (publishedSequence_ != 0 &&
                 sampleTime + frames >= publishedSampleTime_ + kPipelineLatencyFrames) {
        markFault(3); // the 4096-frame budget expired without a reply
      }
    } else if (healthy_.load(std::memory_order_relaxed)) {
      markFault(mapping_->helperState.load(std::memory_order_relaxed) == 2 ? 5 : 3);
    }
    std::copy_n(left, frames, inputLeft_.data());
    std::copy_n(right, frames, inputRight_.data());
    for (uint32_t i = 0; i < frames; ++i) {
      const uint32_t index = static_cast<uint32_t>((sampleTime + i) % kPipelineLatencyFrames);
      const float inLeft = left[i], inRight = right[i];
      left[i] = dryLeft_[index]; right[i] = dryRight_[index];
      dryLeft_[index] = inLeft; dryRight_[index] = inRight;
    }
    if (healthy_.load(std::memory_order_relaxed) && publishedSequence_ == 0) {
      auto &request = mapping_->request;
      const uint64_t sequence = nextRequestSequence_++;
      request.sequence = sequence;
      request.sampleTime = sampleTime;
      request.frames = frames;
      request.eventCount = static_cast<uint32_t>(events.size());
      for (size_t i = 0; i < events.size(); ++i) {
        const auto &event = events[i];
        if (event.sampleOffset >= frames || event.normalizedValue < 0 || event.normalizedValue > 1) { markFault(); break; }
        request.events[i] = {event.parameterID, event.sampleOffset, event.normalizedValue, 0};
      }
      if (healthy_.load(std::memory_order_relaxed)) {
        // Input needs no immediate output: worker consumes this copy after the
        // release publication and the host is already reading the dry delay.
        std::copy_n(inputLeft_.data(), frames, request.left.data());
        std::copy_n(inputRight_.data(), frames, request.right.data());
        mapping_->requestSequence.store(sequence, std::memory_order_release);
        publishedSequence_ = sequence;
        publishedSampleTime_ = sampleTime;
      }
    }
    return !faultPending_.exchange(false, std::memory_order_acq_rel);
  }
  uint32_t latencyFrames() const noexcept override { return latency_; }
  uint32_t tailFrames() const noexcept override { return tail_; }
  PreparedEffectRuntimeStatus runtimeStatus() const noexcept override {
    if (healthy_.load(std::memory_order_acquire) && mapping_ &&
        mapping_->helperState.load(std::memory_order_acquire) == 1)
      return {PreparedEffectRuntimeState::ActiveIsolated, kPipelineLatencyFrames, 0};
    return {PreparedEffectRuntimeState::DryFallback, kPipelineLatencyFrames,
            faultCode_.load(std::memory_order_acquire)};
  }

private:
  void spawnAndAwaitReady() {
    const std::string helper = helperPath();
    int readyPipe[2]{-1, -1};
    if (pipe(readyPipe) != 0) throw Error("Create VST3 runtime ready pipe failed");
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, readyPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, readyPipe[0]);
    posix_spawn_file_actions_addclose(&actions, readyPipe[1]);
    std::array<char *, 4> argv{const_cast<char *>(helper.c_str()), const_cast<char *>("--shared-memory"), const_cast<char *>(name_.c_str()), nullptr};
    const int status = posix_spawn(&child_, helper.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions); closeFd(readyPipe[1]);
    if (status != 0) { closeFd(readyPipe[0]); throw Error("Start VST3 runtime helper failed: " + std::string(std::strerror(status))); }
    pollfd descriptor{readyPipe[0], POLLIN | POLLHUP, 0};
    const int ready = poll(&descriptor, 1, 3000);
    std::array<char, 96> text{};
    const ssize_t count = ready > 0 ? read(readyPipe[0], text.data(), text.size() - 1) : -1;
    closeFd(readyPipe[0]);
    if (count <= 0 || std::strcmp(text.data(), "READY\n") != 0 ||
        mapping_->helperState.load(std::memory_order_acquire) != 1)
      throw Error("VST3 runtime helper did not become ready");
  }
  void markFault(uint32_t code = 3) noexcept {
    bool expected = true;
    if (healthy_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
      faultCode_.store(code, std::memory_order_release);
      faultPending_.store(true, std::memory_order_release);
    }
  }
  bool faultOnce() noexcept { markFault(4); return !faultPending_.exchange(false, std::memory_order_acq_rel); }

  int fd_ = -1; pid_t child_ = -1; SharedMapping *mapping_ = nullptr; std::string name_; size_t mapBytes_ = 0;
  uint32_t maximum_ = 0, latency_ = 0, tail_ = 0;
  uint64_t nextRequestSequence_ = 1, publishedSequence_ = 0, publishedSampleTime_ = 0;
  std::atomic<bool> healthy_{true};
  std::atomic<bool> faultPending_{false};
  std::atomic<uint32_t> faultCode_{0};
  std::array<float, kPipelineLatencyFrames> dryLeft_{}, dryRight_{};
  std::array<float, kMaximumFrames> inputLeft_{}, inputRight_{};
};

struct ControlReply {
  uint32_t latency = 0, tail = 0;
  std::vector<uint8_t> state, parameters;
};

bool waitAndReap(pid_t child, std::chrono::milliseconds timeout, bool &timedOut, int &status) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (result == -1) return false;
    if (std::chrono::steady_clock::now() >= deadline) {
      timedOut = true; (void)kill(child, SIGKILL); (void)waitpid(child, &status, 0); return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

ControlReply control(const PluginInsert &plugin, ControlOperation operation,
                     uint32_t parameterID, float normalized, uint32_t sampleRate,
                     uint32_t maxFrames) {
  if (plugin.hostingMode != PluginHostingMode::OutOfProcess)
    throw Error("Remote VST3 control requires Out-of-Process hosting mode");
  if (sampleRate != 48000 || maxFrames == 0 || maxFrames > kMaximumFrames ||
      plugin.state.size() > kMaximumStateBytes ||
      (operation == ControlOperation::SetNormalized &&
       (!std::isfinite(normalized) || normalized < 0 || normalized > 1)))
    throw Error("Invalid remote VST3 control request");
  const uint32_t requestBytes = static_cast<uint32_t>(plugin.state.size());
  constexpr uint32_t responseCapacity = kMaximumStateBytes + kMaximumControlPayloadBytes;
  const size_t bytes = controlMappingBytes(requestBytes, responseCapacity);
  const std::string name = uniqueMappingName();
  int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd < 0) throw Error("Create VST3 control shared memory failed: " + std::string(std::strerror(errno)));
  ControlMapping *mapping = nullptr;
  pid_t child = -1;
  const auto cleanup = [&] {
    if (child > 0) { (void)kill(child, SIGKILL); (void)waitpid(child, nullptr, 0); child = -1; }
    if (mapping) { mapping->~ControlMapping(); (void)munmap(mapping, bytes); mapping = nullptr; }
    closeFd(fd); (void)shm_unlink(name.c_str());
  };
  try {
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) throw Error("Size VST3 control shared memory failed");
    mapping = static_cast<ControlMapping *>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (mapping == MAP_FAILED) { mapping = nullptr; throw Error("Map VST3 control shared memory failed"); }
    new (mapping) ControlMapping();
    mapping->operation = static_cast<uint32_t>(operation);
    mapping->requestStateBytes = requestBytes;
    mapping->responseCapacityBytes = responseCapacity;
    mapping->parameterID = parameterID;
    mapping->normalizedValue = normalized;
    std::copy(plugin.state.begin(), plugin.state.end(), controlRequestPayload(mapping));
    const std::string helper = helperPath();
    std::array<char *, 4> argv{const_cast<char *>(helper.c_str()), const_cast<char *>("--control-shared-memory"), const_cast<char *>(name.c_str()), nullptr};
    const int spawn = posix_spawn(&child, helper.c_str(), nullptr, nullptr, argv.data(), environ);
    if (spawn != 0) throw Error("Start VST3 control helper failed: " + std::string(std::strerror(spawn)));
    bool timedOut = false; int exitStatus = 0;
    const bool exitedSuccessfully = waitAndReap(child, std::chrono::seconds(5), timedOut, exitStatus); child = -1;
    const uint32_t completion = mapping->completion.load(std::memory_order_acquire);
    if (timedOut || !exitedSuccessfully || completion != 1)
      throw Error(timedOut ? "VST3 control helper timed out" : "VST3 control helper failed (exit=" + std::to_string(exitStatus) + ", completion=" + std::to_string(completion) + ")");
    const uint64_t responseTotal = static_cast<uint64_t>(mapping->responseStateBytes) + mapping->responsePayloadBytes;
    if (mapping->responseStateBytes > kMaximumStateBytes ||
        mapping->responsePayloadBytes > kMaximumControlPayloadBytes ||
        responseTotal > mapping->responseCapacityBytes)
      throw Error("Invalid VST3 control helper reply");
    if ((operation == ControlOperation::ListParameters && mapping->responseStateBytes != 0) ||
        (operation != ControlOperation::ListParameters && mapping->responseStateBytes == 0) ||
        mapping->responsePayloadBytes == 0)
      throw Error("Invalid VST3 control response shape");
    ControlReply reply; reply.latency = mapping->pluginLatencyFrames; reply.tail = mapping->pluginTailFrames;
    const uint8_t *payload = controlResponsePayload(mapping);
    reply.state.assign(payload, payload + mapping->responseStateBytes);
    reply.parameters.assign(payload + mapping->responseStateBytes,
                            payload + responseTotal);
    cleanup(); return reply;
  } catch (...) { cleanup(); throw; }
}

uint16_t read16(const std::vector<uint8_t> &bytes, size_t &cursor) {
  if (cursor + 2 > bytes.size()) throw Error("Malformed VST3 control parameter reply");
  const auto value = static_cast<uint16_t>(bytes[cursor]) | (static_cast<uint16_t>(bytes[cursor + 1]) << 8); cursor += 2; return value;
}
uint32_t read32(const std::vector<uint8_t> &bytes, size_t &cursor) {
  if (cursor + 4 > bytes.size()) throw Error("Malformed VST3 control parameter reply");
  uint32_t value = 0; for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<uint32_t>(bytes[cursor++]) << shift; return value;
}
float readFloat(const std::vector<uint8_t> &bytes, size_t &cursor) {
  const uint32_t raw = read32(bytes, cursor); float value = 0; std::memcpy(&value, &raw, sizeof(value));
  if (!std::isfinite(value) || value < 0 || value > 1) throw Error("Invalid VST3 control normalized value"); return value;
}
std::vector<Vst3Parameter> decodeParameters(const std::vector<uint8_t> &bytes) {
  size_t cursor = 0; const uint32_t count = read32(bytes, cursor);
  if (count > kMaximumControlParameters) throw Error("VST3 control parameter count exceeds limit");
  std::vector<Vst3Parameter> output; output.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Vst3Parameter parameter{}; parameter.id = read32(bytes, cursor);
    parameter.stepCount = static_cast<int32_t>(read32(bytes, cursor)); parameter.flags = read32(bytes, cursor);
    parameter.normalizedValue = readFloat(bytes, cursor); parameter.defaultNormalizedValue = readFloat(bytes, cursor);
    const uint16_t title = read16(bytes, cursor), shortTitle = read16(bytes, cursor), units = read16(bytes, cursor);
    const size_t stringBytes = static_cast<size_t>(title) + shortTitle + units;
    if (title > 512 || shortTitle > 512 || units > 512 || cursor + stringBytes > bytes.size()) throw Error("VST3 control parameter text exceeds limit");
    parameter.title.assign(reinterpret_cast<const char *>(bytes.data() + cursor), title); cursor += title;
    parameter.shortTitle.assign(reinterpret_cast<const char *>(bytes.data() + cursor), shortTitle); cursor += shortTitle;
    parameter.units.assign(reinterpret_cast<const char *>(bytes.data() + cursor), units); cursor += units;
    output.push_back(std::move(parameter));
  }
  if (cursor != bytes.size()) throw Error("Trailing VST3 control parameter data");
  return output;
}
} // namespace

void setVst3RuntimeHelperPathForTesting(std::string path) { testHelperPath() = std::move(path); }

std::unique_ptr<PreparedEffect> prepareVst3OutOfProcessEffect(const PluginInsert &plugin,
                                                               uint32_t sampleRate,
                                                               uint32_t maxFrames) {
  return std::make_unique<RemoteVst3Effect>(plugin, sampleRate, maxFrames);
}

std::vector<Vst3Parameter> remoteVst3Parameters(const PluginInsert &plugin, uint32_t sampleRate, uint32_t maxFrames) {
  return decodeParameters(control(plugin, ControlOperation::ListParameters, 0, 0, sampleRate, maxFrames).parameters);
}
Vst3EffectSnapshot remoteSnapshotVst3Effect(const PluginInsert &plugin, uint32_t sampleRate, uint32_t maxFrames) {
  auto reply = control(plugin, ControlOperation::Snapshot, 0, 0, sampleRate, maxFrames);
  return {reply.latency, reply.tail, std::move(reply.state), decodeParameters(reply.parameters)};
}
Vst3EffectSnapshot remoteSetVst3Parameter(const PluginInsert &plugin, uint32_t parameterID, float normalizedValue, uint32_t sampleRate, uint32_t maxFrames) {
  auto reply = control(plugin, ControlOperation::SetNormalized, parameterID, normalizedValue, sampleRate, maxFrames);
  return {reply.latency, reply.tail, std::move(reply.state), decodeParameters(reply.parameters)};
}

} // namespace daw
