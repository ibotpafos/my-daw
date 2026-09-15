#include "platform/macos/au_scan_cache.hpp"
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace daw {
namespace {
constexpr size_t kMaximumCacheBytes=1024*1024;
constexpr size_t kMaximumEntries=2048;
constexpr size_t kMaximumNameBytes=480;
constexpr size_t kMaximumMetadataBytes=4096;
constexpr std::string_view kHeader="MYDAW_AU_SCAN_CACHE\t1";

bool safeText(std::string_view value,size_t maximum) { for(const auto byte:value) if(static_cast<unsigned char>(byte)<0x20||byte=='\t'||byte=='\r'||byte=='\n') return false; return value.size()<=maximum; }
std::string encode(std::string_view value) { static constexpr char hex[]="0123456789ABCDEF"; std::string result; result.reserve(value.size()); for(const unsigned char byte:value) { if((byte>='A'&&byte<='Z')||(byte>='a'&&byte<='z')||(byte>='0'&&byte<='9')||byte=='-'||byte=='_'||byte=='.'||byte=='/') result.push_back(static_cast<char>(byte)); else { result.push_back('%'); result.push_back(hex[byte>>4]); result.push_back(hex[byte&15]); } } return result; }
int hexValue(char value) { if(value>='0'&&value<='9') return value-'0'; if(value>='A'&&value<='F') return value-'A'+10; if(value>='a'&&value<='f') return value-'a'+10; return -1; }
bool decode(std::string_view encoded,size_t maximum,std::string& result) { result.clear(); result.reserve(encoded.size()); for(size_t index=0;index<encoded.size();) { if(encoded[index]=='%') { if(index+2>=encoded.size()) return false; const int left=hexValue(encoded[index+1]),right=hexValue(encoded[index+2]); if(left<0||right<0) return false; result.push_back(static_cast<char>((left<<4)|right)); index+=3; } else { result.push_back(encoded[index++]); } } return safeText(result,maximum); }
bool parseUnsigned(std::string_view text,uint64_t& value) { if(text.empty()) return false; const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value); return error==std::errc{}&&end==text.data()+text.size(); }
bool parseHex(std::string_view text,uint32_t& value) { if(text.empty()||text.size()!=8) return false; unsigned long parsed=0; for(char byte:text) { const int nibble=hexValue(byte); if(nibble<0) return false; parsed=(parsed<<4)|static_cast<unsigned long>(nibble); } value=static_cast<uint32_t>(parsed); return true; }
std::string hex(uint32_t value) { std::array<char,9> text{}; std::snprintf(text.data(),text.size(),"%08x",value); return text.data(); }
std::vector<std::string_view> fields(std::string_view line) { std::vector<std::string_view> result; size_t begin=0; while(true) { const auto end=line.find('\t',begin); result.push_back(line.substr(begin,end==std::string_view::npos?std::string_view::npos:end-begin)); if(end==std::string_view::npos) return result; begin=end+1; } }
bool validEntry(const AudioUnitScanCacheEntry& entry) { return entry.descriptor.type&&entry.descriptor.subtype&&entry.descriptor.manufacturer&&safeText(entry.descriptor.name,kMaximumNameBytes)&&safeText(entry.bundlePath,kMaximumMetadataBytes)&&safeText(entry.bundleVersion,kMaximumMetadataBytes)&&safeText(entry.quarantineReason,kMaximumMetadataBytes)&&(!entry.available||entry.quarantineReason.empty()); }
std::string key(const AudioUnitScanCacheEntry& entry) { return hex(entry.descriptor.type)+":"+hex(entry.descriptor.subtype)+":"+hex(entry.descriptor.manufacturer)+":"+entry.bundlePath+":"+entry.bundleVersion; }
bool fsyncDirectory(const std::string& path) { const auto slash=path.find_last_of('/'); const std::string directory=slash==std::string::npos?".":(slash==0?"/":path.substr(0,slash)); const int descriptor=open(directory.c_str(),O_RDONLY); if(descriptor<0) return false; const int status=fsync(descriptor); close(descriptor); return status==0; }
}

AudioUnitScanCache makeAudioUnitScanCache(const IsolatedAudioUnitScan& scan,uint64_t nowUnixSeconds) {
    AudioUnitScanCache cache; cache.createdAtUnixSeconds=nowUnixSeconds; cache.entries.reserve(scan.available.size()+scan.quarantined.size());
    if(scan.availableMetadata.empty()) for(const auto& descriptor:scan.available) cache.entries.push_back({descriptor,{}, {},true,{},nowUnixSeconds});
    else for(const auto& item:scan.availableMetadata) cache.entries.push_back({item.descriptor,item.bundlePath,item.bundleVersion,true,{},nowUnixSeconds});
    for(const auto& record:scan.quarantined) cache.entries.push_back({record.descriptor,record.bundlePath,record.bundleVersion,false,record.reason,nowUnixSeconds});
    return cache;
}

AudioUnitScanCacheFreshness freshAudioUnitScanCache(const AudioUnitScanCache& cache,const IsolatedAudioUnitEnumeration& current) {
    AudioUnitScanCacheFreshness result; result.fresh.createdAtUnixSeconds=cache.createdAtUnixSeconds;
    if(!current.helperError.empty()) { result.invalidatedEntries=static_cast<uint32_t>(cache.entries.size()); return result; }
    std::set<std::string> present;
    for(const auto& item:current.components) { AudioUnitScanCacheEntry entry; entry.descriptor=item.descriptor; entry.bundlePath=item.bundlePath; entry.bundleVersion=item.bundleVersion; present.insert(key(entry)); }
    for(const auto& entry:cache.entries) { if(present.contains(key(entry))) result.fresh.entries.push_back(entry); else ++result.invalidatedEntries; }
    return result;
}

std::optional<AudioUnitScanCache> readAudioUnitScanCache(const std::string& path,std::string& error) {
    error.clear(); struct stat metadata{}; if(stat(path.c_str(),&metadata)!=0) { if(errno==ENOENT) return std::nullopt; error="Read Audio Unit cache metadata failed: "+std::string(std::strerror(errno)); return std::nullopt; }
    if(metadata.st_size<0||static_cast<uintmax_t>(metadata.st_size)>kMaximumCacheBytes) { error="Audio Unit cache exceeds 1 MiB"; return std::nullopt; }
    std::ifstream input(path,std::ios::binary); if(!input) { error="Open Audio Unit cache failed"; return std::nullopt; }
    std::string content((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>()); if(!input.good()&&!input.eof()) { error="Read Audio Unit cache failed"; return std::nullopt; }
    if(content.size()>kMaximumCacheBytes) { error="Audio Unit cache exceeds 1 MiB"; return std::nullopt; }
    std::istringstream lines(content); std::string line; if(!std::getline(lines,line)||line!=kHeader) { error="Audio Unit cache header is invalid"; return std::nullopt; }
    if(!std::getline(lines,line)) { error="Audio Unit cache timestamp is missing"; return std::nullopt; }
    const auto timestamp=fields(line); AudioUnitScanCache cache; if(timestamp.size()!=2||timestamp[0]!="created"||!parseUnsigned(timestamp[1],cache.createdAtUnixSeconds)) { error="Audio Unit cache timestamp is invalid"; return std::nullopt; }
    std::set<std::string> keys;
    while(std::getline(lines,line)) {
        if(line.empty()) { error="Audio Unit cache has an empty record"; return std::nullopt; }
        const auto values=fields(line); if(values.size()!=10||values[0]!="entry"||(values[1]!="available"&&values[1]!="quarantined")) { error="Audio Unit cache record is invalid"; return std::nullopt; }
        AudioUnitScanCacheEntry entry; entry.available=values[1]=="available";
        if(!parseHex(values[2],entry.descriptor.type)||!parseHex(values[3],entry.descriptor.subtype)||!parseHex(values[4],entry.descriptor.manufacturer)||!parseUnsigned(values[5],entry.scannedAtUnixSeconds)||!decode(values[6],kMaximumNameBytes,entry.descriptor.name)||!decode(values[7],kMaximumMetadataBytes,entry.bundlePath)||!decode(values[8],kMaximumMetadataBytes,entry.bundleVersion)||!decode(values[9],kMaximumMetadataBytes,entry.quarantineReason)||!validEntry(entry)) { error="Audio Unit cache record fields are invalid"; return std::nullopt; }
        if(cache.entries.size()>=kMaximumEntries||!keys.insert(key(entry)).second) { error="Audio Unit cache has too many or duplicate records"; return std::nullopt; }
        cache.entries.push_back(std::move(entry));
    }
    return cache;
}

bool writeAudioUnitScanCache(const AudioUnitScanCache& cache,const std::string& path,std::string& error) {
    error.clear(); if(path.empty()) { error="Audio Unit cache path is empty"; return false; } if(cache.entries.size()>kMaximumEntries) { error="Audio Unit cache has too many records"; return false; }
    std::set<std::string> keys; std::ostringstream output; output<<kHeader<<'\n'<<"created\t"<<cache.createdAtUnixSeconds<<'\n';
    for(const auto& entry:cache.entries) { if(!validEntry(entry)||!keys.insert(key(entry)).second) { error="Audio Unit cache contains invalid or duplicate record"; return false; } output<<"entry\t"<<(entry.available?"available":"quarantined")<<'\t'<<hex(entry.descriptor.type)<<'\t'<<hex(entry.descriptor.subtype)<<'\t'<<hex(entry.descriptor.manufacturer)<<'\t'<<entry.scannedAtUnixSeconds<<'\t'<<encode(entry.descriptor.name)<<'\t'<<encode(entry.bundlePath)<<'\t'<<encode(entry.bundleVersion)<<'\t'<<encode(entry.quarantineReason)<<'\n'; }
    const auto content=output.str(); if(content.size()>kMaximumCacheBytes) { error="Audio Unit cache serialization exceeds 1 MiB"; return false; }
    const std::string temporary=path+".tmp."+std::to_string(static_cast<unsigned long>(getpid()));
    const int descriptor=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600); if(descriptor<0) { error="Create Audio Unit cache temporary file failed: "+std::string(std::strerror(errno)); return false; }
    size_t written=0; while(written<content.size()) { const auto count=write(descriptor,content.data()+written,content.size()-written); if(count<=0) { const auto reason=std::strerror(errno); close(descriptor); unlink(temporary.c_str()); error="Write Audio Unit cache failed: "+std::string(reason); return false; } written+=static_cast<size_t>(count); }
    if(fsync(descriptor)!=0) { const auto reason=std::strerror(errno); close(descriptor); unlink(temporary.c_str()); error="Sync Audio Unit cache failed: "+std::string(reason); return false; }
    if(close(descriptor)!=0) { unlink(temporary.c_str()); error="Close Audio Unit cache failed"; return false; }
    if(rename(temporary.c_str(),path.c_str())!=0) { const auto reason=std::strerror(errno); unlink(temporary.c_str()); error="Replace Audio Unit cache failed: "+std::string(reason); return false; }
    if(!fsyncDirectory(path)) { error="Sync Audio Unit cache directory failed"; return false; }
    return true;
}
}
