#include "export/dawproject_export.hpp"
#include "export/dawproject_model.hpp"
#include "audio/renderer.hpp"
#include "daw.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#define CHECK(x) do{if(!(x))throw std::runtime_error("Failed: " #x);}while(false)

int main(){try{
    auto root=std::filesystem::temp_directory_path()/("mydaw-dawproject-"+std::to_string(getpid()));
    std::filesystem::create_directories(root);
    struct Cleanup{std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}}cleanup{root};

    std::vector<float> samples(2000);
    for(size_t i=0;i<samples.size()/2;++i){samples[i*2]=float(i%37)/40.0f;samples[i*2+1]=-float(i%29)/32.0f;}
    auto clip=std::make_shared<const daw::Clip>(std::move(samples));
    daw::Session session;
    session.import("Mix",clip,0);
    session.gain(1,-3,1);
    session.pan(1,0.25,2);
    session.masterGain(-2,3);

    auto destination=(root/"out.dawproject").string();
    daw::dawproject::ExportOptions options{};
    options.tempoBpm=120;
    auto result=daw::dawproject::startExport(session.state(),destination,options,"Alpha Test","1.38.0");
    CHECK(result!=nullptr);

    bool done=false;
    for(int i=0;i<400;++i){
        if(result->status.load()!=0){done=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(done);
    CHECK(result->status.load()==1);
    CHECK(result->cancel.load()==false);
    CHECK(std::string(result->error)=="");
    CHECK(result->completedEntries.load()==result->totalEntries);
    CHECK(std::filesystem::exists(destination));

    std::ifstream file(destination,std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    CHECK(bytes.size()>200);
    CHECK(bytes[0]=='P'&&bytes[1]=='K'&&bytes[2]==3&&bytes[3]==4);
    const std::string content(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    CHECK(content.find("project.xml")!=std::string::npos);
    CHECK(content.find("metadata.xml")!=std::string::npos);
    CHECK(content.find("loss-report.json")!=std::string::npos);
    CHECK(content.find("audio/")!=std::string::npos);
    std::cout<<"PASS: DAWproject export zip with project/metadata/loss-report/audio\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
