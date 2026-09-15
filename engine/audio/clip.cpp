#include "audio/clip.hpp"
#include "domain/session.hpp"
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
namespace daw {
namespace {
uint32_t u32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
uint16_t u16(const unsigned char* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
bool tag(const unsigned char* p,const char* s) { return std::memcmp(p,s,4)==0; }
}
Clip::Clip(std::vector<float> samples): samples_(std::move(samples)) {
    if (samples_.empty() || samples_.size()%2 || samples_.size()>48000*60*2) throw Error("Audio clip must be 0–60 seconds, stereo 48 kHz");
    for (float value : samples_) if (!std::isfinite(value) || std::abs(value)>16) throw Error("Invalid audio sample");
    for(size_t bin=0;bin<peaks_.size();++bin) {
        const size_t begin=bin*frames()/peaks_.size(), end=(bin+1)*frames()/peaks_.size();
        for(size_t f=begin;f<end;++f) peaks_[bin]=std::max({peaks_[bin],std::abs(samples_[f*2]),std::abs(samples_[f*2+1])});
    }
}
std::shared_ptr<const Clip> decodeWav(std::span<const unsigned char> b) {
    if (b.size()<12 || !tag(b.data(),"RIFF") || !tag(b.data()+8,"WAVE")) throw Error("Expected a RIFF WAV file");
    size_t end=size_t(u32(b.data()+4))+8;
    if(end!=b.size()) throw Error("Truncated WAV or trailing data");
    std::span<const unsigned char> format, data;
    for(size_t p=12;p<end;) {
        if(end-p<8) throw Error("Truncated WAV chunk");
        size_t size=u32(b.data()+p+4), begin=p+8;
        if(size>end-begin) throw Error("WAV chunk exceeds file");
        if(tag(b.data()+p,"fmt ")) { if(!format.empty()) throw Error("Duplicate WAV format"); format=b.subspan(begin,size); }
        if(tag(b.data()+p,"data")) { if(!data.empty()) throw Error("Duplicate WAV audio"); data=b.subspan(begin,size); }
        p=begin+size+(size&1); if(p>end) throw Error("Missing WAV padding");
    }
    if(format.size()<16 || data.empty()) throw Error("WAV format or audio missing");
    unsigned encoding=u16(format.data()), channels=u16(format.data()+2), rate=u32(format.data()+4);
    unsigned align=u16(format.data()+12), bits=u16(format.data()+14);
    if(rate!=48000) throw Error("This prototype imports 48 kHz WAV only; convert a copy to 48 kHz");
    if(channels!=1 && channels!=2) throw Error("WAV must be mono or stereo");
    if(!((encoding==1 && (bits==16 || bits==24 || bits==32)) || (encoding==3 && bits==32))) throw Error("Supported WAV: PCM16/24/32 or float32 (no extensible/compressed WAV yet)");
    unsigned width=bits/8;
    if(align!=channels*width || u32(format.data()+8)!=rate*align || data.size()%align) throw Error("Invalid WAV frame layout");
    size_t frames=data.size()/align;
    if(!frames || frames>48000*60) throw Error("Import a WAV of at most 60 seconds");
    std::vector<float> samples(frames*2);
    for(size_t f=0;f<frames;++f) for(unsigned ch=0;ch<2;++ch) {
        auto p=data.data()+f*align+(ch%channels)*width; float value;
        if(encoding==3) value=std::bit_cast<float>(u32(p));
        else if(bits==16) value=static_cast<float>(std::bit_cast<int16_t>(u16(p)))/32768.0f;
        else if(bits==24) { int32_t v=int32_t(p[0])|int32_t(p[1])<<8|int32_t(p[2])<<16; if(v&0x800000) v-=0x1000000; value=float(v)/8388608.0f; }
        else value=static_cast<float>(std::bit_cast<int32_t>(u32(p)))/2147483648.0f;
        samples[f*2+ch]=value;
    }
    return std::make_shared<const Clip>(std::move(samples));
}
std::shared_ptr<const Clip> readWav(const std::string& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw Error("Cannot open WAV");
    auto size=file.tellg(); if(size<0 || size>32*1024*1024) throw Error("WAV exceeds 32 MiB import limit");
    std::vector<unsigned char> bytes(static_cast<size_t>(size)); file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),size)) throw Error("Cannot read complete WAV");
    return decodeWav(bytes);
}
std::vector<unsigned char> encodePCM(const Clip& clip) {
    std::vector<unsigned char> bytes(clip.samples().size()*4);
    for(size_t i=0;i<clip.samples().size();++i) { auto bits=std::bit_cast<uint32_t>(clip.samples()[i]); for(unsigned j=0;j<4;++j) bytes[i*4+j]=static_cast<unsigned char>(bits>>(8*j)); }
    return bytes;
}
std::shared_ptr<const Clip> decodePCM(std::span<const unsigned char> bytes) {
    if(bytes.empty() || bytes.size()%8 || bytes.size()>48000*60*8) throw Error("Invalid PCM blob size");
    std::vector<float> values(bytes.size()/4);
    for(size_t i=0;i<values.size();++i) values[i]=std::bit_cast<float>(u32(bytes.data()+i*4));
    return std::make_shared<const Clip>(std::move(values));
}
}
