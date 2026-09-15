#include "audio/export.hpp"
#include "audio/renderer.hpp"
#include "daw.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#define CHECK(x) do{if(!(x))throw std::runtime_error("Failed: " #x);}while(false)
template<class Fn>void rejects(Fn fn){bool bad=false;try{fn();}catch(...){bad=true;}CHECK(bad);}

int main(){try{
    auto root=std::filesystem::temp_directory_path()/("mydaw-export-"+std::to_string(getpid()));std::filesystem::create_directories(root);
    struct Cleanup{std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}}cleanup{root};
    std::vector<float> samples(2000);for(size_t i=0;i<samples.size()/2;++i){samples[i*2]=float(i%37)/40.0f;samples[i*2+1]=-float(i%29)/32.0f;}
    auto clip=std::make_shared<const daw::Clip>(std::move(samples));daw::Session session;session.import("Mix",clip,0);session.setClipFades(1,0,10,20,1);session.gain(1,-3,2);session.pan(1,0.25,3);session.masterGain(-2,4);
    daw::Renderer expected;expected.prepare(session.state());expected.playing=true;std::vector<float> left(1000),right(1000);expected.render(left.data(),right.data(),1000);

    auto floating=(root/"mix-float.wav").string();daw::writeWav(session.state(),floating,daw::WavFormat::Float32);auto decodedFloat=daw::readWav(floating);
    CHECK(decodedFloat->frames()==1000);for(size_t i=0;i<1000;++i){CHECK(decodedFloat->samples()[i*2]==left[i]);CHECK(decodedFloat->samples()[i*2+1]==right[i]);}
    std::ifstream floatFile(floating,std::ios::binary);std::vector<unsigned char> floatHeader(58);floatFile.read(reinterpret_cast<char*>(floatHeader.data()),floatHeader.size());CHECK(floatHeader[20]==3&&floatHeader[34]==32&&std::memcmp(floatHeader.data()+38,"fact",4)==0);

    auto pcm=(root/"mix-24.wav").string();daw::writeWav(session.state(),pcm,daw::WavFormat::PCM24);auto decodedPCM=daw::readWav(pcm);CHECK(decodedPCM->frames()==1000);
    for(size_t i=0;i<2000;++i)CHECK(std::abs(decodedPCM->samples()[i]-decodedFloat->samples()[i])<0.0000003f);
    std::ifstream pcmFile(pcm,std::ios::binary);std::vector<unsigned char> pcmHeader(44);pcmFile.read(reinterpret_cast<char*>(pcmHeader.data()),pcmHeader.size());CHECK(pcmHeader[20]==1&&pcmHeader[34]==24&&std::memcmp(pcmHeader.data()+36,"data",4)==0);

    auto range=(root/"range.wav").string();daw::writeWavRange(session.state(),range,daw::WavFormat::Float32,200,700);auto decodedRange=daw::readWav(range);CHECK(decodedRange->frames()==500);
    daw::Renderer expectedRange;expectedRange.prepare(session.state(),200);expectedRange.playing=true;std::vector<float> rangeLeft(500),rangeRight(500);expectedRange.render(rangeLeft.data(),rangeRight.data(),500);
    for(size_t i=0;i<500;++i){CHECK(decodedRange->samples()[i*2]==rangeLeft[i]);CHECK(decodedRange->samples()[i*2+1]==rangeRight[i]);}
    rejects([&]{daw::writeWavRange(session.state(),range,daw::WavFormat::Float32,700,200);});rejects([&]{daw::writeWavRange(session.state(),range,daw::WavFormat::Float32,0,1001);});

    auto canceled=(root/"keep.wav").string();{std::ofstream old(canceled);old<<"existing";}daw::ExportResult canceledProgress(session.state().revision,1000);canceledProgress.cancel=true;
    rejects([&]{daw::writeWav(session.state(),canceled,daw::WavFormat::Float32,&canceledProgress);});std::ifstream old(canceled);std::string unchanged;old>>unchanged;CHECK(unchanged=="existing");
    for(const auto& entry:std::filesystem::directory_iterator(root))CHECK(entry.path().filename().string().find(".mydaw-export-")==std::string::npos);
    daw::Session empty;rejects([&]{daw::writeWav(empty.state(),(root/"empty.wav").string(),daw::WavFormat::PCM24);});

    auto draft=(root/"source.mydawdraft").string();daw::writeDraft(session.state(),draft);std::unique_ptr<daw_session,decltype(&daw_destroy)> bridge(daw_create(),daw_destroy);CHECK(bridge&&daw_open_draft(bridge.get(),draft.c_str())==0);
    auto bridgePath=(root/"bridge.wav").string();std::unique_ptr<daw_export_job,decltype(&daw_release_export)> job(daw_begin_export(bridge.get(),bridgePath.c_str(),2),daw_release_export);CHECK(job);
    daw_export_status status{};status.struct_size=sizeof(status);for(int i=0;i<200;++i){CHECK(daw_poll_export(job.get(),&status)==0);if(status.status)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    CHECK(status.status==1&&status.revision==session.state().revision&&status.rendered_frames==1000&&status.total_frames==1000&&daw::readWav(bridgePath)->samples()==decodedFloat->samples());
    auto bridgeRangePath=(root/"bridge-range.wav").string();std::unique_ptr<daw_export_job,decltype(&daw_release_export)> rangeJob(daw_begin_export_range(bridge.get(),bridgeRangePath.c_str(),2,200,700),daw_release_export);CHECK(rangeJob);
    status={};status.struct_size=sizeof(status);for(int i=0;i<200;++i){CHECK(daw_poll_export(rangeJob.get(),&status)==0);if(status.status)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    CHECK(status.status==1&&status.total_frames==500&&daw::readWav(bridgeRangePath)->samples()==decodedRange->samples());
    CHECK(!daw_begin_export_range(bridge.get(),bridgeRangePath.c_str(),2,700,200));
    auto releasedExportPath=(root/"released-export.wav").string();auto releasedSession=daw_create();CHECK(releasedSession);CHECK(daw_open_draft(releasedSession,draft.c_str())==0);
    auto releasedJob=daw_begin_export(releasedSession,releasedExportPath.c_str(),2);CHECK(releasedJob);daw_release_export(releasedJob);daw_destroy(releasedSession);
    bool releasedExportComplete=false;for(int i=0;i<1000&&!releasedExportComplete;++i){try{releasedExportComplete=daw::readWav(releasedExportPath)->frames()==1000;}catch(...){std::this_thread::sleep_for(std::chrono::milliseconds(5));}}
    CHECK(releasedExportComplete);
    CHECK(!daw_begin_export(bridge.get(),bridgePath.c_str(),99));
    std::cout<<"PASS: full/range playback parity, PCM24 TPDF tolerance, WAV headers, atomic cancel, async C bridge\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
