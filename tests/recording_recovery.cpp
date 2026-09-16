#include "audio/recording.hpp"
#include "daw.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#define CHECK(x) do{if(!(x))throw std::runtime_error("Failed: " #x);}while(false)
template<class Fn>void rejects(Fn fn){bool bad=false;try{fn();}catch(...){bad=true;}CHECK(bad);}

int main(int argc,char** argv){try{
    if(argc==3&&std::string(argv[1])=="--fixture"){
        daw::RecordingWriter fixture(argv[2],48000,48000,48000);std::vector<float> audio(12000,0.2f);fixture.writeMono(audio.data(),audio.size());fixture.finish();return 0;
    }
    auto root=std::filesystem::temp_directory_path()/(("mydaw-recording-")+std::to_string(getpid()));
    std::filesystem::create_directories(root);
    struct Cleanup{std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}}cleanup{root};

    auto normal=(root/"normal.mydawtake").string();float values[]={0.25f,-0.5f,NAN,20.0f,0.75f};
    daw::RecordingWriter writer(normal,12000,20,16);writer.writeMono(values,5);float preview[512]{};float detailedPreview[daw::kRecordingPreviewDetailBins]{};writer.previewPeaks(preview,512);writer.previewPeaks(detailedPreview,daw::kRecordingPreviewDetailBins);CHECK(preview[0]>0.0f&&preview[0]<=1.0f);CHECK(preview[511]>0.0f&&preview[511]<=1.0f);CHECK(detailedPreview[0]>0.0f&&detailedPreview[0]<=1.0f);CHECK(detailedPreview[daw::kRecordingPreviewDetailBins-1]>0.0f&&detailedPreview[daw::kRecordingPreviewDetailBins-1]<=1.0f);auto clip=writer.finish();
    CHECK(clip->frames()==5&&clip->samples()[0]==0.25f&&clip->samples()[1]==0.25f&&clip->samples()[4]==0&&clip->samples()[6]==16);
    auto recovered=daw::recoverTake(normal);CHECK(recovered.startFrame==12000&&recovered.clip->samples()==clip->samples());
    auto detailedPath=(root/"detailed.mydawtake").string();std::vector<float> detailedInput(daw::kRecordingPreviewDetailBins);detailedInput.back()=0.75f;
    daw::RecordingWriter detailWriter(detailedPath,0,daw::kRecordingPreviewDetailBins,daw::kRecordingPreviewDetailBins);detailWriter.writeMono(detailedInput.data(),detailedInput.size());float latePeak[daw::kRecordingPreviewDetailBins]{};detailWriter.previewPeaks(latePeak,daw::kRecordingPreviewDetailBins);CHECK(latePeak[daw::kRecordingPreviewDetailBins-1]==0.75f);detailWriter.discard();
    std::vector<float> loopAudio(20);for(size_t i=0;i<10;++i)loopAudio[i*2]=loopAudio[i*2+1]=float(i);
    daw::Clip loopRecording(std::move(loopAudio));auto passes=daw::splitLoopPasses(loopRecording,4);
    CHECK(passes.size()==3&&passes[0]->frames()==4&&passes[1]->frames()==4&&passes[2]->frames()==2);
    CHECK(passes[0]->samples().front()==0&&passes[1]->samples().front()==4&&passes[2]->samples().front()==8);
    rejects([&]{daw::splitLoopPasses(loopRecording,0);});

    auto overflowPath=(root/"overflow.mydawtake").string();daw::RecordingWriter overflow(overflowPath,0,10,2);float four[]={1,2,3,4};overflow.writeMono(four,4);
    CHECK(overflow.overflowed()&&overflow.frames()==2);auto prefix=overflow.finish();CHECK(prefix->frames()==2&&prefix->samples()[2]==2);overflow.discard();CHECK(!std::filesystem::exists(overflowPath));

    std::unique_ptr<daw_session,decltype(&daw_destroy)> session(daw_create(),daw_destroy);CHECK(session);
    daw_snapshot snapshot{};snapshot.struct_size=sizeof(snapshot);CHECK(daw_get_snapshot(session.get(),&snapshot)==0);
    CHECK(daw_recover_take(session.get(),normal.c_str(),"Recovered",snapshot.revision)==0&&!std::filesystem::exists(normal));
    CHECK(daw_get_snapshot(session.get(),&snapshot)==0&&snapshot.track_count==1);
    daw_clip region{};region.struct_size=sizeof(region);CHECK(daw_get_clip(session.get(),1,0,&region)==0&&region.start==12000&&region.length==5);
    CHECK(daw_recover_take(session.get(),normal.c_str(),"Missing",snapshot.revision)==1);CHECK(daw_get_snapshot(session.get(),&snapshot)==0&&snapshot.track_count==1);

    auto crash=(root/(std::to_string(getpid()+1)+"-crash.mydawtake")).string();pid_t child=fork();CHECK(child>=0);
    if(child==0){
        try{daw::RecordingWriter live(crash,24000,48000,48000);std::vector<float> audio(30000,0.125f);live.writeMono(audio.data(),audio.size());
            for(int i=0;i<400&&live.committedFrames()<24000;++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
            _exit(live.committedFrames()>=24000?0:2);
        }catch(...){_exit(3);}
    }
    int status=0;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    auto afterCrash=daw::recoverTake(crash);CHECK(afterCrash.startFrame==24000&&afterCrash.clip->frames()>=24000&&afterCrash.clip->frames()<=30000);
    CHECK(std::abs(afterCrash.clip->samples().back()-0.125f)<0.0001f);

    auto corrupt=(root/"corrupt.mydawtake").string();{FILE* file=fopen(corrupt.c_str(),"wb");CHECK(file);fwrite("bad",1,3,file);fclose(file);}rejects([&]{daw::recoverTake(corrupt);});
    // Pre-roll skip: the leading captured frames vanish and the take label
    // still starts at the punch-in frame; a fully skipped pass keeps zero.
    {   auto skipPath=(root/"preroll.mydawtake").string();
        {
            daw::RecordingWriter writer(skipPath,96000,48000,48000,1500);
            std::vector<float> block(1000,0.0f);
            for(size_t pass=0;pass<4;++pass){for(auto& value:block)value=0.1f*float(pass+1);writer.writeMono(block.data(),block.size());}
            CHECK(writer.frames()==2500);
            auto clip=writer.finish();
            CHECK(clip&&clip->samples().size()==5000&&std::abs(clip->samples()[0]-0.2f)<1e-6f&&std::abs(clip->samples()[4999]-0.4f)<1e-6f);
        }
        auto skipped=daw::recoverTake(skipPath);CHECK(skipped.startFrame==96000&&skipped.clip->frames()==2500);
        {
            daw::RecordingWriter all(root/"all-skipped.mydawtake",0,48000,48000,5000);
            std::vector<float> block(1000,0.5f);for(size_t pass=0;pass<4;++pass)all.writeMono(block.data(),block.size());
            CHECK(all.frames()==0);all.discard();
        }
    }
    std::cout<<"PASS: SPSC prefix, checkpointed take, exact loop-pass split, model recovery, cleanup, corrupt rejection, process-crash recovery\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
