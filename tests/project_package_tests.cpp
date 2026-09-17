#include "storage/project_package.hpp"
#include "storage/zip_package.hpp"
#include "domain/session.hpp"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
#define CHECK(x) do{if(!(x))throw std::runtime_error("Failed: " #x);}while(false)
static bool writeAll(const std::string& path,const std::vector<uint8_t>& data){auto file=std::fopen(path.c_str(),"wb");if(!file)return false;const auto ok=std::fwrite(data.data(),1,data.size(),file)==data.size();std::fclose(file);return ok;}
using namespace daw;
int main(){try{
    auto root=std::filesystem::temp_directory_path()/("mydaw-package-"+std::to_string(getpid()));
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    struct Cleanup{std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}}cleanup{root};
    const auto draft=root/"tone.mydawdraft";
    {   Session s;auto clip=std::make_shared<const Clip>(std::vector<float>(9600,0.25f));s.import("T",clip,0);s.setClipPan(1,0,0.5,1);
        writeDraft(s.state(),draft.string());
        std::string error;
        const auto zip=root/"tone.mydawzip";
        CHECK(writeProjectPackage(draft.string(),zip.string(),error));
        CHECK(error.empty());
        std::vector<uint8_t> manifest;
        CHECK(readZipEntry(zip.string(),"manifest.json",manifest,error));
        const std::string text(reinterpret_cast<const char*>(manifest.data()),manifest.size());
        CHECK(text.find("mydaw-package")!=std::string::npos&&text.find("\"draftVersion\":22")!=std::string::npos&&text.find("\"draftBytes\"")!=(std::string::npos-1));
        std::vector<uint8_t> readme;
        CHECK(readZipEntry(zip.string(),"README.txt",readme,error));
        // A foreign archive carries a manifest without our format tag.
        {   std::vector<uint8_t> junk(6,'h');const std::string junkText="hello!";junk.assign(junkText.begin(),junkText.end());
            const std::string bytes(junk.begin(),junk.end());
            CHECK(writeZipPackage({{"manifest.json",std::vector<uint8_t>(junk.begin(),junk.end())}},(root/"foreign.mydawzip").string(),error));
            std::string extractError;
            CHECK(!extractProjectPackage((root/"foreign.mydawzip").string(),(root/"x.mydawdraft").string(),extractError));
            CHECK(extractError.find("not a My DAW package")!=std::string::npos);
        }
        // Restore: bytes flow back into a fully usable draft.
        const auto restored=root/"restored.mydawdraft";
        CHECK(extractProjectPackage(zip.string(),restored.string(),error));
        const auto loaded=readDraft(restored.string());
        CHECK(loaded.tracks.size()==1&&loaded.tracks[0].regions.size()==1);
        CHECK(std::abs(loaded.tracks[0].regions[0].pan-0.5)<1e-12);
        CHECK(loaded.tracks[0].audio&&loaded.tracks[0].audio->samples().size()==9600);
        // A second extraction over the same target republishes cleanly.
        CHECK(extractProjectPackage(zip.string(),restored.string(),error));
        // Corrupt one data byte deep inside the stored draft entry: CRC gate.
        {   std::FILE* file=std::fopen(zip.string().c_str(),"rb");CHECK(file);std::fseek(file,0,SEEK_END);const long size=std::ftell(file);std::fseek(file,0,SEEK_SET);
            std::vector<uint8_t> bytes(static_cast<size_t>(size));CHECK(std::fread(bytes.data(),1,bytes.size(),file)==bytes.size());std::fclose(file);
            const size_t victim=200;bytes[victim]^=0x40;
            CHECK(writeAll(zip.string()+".corrupt",bytes));
            std::string crcError;
            CHECK(!extractProjectPackage(zip.string()+".corrupt",(root/"y.mydawdraft").string(),crcError));
            CHECK(crcError.find("CRC mismatch")!=std::string::npos);
        }
        // Missing draft and non-archive inputs are refused by name.
        CHECK(!writeProjectPackage((root/"absent.mydawdraft").string(),(root/"z.zip").string(),error));
        CHECK(error.find("not found")!=std::string::npos);
        std::string notZip;
        CHECK(!extractProjectPackage(draft.string(),(root/"w.mydawdraft").string(),notZip));
        CHECK(notZip.find("central directory")!=std::string::npos);
    }
    std::cout<<"PASS: package write, manifest, extract round trip, foreign/CRC/absent rejects\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}