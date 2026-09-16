#include "audio/recording.hpp"
#include "domain/session.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace daw {
namespace {
constexpr std::array<unsigned char,8> magic{{'M','Y','D','A','W','R','E','C'}};
constexpr uint32_t headerSize=64;
constexpr uint64_t kMaxMidiNoteLength=480000;
void put32(unsigned char* p,uint32_t v){for(int i=0;i<4;++i)p[i]=static_cast<unsigned char>((v>>(i*8))&0xff);}
void put64(unsigned char* p,uint64_t v){for(int i=0;i<8;++i)p[i]=static_cast<unsigned char>((v>>(i*8))&0xff);}
uint32_t get32(const unsigned char* p){uint32_t v=0;for(int i=0;i<4;++i)v|=static_cast<uint32_t>(p[i])<<(i*8);return v;}
uint64_t get64(const unsigned char* p){uint64_t v=0;for(int i=0;i<8;++i)v|=static_cast<uint64_t>(p[i])<<(i*8);return v;}
bool writeAll(int fd,const void* data,size_t size,off_t offset){const auto* p=static_cast<const unsigned char*>(data);size_t done=0;while(done<size){auto n=pwrite(fd,p+done,size-done,offset+static_cast<off_t>(done));if(n<0&&errno==EINTR)continue;if(n<=0)return false;done+=static_cast<size_t>(n);}return true;}
bool readAll(int fd,void* data,size_t size,off_t offset){auto* p=static_cast<unsigned char*>(data);size_t done=0;while(done<size){auto n=pread(fd,p+done,size-done,offset+static_cast<off_t>(done));if(n<0&&errno==EINTR)continue;if(n<=0)return false;done+=static_cast<size_t>(n);}return true;}
void removeQuietly(const std::string& path)noexcept{if(!path.empty())unlink(path.c_str());}
}

RecordingWriter::RecordingWriter(const std::string& path,uint64_t startFrame):path_(path),startFrame_(startFrame){
    fd_=open(path.c_str(),O_CREAT|O_TRUNC|O_RDWR,0600);if(fd_<0)throw Error("Cannot create recoverable recording");
    std::array<unsigned char,headerSize> h{};std::copy(magic.begin(),magic.end(),h.begin());put32(h.data()+8,1);put32(h.data()+12,headerSize);put32(h.data()+16,48000);put32(h.data()+20,2);put64(h.data()+24,startFrame_);put64(h.data()+32,0);
    if(!writeAll(fd_,h.data(),h.size(),0)){close(fd_);fd_=-1;removeQuietly(path_);throw Error("Cannot write recoverable recording header");}
}
RecordingWriter::~RecordingWriter(){stopPreserving();}
void RecordingWriter::stopPreserving() noexcept {if(fd_>=0){close(fd_);fd_=-1;}}
void RecordingWriter::append(const float* left,const float* right,uint32_t frames) noexcept {
    if(fd_<0||failed_.load(std::memory_order_relaxed)||!left||!right)return;
    const auto old=committed_.load(std::memory_order_relaxed);if(frames>48000*60||old>48000*60-frames){failed_.store(true,std::memory_order_release);return;}
    std::vector<float> interleaved(static_cast<size_t>(frames)*2);for(uint32_t i=0;i<frames;++i){interleaved[i*2]=left[i];interleaved[i*2+1]=right[i];}
    const auto offset=static_cast<off_t>(headerSize+old*2*sizeof(float));if(!writeAll(fd_,interleaved.data(),interleaved.size()*sizeof(float),offset)){failed_.store(true,std::memory_order_release);return;}
    const auto next=old+frames;unsigned char count[8]{};put64(count,next);if(!writeAll(fd_,count,sizeof(count),32)){failed_.store(true,std::memory_order_release);return;}committed_.store(next,std::memory_order_release);
}
std::shared_ptr<const Clip> RecordingWriter::finish(){
    stopPreserving();if(failed_.load(std::memory_order_acquire))throw Error("Recording writer failed; the confirmed part remains recoverable");
    return recoverTake(path_).clip;
}
void RecordingWriter::discard() noexcept {stopPreserving();removeQuietly(path_);committed_.store(0,std::memory_order_release);}

RecoveredTake recoverTake(const std::string& path){
    int fd=open(path.c_str(),O_RDONLY);if(fd<0)throw Error("Cannot open recoverable recording");
    struct Close{int fd;~Close(){close(fd);}} closer{fd};
    std::array<unsigned char,headerSize> h{};if(!readAll(fd,h.data(),h.size(),0)||!std::equal(magic.begin(),magic.end(),h.begin())||get32(h.data()+8)!=1||get32(h.data()+12)!=headerSize||get32(h.data()+16)!=48000||get32(h.data()+20)!=2)throw Error("Invalid recoverable recording header");
    auto frames=get64(h.data()+32);struct stat info{};if(fstat(fd,&info)!=0||info.st_size<static_cast<off_t>(headerSize))throw Error("Cannot inspect recoverable recording");
    auto available=static_cast<uint64_t>(info.st_size-headerSize)/(2*sizeof(float));frames=std::min(frames,available);
    if(!frames||frames>48000*60)throw Error("Recoverable recording contains no confirmed audio");
    std::vector<float> samples(static_cast<size_t>(frames*2));if(!readAll(fd,samples.data(),samples.size()*sizeof(float),headerSize))throw Error("Recoverable recording is truncated");
    return {get64(h.data()+24),std::make_shared<const Clip>(std::move(samples))};
}
std::vector<std::shared_ptr<const Clip>> splitLoopPasses(const Clip& recording,uint64_t loopFrames){
    if(!loopFrames||loopFrames>48000*600)throw Error("Invalid loop pass length");
    std::vector<std::shared_ptr<const Clip>> passes;const auto frames=recording.frames();
    for(uint64_t begin=0;begin<frames;begin+=loopFrames){const auto count=std::min(loopFrames,frames-begin);std::vector<float> samples(count*2);std::copy_n(recording.samples().begin()+static_cast<std::ptrdiff_t>(begin*2),static_cast<std::ptrdiff_t>(count*2),samples.begin());passes.push_back(std::make_shared<const Clip>(std::move(samples)));}
    if(passes.empty())throw Error("Recording contains no audio frames");
    return passes;
}

namespace {
// A recorded length is always at least one frame and never longer than the
// domain single-note limit, so a take can only ever yield notes that pass the
// length rule of validate() on their own.
uint64_t recordedLength(uint64_t start,uint64_t end) noexcept {
    if(end<=start)return 1;
    return std::min(end-start,kMaxMidiNoteLength);
}
}

MidiRecorder::MidiRecorder(uint64_t clipStart,uint64_t clipLength):clipStart_(clipStart),clipLength_(clipLength){}
void MidiRecorder::start(uint64_t transportFrame){
    if(armed_)throw Error("MIDI recorder is already armed");
    armed_=true;startFrame_=transportFrame;recorded_.clear();open_.fill(false);openStart_.fill(0);openVelocity_.fill(0);
}
void MidiRecorder::event(uint64_t transportFrame,uint8_t channel,uint8_t pitch,uint8_t velocity,bool noteOff){
    if(!armed_||channel>15||pitch>127)return;
    const auto key=static_cast<size_t>(channel)*128+pitch;
    if(noteOff||velocity==0){
        if(!open_[key])return;
        auto start=openStart_[key];open_[key]=false;
        if(start<clipStart_||start>=clipStart_+clipLength_)return;
        auto end=std::min(transportFrame,clipStart_+clipLength_);
        recorded_.push_back({start-clipStart_,recordedLength(start,end),pitch,channel,openVelocity_[key]});
        return;
    }
    if(open_[key]){
        auto start=openStart_[key];
        if(start>=clipStart_&&start<clipStart_+clipLength_){auto end=std::min(transportFrame,clipStart_+clipLength_);recorded_.push_back({start-clipStart_,recordedLength(start,end),pitch,channel,openVelocity_[key]});}
    }
    open_[key]=true;openStart_[key]=transportFrame;openVelocity_[key]=velocity;
}
std::vector<MidiNote> MidiRecorder::stop(uint64_t transportFrame){
    if(!armed_)throw Error("MIDI recorder is not armed");
    armed_=false;
    for(size_t key=0;key<open_.size();++key){
        if(!open_[key])continue;open_[key]=false;auto start=openStart_[key];if(start<clipStart_||start>=clipStart_+clipLength_)continue;auto end=std::min(transportFrame,clipStart_+clipLength_);recorded_.push_back({start-clipStart_,recordedLength(start,end),static_cast<uint8_t>(key%128),static_cast<uint8_t>(key/128),openVelocity_[key]});
    }
    return recorded_;
}

}
