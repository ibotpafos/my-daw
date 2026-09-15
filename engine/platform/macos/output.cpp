#include "audio/output.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <cstring>
namespace daw {
namespace {
void checked(OSStatus status,const char* action) { if(status!=noErr) throw Error(std::string(action)+" (Core Audio "+std::to_string(status)+")"); }
AudioDeviceID defaultDevice() {
    AudioDeviceID id=0; UInt32 size=sizeof(id);
    AudioObjectPropertyAddress p{kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    checked(AudioObjectGetPropertyData(kAudioObjectSystemObject,&p,0,nullptr,&size,&id),"Find output device");
    if(!id) throw Error("No default output device"); return id;
}
class MacOutput final:public Output {
    AudioUnit unit=nullptr;
    std::atomic<uint32_t> device{0};
    std::atomic<uint32_t> state{static_cast<uint32_t>(OutputState::idle)};
    std::atomic<uint64_t> generation{0},callbackErrors{0};
    void teardown(OutputState reason) noexcept {
        renderer.playing.store(false);
        if(unit) { AudioOutputUnitStop(unit); AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); unit=nullptr; }
        device.store(0);state.store(static_cast<uint32_t>(reason));
    }
    static OSStatus callback(void* ref,AudioUnitRenderActionFlags*,const AudioTimeStamp*,UInt32,UInt32 frames,AudioBufferList* data) noexcept {
        auto& self=*static_cast<MacOutput*>(ref);
        if(!data){self.callbackErrors.fetch_add(1,std::memory_order_relaxed);return kAudio_ParamError;}
        if(data->mNumberBuffers!=2 || data->mBuffers[0].mNumberChannels!=1 || data->mBuffers[1].mNumberChannels!=1
            || !data->mBuffers[0].mData || !data->mBuffers[1].mData
            || frames>data->mBuffers[0].mDataByteSize/sizeof(float) || frames>data->mBuffers[1].mDataByteSize/sizeof(float)) {
            for(UInt32 i=0;i<data->mNumberBuffers;++i) if(data->mBuffers[i].mData) std::memset(data->mBuffers[i].mData,0,data->mBuffers[i].mDataByteSize);
            self.callbackErrors.fetch_add(1,std::memory_order_relaxed);
            return kAudio_ParamError;
        }
        self.renderer.render(static_cast<float*>(data->mBuffers[0].mData),static_cast<float*>(data->mBuffers[1].mData),frames); return noErr;
    }
public:
    ~MacOutput() override { stop(); }
    void stop() noexcept override { teardown(OutputState::stopped); }
    void markStalled() noexcept override { teardown(OutputState::stalled); }
    OutputTelemetry telemetry() const noexcept override { return {static_cast<OutputState>(state.load()),device.load(),generation.load(),renderer.callbacks.load(),callbackErrors.load()}; }
    void prepare(const State& state, uint64_t startFrame, uint64_t loopStart, uint64_t loopEnd) override {
        if(unit||renderer.playing.load(std::memory_order_acquire))
            throw Error("Stop audio output before preparing a new render graph");
        renderer.prepare(state,startFrame,loopStart,loopEnd);
    }
    void startPrepared() override {
        if(renderer.playing.load(std::memory_order_acquire)) throw Error("Audio output is already running");
        if(renderer.duration()==0) throw Error("Audio output has no prepared render graph");
        try {
            auto selected=defaultDevice();device.store(selected);
            AudioComponentDescription description{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};
            auto component=AudioComponentFindNext(nullptr,&description);
            if(!component) throw Error("Core Audio HAL output unavailable");
            checked(AudioComponentInstanceNew(component,&unit),"Create HAL output");
            checked(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&selected,sizeof(selected)),"Select output device");
            AudioStreamBasicDescription format{}; format.mSampleRate=48000; format.mFormatID=kAudioFormatLinearPCM;
            format.mFormatFlags=UInt32(kAudioFormatFlagsNativeFloatPacked)|UInt32(kAudioFormatFlagIsNonInterleaved);
            format.mBytesPerPacket=4; format.mFramesPerPacket=1; format.mBytesPerFrame=4; format.mChannelsPerFrame=2; format.mBitsPerChannel=32;
            checked(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&format,sizeof(format)),"Configure stereo output");
            AURenderCallbackStruct render{callback,this};
            checked(AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&render,sizeof(render)),"Install render callback");
            checked(AudioUnitInitialize(unit),"Initialize output");
            renderer.playing.store(true);
            checked(AudioOutputUnitStart(unit),"Start output");
            generation.fetch_add(1);this->state.store(static_cast<uint32_t>(OutputState::running));
        } catch(...) { stop(); throw; }
    }
    void start(const State& state, uint64_t startFrame, uint64_t loopStart, uint64_t loopEnd) override {
        prepare(state,startFrame,loopStart,loopEnd);
        startPrepared();
    }
    void checkDevice() override {
        if(!unit) return;
        if(callbackErrors.load()) { teardown(OutputState::callbackError); throw Error("Output callback received an invalid audio buffer. Press Play to retry."); }
        try {
            UInt32 alive=0,size=sizeof(alive);
            AudioObjectPropertyAddress p{kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
            auto selected=static_cast<AudioDeviceID>(device.load());
            checked(AudioObjectGetPropertyData(selected,&p,0,nullptr,&size,&alive),"Check output device");
            if(!alive || defaultDevice()!=selected) throw Error("Output device changed or disconnected. Press Play to use the current output.");
        } catch(...) { teardown(OutputState::deviceLost); throw; }
    }
};
}
std::unique_ptr<Output> makeOutput() { return std::make_unique<MacOutput>(); }
}
