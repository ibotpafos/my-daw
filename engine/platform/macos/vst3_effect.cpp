#include "platform/macos/vst3_effect.hpp"
#include "platform/macos/vst3_runtime.hpp"

#include "plugins/plugin_descriptor.hpp"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "base/funknown.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace daw {
std::unique_ptr<PreparedEffect> prepareVst3OutOfProcessEffect(
    const PluginInsert &, uint32_t, uint32_t);
namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;

constexpr uint32_t kMaximumFrames = 4096;
constexpr size_t kMaximumStateBytes = 8U * 1024U * 1024U;
constexpr int32 kExpectedBuses = 1;

// A bounded, allocation-free IEventList. The SDK's EventList implementation
// lives in eventlist.cpp, which is deliberately not compiled into this target;
// more importantly, a fixed array here gives the audio thread a container it
// can never grow. The instance is a member of Vst3Effect, sized once at
// prepare time; addEvent fails closed past capacity. Refcounting returns a
// constant because the list is never passed out of the process call.
class BoundedEventList final : public IEventList {
public:
  static constexpr int32 kCapacity = 512;

  tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override {
    if (!obj) return kInvalidArgument;
    if (FUnknownPrivate::iidEqual(iid, IEventList::iid) ||
        FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
      *obj = static_cast<IEventList *>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }

  int32 PLUGIN_API addEvent(Event &event) override {
    if (count_ >= kCapacity) return kResultFalse;
    events_[count_] = event;
    ++count_;
    return kResultTrue;
  }
  int32 PLUGIN_API getEventCount() override { return count_; }
  tresult PLUGIN_API getEvent(int32 index, Event &event) override {
    if (index < 0 || static_cast<uint32_t>(index) >= count_) return kResultFalse;
    event = events_[static_cast<uint32_t>(index)];
    return kResultTrue;
  }
  void clear() noexcept { count_ = 0; }

private:
  // Event is a 72-byte POD union; 512 entries bound the worst-case lane at
  // 36 KiB, resident for the life of the prepared effect.
  Event events_[kCapacity]{};
  uint32_t count_ = 0;
};

void require(tresult result, const char *operation) {
  if (result != kResultOk && result != kResultTrue)
    throw Error(std::string("VST3 ") + operation + " failed");
}

std::vector<uint8_t> streamBytes(MemoryStream &stream) {
  const auto size = stream.getSize();
  if (size < 0 || static_cast<uint64_t>(size) > kMaximumStateBytes)
    throw Error("VST3 state exceeds 8 MiB");
  if (size == 0)
    return {};
  const auto *data = stream.getData();
  if (size != 0 && data == nullptr)
    throw Error("VST3 state capture failed");
  return {reinterpret_cast<const uint8_t *>(data),
          reinterpret_cast<const uint8_t *>(data) + static_cast<size_t>(size)};
}

VST3::UID stateClassId(const Vst3StateEnvelope &envelope) {
  const auto parsed = VST3::UID::fromString(
      textualVst3Fuid(envelope.descriptor.vst3ClassFuid), false);
  if (!parsed)
    throw Error("VST3 class FUID is invalid");
  return *parsed;
}

// Only the first main audio/event buses are used. Additional buses stay
// inactive. A MIDI source may have no audio input at all: never invent a
// silent input bus for it, since bus counts are part of the VST3 contract.
struct MainBuses {
  bool audioInput = false;
  bool eventInput = false;
};

MainBuses mainBuses(IComponent &component) {
  const auto audioInputs = component.getBusCount(kAudio, kInput);
  const auto audioOutputs = component.getBusCount(kAudio, kOutput);
  const auto eventInputs = component.getBusCount(kEvent, kInput);
  if (audioInputs < 0 || audioOutputs < kExpectedBuses || eventInputs < 0)
    throw Error("VST3 invalid main bus counts");
  auto requireMain = [&](MediaType media, BusDirection direction) {
    BusInfo info{};
    require(component.getBusInfo(media, direction, 0, info), "read main bus");
    if (info.mediaType != media || info.direction != direction ||
        info.busType != kMain || info.channelCount <= 0)
      throw Error("VST3 unsupported main bus");
  };
  requireMain(kAudio, kOutput);
  if (audioInputs > 0)
    requireMain(kAudio, kInput);
  if (eventInputs > 0)
    requireMain(kEvent, kInput);
  if (audioInputs == 0 && eventInputs == 0)
    throw Error("VST3 source requires a main note-event input");
  return {audioInputs > 0, eventInputs > 0};
}

void requireStereo(IComponent &component, BusDirection direction) {
  BusInfo info{};
  require(component.getBusInfo(kAudio, direction, 0, info), "read negotiated audio bus");
  if (info.channelCount != 2)
    throw Error("VST3 main audio bus did not negotiate stereo");
}

std::string string128(const TChar *value) {
  return StringConvert::convert(value);
}

Vst3Parameter parameter(IEditController &controller, int32 index) {
  ParameterInfo info{};
  require(controller.getParameterInfo(index, info), "read parameter metadata");
  const auto current = controller.getParamNormalized(info.id);
  if (!std::isfinite(current))
    throw Error("VST3 parameter value is invalid");
  return {static_cast<uint32_t>(info.id),
          string128(info.title),
          string128(info.shortTitle),
          string128(info.units),
          static_cast<float>(std::clamp(current, 0.0, 1.0)),
          static_cast<float>(std::clamp(info.defaultNormalizedValue, 0.0, 1.0)),
          info.stepCount,
          static_cast<uint32_t>(info.flags)};
}

class Vst3Effect final : public PreparedEffect {
public:
  Vst3Effect(const PluginInsert &plugin, uint32_t sampleRate,
             uint32_t maxFrames)
      : changes(static_cast<int32>(plugin.parameterAutomation.size())) {
    try {
      initialize(plugin, sampleRate, maxFrames);
    } catch (...) {
      // A throwing constructor does not run this class's destructor. Unwind
      // initialized SDK objects while their module and host are still alive.
      shutdown();
      throw;
    }
  }

  ~Vst3Effect() override { shutdown(); }

  bool process(float *left, float *right, uint32_t frames, uint64_t sampleTime,
               std::span<const PreparedParameterEvent> parameterEvents,
               std::span<const PreparedMidiEvent> midiEvents) noexcept
      override {
    if (left == nullptr || right == nullptr || frames > maximum ||
        midiEvents.size() > static_cast<size_t>(BoundedEventList::kCapacity))
      return false;
    std::copy_n(left, frames, inputLeft.data());
    std::copy_n(right, frames, inputRight.data());
    float *inputChannels[]{inputLeft.data(), inputRight.data()};
    float *outputChannels[]{left, right};
    AudioBusBuffers inputs{};
    inputs.numChannels = 2;
    inputs.silenceFlags = 0;
    inputs.channelBuffers32 = inputChannels;
    AudioBusBuffers outputs{};
    outputs.numChannels = 2;
    outputs.silenceFlags = 0;
    outputs.channelBuffers32 = outputChannels;
    ProcessContext context{};
    context.state = ProcessContext::kContTimeValid;
    context.sampleRate = 48000;
    context.continousTimeSamples = static_cast<int64>(std::min<uint64_t>(
        sampleTime, static_cast<uint64_t>(std::numeric_limits<int64>::max())));
    context.projectTimeSamples = context.continousTimeSamples;
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = static_cast<int32>(frames);
    data.numInputs = buses.audioInput ? 1 : 0;
    data.numOutputs = 1;
    data.inputs = buses.audioInput ? &inputs : nullptr;
    data.outputs = &outputs;
    data.processContext = &context;
    changes.clearQueue();
    for (const auto &event : parameterEvents) {
      if (event.sampleOffset >= frames ||
          !std::isfinite(event.normalizedValue) ||
          event.normalizedValue < 0.0f || event.normalizedValue > 1.0f) {
        std::copy_n(inputLeft.data(), frames, left);
        std::copy_n(inputRight.data(), frames, right);
        return false;
      }
      int32 queueIndex = -1;
      auto *queue = changes.addParameterData(
          static_cast<ParamID>(event.parameterID), queueIndex);
      int32 pointIndex = -1;
      if (!queue || queueIndex < 0 ||
          queue->addPoint(static_cast<int32>(event.sampleOffset),
                          event.normalizedValue, pointIndex) != kResultTrue) {
        std::copy_n(inputLeft.data(), frames, left);
        std::copy_n(inputRight.data(), frames, right);
        return false;
      }
    }
    data.inputParameterChanges = parameterEvents.empty() ? nullptr : &changes;
    data.outputParameterChanges = nullptr;
    // The MIDI lane is rebuilt from the caller's bounded span on every block;
    // the list itself never allocates. Like automation, a malformed offset
    // fails the block closed so notes can never be silently dropped.
    inputEvents.clear();
    for (const auto &event : midiEvents) {
      if (event.sampleOffset >= frames || event.channel > 15 || event.pitch > 127 ||
          event.velocity > 127) {
        std::copy_n(inputLeft.data(), frames, left);
        std::copy_n(inputRight.data(), frames, right);
        return false;
      }
      // Every insert sees the track's MIDI span. An audio-only effect has
      // no event bus, but must still process the audio from an upstream synth.
      if (!buses.eventInput)
        continue;
      Event vst{};
      vst.busIndex = 0;
      vst.flags = 0;
      vst.sampleOffset = static_cast<int32>(event.sampleOffset);
      vst.ppqPosition = 0.0;
      if (event.noteOff) {
        vst.type = Event::kNoteOffEvent;
        vst.noteOff.channel = static_cast<int16>(event.channel);
        vst.noteOff.pitch = static_cast<int16>(event.pitch);
        vst.noteOff.tuning = 0.0f;
        vst.noteOff.velocity = static_cast<float>(event.velocity) / 127.0f;
        vst.noteOff.noteId = -1;
      } else {
        vst.type = Event::kNoteOnEvent;
        vst.noteOn.channel = static_cast<int16>(event.channel);
        vst.noteOn.pitch = static_cast<int16>(event.pitch);
        vst.noteOn.tuning = 0.0f;
        // VST3 note velocity is normalized; the MIDI byte 0..127 maps to
        // 0.0..1.0 with 127 as full scale, and length 0 defers the release
        // to the explicit note-off event that always follows.
        vst.noteOn.velocity = static_cast<float>(event.velocity) / 127.0f;
        vst.noteOn.length = 0;
        vst.noteOn.noteId = -1;
      }
      if (inputEvents.addEvent(vst) != kResultTrue) {
        std::copy_n(inputLeft.data(), frames, left);
        std::copy_n(inputRight.data(), frames, right);
        return false;
      }
    }
    data.inputEvents = buses.eventInput ? &inputEvents : nullptr;
    data.outputEvents = nullptr;
    // The in-place host buffers may contain upstream audio. A no-input
    // generator must not leak it when it reports silence or writes no samples.
    if (!buses.audioInput) {
      std::fill_n(left, frames, 0.0f);
      std::fill_n(right, frames, 0.0f);
    }
    const auto result = processor()->process(data);
    // Silence flags are optional optimization hints, not a replacement for
    // the valid sample buffers required by VST3. In particular, the pinned
    // mda synth can retain a silent flag from an earlier internal 16-sample
    // slice even when a later slice contains a note. Do not erase that audio.
    // Source buffers were initialized above; downstream inserts receive the
    // actual samples with no silence optimization, as on the effect path.
    if (result == kResultOk || result == kResultTrue)
      return true;
    std::copy_n(inputLeft.data(), frames, left);
    std::copy_n(inputRight.data(), frames, right);
    return false;
  }

  uint32_t latencyFrames() const noexcept override { return latency; }
  uint32_t tailFrames() const noexcept override { return tail; }

  std::vector<Vst3Parameter> parameters() {
    const auto count = controller->getParameterCount();
    if (count < 0 || count > 4096)
      throw Error("VST3 parameter count is invalid");
    std::vector<Vst3Parameter> values;
    values.reserve(static_cast<size_t>(count));
    for (int32 index = 0; index < count; ++index)
      values.push_back(parameter(*controller, index));
    return values;
  }

  void setParameter(uint32_t id, float normalized) {
    if (!std::isfinite(normalized) || normalized < 0 || normalized > 1)
      throw Error("VST3 normalized parameter must be in 0...1");
    require(
        controller->setParamNormalized(static_cast<ParamID>(id), normalized),
        "set parameter");
    // A controller value is UI state. Feed the matching automation point
    // to the processor as well, so a subsequent component snapshot holds
    // the actual DSP value even for plug-ins with split component/control.
    ParameterChanges changes(1);
    int32 queueIndex = -1;
    auto *queue =
        changes.addParameterData(static_cast<ParamID>(id), queueIndex);
    if (queue == nullptr || queueIndex < 0)
      throw Error("VST3 create parameter queue failed");
    int32 pointIndex = -1;
    require(queue->addPoint(0, normalized, pointIndex),
            "queue parameter change");
    float inputLeftSample = 0;
    float inputRightSample = 0;
    float outputLeftSample = 0;
    float outputRightSample = 0;
    float *inputChannels[]{&inputLeftSample, &inputRightSample};
    float *outputChannels[]{&outputLeftSample, &outputRightSample};
    AudioBusBuffers input{};
    input.numChannels = 2;
    input.channelBuffers32 = inputChannels;
    AudioBusBuffers output{};
    output.numChannels = 2;
    output.channelBuffers32 = outputChannels;
    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = 1;
    data.numInputs = buses.audioInput ? 1 : 0;
    data.numOutputs = 1;
    data.inputs = buses.audioInput ? &input : nullptr;
    data.outputs = &output;
    data.inputParameterChanges = &changes;
    require(processor()->process(data), "apply parameter change");
  }

  Vst3EffectSnapshot snapshot() {
    Vst3StateEnvelope state = envelope;
    MemoryStream componentState;
    require(component->getState(&componentState), "capture component state");
    state.componentState = streamBytes(componentState);
    MemoryStream controllerState;
    const auto controllerResult = controller->getState(&controllerState);
    // SDK EditController defaults to kNotImplemented when there is no extra
    // UI state (e.g. ADelay). DSP state remains mandatory. Do not swallow
    // actual errors or silently discard previously persisted controller data.
    if (controllerResult == kNotImplemented && controllerState.getSize() == 0 &&
        envelope.controllerState.empty()) {
      state.controllerState.clear();
    } else {
      require(controllerResult, "capture controller state");
      state.controllerState = streamBytes(controllerState);
    }
    Vst3EffectSnapshot result;
    result.latencyFrames = processor()->getLatencySamples();
    result.tailFrames = processor()->getTailSamples();
    result.state = encodeVst3StateEnvelope(state);
    result.parameters = parameters();
    return result;
  }

private:
  void initialize(const PluginInsert &plugin, uint32_t sampleRate, uint32_t maxFrames) {
    if (sampleRate != 48000 || maxFrames == 0 || maxFrames > kMaximumFrames)
      throw Error(
          "VST3 inserts require 48 kHz and 1..4096 frame blocks");
    std::string error;
    const auto decoded = decodeVst3StateEnvelope(plugin.state, &error);
    if (!decoded)
      throw Error("VST3 insert state: " + error);
    envelope = *decoded;
    module =
        VST3::Hosting::Module::create(envelope.descriptor.modulePath, error);
    if (!module)
      throw Error("Load VST3 module failed: " + error);
    module->getFactory().setHostContext(&host);
    const auto classId = stateClassId(envelope);
    component = module->getFactory().createInstance<IComponent>(classId);
    if (!component)
      throw Error("Create VST3 component failed");
    require(component->initialize(&host), "initialize component");
    initializedComponent = true;
    processorInterface = FUnknownPtr<IAudioProcessor>(component);
    if (!processorInterface)
      throw Error("VST3 component has no audio processor interface");
    TUID controllerId{};
    require(component->getControllerClassId(controllerId),
            "read controller class");
    controller = module->getFactory().createInstance<IEditController>(
        VST3::UID(controllerId));
    if (!controller)
      throw Error("Create VST3 controller failed");
    require(controller->initialize(&host), "initialize controller");
    initializedController = true;
    connect();
    restore(envelope);
    buses = mainBuses(*component);
    require(processor()->canProcessSampleSize(kSample32), "negotiate float32 processing");
    SpeakerArrangement stereo = SpeakerArr::kStereo;
    require(processor()->setBusArrangements(buses.audioInput ? &stereo : nullptr,
                                           buses.audioInput ? 1 : 0, &stereo, 1),
            "negotiate stereo buses");
    requireStereo(*component, kOutput);
    if (buses.audioInput)
      requireStereo(*component, kInput);
    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(maxFrames);
    setup.sampleRate = static_cast<SampleRate>(sampleRate);
    require(processor()->setupProcessing(setup), "setup processing");
    if (buses.audioInput) {
      require(component->activateBus(kAudio, kInput, 0, true), "activate audio input bus");
      inputActive = true;
    }
    if (buses.eventInput) {
      require(component->activateBus(kEvent, kInput, 0, true), "activate note-event input bus");
      eventInputActive = true;
    }
    require(component->activateBus(kAudio, kOutput, 0, true),
            "activate output bus");
    outputActive = true;
    require(component->setActive(true), "activate component");
    active = true;
    require(processor()->setProcessing(true), "start processing");
    processing = true;
    latency = processor()->getLatencySamples();
    tail = processor()->getTailSamples();
    maximum = maxFrames;
    inputLeft.resize(maxFrames);
    inputRight.resize(maxFrames);
    // ParameterChanges uses vectors internally. Grow every queue while
    // this effect is prepared, then clear it; process() only reuses that
    // capacity on the audio thread.
    for (const auto &lane : plugin.parameterAutomation) {
      if (lane.points.empty())
        continue;
      int32 queueIndex = -1;
      auto *queue = changes.addParameterData(
          static_cast<ParamID>(lane.parameterID), queueIndex);
      if (!queue || queueIndex < 0)
        throw Error("VST3 prepare parameter queue failed");
      for (size_t point = 0; point < lane.points.size() + 2; ++point) {
        int32 pointIndex = -1;
        require(queue->addPoint(static_cast<int32>(point), 0.0, pointIndex),
                "reserve parameter point");
      }
    }
    changes.clearQueue();
  }

  void shutdown() noexcept {
    try {
      if (processing)
        (void)processor()->setProcessing(false);
      if (active)
        (void)component->setActive(false);
      if (outputActive)
        (void)component->activateBus(kAudio, kOutput, 0, false);
      if (eventInputActive)
        (void)component->activateBus(kEvent, kInput, 0, false);
      if (inputActive)
        (void)component->activateBus(kAudio, kInput, 0, false);
      disconnect();
      if (initializedController)
        (void)controller->terminate();
      if (initializedComponent)
        (void)component->terminate();
    } catch (...) {
    }
  }

  IAudioProcessor *processor() const { return processorInterface.get(); }
  void connect() {
    componentConnection = FUnknownPtr<IConnectionPoint>(component);
    controllerConnection = FUnknownPtr<IConnectionPoint>(controller);
    if (componentConnection && controllerConnection) {
      require(componentConnection->connect(controllerConnection),
              "connect component");
      componentConnected = true;
      require(controllerConnection->connect(componentConnection),
              "connect controller");
      controllerConnected = true;
    }
  }
  void disconnect() noexcept {
    if (controllerConnected && controllerConnection && componentConnection)
      (void)controllerConnection->disconnect(componentConnection);
    if (componentConnected && componentConnection && controllerConnection)
      (void)componentConnection->disconnect(controllerConnection);
  }
  void restore(const Vst3StateEnvelope &state) {
    if (!state.componentState.empty()) {
      // MemoryStream is non-owning here. The backing envelope remains
      // alive for the synchronous VST3 restoration calls.
      MemoryStream stream(const_cast<uint8_t *>(state.componentState.data()),
                          static_cast<TSize>(state.componentState.size()));
      require(component->setState(&stream), "restore component state");
      MemoryStream controllerCopy(
          const_cast<uint8_t *>(state.componentState.data()),
          static_cast<TSize>(state.componentState.size()));
      require(controller->setComponentState(&controllerCopy),
              "restore controller component state");
    }
    if (!state.controllerState.empty()) {
      MemoryStream stream(const_cast<uint8_t *>(state.controllerState.data()),
                          static_cast<TSize>(state.controllerState.size()));
      require(controller->setState(&stream), "restore controller state");
    }
  }

  Vst3StateEnvelope envelope;
  VST3::Hosting::Module::Ptr module;
  HostApplication host;
  IPtr<IComponent> component;
  IPtr<IAudioProcessor> processorInterface;
  IPtr<IEditController> controller;
  FUnknownPtr<IConnectionPoint> componentConnection;
  FUnknownPtr<IConnectionPoint> controllerConnection;
  ParameterChanges changes;
  BoundedEventList inputEvents;
  std::vector<float> inputLeft;
  std::vector<float> inputRight;
  MainBuses buses;
  uint32_t maximum = 0;
  uint32_t latency = 0;
  uint32_t tail = 0;
  bool initializedComponent = false;
  bool initializedController = false;
  bool componentConnected = false;
  bool controllerConnected = false;
  bool inputActive = false;
  bool eventInputActive = false;
  bool outputActive = false;
  bool active = false;
  bool processing = false;
};
} // namespace

bool isVst3Insert(const PluginInsert &plugin) noexcept {
  return isVst3PluginInsert(plugin);
}

std::unique_ptr<PreparedEffect> prepareVst3Effect(const PluginInsert &plugin,
                                                  uint32_t sampleRate,
                                                  uint32_t maxFrames) {
  if (plugin.hostingMode == PluginHostingMode::OutOfProcess)
    return prepareVst3OutOfProcessEffect(plugin, sampleRate, maxFrames);
  return std::make_unique<Vst3Effect>(plugin, sampleRate, maxFrames);
}

std::vector<Vst3Parameter> vst3Parameters(const PluginInsert &plugin,
                                          uint32_t sampleRate,
                                          uint32_t maxFrames) {
  if (plugin.hostingMode == PluginHostingMode::OutOfProcess)
    return remoteVst3Parameters(plugin, sampleRate, maxFrames);
  auto effect = std::make_unique<Vst3Effect>(plugin, sampleRate, maxFrames);
  return effect->parameters();
}

Vst3EffectSnapshot snapshotVst3Effect(const PluginInsert &plugin,
                                      uint32_t sampleRate, uint32_t maxFrames) {
  if (plugin.hostingMode == PluginHostingMode::OutOfProcess)
    return remoteSnapshotVst3Effect(plugin, sampleRate, maxFrames);
  auto effect = std::make_unique<Vst3Effect>(plugin, sampleRate, maxFrames);
  return effect->snapshot();
}

Vst3EffectSnapshot setVst3Parameter(const PluginInsert &plugin,
                                    uint32_t parameterID, float normalizedValue,
                                    uint32_t sampleRate, uint32_t maxFrames) {
  if (plugin.hostingMode == PluginHostingMode::OutOfProcess)
    return remoteSetVst3Parameter(plugin, parameterID, normalizedValue, sampleRate, maxFrames);
  auto effect = std::make_unique<Vst3Effect>(plugin, sampleRate, maxFrames);
  effect->setParameter(parameterID, normalizedValue);
  return effect->snapshot();
}
} // namespace daw
