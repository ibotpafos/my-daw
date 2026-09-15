#pragma once
#include "domain/session.hpp"
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace daw {
// A normalized project-timeline automation value, resolved by Renderer to a
// sample offset in one bounded host block. The storage belongs to Renderer;
// implementations must consume it synchronously and never retain the span.
struct PreparedParameterEvent {
  uint32_t parameterID = 0;
  uint32_t sampleOffset = 0;
  float normalizedValue = 0;
};
struct AudioUnitDescriptor {
  uint32_t type = 0, subtype = 0, manufacturer = 0;
  std::string name;
};
struct AudioUnitSnapshot {
  std::string name;
  uint32_t latencyFrames = 0;
  std::vector<uint8_t> state;
};
// Runtime state is deliberately separate from the persisted PluginInsert
// policy. Implementations expose it through atomics so Renderer and the C ABI
// can inspect a published graph without touching plug-in state on the control
// thread.  The default keeps every existing in-process effect compatible.
enum class PreparedEffectRuntimeState : uint32_t {
  Unprepared = 0,
  ActiveInProcess = 1,
  ActiveIsolated = 2,
  DryFallback = 3,
  Failed = 4,
};
struct PreparedEffectRuntimeStatus {
  PreparedEffectRuntimeState state = PreparedEffectRuntimeState::ActiveInProcess;
  uint32_t extraPipelineLatencyFrames = 0;
  uint32_t faultCode = 0;
};
// `IAudioProcessor::getTailSamples()` uses UINT32_MAX for kInfiniteTail.
// Keep that distinction at the host boundary instead of turning it into a
// large finite duration: offline export needs an explicit user policy for an
// unbounded processor.
struct EffectTail {
  uint32_t finiteFrames = 0;
  bool infinite = false;
};
class PreparedEffect {
public:
  virtual ~PreparedEffect() = default;
  // On failure the implementation restores dry input and returns false.
  virtual bool process(float *left, float *right, uint32_t frames,
                       uint64_t sampleTime,
                       std::span<const PreparedParameterEvent> parameterEvents =
                           {}) noexcept = 0;
  // The initialized instance's algorithmic delay at the configured sample rate.
  // This is queried once on the control thread and is safe to cache for RT use.
  virtual uint32_t latencyFrames() const noexcept = 0;
  // Raw VST3 tail declaration captured after activation, including the
  // UINT32_MAX infinite sentinel. Existing AU effects default to zero until
  // their host reports a comparable value.
  virtual uint32_t tailFrames() const noexcept { return 0; }
  virtual EffectTail tail() const noexcept {
    const auto frames = tailFrames();
    return {frames == std::numeric_limits<uint32_t>::max() ? 0U : frames,
            frames == std::numeric_limits<uint32_t>::max()};
  }
  // Must be safe to call while the audio graph is published. Implementations
  // with mutable state publish this value from lock-free atomics.
  virtual PreparedEffectRuntimeStatus runtimeStatus() const noexcept {
    return {};
  }
};
std::vector<AudioUnitDescriptor> supportedAudioUnits();
bool audioUnitAvailable(const AudioUnitDescriptor &) noexcept;
AudioUnitSnapshot snapshotAudioUnit(const AudioUnitDescriptor &,
                                    uint32_t sampleRate = 48000,
                                    uint32_t maxFrames = 4096);
std::unique_ptr<PreparedEffect> prepareAudioUnit(const PluginInsert &,
                                                 uint32_t sampleRate = 48000,
                                                 uint32_t maxFrames = 4096);

// VST3 inserts carry the MDVS envelope in PluginInsert::state.  The public
// audio contract remains PreparedEffect, so the renderer can host AU and VST3
// effects in one serial master chain.
struct Vst3Parameter {
  uint32_t id = 0;
  std::string title;
  std::string shortTitle;
  std::string units;
  float normalizedValue = 0;
  float defaultNormalizedValue = 0;
  int32_t stepCount = 0;
  uint32_t flags = 0;
};
struct Vst3EffectSnapshot {
  uint32_t latencyFrames = 0;
  uint32_t tailFrames = 0;
  std::vector<uint8_t> state;
  std::vector<Vst3Parameter> parameters;
};
bool isVst3Insert(const PluginInsert &) noexcept;
std::vector<Vst3Parameter> vst3Parameters(const PluginInsert &,
                                          uint32_t sampleRate = 48000,
                                          uint32_t maxFrames = 4096);
Vst3EffectSnapshot snapshotVst3Effect(const PluginInsert &,
                                      uint32_t sampleRate = 48000,
                                      uint32_t maxFrames = 4096);
Vst3EffectSnapshot setVst3Parameter(const PluginInsert &, uint32_t parameterID,
                                    float normalizedValue,
                                    uint32_t sampleRate = 48000,
                                    uint32_t maxFrames = 4096);
std::unique_ptr<PreparedEffect> prepareVst3Effect(const PluginInsert &,
                                                  uint32_t sampleRate = 48000,
                                                  uint32_t maxFrames = 4096);
} // namespace daw
