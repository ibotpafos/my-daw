#include "audio/clip.hpp"
#include "audio/clip_resampler.hpp"
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
bool supportedRate(uint32_t rate) {
    return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000 || rate == 192000;
}
uint16_t be16(const unsigned char* p) { return uint16_t(p[0]) << 8 | uint16_t(p[1]); }
uint32_t be32(const unsigned char* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
// AIFF stores the sample rate as an 80-bit IEEE extended float (big-endian).
double beExtended80(const unsigned char* p) {
    const int sign = (p[0] & 0x80) ? -1 : 1;
    const int exponent = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mantissa = 0;
    for (int i = 2; i < 10; ++i) mantissa = (mantissa << 8) | p[i];
    if (exponent == 0 && mantissa == 0) return 0.0;
    return sign * std::ldexp(static_cast<double>(mantissa) / 9223372036854775808.0, exponent - 16383);
}
int32_t beInt(const unsigned char* p, unsigned width) {
    uint32_t u = 0;
    for (unsigned i = 0; i < width; ++i) u = (u << 8) | p[i];
    unsigned shift = (4 - width) * 8; // sign-extend a 1..4 byte big-endian integer
    return static_cast<int32_t>(u << shift) >> shift;
}
}
void checkImportCanceled(const ImportControl& control) {
    if (control.cancel && control.cancel->load(std::memory_order_acquire)) throw ImportCanceled{};
}
void updateImportProgress(const ImportControl& control, uint64_t completed, uint64_t total) {
    if (!control.progress) return;
    if (total == 0) { control.progress->store(control.progressEnd, std::memory_order_release); return; }
    const uint64_t span = control.progressEnd - control.progressBegin;
    const uint64_t bounded = std::min(completed, total);
    control.progress->store(static_cast<uint32_t>(control.progressBegin + bounded * span / total),
                            std::memory_order_release);
}
void updateImportPhase(const ImportControl& control, uint8_t phase) {
    if (control.setPhase) control.setPhase(control.phaseContext, phase);
}
Clip::Clip(std::vector<float> samples, const ImportControl& control): samples_(std::move(samples)) {
    if (samples_.empty() || samples_.size()%2 || samples_.size()>48000*60*2) throw Error("Audio clip must be 0–60 seconds, stereo 48 kHz");
    for (size_t index=0;index<samples_.size();++index) {
        if ((index&0x3fffU)==0) checkImportCanceled(control);
        const float value=samples_[index]; if (!std::isfinite(value) || std::abs(value)>16) throw Error("Invalid audio sample");
    }
    const auto buildPeaks = [this, &control](auto& cache) {
        for(size_t bin=0;bin<cache.size();++bin) {
            const size_t begin=bin*frames()/cache.size(), end=(bin+1)*frames()/cache.size();
            for(size_t f=begin;f<end;++f) {
                if ((f&0x3fffU)==0) checkImportCanceled(control);
                cache[bin]=std::max({cache[bin],std::abs(samples_[f*2]),std::abs(samples_[f*2+1])});
            }
        }
    };
    buildPeaks(peaks_);
    buildPeaks(detailPeaks_);
}
std::shared_ptr<const Clip> decodeWav(std::span<const unsigned char> b) {
    return decodeWav(b, {});
}
std::shared_ptr<const Clip> decodeWav(std::span<const unsigned char> b, const ImportControl& control,
                                      uint32_t* sourceRate, uint32_t* sourceChannels, uint64_t* sourceFrames) {
    checkImportCanceled(control);
    updateImportPhase(control, 2); // Decoding
    ImportControl decoding = control;
    decoding.progressEnd = control.progressBegin + (control.progressEnd - control.progressBegin) * 65 / 100;
    ImportControl converting = control;
    converting.progressBegin = decoding.progressEnd;
    if (b.size()<12 || !tag(b.data(),"RIFF") || !tag(b.data()+8,"WAVE")) throw Error("Expected a RIFF WAV file");
    size_t end=size_t(u32(b.data()+4))+8;
    if(end!=b.size()) throw Error("Truncated WAV or trailing data");
    std::span<const unsigned char> format, data;
    size_t chunks=0;
    for(size_t p=12;p<end;) {
        if ((chunks++&0x3ffU)==0) checkImportCanceled(decoding);
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
    if(!supportedRate(rate)) throw Error("Supported WAV sample rates: 44.1, 48, 88.2, 96, or 192 kHz");
    if(channels!=1 && channels!=2) throw Error("WAV must be mono or stereo");
    if(!((encoding==1 && (bits==16 || bits==24 || bits==32)) || (encoding==3 && bits==32))) throw Error("Supported WAV: PCM16/24/32 or float32 (no extensible/compressed WAV yet)");
    unsigned width=bits/8;
    if(align!=channels*width || u32(format.data()+8)!=rate*align || data.size()%align) throw Error("Invalid WAV frame layout");
    size_t frames=data.size()/align;
    if(!frames || frames>uint64_t(rate)*60) throw Error("Import a WAV of at most 60 seconds");
    if (sourceRate) *sourceRate = rate;
    if (sourceChannels) *sourceChannels = channels;
    if (sourceFrames) *sourceFrames = frames;
    std::vector<float> samples(frames*2);
    for(size_t f=0;f<frames;++f) for(unsigned ch=0;ch<2;++ch) {
        if ((f & 0x3fffU) == 0) { checkImportCanceled(decoding); updateImportProgress(decoding, f, frames); }
        auto p=data.data()+f*align+(ch%channels)*width; float value;
        if(encoding==3) value=std::bit_cast<float>(u32(p));
        else if(bits==16) value=static_cast<float>(std::bit_cast<int16_t>(u16(p)))/32768.0f;
        else if(bits==24) { int32_t v=int32_t(p[0])|int32_t(p[1])<<8|int32_t(p[2])<<16; if(v&0x800000) v-=0x1000000; value=float(v)/8388608.0f; }
        else value=static_cast<float>(std::bit_cast<int32_t>(u32(p)))/2147483648.0f;
        samples[f*2+ch]=value;
    }
    for (size_t index=0;index<samples.size();++index) {
        if ((index&0x3fffU)==0) checkImportCanceled(decoding);
        const float value=samples[index]; if (!std::isfinite(value) || std::abs(value)>16) throw Error("Invalid audio sample");
    }
    checkImportCanceled(decoding);
    updateImportProgress(decoding, frames, frames);
    updateImportPhase(control, 3); // Converting (also the 48 kHz fast path)
    return std::make_shared<const Clip>(resampleStereoTo48k(samples, rate, converting), converting);
}
std::shared_ptr<const Clip> readWav(const std::string& path) {
    return readWav(path, {});
}
std::shared_ptr<const Clip> readWav(const std::string& path, const ImportControl& control,
                                    uint32_t* sourceRate, uint32_t* sourceChannels, uint64_t* sourceFrames) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw Error("Cannot open WAV");
    auto size=file.tellg(); if(size<0 || size>32*1024*1024) throw Error("WAV exceeds 32 MiB import limit");
    const size_t byteCount = static_cast<size_t>(size);
    std::vector<unsigned char> bytes(byteCount); file.seekg(0);
    updateImportPhase(control, 1); // Reading
    ImportControl reading = control;
    reading.progressEnd = control.progressBegin + (control.progressEnd - control.progressBegin) / 5;
    ImportControl decoding = control;
    decoding.progressBegin = reading.progressEnd;
    constexpr size_t kReadBlock = 64 * 1024;
    for (size_t offset = 0; offset < byteCount; offset += kReadBlock) {
        checkImportCanceled(reading);
        const auto count = static_cast<std::streamsize>(std::min(kReadBlock, byteCount - offset));
        if(!file.read(reinterpret_cast<char*>(bytes.data() + offset), count)) throw Error("Cannot read complete WAV");
        updateImportProgress(reading, offset + static_cast<size_t>(count), byteCount);
    }
    checkImportCanceled(reading);
    return decodeWav(bytes, decoding, sourceRate, sourceChannels, sourceFrames);
}
// ---- AIFF / AIFC (PCM 16/24/32 integer, 32-bit float) decoder ----
// Produces the same stereo float32 representation at the source sample rate as
// decodeWav, then flows through the shared resampleStereoTo48k path.
std::shared_ptr<const Clip> decodeAiff(std::span<const unsigned char> b, const ImportControl& control,
                                       uint32_t* sourceRate, uint32_t* sourceChannels, uint64_t* sourceFrames) {
    checkImportCanceled(control);
    updateImportPhase(control, 2); // Decoding
    ImportControl decoding = control;
    decoding.progressEnd = control.progressBegin + (control.progressEnd - control.progressBegin) * 65 / 100;
    ImportControl converting = control;
    converting.progressBegin = decoding.progressEnd;

    if (b.size() < 12 || !tag(b.data(), "FORM")) throw Error("Expected a FORM/AIFF file");
    const size_t formSize = be32(b.data() + 4);
    const size_t end = formSize + 8;
    if (end != b.size()) throw Error("Truncated AIFF or trailing data");
    const bool isAifc = tag(b.data() + 8, "AIFC");
    if (!isAifc && !tag(b.data() + 8, "AIFF")) throw Error("Expected an AIFF or AIFC file");

    std::span<const unsigned char> comm, ssnd;
    size_t chunks = 0;
    for (size_t p = 12; p < end;) {
        if ((chunks++ & 0x3ffU) == 0) checkImportCanceled(decoding);
        if (end - p < 8) throw Error("Truncated AIFF chunk");
        size_t size = be32(b.data() + p + 4);
        size_t begin = p + 8;
        if (size > end - begin) throw Error("AIFF chunk exceeds file");
        if (tag(b.data() + p, "COMM")) { if (!comm.empty()) throw Error("Duplicate AIFF format"); comm = b.subspan(begin, size); }
        if (tag(b.data() + p, "SSND")) { if (!ssnd.empty()) throw Error("Duplicate AIFF audio"); ssnd = b.subspan(begin, size); }
        p = begin + size + (size & 1);
        if (p > end) throw Error("Missing AIFF padding");
    }
    if (comm.size() < 18 || ssnd.size() < 8) throw Error("AIFF format or audio missing");

    unsigned channels = be16(comm.data() + 0);
    uint64_t frames = be32(comm.data() + 2);
    unsigned bits = be16(comm.data() + 6);
    double rateD = beExtended80(comm.data() + 8);
    uint32_t rate = static_cast<uint32_t>(std::lround(rateD));
    std::string compression;
    if (isAifc) {
        if (comm.size() < 22) throw Error("AIFC format chunk too small for compression type");
        compression.assign(reinterpret_cast<const char*>(comm.data() + 18), 4);
    }

    if (!supportedRate(rate)) throw Error("Supported AIFF sample rates: 44.1, 48, 88.2, 96, or 192 kHz");
    if (channels != 1 && channels != 2) throw Error("AIFF must be mono or stereo");
    if (bits != 16 && bits != 24 && bits != 32) throw Error("Supported AIFF: 16/24/32-bit PCM or 32-bit float");
    bool isFloat = false;
    if (isAifc) {
        if (compression == "fl32") isFloat = true;
        else if (compression == "NONE" || compression == "none") isFloat = false;
        else throw Error("Unsupported AIFF-C compression (only NONE / fl32 supported)");
    }
    if (isFloat && bits != 32) throw Error("AIFF float must be 32-bit");

    unsigned width = bits / 8;
    unsigned frameBytes = channels * width;
    if ((ssnd.size() - 8) % frameBytes != 0) throw Error("Invalid AIFF audio frame layout");
    uint32_t ssndOffset = be32(ssnd.data() + 0);
    if (ssndOffset > ssnd.size() - 8) throw Error("AIFF sound data offset exceeds chunk");
    const unsigned char* dataPtr = ssnd.data() + 8 + ssndOffset;
    size_t avail = ssnd.size() - 8 - ssndOffset;
    if (avail < frames * frameBytes) throw Error("AIFF audio shorter than declared frames");

    if (!frames || frames > uint64_t(rate) * 60) throw Error("Import an AIFF of at most 60 seconds");

    if (sourceRate) *sourceRate = rate;
    if (sourceChannels) *sourceChannels = channels;
    if (sourceFrames) *sourceFrames = frames;

    std::vector<float> samples(static_cast<size_t>(frames) * 2);
    for (size_t f = 0; f < frames; ++f) {
        for (unsigned ch = 0; ch < 2; ++ch) {
            if ((f & 0x3fffU) == 0) { checkImportCanceled(decoding); updateImportProgress(decoding, f, frames); }
            const unsigned char* p = dataPtr + f * frameBytes + (ch % channels) * width;
            float value;
            if (isFloat) {
                value = std::bit_cast<float>(be32(p));
            } else {
                int32_t v = beInt(p, width);
                if (bits == 16) value = static_cast<float>(v) / 32768.0f;
                else if (bits == 24) value = static_cast<float>(v) / 8388608.0f;
                else value = static_cast<float>(v) / 2147483648.0f;
            }
            samples[f * 2 + ch] = value;
        }
    }
    for (size_t index = 0; index < samples.size(); ++index) {
        if ((index & 0x3fffU) == 0) checkImportCanceled(decoding);
        const float value = samples[index];
        if (!std::isfinite(value) || std::abs(value) > 16) throw Error("Invalid audio sample");
    }
    checkImportCanceled(decoding);
    updateImportProgress(decoding, frames, frames);
    updateImportPhase(control, 3); // Converting (also the 48 kHz fast path)
    return std::make_shared<const Clip>(resampleStereoTo48k(samples, rate, converting), converting);
}
std::shared_ptr<const Clip> decodeAiff(std::span<const unsigned char> b) {
    return decodeAiff(b, {});
}
std::shared_ptr<const Clip> readAiff(const std::string& path) {
    return readAiff(path, {});
}
std::shared_ptr<const Clip> readAiff(const std::string& path, const ImportControl& control,
                                     uint32_t* sourceRate, uint32_t* sourceChannels, uint64_t* sourceFrames) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw Error("Cannot open AIFF");
    auto size = file.tellg();
    if (size < 0 || size > 32 * 1024 * 1024) throw Error("AIFF exceeds 32 MiB import limit");
    const size_t byteCount = static_cast<size_t>(size);
    std::vector<unsigned char> bytes(byteCount);
    file.seekg(0);
    updateImportPhase(control, 1); // Reading
    ImportControl reading = control;
    reading.progressEnd = control.progressBegin + (control.progressEnd - control.progressBegin) / 5;
    ImportControl decoding = control;
    decoding.progressBegin = reading.progressEnd;
    constexpr size_t kReadBlock = 64 * 1024;
    for (size_t offset = 0; offset < byteCount; offset += kReadBlock) {
        checkImportCanceled(reading);
        const auto count = static_cast<std::streamsize>(std::min(kReadBlock, byteCount - offset));
        if (!file.read(reinterpret_cast<char*>(bytes.data() + offset), count)) throw Error("Cannot read complete AIFF");
        updateImportProgress(reading, offset + static_cast<size_t>(count), byteCount);
    }
    checkImportCanceled(reading);
    return decodeAiff(bytes, decoding, sourceRate, sourceChannels, sourceFrames);
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
