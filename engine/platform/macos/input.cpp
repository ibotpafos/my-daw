#include "audio/input.hpp"
#include "audio/recording.hpp"
#include "domain/session.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace daw {
namespace {
void checkedInput(OSStatus status,const char* action) {
    if(status!=noErr) throw Error(std::string(action)+" (Core Audio "+std::to_string(status)+")");
}

AudioDeviceID defaultInputDevice() {
    AudioDeviceID id=0; UInt32 size=sizeof(id);
    AudioObjectPropertyAddress property{kAudioHardwarePropertyDefaultInputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    checkedInput(AudioObjectGetPropertyData(kAudioObjectSystemObject,&property,0,nullptr,&size,&id),"Find input device");
    if(!id) throw Error("No default input device");
    return id;
}

double nominalRate(AudioDeviceID device) {
    Float64 rate=0; UInt32 size=sizeof(rate);
    AudioObjectPropertyAddress property{kAudioDevicePropertyNominalSampleRate,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    checkedInput(AudioObjectGetPropertyData(device,&property,0,nullptr,&size,&rate),"Read input sample rate");
    return rate;
}

class MacInput final:public Input {
    static constexpr UInt32 maxSlice=4096;
    AudioUnit unit=nullptr;
    AudioDeviceID device=0;
    std::unique_ptr<RecordingWriter> capture;
    std::string recoveryPath;
    uint64_t capacityFrames=0,startFrame=0;
    std::vector<float> scratch=std::vector<float>(maxSlice);
    std::atomic<uint64_t> callbackCount{0};
    bool active=false;

    static OSStatus callback(void* ref,AudioUnitRenderActionFlags* flags,const AudioTimeStamp* time,UInt32,UInt32 frames,AudioBufferList*) noexcept {
        auto& self=*static_cast<MacInput*>(ref);
        if(!self.unit || frames>maxSlice) return kAudio_ParamError;
        AudioBufferList list{};
        list.mNumberBuffers=1;
        list.mBuffers[0].mNumberChannels=1;
        list.mBuffers[0].mDataByteSize=frames*sizeof(float);
        list.mBuffers[0].mData=self.scratch.data();
        auto status=AudioUnitRender(self.unit,flags,time,1,frames,&list);
        if(status==noErr) {
            self.capture->writeMono(self.scratch.data(),frames);
            self.callbackCount.fetch_add(1,std::memory_order_relaxed);
        }
        return status;
    }

    void shutdown() noexcept {
        active=false;
        if(unit) {
            AudioOutputUnitStop(unit);
            AudioUnitUninitialize(unit);
            AudioComponentInstanceDispose(unit);
            unit=nullptr;
        }
        device=0;
    }
public:
    MacInput(uint64_t capacity,std::string path,uint64_t start):recoveryPath(std::move(path)),capacityFrames(capacity),startFrame(start) {}
    ~MacInput() override { shutdown(); }
    void start() override {
        if(active) throw Error("Recording is already active");
        try {
            capture=std::make_unique<RecordingWriter>(recoveryPath,startFrame,capacityFrames);
            device=defaultInputDevice();
            if(std::abs(nominalRate(device)-48000.0)>0.5) throw Error("Set the input device to 48 kHz in Audio MIDI Setup before recording");
            AudioComponentDescription description{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};
            auto component=AudioComponentFindNext(nullptr,&description);
            if(!component) throw Error("Core Audio HAL input unavailable");
            checkedInput(AudioComponentInstanceNew(component,&unit),"Create HAL input");
            UInt32 enabled=1,disabled=0;
            checkedInput(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&enabled,sizeof(enabled)),"Enable audio input");
            checkedInput(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&disabled,sizeof(disabled)),"Disable audio output");
            checkedInput(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,sizeof(device)),"Select input device");
            AudioStreamBasicDescription format{};
            format.mSampleRate=48000; format.mFormatID=kAudioFormatLinearPCM;
            format.mFormatFlags=UInt32(kAudioFormatFlagsNativeFloatPacked)|UInt32(kAudioFormatFlagIsNonInterleaved);
            format.mBytesPerPacket=4; format.mFramesPerPacket=1; format.mBytesPerFrame=4; format.mChannelsPerFrame=1; format.mBitsPerChannel=32;
            checkedInput(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,1,&format,sizeof(format)),"Configure mono input");
            UInt32 maximum=maxSlice;
            checkedInput(AudioUnitSetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum)),"Limit input callback size");
            AURenderCallbackStruct inputCallback{callback,this};
            checkedInput(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_SetInputCallback,kAudioUnitScope_Global,0,&inputCallback,sizeof(inputCallback)),"Install input callback");
            checkedInput(AudioUnitInitialize(unit),"Initialize input");
            checkedInput(AudioOutputUnitStart(unit),"Start input");
            active=true;
        } catch(...) { shutdown(); capture.reset(); throw; }
    }
    std::shared_ptr<const Clip> stop() override {
        if(!active) throw Error("Recording is not active");
        AudioOutputUnitStop(unit);
        active=false;
        try { auto result=capture->finish(); shutdown(); return result; }
        catch(...) { shutdown(); throw; }
    }
    void cancel() noexcept override { shutdown(); if(capture)capture->stopPreserving(); }
    void checkDevice() override {
        if(!active) return;
        try {
            UInt32 alive=0,size=sizeof(alive);
            AudioObjectPropertyAddress property{kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
            checkedInput(AudioObjectGetPropertyData(device,&property,0,nullptr,&size,&alive),"Check input device");
            if(!alive || defaultInputDevice()!=device) throw Error("Input device changed or disconnected. Start a new recording with the current input.");
        } catch(...) { shutdown(); throw; }
    }
    uint64_t frames() const noexcept override { return capture?capture->frames():0; }
    uint64_t callbacks() const noexcept override { return callbackCount.load(std::memory_order_relaxed); }
    bool overflowed() const noexcept override { return capture&&capture->overflowed(); }
    void discardRecovery() noexcept override { if(capture)capture->discard(); }
};
}

std::unique_ptr<Input> makeInput(uint64_t capacityFrames,const std::string& recoveryPath,uint64_t startFrame) { return std::make_unique<MacInput>(capacityFrames,recoveryPath,startFrame); }
}
