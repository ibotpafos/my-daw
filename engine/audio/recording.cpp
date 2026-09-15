#include "audio/recording.hpp"
#include "domain/session.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace daw {
namespace {
constexpr size_t headerSize=64;
constexpr uint64_t confirmInterval=24000;
constexpr std::array<unsigned char,8> magic{'M','Y','D','A','W','T','A','K'};
void removeQuietly(const std::string& path) noexcept{std::error_code error;std::filesystem::remove(path,error);}
void put32(unsigned char* p,uint32_t v){for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(v>>(i*8));}
void put64(unsigned char* p,uint64_t v){for(unsigned i=0;i<8;++i)p[i]=static_cast<unsigned char>(v>>(i*8));}
uint32_t get32(const unsigned char* p){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(p[i])<<(i*8);return v;}
uint64_t get64(const unsigned char* p){uint64_t v=0;for(unsigned i=0;i<8;++i)v|=uint64_t(p[i])<<(i*8);return v;}
bool writeAll(int fd,const void* data,size_t size,off_t offset) noexcept {
    auto p=static_cast<const unsigned char*>(data);
    while(size){auto n=pwrite(fd,p,size,offset);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;size-=static_cast<size_t>(n);offset+=n;}
    return true;
}
bool readAll(int fd,void* data,size_t size,off_t offset) noexcept {
    auto p=static_cast<unsigned char*>(data);
    while(size){auto n=pread(fd,p,size,offset);if(n<0&&errno==EINTR)continue;if(n<=0)return false;p+=n;size-=static_cast<size_t>(n);offset+=n;}
    return true;
}
std::array<unsigned char,headerSize> header(uint64_t start,uint64_t frames) {
    std::array<unsigned char,headerSize> h{}; std::copy(magic.begin(),magic.end(),h.begin());
    put32(h.data()+8,1);put32(h.data()+12,headerSize);put32(h.data()+16,48000);put32(h.data()+20,2);put64(h.data()+24,start);put64(h.data()+32,frames);return h;
}
void confirm(int fd,uint64_t start,uint64_t frames) {
    if(fsync(fd)!=0) throw Error("Cannot sync recording audio");
    auto h=header(start,frames); if(!writeAll(fd,h.data(),h.size(),0)) throw Error("Cannot update recording checkpoint");
    if(fsync(fd)!=0) throw Error("Cannot sync recording checkpoint");
}
}

RecordingWriter::RecordingWriter(std::string path,uint64_t startFrame,uint64_t capacityFrames,uint64_t ringFrames)
    :path_(std::move(path)),startFrame_(startFrame),capacityFrames_(capacityFrames) {
    if(path_.empty()||!capacityFrames_||capacityFrames_>48000*60||!ringFrames) throw Error("Invalid recording writer configuration");
    ringFrames=std::min(ringFrames,capacityFrames_); ring_.resize(static_cast<size_t>(ringFrames));
    fd_=open(path_.c_str(),O_CREAT|O_EXCL|O_RDWR,0600); if(fd_<0) throw Error("Cannot create recoverable recording");
    auto h=header(startFrame_,0); if(!writeAll(fd_,h.data(),h.size(),0)||fsync(fd_)!=0){closeFile();removeQuietly(path_);throw Error("Cannot initialize recoverable recording");}
    try{worker_=std::thread([this]{run();});}catch(...){closeFile();removeQuietly(path_);throw;}
}

RecordingWriter::~RecordingWriter(){stopPreserving();if(!committedFrames())removeQuietly(path_);}

void RecordingWriter::writeMono(const float* input,uint32_t count) noexcept {
    if(!input||!count||stopping_.load(std::memory_order_relaxed)||failed_.load(std::memory_order_relaxed)||overflow_.load(std::memory_order_relaxed))return;
    auto write=written_.load(std::memory_order_relaxed),read=read_.load(std::memory_order_acquire);
    auto remaining=capacityFrames_-std::min(accepted_.load(std::memory_order_relaxed),capacityFrames_);
    auto free=ring_.size()-std::min<uint64_t>(write-read,ring_.size());
    auto accepted=std::min<uint64_t>({count,remaining,free});
    for(uint64_t i=0;i<accepted;++i){auto value=std::isfinite(input[i])?std::clamp(input[i],-16.0f,16.0f):0.0f;ring_[static_cast<size_t>((write+i)%ring_.size())]=value;}
    accepted_.fetch_add(accepted,std::memory_order_relaxed);written_.store(write+accepted,std::memory_order_release);
    if(accepted<count)overflow_.store(true,std::memory_order_release);
}

void RecordingWriter::run() noexcept {
    try{
        std::vector<float> stereo(std::min<uint64_t>(4096,ring_.size())*2); uint64_t lastConfirmed=0;
        for(;;){
            auto read=read_.load(std::memory_order_relaxed),write=written_.load(std::memory_order_acquire);
            if(read==write){if(stopping_.load(std::memory_order_acquire))break;std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            auto count=std::min<uint64_t>({write-read,ring_.size()-read%ring_.size(),stereo.size()/2});
            for(uint64_t i=0;i<count;++i){auto value=ring_[static_cast<size_t>((read+i)%ring_.size())];stereo[static_cast<size_t>(i*2)]=value;stereo[static_cast<size_t>(i*2+1)]=value;}
            auto offset=static_cast<off_t>(headerSize+read*2*sizeof(float));
            if(!writeAll(fd_,stereo.data(),static_cast<size_t>(count*2*sizeof(float)),offset))throw Error("Cannot write recording audio");
            read+=count;read_.store(read,std::memory_order_release);
            if(read-lastConfirmed>=confirmInterval){confirm(fd_,startFrame_,read);committed_.store(read,std::memory_order_release);lastConfirmed=read;}
        }
        auto frames=read_.load(std::memory_order_acquire);if(frames>lastConfirmed){confirm(fd_,startFrame_,frames);committed_.store(frames,std::memory_order_release);}
    }catch(...){failed_.store(true,std::memory_order_release);overflow_.store(true,std::memory_order_release);}
}

void RecordingWriter::closeFile() noexcept {if(fd_>=0){close(fd_);fd_=-1;}}
void RecordingWriter::stopPreserving() noexcept {stopping_.store(true,std::memory_order_release);if(worker_.joinable())worker_.join();closeFile();}

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
    if(passes.empty())throw Error("Recording contains no audio frames");return passes;
}
}
