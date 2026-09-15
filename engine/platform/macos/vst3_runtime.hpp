#pragma once

#include "audio/effect.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <memory>
#include <string>

// This file intentionally contains only fixed-size POD protocol data.  It is
// mapped by the host and the disposable VST3 worker; neither side needs an
// allocator or a lock in its audio callback.
namespace daw::vst3runtime {

constexpr uint32_t kProtocolMagic = 0x4d445652U; // MDVR
constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kMaximumFrames = 4096;
// The renderer has already accepted at most this many points per project. The
// isolated wire protocol must not turn a valid persisted automation lane into
// a runtime fault merely because it uses a smaller private cap.
constexpr uint32_t kMaximumParameterEvents = static_cast<uint32_t>(kMaxProjectPluginParameterAutomationPoints);
constexpr uint32_t kMaximumStateBytes = 16U * 1024U * 1024U;
constexpr uint32_t kMaximumControlParameters = 4096;
constexpr uint32_t kMaximumControlPayloadBytes = 8U * 1024U * 1024U;

struct ParameterEvent {
  uint32_t parameterID = 0;
  uint32_t sampleOffset = 0;
  float normalizedValue = 0;
  uint32_t reserved = 0;
};

struct RequestBlock {
  uint64_t sequence = 0;
  uint64_t sampleTime = 0;
  uint32_t frames = 0;
  uint32_t eventCount = 0;
  std::array<ParameterEvent, kMaximumParameterEvents> events{};
  std::array<float, kMaximumFrames> left{};
  std::array<float, kMaximumFrames> right{};
};
struct ReplyBlock {
  uint64_t sequence = 0;
  uint64_t sampleTime = 0;
  uint32_t frames = 0;
  uint32_t reserved = 0;
  std::array<float, kMaximumFrames> left{};
  std::array<float, kMaximumFrames> right{};
};

struct SharedMapping {
  uint32_t magic = kProtocolMagic;
  uint32_t version = kProtocolVersion;
  uint32_t stateBytes = 0;
  uint32_t reserved = 0;
  std::atomic<uint64_t> requestSequence{0};
  std::atomic<uint64_t> replySequence{0};
  std::atomic<uint64_t> helperHeartbeat{0};
  std::atomic<uint32_t> helperState{0}; // 0 booting, 1 ready, 2 faulted
  std::atomic<uint32_t> shutdown{0};
  uint32_t pluginLatencyFrames = 0;
  uint32_t pluginTailFrames = 0;
  RequestBlock request{};
  ReplyBlock reply{};
};

static_assert(std::is_trivially_destructible_v<SharedMapping>);
constexpr size_t mappingBytes(uint32_t stateBytes) { return sizeof(SharedMapping) + stateBytes; }
inline uint8_t *statePayload(SharedMapping *mapping) { return reinterpret_cast<uint8_t *>(mapping) + sizeof(SharedMapping); }
inline const uint8_t *statePayload(const SharedMapping *mapping) { return reinterpret_cast<const uint8_t *>(mapping) + sizeof(SharedMapping); }

constexpr uint32_t kControlMagic = 0x4d445643U; // MDVC
constexpr uint32_t kControlVersion = 1;
enum class ControlOperation : uint32_t { ListParameters = 1, Snapshot = 2, SetNormalized = 3 };
// One-shot, control-thread-only mapping. The trailing layout is exactly
// request MDVS, response MDVS, then response parameter bytes. The response
// capacity is supplied by the host and validated by both participants.
struct ControlMapping {
  uint32_t magic = kControlMagic;
  uint32_t version = kControlVersion;
  uint32_t operation = 0;
  std::atomic<uint32_t> completion{0}; // 0 pending, 1 success, 2 failure
  uint32_t requestStateBytes = 0;
  uint32_t responseStateBytes = 0;
  uint32_t responsePayloadBytes = 0;
  uint32_t responseCapacityBytes = 0;
  uint32_t parameterID = 0;
  float normalizedValue = 0;
  uint32_t pluginLatencyFrames = 0;
  uint32_t pluginTailFrames = 0;
};
static_assert(std::is_trivially_destructible_v<ControlMapping>);
constexpr size_t controlMappingBytes(uint32_t requestStateBytes, uint32_t responseCapacityBytes) {
  return sizeof(ControlMapping) + requestStateBytes + responseCapacityBytes;
}
inline uint8_t *controlRequestPayload(ControlMapping *mapping) { return reinterpret_cast<uint8_t *>(mapping) + sizeof(ControlMapping); }
inline const uint8_t *controlRequestPayload(const ControlMapping *mapping) { return reinterpret_cast<const uint8_t *>(mapping) + sizeof(ControlMapping); }
inline uint8_t *controlResponsePayload(ControlMapping *mapping) { return controlRequestPayload(mapping) + mapping->requestStateBytes; }
inline const uint8_t *controlResponsePayload(const ControlMapping *mapping) { return controlRequestPayload(mapping) + mapping->requestStateBytes; }

} // namespace daw::vst3runtime

namespace daw {
// Test-only control hook. Production code never calls this; keeping the override
// out of the process environment prevents an injected helper path at launch.
void setVst3RuntimeHelperPathForTesting(std::string path);
std::unique_ptr<PreparedEffect> prepareVst3OutOfProcessEffect(
    const PluginInsert &plugin, uint32_t sampleRate = 48000,
    uint32_t maxFrames = 4096);
std::vector<Vst3Parameter> remoteVst3Parameters(const PluginInsert &,
                                                 uint32_t sampleRate = 48000,
                                                 uint32_t maxFrames = 4096);
Vst3EffectSnapshot remoteSnapshotVst3Effect(const PluginInsert &,
                                             uint32_t sampleRate = 48000,
                                             uint32_t maxFrames = 4096);
Vst3EffectSnapshot remoteSetVst3Parameter(const PluginInsert &, uint32_t parameterID,
                                           float normalizedValue,
                                           uint32_t sampleRate = 48000,
                                           uint32_t maxFrames = 4096);
} // namespace daw
