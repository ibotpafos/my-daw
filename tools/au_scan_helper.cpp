#include <AudioToolbox/AudioToolbox.h>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <pwd.h>
#include <string>
#include <string_view>
#include <vector>

namespace {
bool parseHex(const char* text,uint32_t& value) { char* end=nullptr; errno=0; const auto parsed=std::strtoul(text,&end,16); if(errno||!end||*end!='\0'||parsed>0xffffffffUL) return false; value=static_cast<uint32_t>(parsed); return true; }
bool printableName(const char* value) { for(;*value;++value) if(*value=='\t'||*value=='\r'||*value=='\n') return false; return true; }
std::string componentKey(uint32_t type,uint32_t subtype,uint32_t manufacturer) { char text[27]{}; std::snprintf(text,sizeof(text),"%08x:%08x:%08x",type,subtype,manufacturer); return text; }
std::string cfString(CFStringRef value) { if(!value) return {}; const auto length=CFStringGetLength(value); const auto maximum=CFStringGetMaximumSizeForEncoding(length,kCFStringEncodingUTF8)+1; if(maximum<=1||maximum>4097) return {}; std::vector<char> text(static_cast<size_t>(maximum)); return CFStringGetCString(value,text.data(),maximum,kCFStringEncodingUTF8)?std::string(text.data()):std::string{}; }
bool fourCC(CFTypeRef value,uint32_t& result) { if(!value||CFGetTypeID(value)!=CFStringGetTypeID()) return false; const auto text=cfString(static_cast<CFStringRef>(value)); if(text.size()!=4) return false; result=(uint32_t(uint8_t(text[0]))<<24)|(uint32_t(uint8_t(text[1]))<<16)|(uint32_t(uint8_t(text[2]))<<8)|uint32_t(uint8_t(text[3])); return true; }
std::string encode(std::string_view value) { static constexpr char hex[]="0123456789ABCDEF"; std::string result; for(const unsigned char byte:value) { if((byte>='A'&&byte<='Z')||(byte>='a'&&byte<='z')||(byte>='0'&&byte<='9')||byte=='-'||byte=='_'||byte=='.'||byte=='/') result.push_back(static_cast<char>(byte)); else { result.push_back('%'); result.push_back(hex[byte>>4]); result.push_back(hex[byte&15]); } } return result; }
struct BundleMetadata { std::string path,version; };
using BundleIndex=std::map<std::string,BundleMetadata>;
void indexBundle(const std::filesystem::path& path,BundleIndex& index) {
    CFURLRef url=CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,reinterpret_cast<const UInt8*>(path.c_str()),static_cast<CFIndex>(path.string().size()),true); if(!url) return;
    CFBundleRef bundle=CFBundleCreate(kCFAllocatorDefault,url); CFRelease(url); if(!bundle) return;
    auto version=cfString(static_cast<CFStringRef>(CFBundleGetValueForInfoDictionaryKey(bundle,CFSTR("CFBundleShortVersionString"))));
    if(version.empty()) version=cfString(static_cast<CFStringRef>(CFBundleGetValueForInfoDictionaryKey(bundle,CFSTR("CFBundleVersion"))));
    const auto info=CFBundleGetInfoDictionary(bundle); const auto components=info?static_cast<CFArrayRef>(CFDictionaryGetValue(info,CFSTR("AudioComponents"))):nullptr;
    if(components&&CFGetTypeID(components)==CFArrayGetTypeID()) for(CFIndex indexNumber=0;indexNumber<CFArrayGetCount(components);++indexNumber) { const auto component=static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(components,indexNumber)); if(!component||CFGetTypeID(component)!=CFDictionaryGetTypeID()) continue; uint32_t type=0,subtype=0,manufacturer=0; if(!fourCC(CFDictionaryGetValue(component,CFSTR("type")),type)||!fourCC(CFDictionaryGetValue(component,CFSTR("subtype")),subtype)||!fourCC(CFDictionaryGetValue(component,CFSTR("manufacturer")),manufacturer)) continue; const auto key=componentKey(type,subtype,manufacturer); const BundleMetadata metadata{path.string(),version}; const auto found=index.find(key); if(found==index.end()||metadata.path<found->second.path) index[key]=metadata; }
    CFRelease(bundle);
}
BundleIndex bundleIndex() {
    std::vector<std::filesystem::path> roots{"/Library/Audio/Plug-Ins/Components","/System/Library/Audio/Plug-Ins/Components"};
    if(const auto account=getpwuid(getuid());account&&account->pw_dir) roots.emplace_back(std::string(account->pw_dir)+"/Library/Audio/Plug-Ins/Components");
    BundleIndex result; for(const auto& root:roots) { std::error_code error; if(!std::filesystem::is_directory(root,error)) continue; std::filesystem::recursive_directory_iterator iterator(root,std::filesystem::directory_options::skip_permission_denied,error),end; while(!error&&iterator!=end) { const auto path=iterator->path(); if(path.extension()==".component") { indexBundle(path,result); iterator.disable_recursion_pending(); } iterator.increment(error); } }
    return result;
}
void printDescription(const AudioComponentDescription& value,const char* name,const BundleIndex& bundles) { const auto found=bundles.find(componentKey(value.componentType,value.componentSubType,value.componentManufacturer)); const BundleMetadata metadata=found==bundles.end()?BundleMetadata{}:found->second; std::printf("%08x\t%08x\t%08x\t%s\t%s\t%s\n",value.componentType,value.componentSubType,value.componentManufacturer,name,encode(metadata.path).c_str(),encode(metadata.version).c_str()); }
AudioStreamBasicDescription stereoFormat() { AudioStreamBasicDescription value{}; value.mSampleRate=48000; value.mFormatID=kAudioFormatLinearPCM; value.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked|kAudioFormatFlagIsNonInterleaved|kAudioFormatFlagsNativeEndian; value.mBytesPerPacket=4; value.mFramesPerPacket=1; value.mBytesPerFrame=4; value.mChannelsPerFrame=2; value.mBitsPerChannel=32; return value; }
int list() {
    AudioComponentDescription wildcard{0,0,0,0,0}; AudioComponent component=nullptr;
    const auto bundles=bundleIndex();
    while((component=AudioComponentFindNext(component,&wildcard))) {
        AudioComponentDescription description{}; if(AudioComponentGetDescription(component,&description)!=noErr) continue;
        if(description.componentType!=kAudioUnitType_Effect&&description.componentType!=kAudioUnitType_MusicEffect) continue;
        CFStringRef name=nullptr; if(AudioComponentCopyName(component,&name)!=noErr||!name) continue;
        std::array<char,481> bytes{}; const bool converted=CFStringGetCString(name,bytes.data(),bytes.size(),kCFStringEncodingUTF8); CFRelease(name);
        if(converted&&bytes[0]&&printableName(bytes.data())) printDescription(description,bytes.data(),bundles);
    }
    return 0;
}
int probe(uint32_t type,uint32_t subtype,uint32_t manufacturer) {
    AudioComponentDescription description{type,subtype,manufacturer,0,0}; auto component=AudioComponentFindNext(nullptr,&description); if(!component) return 10;
    AudioUnit unit=nullptr; if(AudioComponentInstanceNew(component,&unit)!=noErr||!unit) return 11;
    bool initialized=false;
    const auto dispose=[&]{ if(unit) { if(initialized) AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); unit=nullptr; } };
    const auto stream=stereoFormat(); UInt32 maximum=4096;
    if(AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Input,0,&stream,sizeof(stream))!=noErr||AudioUnitSetProperty(unit,kAudioUnitProperty_StreamFormat,kAudioUnitScope_Output,0,&stream,sizeof(stream))!=noErr||AudioUnitSetProperty(unit,kAudioUnitProperty_MaximumFramesPerSlice,kAudioUnitScope_Global,0,&maximum,sizeof(maximum))!=noErr||AudioUnitInitialize(unit)!=noErr) { dispose(); return 12; }
    initialized=true;
    Float64 latency=0; UInt32 latencySize=sizeof(latency); if(AudioUnitGetProperty(unit,kAudioUnitProperty_Latency,kAudioUnitScope_Global,0,&latency,&latencySize)!=noErr||!std::isfinite(latency)||latency<0||latency>10) { dispose(); return 13; }
    CFPropertyListRef state=nullptr; UInt32 stateSize=sizeof(state); const auto stateStatus=AudioUnitGetProperty(unit,kAudioUnitProperty_ClassInfo,kAudioUnitScope_Global,0,&state,&stateSize); if(state) CFRelease(state); dispose();
    if(stateStatus!=noErr) return 14;
    std::fputs("ok\n",stdout);
    return 0;
}
}

int main(int argc,char** argv) {
    if(argc==2&&std::strcmp(argv[1],"--list")==0) return list();
    if(argc==5&&std::strcmp(argv[1],"--probe")==0) { uint32_t type=0,subtype=0,manufacturer=0; if(!parseHex(argv[2],type)||!parseHex(argv[3],subtype)||!parseHex(argv[4],manufacturer)||type==0||subtype==0||manufacturer==0) return 2; return probe(type,subtype,manufacturer); }
    return 2;
}
