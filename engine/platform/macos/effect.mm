#include "audio/effect.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
namespace daw {
namespace {
constexpr uint32_t fourcc(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
         (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}
void checked(OSStatus status, const char *operation) {
  if (status != noErr)
    throw Error(std::string(operation) + " failed (OSStatus " +
                std::to_string(status) + ")");
}
AudioStreamBasicDescription format(uint32_t sampleRate) {
  AudioStreamBasicDescription value{};
  value.mSampleRate = sampleRate;
  value.mFormatID = kAudioFormatLinearPCM;
  value.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked |
                       kAudioFormatFlagIsNonInterleaved |
                       kAudioFormatFlagsNativeEndian;
  value.mBytesPerPacket = 4;
  value.mFramesPerPacket = 1;
  value.mBytesPerFrame = 4;
  value.mChannelsPerFrame = 2;
  value.mBitsPerChannel = 32;
  return value;
}
AudioComponent find(const AudioUnitDescriptor &descriptor) {
  AudioComponentDescription value{descriptor.type, descriptor.subtype,
                                  descriptor.manufacturer, 0, 0};
  auto component = AudioComponentFindNext(nullptr, &value);
  if (!component)
    throw Error("Audio Unit is unavailable");
  return component;
}
std::string componentName(AudioComponent component) {
  CFStringRef value = nullptr;
  checked(AudioComponentCopyName(component, &value), "Read Audio Unit name");
  if (!value)
    return "Audio Unit";
  std::array<char, 481> bytes{};
  const auto ok = CFStringGetCString(value, bytes.data(), bytes.size(),
                                     kCFStringEncodingUTF8);
  CFRelease(value);
  if (!ok)
    throw Error("Audio Unit name is too long");
  return bytes.data();
}
struct AsyncInstantiationState {
  std::mutex mutex;
  std::condition_variable completed;
  AudioComponentInstance instance = nullptr;
  OSStatus status = noErr;
  bool finished = false;
  bool abandoned = false;
};

AudioUnit instantiateOutOfProcess(AudioComponent component) {
  if (pthread_main_np() != 0)
    throw Error("Out-of-process Audio Unit preparation cannot wait on the main thread");

  AudioComponentDescription description{};
  checked(AudioComponentGetDescription(component, &description),
          "Read Audio Unit component description");
  if ((description.componentFlags & kAudioComponentFlag_IsV3AudioUnit) == 0)
    throw Error("Out-of-process hosting is available only for AUv3 Audio Units");

  auto state = std::make_shared<AsyncInstantiationState>();
  AudioComponentInstantiate(
      component, kAudioComponentInstantiation_LoadOutOfProcess,
      ^(AudioComponentInstance instance, OSStatus status) {
        AudioComponentInstance stale = nullptr;
        {
          std::lock_guard lock(state->mutex);
          if (state->abandoned) {
            stale = instance;
          } else {
            state->instance = instance;
            state->status = status;
            state->finished = true;
          }
        }
        if (stale)
          AudioComponentInstanceDispose(stale);
        state->completed.notify_one();
      });

  std::unique_lock lock(state->mutex);
  constexpr auto timeout = std::chrono::seconds(10);
  if (!state->completed.wait_for(lock, timeout,
                                 [&] { return state->finished; })) {
    state->abandoned = true;
    throw Error("Out-of-process Audio Unit instantiation timed out");
  }
  if (state->status != noErr) {
    const auto failed = state->instance;
    lock.unlock();
    if (failed)
      AudioComponentInstanceDispose(failed);
    checked(state->status, "Instantiate out-of-process Audio Unit");
  }
  if (!state->instance)
    throw Error("Out-of-process Audio Unit returned no instance");
  auto instance = state->instance;
  state->instance = nullptr;
  lock.unlock();
  UInt32 loadedOutOfProcess = 0;
  UInt32 propertySize = sizeof(loadedOutOfProcess);
  const auto propertyStatus = AudioUnitGetProperty(
      instance, kAudioUnitProperty_LoadedOutOfProcess,
      kAudioUnitScope_Global, 0, &loadedOutOfProcess, &propertySize);
  if (propertyStatus != noErr || propertySize != sizeof(loadedOutOfProcess) ||
      loadedOutOfProcess == 0) {
    AudioComponentInstanceDispose(instance);
    throw Error("Audio Unit did not honor the required out-of-process hosting policy");
  }
  return instance;
}

struct Unit {
  AudioUnit value = nullptr;
  bool initialized = false;
  explicit Unit(AudioComponent component,
                PluginHostingMode hostingMode = PluginHostingMode::InProcess) {
    switch (hostingMode) {
    case PluginHostingMode::InProcess:
      checked(AudioComponentInstanceNew(component, &value),
              "Instantiate Audio Unit");
      break;
    case PluginHostingMode::OutOfProcess:
      value = instantiateOutOfProcess(component);
      break;
    default:
      throw Error("Invalid Audio Unit hosting mode");
    }
  }
  ~Unit() {
    if (value) {
      if (initialized)
        AudioUnitUninitialize(value);
      AudioComponentInstanceDispose(value);
    }
  }
};
void configure(Unit &unit, uint32_t sampleRate, uint32_t maxFrames,
               const std::vector<uint8_t> &state) {
  auto stream = format(sampleRate);
  checked(AudioUnitSetProperty(unit.value, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Input, 0, &stream,
                               sizeof(stream)),
          "Configure Audio Unit input");
  checked(AudioUnitSetProperty(unit.value, kAudioUnitProperty_StreamFormat,
                               kAudioUnitScope_Output, 0, &stream,
                               sizeof(stream)),
          "Configure Audio Unit output");
  UInt32 maximum = maxFrames;
  checked(AudioUnitSetProperty(
              unit.value, kAudioUnitProperty_MaximumFramesPerSlice,
              kAudioUnitScope_Global, 0, &maximum, sizeof(maximum)),
          "Configure Audio Unit block limit");
  if (!state.empty()) {
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, state.data(),
                                  static_cast<CFIndex>(state.size()));
    if (!data)
      throw Error("Create Audio Unit state data failed");
    CFErrorRef error = nullptr;
    auto property = CFPropertyListCreateWithData(
        kCFAllocatorDefault, data, kCFPropertyListImmutable, nullptr, &error);
    CFRelease(data);
    if (!property) {
      if (error)
        CFRelease(error);
      throw Error("Decode Audio Unit state failed");
    }
    checked(AudioUnitSetProperty(unit.value, kAudioUnitProperty_ClassInfo,
                                 kAudioUnitScope_Global, 0, &property,
                                 sizeof(property)),
            "Restore Audio Unit state");
    CFRelease(property);
  }
}
AudioUnitSnapshot capture(Unit &unit, const std::string &name,
                          uint32_t sampleRate) {
  Float64 seconds = 0;
  UInt32 size = sizeof(seconds);
  checked(AudioUnitGetProperty(unit.value, kAudioUnitProperty_Latency,
                               kAudioUnitScope_Global, 0, &seconds, &size),
          "Read Audio Unit latency");
  if (!std::isfinite(seconds) || seconds < 0 || seconds > 10)
    throw Error("Invalid Audio Unit latency");
  CFPropertyListRef property = nullptr;
  size = sizeof(property);
  checked(AudioUnitGetProperty(unit.value, kAudioUnitProperty_ClassInfo,
                               kAudioUnitScope_Global, 0, &property, &size),
          "Capture Audio Unit state");
  if (!property)
    throw Error("Audio Unit returned no state");
  CFErrorRef error = nullptr;
  auto data =
      CFPropertyListCreateData(kCFAllocatorDefault, property,
                               kCFPropertyListBinaryFormat_v1_0, 0, &error);
  CFRelease(property);
  if (!data) {
    if (error)
      CFRelease(error);
    throw Error("Encode Audio Unit state failed");
  }
  const auto length = CFDataGetLength(data);
  if (length < 0 || length > 1024 * 1024) {
    CFRelease(data);
    throw Error("Audio Unit state exceeds 1 MiB");
  }
  std::vector<uint8_t> state(static_cast<size_t>(length));
  if (length)
    std::memcpy(state.data(), CFDataGetBytePtr(data),
                static_cast<size_t>(length));
  CFRelease(data);
  return {name, static_cast<uint32_t>(std::llround(seconds * sampleRate)),
          std::move(state)};
}
class AppleEffect final : public PreparedEffect {
  struct ParameterRange {
    AudioUnitParameterID id = 0;
    float minimum = 0, maximum = 1;
  };
  Unit unit;
  uint32_t maximum, currentFrames = 0, latency = 0;
  std::vector<float> inputLeft, inputRight;
  std::vector<ParameterRange> parameters;
  std::vector<AudioUnitParameterEvent> scheduled;
  struct StereoList {
    UInt32 count = 2;
    AudioBuffer buffers[2]{};
  };
  static OSStatus input(void *context, AudioUnitRenderActionFlags *,
                        const AudioTimeStamp *, UInt32, UInt32 frames,
                        AudioBufferList *data) noexcept {
    auto &self = *static_cast<AppleEffect *>(context);
    if (!data || frames > self.currentFrames || data->mNumberBuffers != 2)
      return kAudio_ParamError;
    const float *sources[2]{self.inputLeft.data(), self.inputRight.data()};
    for (UInt32 channel = 0; channel < 2; ++channel) {
      auto &buffer = data->mBuffers[channel];
      buffer.mNumberChannels = 1;
      buffer.mDataByteSize = frames * sizeof(float);
      if (buffer.mData)
        std::memcpy(buffer.mData, sources[channel], buffer.mDataByteSize);
      else
        buffer.mData = const_cast<float *>(sources[channel]);
    }
    return noErr;
  }

public:
  AppleEffect(const PluginInsert &plugin, uint32_t sampleRate,
              uint32_t maxFrames)
      : unit(find(
            {plugin.type, plugin.subtype, plugin.manufacturer, plugin.name}),
             plugin.hostingMode),
        maximum(maxFrames), inputLeft(maxFrames), inputRight(maxFrames) {
    configure(unit, sampleRate, maxFrames, plugin.state);
    AURenderCallbackStruct callback{input, this};
    checked(AudioUnitSetProperty(
                unit.value, kAudioUnitProperty_SetRenderCallback,
                kAudioUnitScope_Input, 0, &callback, sizeof(callback)),
            "Install Audio Unit input callback");
    checked(AudioUnitInitialize(unit.value), "Initialize Audio Unit");
    unit.initialized = true;
    Float64 seconds = 0;
    UInt32 size = sizeof(seconds);
    checked(AudioUnitGetProperty(unit.value, kAudioUnitProperty_Latency,
                                 kAudioUnitScope_Global, 0, &seconds, &size),
            "Read initialized Audio Unit latency");
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 10)
      throw Error("Invalid initialized Audio Unit latency");
    latency = static_cast<uint32_t>(std::llround(seconds * sampleRate));
    UInt32 listSize = 0;
    const auto listStatus =
        AudioUnitGetPropertyInfo(unit.value, kAudioUnitProperty_ParameterList,
                                 kAudioUnitScope_Global, 0, &listSize, nullptr);
    if (listStatus != kAudioUnitErr_InvalidProperty) {
      checked(listStatus, "Read Audio Unit parameter list size");
      if (listSize % sizeof(AudioUnitParameterID) ||
          listSize > 65536U * sizeof(AudioUnitParameterID))
        throw Error("Invalid Audio Unit parameter list");
      std::vector<AudioUnitParameterID> ids(listSize /
                                            sizeof(AudioUnitParameterID));
      if (!ids.empty())
        checked(AudioUnitGetProperty(
                    unit.value, kAudioUnitProperty_ParameterList,
                    kAudioUnitScope_Global, 0, ids.data(), &listSize),
                "Read Audio Unit parameter list");
      for (const auto id : ids) {
        AudioUnitParameterInfo info{};
        UInt32 infoSize = sizeof(info);
        if (AudioUnitGetProperty(unit.value, kAudioUnitProperty_ParameterInfo,
                                 kAudioUnitScope_Global, id, &info,
                                 &infoSize) == noErr &&
            std::isfinite(info.minValue) && std::isfinite(info.maxValue) &&
            info.maxValue >= info.minValue)
          parameters.push_back({id, info.minValue, info.maxValue});
      }
      std::sort(parameters.begin(), parameters.end(),
                [](const auto &a, const auto &b) { return a.id < b.id; });
    }
    size_t capacity = 0;
    for (const auto &lane : plugin.parameterAutomation)
      capacity += lane.points.size() + 2;
    scheduled.resize(capacity);
  }
  bool process(float *left, float *right, uint32_t frames, uint64_t sampleTime,
               std::span<const PreparedParameterEvent> parameterEvents,
               std::span<const PreparedMidiEvent> midiEvents) noexcept
      override {
    // AU instruments: separate MusicDevice arc. This catalog only contains
    // AudioUnit effect types, so a caller that hands notes to an AU effect is
    // a programming error; reject it instead of silently dropping the lane.
    if (!midiEvents.empty())
      return false;
    if (!left || !right || frames > maximum)
      return false;
    std::copy_n(left, frames, inputLeft.begin());
    std::copy_n(right, frames, inputRight.begin());
    currentFrames = frames;
    size_t scheduledCount = 0;
    for (size_t index = 0; index < parameterEvents.size(); ++index) {
      const auto &event = parameterEvents[index];
      if (event.sampleOffset >= frames ||
          !std::isfinite(event.normalizedValue) || event.normalizedValue < 0 ||
          event.normalizedValue > 1 || scheduledCount >= scheduled.size()) {
        std::copy_n(inputLeft.begin(), frames, left);
        std::copy_n(inputRight.begin(), frames, right);
        return false;
      }
      const auto found = std::lower_bound(
          parameters.begin(), parameters.end(), event.parameterID,
          [](const auto &range, uint32_t id) { return range.id < id; });
      if (found == parameters.end() || found->id != event.parameterID)
        continue;
      const auto native = [&](float normalized) {
        return found->minimum + (found->maximum - found->minimum) * normalized;
      };
      auto &scheduledEvent = scheduled[scheduledCount++];
      scheduledEvent.scope = kAudioUnitScope_Global;
      scheduledEvent.element = 0;
      scheduledEvent.parameter = found->id;
      const bool ramp =
          index + 1 < parameterEvents.size() &&
          parameterEvents[index + 1].parameterID == event.parameterID &&
          parameterEvents[index + 1].sampleOffset > event.sampleOffset;
      if (ramp) {
        const auto &next = parameterEvents[index + 1];
        scheduledEvent.eventType = kParameterEvent_Ramped;
        scheduledEvent.eventValues.ramp.startBufferOffset =
            static_cast<SInt32>(event.sampleOffset);
        scheduledEvent.eventValues.ramp.durationInFrames =
            next.sampleOffset - event.sampleOffset;
        scheduledEvent.eventValues.ramp.startValue =
            native(event.normalizedValue);
        scheduledEvent.eventValues.ramp.endValue = native(next.normalizedValue);
      } else {
        scheduledEvent.eventType = kParameterEvent_Immediate;
        scheduledEvent.eventValues.immediate.bufferOffset = event.sampleOffset;
        scheduledEvent.eventValues.immediate.value =
            native(event.normalizedValue);
      }
    }
    if (scheduledCount && AudioUnitScheduleParameters(
                              unit.value, scheduled.data(),
                              static_cast<UInt32>(scheduledCount)) != noErr) {
      std::copy_n(inputLeft.begin(), frames, left);
      std::copy_n(inputRight.begin(), frames, right);
      return false;
    }
    StereoList output;
    output.buffers[0] = {1, static_cast<UInt32>(frames * sizeof(float)), left};
    output.buffers[1] = {1, static_cast<UInt32>(frames * sizeof(float)), right};
    AudioTimeStamp time{};
    time.mFlags = kAudioTimeStampSampleTimeValid;
    time.mSampleTime = static_cast<Float64>(sampleTime);
    AudioUnitRenderActionFlags flags = 0;
    const auto status =
        AudioUnitRender(unit.value, &flags, &time, 0, frames,
                        reinterpret_cast<AudioBufferList *>(&output));
    if (status == noErr)
      return true;
    std::copy_n(inputLeft.begin(), frames, left);
    std::copy_n(inputRight.begin(), frames, right);
    return false;
  }
  uint32_t latencyFrames() const noexcept override { return latency; }
};
} // namespace
std::vector<AudioUnitDescriptor> supportedAudioUnits() {
  const std::array<AudioUnitDescriptor, 3> approved{
      {{kAudioUnitType_Effect, fourcc('d', 'c', 'm', 'p'),
        kAudioUnitManufacturer_Apple, "Apple: AUDynamicsProcessor"},
       {kAudioUnitType_Effect, fourcc('l', 'm', 't', 'r'),
        kAudioUnitManufacturer_Apple, "Apple: AUPeakLimiter"},
       {kAudioUnitType_Effect, fourcc('m', 'r', 'e', 'v'),
        kAudioUnitManufacturer_Apple, "Apple: AUMatrixReverb"}}};
  std::vector<AudioUnitDescriptor> result;
  for (auto item : approved) {
    try {
      auto component = find(item);
      item.name = componentName(component);
      result.push_back(std::move(item));
    } catch (const std::exception &) {
    }
  }
  return result;
}
bool audioUnitAvailable(const AudioUnitDescriptor &descriptor) noexcept {
  try {
    return find(descriptor) != nullptr;
  } catch (...) {
    return false;
  }
}
AudioUnitSnapshot snapshotAudioUnit(const AudioUnitDescriptor &descriptor,
                                    uint32_t sampleRate, uint32_t maxFrames) {
  auto component = find(descriptor);
  Unit unit(component);
  configure(unit, sampleRate, maxFrames, {});
  checked(AudioUnitInitialize(unit.value), "Initialize Audio Unit");
  unit.initialized = true;
  return capture(unit, componentName(component), sampleRate);
}
std::unique_ptr<PreparedEffect> prepareAudioUnit(const PluginInsert &plugin,
                                                 uint32_t sampleRate,
                                                 uint32_t maxFrames) {
  return std::make_unique<AppleEffect>(plugin, sampleRate, maxFrames);
}
} // namespace daw
