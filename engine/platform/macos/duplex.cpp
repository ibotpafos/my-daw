#include "audio/duplex.hpp"
#include "platform/macos/audio_device.hpp"
#include "audio/recording.hpp"
#include "domain/session.hpp"
#include <algorithm>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace daw {
namespace {
void checkedDuplex(OSStatus status,const char* action){if(status!=noErr)throw Error(std::string(action)+" (Core Audio "+std::to_string(status)+")");}
class MacDuplex final:public Duplex {
    static constexpr UInt32 maxSlice=4096;
    State snapshot;
    AudioDeviceConfiguration configuration;
    AudioDeviceInfo openedDevice;
    uint64_t capacityFrames=0,startFrame=0,loopStart=0,loopEnd=0,prerollFrames=0;
    bool monitorOn=false;
    std::string recoveryPath;
    AudioUnit unit=nullptr;AudioDeviceID device=0;
    std::unique_ptr<RecordingWriter> capture;
    std::vector<float> inputScratch=std::vector<float>(maxSlice);
    std::atomic<uint64_t> callbackCount{0},callbackErrors{0};
    std::atomic<uint32_t> outputState{static_cast<uint32_t>(OutputState::idle)};
    std::atomic<uint64_t> generation{0};bool active=false;
    static void silence(AudioBufferList* data) noexcept {if(!data)return;for(UInt32 i=0;i<data->mNumberBuffers;++i)if(data->mBuffers[i].mData)std::memset(data->mBuffers[i].mData,0,data->mBuffers[i].mDataByteSize);}
    static OSStatus callback(void* ref,AudioUnitRenderActionFlags* flags,const AudioTimeStamp* time,UInt32,UInt32 frames,AudioBufferList* output) noexcept {
        auto& self=*static_cast<MacDuplex*>(ref);
        if(!self.unit||!output||frames>maxSlice||output->mNumberBuffers!=2||output->mBuffers[0].mNumberChannels!=1||output->mBuffers[1].mNumberChannels!=1||!output->mBuffers[0].mData||!output->mBuffers[1].mData||frames>output->mBuffers[0].mDataByteSize/sizeof(float)||frames>output->mBuffers[1].mDataByteSize/sizeof(float)){silence(output);self.callbackErrors.fetch_add(1,std::memory_order_relaxed);return kAudio_ParamError;}
        AudioBufferList input{};input.mNumberBuffers=1;input.mBuffers[0].mNumberChannels=1;input.mBuffers[0].mDataByteSize=frames*sizeof(float);input.mBuffers[0].mData=self.inputScratch.data();
        auto status=AudioUnitRender(self.unit,flags,time,1,frames,&input);if(status!=noErr){silence(output);self.callbackErrors.fetch_add(1,std::memory_order_relaxed);return status;}
        self.capture->writeMono(self.inputScratch.data(),frames);
        self.renderer.render(static_cast<float*>(output->mBuffers[0].mData),static_cast<float*>(output->mBuffers[1].mData),frames);
        if(self.monitorOn){auto* outLeft=static_cast<float*>(output->mBuffers[0].mData);auto* outRight=static_cast<float*>(output->mBuffers[1].mData);const float* in=self.inputScratch.data();for(uint32_t f=0;f<frames;++f){outLeft[f]+=in[f];outRight[f]+=in[f];}}
        self.callbackCount.fetch_add(1,std::memory_order_relaxed);return noErr;
    }
    void shutdown(OutputState reason) noexcept {active=false;renderer.playing.store(false);if(unit){AudioOutputUnitStop(unit);AudioUnitUninitialize(unit);AudioComponentInstanceDispose(unit);unit=nullptr;}device=0;outputState.store(static_cast<uint32_t>(reason));}
public:
    MacDuplex(const State& state,uint64_t capacity,std::string path,uint64_t start,uint64_t loopBegin,uint64_t loopFinish,uint64_t preroll,bool monitor,const AudioDeviceConfiguration& config):snapshot(state),configuration(config),capacityFrames(capacity),startFrame(start),loopStart(loopBegin),loopEnd(loopFinish),prerollFrames(preroll),monitorOn(monitor),recoveryPath(std::move(path)){}
    ~MacDuplex() override {cancel();}
    void start() override {
        if(active)throw Error("Recording is already active");
        try{
            openedDevice=openAudioDevice(configuration,AudioDeviceDirection::Input);
            const auto outputDevice=openAudioDevice(configuration,AudioDeviceDirection::Output);
            if(openedDevice.id!=outputDevice.id)throw Error("Loop recording requires one input/output device or a Core Audio aggregate. Select it in Audio Settings.");
            device=openedDevice.id;
            const uint64_t lead=std::min<uint64_t>(prerollFrames,startFrame); // pre-roll never precedes frame 0
            capture=std::make_unique<RecordingWriter>(recoveryPath,startFrame,capacityFrames,48000*2,lead);renderer.prepare(snapshot,startFrame-lead,loopStart,loopEnd);
            AudioComponentDescription description{kAudioUnitType_Output,kAudioUnitSubType_HALOutput,kAudioUnitManufacturer_Apple,0,0};auto component=AudioComponentFindNext(nullptr,&description);if(!component)throw Error("Core Audio HAL duplex unavailable");
            checkedDuplex(AudioComponentInstanceNew(component,&unit),"Create HAL duplex");UInt32 enabled=1;
            checkedDuplex(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Input,1,&enabled,sizeof(enabled)),"Enable audio input");
            checkedDuplex(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_EnableIO,kAudioUnitScope_Output,0,&enabled,sizeof(enabled)),"Enable audio output");
            checkedDuplex(AudioUnitSetProperty(unit,kAudioOutputUnitProperty_CurrentDevice,kAudioUnitScope_Global,0,&device,sizeof(device)),"Select duplex device");
            AudioStreamBasicDescription inputFormat{};inputFormat.mSampleRate=48000;inputFormat.mFormatID=kAudioFormatLinearPCM;inputFormat.mFormatFlags=UInt32(kAudioFormatFlagsNativeFloatPacked)|UInt32(kAudioFormatFlagIsNonInterleaved);inputFormat.mBytesPerPacket=4;inputFormat.mFramesPerPacket=1;inputFormat.mBytesPerFrame=4;inputFormat.mChannelsPerFrame=1;inputFormat.mBitsPerChannel=32;
            checkedDuplex(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,1,&inputFormat,sizeof(inputFormat)),"Configure duplex input");
            auto outputFormat=inputFormat;outputFormat.mChannelsPerFrame=2;checkedDuplex(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&outputFormat,sizeof(outputFormat)),"Configure duplex output");
            mapAudioInput(unit,configuration.inputChannel);
            mapAudioOutput(unit,openedDevice,configuration.outputLeft,configuration.outputRight);
            UInt32 maximum=maxSlice;checkedDuplex(AudioUnitSetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum)),"Limit duplex callback size");
            AURenderCallbackStruct render{callback,this};checkedDuplex(AudioUnitSetProperty(unit,kAudioUnitProperty_SetRenderCallback,kAudioUnitScope_Input,0,&render,sizeof(render)),"Install duplex callback");
            checkedDuplex(AudioUnitInitialize(unit),"Initialize duplex audio");renderer.playing.store(true);checkedDuplex(AudioOutputUnitStart(unit),"Start duplex audio");active=true;generation.fetch_add(1);outputState.store(static_cast<uint32_t>(OutputState::running));
        }catch(...){shutdown(OutputState::stopped);capture.reset();throw;}
    }
    std::shared_ptr<const Clip> stop() override {if(!active)throw Error("Recording is not active");AudioOutputUnitStop(unit);active=false;try{auto result=capture->finish();shutdown(OutputState::stopped);return result;}catch(...){shutdown(OutputState::stopped);throw;}}
    void cancel() noexcept override {shutdown(OutputState::stopped);if(capture)capture->stopPreserving();}
    void markStalled() noexcept override{shutdown(OutputState::stalled);if(capture)capture->stopPreserving();}
    void checkDevices() override {if(!active)return;if(callbackErrors.load()){shutdown(OutputState::callbackError);throw Error("Duplex callback received an invalid audio buffer");}try{checkAudioDevice(openedDevice,configuration.inputUID.empty(),AudioDeviceDirection::Input);checkAudioDevice(openedDevice,configuration.outputUID.empty(),AudioDeviceDirection::Output);}catch(...){shutdown(OutputState::deviceLost);throw;}}
    uint64_t frames() const noexcept override{return capture?capture->frames():0;}uint64_t callbacks() const noexcept override{return callbackCount.load(std::memory_order_relaxed);}bool overflowed() const noexcept override{return callbackErrors.load()||(capture&&capture->overflowed());}
    void discardRecovery() noexcept override{if(capture)capture->discard();}
    OutputTelemetry telemetry() const noexcept override{return {static_cast<OutputState>(outputState.load()),device,generation.load(),renderer.callbacks.load(),callbackErrors.load()};}
};
}
std::unique_ptr<Duplex> makeDuplex(const State& state,uint64_t capacityFrames,const std::string& recoveryPath,uint64_t startFrame,uint64_t loopStart,uint64_t loopEnd,uint64_t prerollFrames,bool monitorInput,const AudioDeviceConfiguration& config){return std::make_unique<MacDuplex>(state,capacityFrames,recoveryPath,startFrame,loopStart,loopEnd,prerollFrames,monitorInput,config);}
}
