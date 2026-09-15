#include "storage/project_package.hpp"
#include "storage/zip_package.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>

namespace daw {
namespace {
constexpr uint64_t kMaxPackageBytes=1ull<<30;
constexpr const char* kDraftEntry="project.mydawdraft";
bool readFile(const std::string& path,std::vector<uint8_t>& out,std::string& error){
  auto file=std::fopen(path.c_str(),"rb");
  if(!file){error="File not found: "+path;return false;}
  std::fseek(file,0,SEEK_END);const long size=std::ftell(file);std::fseek(file,0,SEEK_SET);
  if(size<0||static_cast<uint64_t>(size)>kMaxPackageBytes){std::fclose(file);error="Package exceeds the 1 GB ceiling";return false;}
  out.resize(static_cast<size_t>(size));
  if(size&&std::fread(out.data(),1,out.size(),file)!=out.size()){std::fclose(file);error="Package read failed";return false;}
  std::fclose(file);return true;
}
uint32_t read32(const std::vector<uint8_t>& bytes,size_t at){return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24;}
uint16_t read16(const std::vector<uint8_t>& bytes,size_t at){return uint16_t(bytes[at])|uint16_t(bytes[at+1])<<8;}
uint32_t crc32Of(const uint8_t* data,size_t size){
  static uint32_t table[256];static bool ready=false;
  if(!ready){for(uint32_t i=0;i<256;++i){uint32_t value=i;for(int bit=0;bit<8;++bit)value=value&1?0xEDB88320U^(value>>1):value>>1;table[i]=value;}ready=true;}
  uint32_t crc=0xFFFFFFFFU;
  for(size_t i=0;i<size;++i)crc=table[(crc^data[i])&0xFF]^(crc>>8);
  return crc^0xFFFFFFFFU;
}
bool isSqlite(const std::vector<uint8_t>& bytes){return bytes.size()>60&&std::memcmp(bytes.data(),"SQLite format 3\0",16)==0;}
uint32_t draftVersionOf(const std::vector<uint8_t>& bytes){return uint32_t(bytes[60])<<24|uint32_t(bytes[61])<<16|uint32_t(bytes[62])<<8|uint32_t(bytes[63]);}
bool writeFileAtomic(const std::string& path,const std::vector<uint8_t>& bytes,std::string& error){
  const auto temporary=path+".mydawtmp";
  auto descriptor=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0644);
  if(descriptor<0){error="Cannot create target file";return false;}
  size_t written=0;bool failed=false;
  while(written<bytes.size()){auto step=::write(descriptor,bytes.data()+written,bytes.size()-written);if(step<=0){failed=true;break;}written+=static_cast<size_t>(step);}
  if(!failed&&::fsync(descriptor)!=0)failed=true;
  ::close(descriptor);
  if(failed){::unlink(temporary.c_str());error="Target write failed";return false;}
  if(::rename(temporary.c_str(),path.c_str())!=0){::unlink(temporary.c_str());error="Target publish failed";return false;}
  return true;
}
} // namespace

bool readZipEntry(const std::string& zipPath,const std::string& name,std::vector<uint8_t>& out,std::string& error){
  std::vector<uint8_t> bytes;
  if(!readFile(zipPath,bytes,error))return false;
  if(bytes.size()<22){error="Package is too small to be a ZIP";return false;}
  size_t eocd=0;bool found=false;
  const size_t floor=bytes.size()>66000?bytes.size()-66000:0;
  for(size_t at=bytes.size()-22+1;at-->floor;){if(bytes.size()-at>=22&&read32(bytes,at)==0x06054B50U){eocd=at;found=true;break;}}
  if(!found){error="ZIP central directory is missing";return false;}
  const uint16_t count=read16(bytes,eocd+10);const uint32_t directory=read32(bytes,eocd+16);
  size_t at=directory;
  for(uint16_t item=0;item<count;++item){
    if(at+46>bytes.size()||read32(bytes,at)!=0x02014B50U){error="ZIP central entry is corrupt";return false;}
    const uint16_t method=read16(bytes,at+10),nameSize=read16(bytes,at+28),extraSize=read16(bytes,at+30),commentSize=read16(bytes,at+32);
    const uint32_t size=read32(bytes,at+20),offset=read32(bytes,at+42);
    if(at+46+nameSize>bytes.size()){error="ZIP name overflows the archive";return false;}
    std::string entry(reinterpret_cast<const char*>(bytes.data()+at+46),nameSize);
    const size_t next=at+46+nameSize+extraSize+commentSize;
    if(entry==name){
      if(method!=0){error="Package entry is compressed; only stored entries are supported";return false;}
      if(offset+30>bytes.size()||read32(bytes,offset)!=0x04034B50U){error="ZIP local header is corrupt";return false;}
      const uint16_t localName=read16(bytes,offset+26),localExtra=read16(bytes,offset+28);
      if(read16(bytes,offset+8)!=0){error="ZIP local entry is compressed";return false;}
      const uint64_t data=uint64_t(offset)+30+localName+localExtra;
      if(data+size>bytes.size()){error="ZIP entry overflows the archive";return false;}
      out.assign(bytes.begin()+static_cast<ptrdiff_t>(data),bytes.begin()+static_cast<ptrdiff_t>(data+size));
      if(crc32Of(out.data(),out.size())!=read32(bytes,at+16)){error="ZIP entry CRC mismatch";return false;}
      return true;
    }
    at=next;
  }
  error="Package entry not found: "+name;
  return false;
}

bool writeProjectPackage(const std::string& draftPath,const std::string& destination,std::string& error,const std::atomic<bool>* cancel){
  std::vector<uint8_t> draft;
  if(!readFile(draftPath,draft,error))return false;
  if(!isSqlite(draft)){error="Draft file is not a My DAW database";return false;}
  char stamp[32];{const auto now=std::time(nullptr);std::tm parts{};gmtime_r(&now,&parts);std::strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%SZ",&parts);}
  char manifestBytes[512];
  const auto drafted=draftVersionOf(draft);
  std::snprintf(manifestBytes,sizeof(manifestBytes),"{\"format\":\"mydaw-package\",\"version\":1,\"entry\":\"%s\",\"draftVersion\":%u,\"draftBytes\":%zu,\"createdUtc\":\"%s\"}\n",kDraftEntry,drafted,draft.size(),stamp);
  std::string manifest=manifestBytes;
  const std::string readme="My DAW project package\n";
  std::vector<ZipPackageEntry> entries={{"project.mydawdraft",std::move(draft)},{"manifest.json",{manifest.begin(),manifest.end()}},{"README.txt",{readme.begin(),readme.end()}}};
  return writeZipPackage(entries,destination,error,cancel);
}

bool extractProjectPackage(const std::string& zipPath,const std::string& targetDraftPath,std::string& error,const std::atomic<bool>* cancel){
  if(cancel&&cancel->load(std::memory_order_relaxed)){error="Package extraction was canceled";return false;}
  std::vector<uint8_t> manifest;
  if(!readZipEntry(zipPath,"manifest.json",manifest,error))return false;
  const std::string text(reinterpret_cast<const char*>(manifest.data()),manifest.size());
  if(text.find("\"format\":\"mydaw-package\"")==std::string::npos){error="Archive is not a My DAW package";return false;}
  std::vector<uint8_t> draft;
  if(!readZipEntry(zipPath,kDraftEntry,draft,error))return false;
  if(!isSqlite(draft)){error="Packaged draft is not a My DAW database";return false;}
  if(!writeFileAtomic(targetDraftPath,draft,error))return false;
  return true;
}

} // namespace daw
