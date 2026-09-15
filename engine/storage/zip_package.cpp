#include "storage/zip_package.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string_view>
#include <unistd.h>

namespace daw { namespace {
constexpr size_t kMaximumEntries=4096,kMaximumNameBytes=240;
constexpr uint64_t kMaximumEntryBytes=128ULL*1024*1024,kMaximumPackageBytes=512ULL*1024*1024;
constexpr uint32_t kZip32Limit=0xffffffffU;
constexpr uint16_t kUtf8Flag=0x0800,kDosDate=0x0021;
struct PreparedEntry { const ZipPackageEntry* entry=nullptr; uint32_t crc=0,offset=0; };
thread_local const std::atomic<bool>* activeCancel=nullptr;
bool canceled(){return activeCancel&&activeCancel->load(std::memory_order_acquire);}
uint32_t crc32(const std::vector<uint8_t>& bytes){uint32_t value=0xffffffffU;for(size_t index=0;index<bytes.size();++index){if((index&0xffffU)==0&&canceled())return 0;value^=bytes[index];for(unsigned bit=0;bit<8;++bit)value=(value>>1)^(0xedb88320U&static_cast<uint32_t>(-(value&1U)));}return ~value;}
bool validEntryName(std::string_view name){if(name.empty()||name.size()>kMaximumNameBytes||name.front()=='/'||name.back()=='/')return false;size_t begin=0;while(begin<name.size()){const auto end=name.find('/',begin);const auto part=name.substr(begin,end==std::string_view::npos?std::string_view::npos:end-begin);if(part.empty()||part=="."||part=="..")return false;for(const unsigned char byte:part)if(byte<0x20||byte==0x7f||byte=='\\')return false;if(end==std::string_view::npos)break;begin=end+1;}return true;}
bool writeAll(int descriptor,const void* data,size_t size,std::string& error){const auto* cursor=static_cast<const uint8_t*>(data);size_t written=0;while(written<size){if(canceled()){error="ZIP package canceled";return false;}const auto chunk=std::min<size_t>(size-written,64*1024);const auto count=write(descriptor,cursor+written,chunk);if(count<0&&errno==EINTR)continue;if(count<=0){error="Write ZIP package failed: "+std::string(std::strerror(errno));return false;}written+=static_cast<size_t>(count);}return true;}
bool write16(int descriptor,uint16_t value,std::string& error){const std::array<uint8_t,2> bytes{static_cast<uint8_t>(value),static_cast<uint8_t>(value>>8)};return writeAll(descriptor,bytes.data(),bytes.size(),error);}
bool write32(int descriptor,uint32_t value,std::string& error){const std::array<uint8_t,4> bytes{static_cast<uint8_t>(value),static_cast<uint8_t>(value>>8),static_cast<uint8_t>(value>>16),static_cast<uint8_t>(value>>24)};return writeAll(descriptor,bytes.data(),bytes.size(),error);}
bool local(int descriptor,const PreparedEntry& item,std::string& error){const uint32_t size=static_cast<uint32_t>(item.entry->bytes.size());const uint16_t nameSize=static_cast<uint16_t>(item.entry->name.size());return write32(descriptor,0x04034b50U,error)&&write16(descriptor,20,error)&&write16(descriptor,kUtf8Flag,error)&&write16(descriptor,0,error)&&write16(descriptor,0,error)&&write16(descriptor,kDosDate,error)&&write32(descriptor,item.crc,error)&&write32(descriptor,size,error)&&write32(descriptor,size,error)&&write16(descriptor,nameSize,error)&&write16(descriptor,0,error)&&writeAll(descriptor,item.entry->name.data(),nameSize,error)&&writeAll(descriptor,item.entry->bytes.data(),item.entry->bytes.size(),error);}
bool central(int descriptor,const PreparedEntry& item,std::string& error){const uint32_t size=static_cast<uint32_t>(item.entry->bytes.size());const uint16_t nameSize=static_cast<uint16_t>(item.entry->name.size());return write32(descriptor,0x02014b50U,error)&&write16(descriptor,20,error)&&write16(descriptor,20,error)&&write16(descriptor,kUtf8Flag,error)&&write16(descriptor,0,error)&&write16(descriptor,0,error)&&write16(descriptor,kDosDate,error)&&write32(descriptor,item.crc,error)&&write32(descriptor,size,error)&&write32(descriptor,size,error)&&write16(descriptor,nameSize,error)&&write16(descriptor,0,error)&&write16(descriptor,0,error)&&write16(descriptor,0,error)&&write16(descriptor,0,error)&&write32(descriptor,0,error)&&write32(descriptor,item.offset,error)&&writeAll(descriptor,item.entry->name.data(),nameSize,error);}
bool syncDirectory(const std::string& path){const auto slash=path.find_last_of('/');const auto directory=slash==std::string::npos?std::string{"."}:(slash==0?std::string{"/"}:path.substr(0,slash));const int descriptor=open(directory.c_str(),O_RDONLY);if(descriptor<0)return false;const bool success=fsync(descriptor)==0;close(descriptor);return success;}
void removeTemporary(const std::string& path){if(!path.empty())unlink(path.c_str());}
}}

namespace daw {
bool writeZipPackage(const std::vector<ZipPackageEntry>& entries,const std::string& destination,std::string& error,const std::atomic<bool>* cancel){
    struct CancelScope{const std::atomic<bool>* previous;explicit CancelScope(const std::atomic<bool>* value):previous(activeCancel){activeCancel=value;}~CancelScope(){activeCancel=previous;}}scope(cancel);
    error.clear();
    if(destination.empty()){error="ZIP package destination is empty";return false;}
    if(entries.size()>kMaximumEntries||entries.size()>0xffffU){error="ZIP package has too many entries";return false;}
    std::vector<PreparedEntry> prepared;prepared.reserve(entries.size());std::set<std::string> names;uint64_t localBytes=0,centralBytes=0;
    for(const auto& entry:entries){
        if(canceled()){error="ZIP package canceled";return false;}
        if(!validEntryName(entry.name)||entry.bytes.size()>kMaximumEntryBytes||!names.insert(entry.name).second){error="ZIP package has invalid, duplicate, or oversized entry";return false;}
        localBytes+=30+entry.name.size()+entry.bytes.size();centralBytes+=46+entry.name.size();
        if(localBytes>kZip32Limit||centralBytes>kZip32Limit||localBytes+centralBytes+22>kMaximumPackageBytes||localBytes+centralBytes+22>kZip32Limit){error="ZIP package exceeds bounded ZIP32 limits";return false;}
        const auto crc=crc32(entry.bytes);if(canceled()){error="ZIP package canceled";return false;}prepared.push_back({&entry,crc,0});
    }
    std::sort(prepared.begin(),prepared.end(),[](const auto& left,const auto& right){return left.entry->name<right.entry->name;});
    std::string temporary=destination+".tmp.XXXXXX";std::vector<char> pattern(temporary.begin(),temporary.end());pattern.push_back(0);const int descriptor=mkstemp(pattern.data());temporary=pattern.data();
    if(descriptor<0){error="Create ZIP package temporary file failed: "+std::string(std::strerror(errno));return false;}
    uint64_t offset=0;
    for(auto& item:prepared){item.offset=static_cast<uint32_t>(offset);if(!local(descriptor,item,error)){close(descriptor);removeTemporary(temporary);return false;}offset+=30+item.entry->name.size()+item.entry->bytes.size();}
    const uint32_t centralOffset=static_cast<uint32_t>(offset);
    for(const auto& item:prepared){if(!central(descriptor,item,error)){close(descriptor);removeTemporary(temporary);return false;}offset+=46+item.entry->name.size();}
    const uint32_t centralSize=static_cast<uint32_t>(offset-centralOffset),count=static_cast<uint32_t>(prepared.size());
    const bool footer=write32(descriptor,0x06054b50U,error)&&write16(descriptor,0,error)&&write16(descriptor,0,error)&&write16(descriptor,static_cast<uint16_t>(count),error)&&write16(descriptor,static_cast<uint16_t>(count),error)&&write32(descriptor,centralSize,error)&&write32(descriptor,centralOffset,error)&&write16(descriptor,0,error);
    if(!footer){close(descriptor);removeTemporary(temporary);return false;}
    if(fsync(descriptor)!=0){error="Sync ZIP package failed: "+std::string(std::strerror(errno));close(descriptor);removeTemporary(temporary);return false;}
    if(close(descriptor)!=0){error="Close ZIP package failed";removeTemporary(temporary);return false;}
    if(canceled()){error="ZIP package canceled";removeTemporary(temporary);return false;}
    if(rename(temporary.c_str(),destination.c_str())!=0){error="Replace ZIP package failed: "+std::string(std::strerror(errno));removeTemporary(temporary);return false;}
    // The new archive is already atomically visible. A directory fsync is a
    // best-effort durability upgrade and cannot truthfully turn publication
    // into a failed operation after the destination has been replaced.
    (void)syncDirectory(destination);
    return true;
}
}
