// Native HAL adapter contract test. Only C HAL calls are supplied by this
// executable; MacDuplex, device/stream readers, routing, capture and writer are
// the production implementation. This executable never opens real hardware.
#include "audio/duplex.hpp"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace {
AudioDeviceIOProc callback = nullptr;
void* context = nullptr;
int creates = 0, starts = 0, stops = 0, destroys = 0;
bool running = false, badLatency = false, formatBad = false, separateOutputLatency = false;
size_t checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw daw::Error(message); }
struct Buffers { UInt32 count = 2; AudioBuffer buffers[2]{}; };
static_assert(offsetof(Buffers,buffers) == offsetof(AudioBufferList,mBuffers));
template<class T> OSStatus store(UInt32* size, void* data, const T& value) {
    if (!size || !data || *size < sizeof(T)) return kAudioHardwareBadPropertySizeError;
    std::memcpy(data,&value,sizeof(value)); *size = sizeof(value); return noErr;
}
}
extern "C" OSStatus AudioObjectGetPropertyDataSize(AudioObjectID object, const AudioObjectPropertyAddress* p,
                                                   UInt32, const void*, UInt32* size) {
    if (!p || !size) return kAudioHardwareIllegalOperationError;
    if (object == kAudioObjectSystemObject && p->mSelector == kAudioHardwarePropertyDevices) *size = sizeof(AudioDeviceID);
    else if (object == 77 && p->mSelector == kAudioDevicePropertyStreamConfiguration) *size = sizeof(Buffers);
    else if (object == 77 && p->mSelector == kAudioDevicePropertyStreams) *size = sizeof(AudioStreamID) * 2;
    else return kAudioHardwareUnknownPropertyError;
    return noErr;
}
extern "C" OSStatus AudioObjectGetPropertyData(AudioObjectID object, const AudioObjectPropertyAddress* p,
                                               UInt32, const void*, UInt32* size, void* data) {
    if (!p) return kAudioHardwareIllegalOperationError;
    const auto selector = p->mSelector;
    if (object == kAudioObjectSystemObject) {
        if (selector == kAudioHardwarePropertyDevices || selector == kAudioHardwarePropertyDefaultInputDevice ||
            selector == kAudioHardwarePropertyDefaultOutputDevice) return store(size,data,AudioDeviceID(77));
    }
    if (object == 77) {
        if (selector == kAudioDevicePropertyDeviceIsAlive) return store(size,data,UInt32(1));
        if (selector == kAudioDevicePropertyDeviceUID || selector == kAudioObjectPropertyName) {
            CFStringRef text = CFStringCreateCopy(kCFAllocatorDefault, CFSTR("native-recording-fixture"));
            return store(size,data,text);
        }
        if (selector == kAudioDevicePropertyNominalSampleRate) return store(size,data,Float64(48000));
        if (selector == kAudioDevicePropertyBufferFrameSize) return store(size,data,UInt32(256));
        if (selector == kAudioDevicePropertyStreamConfiguration) {
            Buffers value; value.buffers[0].mNumberChannels = 2; value.buffers[1].mNumberChannels = 2;
            return store(size,data,value);
        }
        const bool input = p->mScope == kAudioDevicePropertyScopeInput;
        if (selector == kAudioDevicePropertyLatency) {
            if (badLatency) return kAudioHardwareUnknownPropertyError;
            return store(size,data,UInt32(input ? 17 : 43));
        }
        if (selector == kAudioDevicePropertySafetyOffset) return store(size,data,UInt32(input ? 13 : 29));
        if (selector == kAudioDevicePropertyStreams) {
            const std::array<AudioStreamID,2> value = input ? std::array<AudioStreamID,2>{101,102} : std::array<AudioStreamID,2>{201,202};
            return store(size,data,value);
        }
    }
    if (object == 101 || object == 102 || object == 201 || object == 202) {
        if (selector == kAudioStreamPropertyStartingChannel) return store(size,data,UInt32(object % 100 == 1 ? 1 : 3));
        if (selector == kAudioStreamPropertyLatency) return store(size,data,UInt32(object < 200 ? 3 :
            (separateOutputLatency && object == 201 ? 9 : 5)));
        if (selector == kAudioStreamPropertyVirtualFormat) {
            AudioStreamBasicDescription format{};
            format.mSampleRate = 48000; format.mFormatID = kAudioFormatLinearPCM;
            format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
            format.mBytesPerPacket = format.mBytesPerFrame = 8; format.mFramesPerPacket = 1;
            format.mChannelsPerFrame = 2; format.mBitsPerChannel = formatBad ? 16 : 32;
            return store(size,data,format);
        }
    }
    return kAudioHardwareUnknownPropertyError;
}
extern "C" OSStatus AudioDeviceCreateIOProcID(AudioObjectID device, AudioDeviceIOProc proc, void* client, AudioDeviceIOProcID* out) {
    ++creates;
    if (device != 77 || callback || !out) return kAudioHardwareIllegalOperationError;
    callback = proc; context = client; *out = proc; return noErr;
}
extern "C" OSStatus AudioDeviceStart(AudioObjectID device, AudioDeviceIOProcID proc) {
    ++starts; if (device != 77 || proc != callback) return kAudioHardwareIllegalOperationError;
    running = true; return noErr;
}
extern "C" OSStatus AudioDeviceStop(AudioObjectID, AudioDeviceIOProcID) { ++stops; running = false; return noErr; }
extern "C" OSStatus AudioDeviceDestroyIOProcID(AudioObjectID, AudioDeviceIOProcID) {
    ++destroys; callback = nullptr; context = nullptr; return noErr;
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("recording-native-hal-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        daw::AudioDeviceConfiguration config; config.inputUID = config.outputUID = "native-recording-fixture";
        config.inputChannel = 3; config.outputLeft = 3; config.outputRight = 2;
        daw::Session model;
        std::vector<float> source(4096*2);
        for (size_t f=0;f<4096;++f) { source[2*f]=0.125f;source[2*f+1]=-0.125f; }
        model.import("Bed",std::make_shared<daw::Clip>(source),0);
        for (int fault=0;fault<3;++fault) {
            badLatency=fault==0;formatBad=fault==1;separateOutputLatency=fault==2;
            auto wrong=config;if(fault==2)wrong.outputLeft=0;
            auto io=daw::makeDuplex(model.state(),512,(root/"invalid.mydawtake").string(),0,0,0,0,false,wrong);
            bool rejected=false;try{io->start();}catch(const daw::Error&){rejected=true;}
            check(rejected && !running && !callback && starts==0,"invalid native profile rejects before audio start");
        }
        badLatency=formatBad=separateOutputLatency=false;
        const auto path=(root/"native.mydawtake").string();
        auto io=daw::makeDuplex(model.state(),512,path,0,0,0,0,false,config);io->start();
        check(running&&creates==1&&starts==1,"actual MacDuplex registered simulated HAL callback");
        check(!io->timing().ready,"paired latency not fabricated at startup");
        const auto h=io->timing().hardware;
        check(h.inputStreamID==102&&h.outputLeftStreamID==202&&h.inputStream==3&&h.outputStream==5,"selected streams, not first device stream");
        std::array<float,512> in0{},in1{},out0{},out1{};
        Buffers input,output;
        input.buffers[0]={2,sizeof(in0),in0.data()};input.buffers[1]={2,sizeof(in1),in1.data()};
        output.buffers[0]={2,sizeof(out0),out0.data()};output.buffers[1]={2,sizeof(out1),out1.data()};
        for(size_t f=0;f<256;++f){in0[2*f]=in0[2*f+1]=0.9f;in1[2*f]=0.8f;in1[2*f+1]=0.25f;}
        mach_timebase_info_data_t timebase{};check(mach_timebase_info(&timebase)==KERN_SUCCESS,"host frequency");
        const auto ticks=1e9*double(timebase.denom)/timebase.numer/48000;
        for(uint64_t frame=0;frame<1024;frame+=256){
            std::fill(out0.begin(),out0.end(),99);std::fill(out1.begin(),out1.end(),99);
            AudioTimeStamp inTime{},outTime{},now{};
            inTime.mFlags=outTime.mFlags=kAudioTimeStampSampleTimeValid|kAudioTimeStampHostTimeValid;
            inTime.mSampleTime=double(frame);outTime.mSampleTime=double(frame+256);
            inTime.mHostTime=100000000+uint64_t(double(frame)*ticks);
            outTime.mHostTime=100000000+uint64_t(double(frame+256)*ticks);
            check(callback(77,&now,reinterpret_cast<AudioBufferList*>(&input),&inTime,
                reinterpret_cast<AudioBufferList*>(&output),&outTime,context)==noErr,"HAL callback returns noErr");
            check(std::all_of(out0.begin(),out0.end(),[](float v){return v==0;}),"unselected output stream stays silent");
            if(frame==0)check(out1[511]>0&&out1[510]<0,"actual non-default swapped L/R channel routing");
            io->checkDevices();
        }
        check(io->progress().complete&&io->frames()==512&&io->timing().compensationFrames==324,"native paired-stamp compensation and drained cap");
        const auto clip=io->stop();
        check(!running&&!callback&&stops==1&&destroys==1,"native registration quiesced/destroyed");
        check(std::all_of(clip->samples().begin(),clip->samples().end(),[](float v){return v==0.25f;}),"only selected dry input reaches file");
        io->discardRecovery();io.reset();
        // A changed report is not silently adopted halfway through a take.
        auto changed=daw::makeDuplex(model.state(),512,(root/"change.mydawtake").string(),0,0,0,0,false,config);
        changed->start();badLatency=true;
        bool rejected=false;try{changed->checkDevices();}catch(const daw::Error&){rejected=true;}
        check(rejected&&!running&&!callback,"native latency read failure stops I/O");badLatency=false;
        changed.reset();
        std::filesystem::remove_all(root);
        std::cout<<"recording native HAL: "<<checks<<" assertions passed (HAL C calls simulated, native adapter actual)\n";
    } catch(const std::exception& e){std::filesystem::remove_all(root);std::cerr<<e.what()<<'\n';return 1;}
}
