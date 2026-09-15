#include "storage/save_job.hpp"
#include "jobs/limiter.hpp"
#include "daw.h"
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spawn.h>
#include <vector>
extern char** environ;
#define CHECK(x) do {if(!(x))throw std::runtime_error("Failed: " #x);}while(false)
int crashStage=0;
void crashAt(int stage){if(stage==crashStage)raise(SIGKILL);}
daw_save_status waitJob(daw_save_job* job){
    daw_save_status status{};status.struct_size=sizeof(status);
    for(int i=0;i<1000;++i){CHECK(daw_poll_save(job,&status)==0);if(status.status)return status;std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    throw std::runtime_error("Save job timeout");
}
int main(int argc, char** argv){try{
    if(argc==4 && std::string(argv[1])=="--crash-child") {
        crashStage=std::stoi(argv[2]);
        daw::Session updated;updated.add("Recovered",0);updated.gain(1,-9,1);
        daw::writeDraft(updated.state(),argv[3],crashAt);return 11;
    }
    auto directory=std::filesystem::temp_directory_path()/("mydaw-storage-"+std::to_string(getpid()));std::filesystem::create_directory(directory);
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}}cleanup{directory};
    auto path=(directory/"snapshot.mydawdraft").string();daw::Session baseline;baseline.add("Original",0);
    daw::Session updated;updated.add("Recovered",0);updated.gain(1,-9,1);
    // Fresh processes: Foundation must not be called in a post-fork child.
    for(int stage:{1,2,3}){
        daw::writeDraft(baseline.state(),path);crashStage=stage;
        auto stageText=std::to_string(stage);
        char mode[]="--crash-child";
        char* args[]={argv[0],mode,stageText.data(),path.data(),nullptr};
        pid_t child;CHECK(posix_spawn(&child,argv[0],nullptr,nullptr,args,environ)==0);
        int result=0;CHECK(waitpid(child,&result,0)==child);CHECK(WIFSIGNALED(result)&&WTERMSIG(result)==SIGKILL);
        auto loaded=daw::readDraft(path);CHECK(loaded.tracks[0].name==(stage==3?"Recovered":"Original"));
    }
    auto session=daw_create();CHECK(session);CHECK(daw_add_track(session,"Captured",0)==0);
    auto job=daw_begin_save(session,path.c_str());CHECK(job);
    CHECK(daw_rename_track(session,1,"Newer edit",1)==0); // worker owns revision 1, UI is revision 2
    daw_destroy(session); // worker must not retain or access the session
    auto status=waitJob(job);CHECK(status.status==1 && status.revision==1);daw_release_save(job);
    auto loaded=daw::readDraft(path);CHECK(loaded.revision==1 && loaded.tracks[0].name=="Captured");
    session=daw_create();CHECK(daw_add_track(session,"Failure",0)==0);
    auto invalid=(directory/"missing"/"file").string();job=daw_begin_save(session,invalid.c_str());CHECK(job);
    status=waitJob(job);CHECK(status.status==2 && status.error[0]);daw_release_save(job);daw_destroy(session);
    CHECK(daw::readDraft(path).tracks[0].name=="Captured");
    auto recovery=(directory/"checkpoint.mydawdraft").string();
    auto detached=daw::startSave(updated.state(),recovery);
    for(int i=0;i<1000 && !detached->status.load(std::memory_order_acquire);++i)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(detached->status.load(std::memory_order_acquire)==1);
    auto restored=daw::readDraft(recovery);CHECK(restored.revision==2 && restored.tracks[0].gain==-9);
    std::vector<std::shared_ptr<daw::BackgroundJobPermit>> permits;
    for(uint32_t i=0;i<daw::backgroundJobLimit;++i){auto permit=daw::tryAcquireBackgroundJob();CHECK(permit);permits.push_back(std::move(permit));}
    CHECK(daw::backgroundJobsInFlight()==daw::backgroundJobLimit);
    session=daw_create();CHECK(session);CHECK(daw_add_track(session,"Saturated",0)==0);CHECK(!daw_begin_save(session,(directory/"busy.mydawdraft").c_str()));
    char busyError[512]{};daw_error(session,busyError,sizeof(busyError));CHECK(std::string(busyError)=="Background job capacity reached");daw_destroy(session);
    permits.clear();CHECK(daw::backgroundJobsInFlight()==0);
    auto releasedPath=(directory/"released-handle.mydawdraft").string();session=daw_create();CHECK(session);CHECK(daw_add_track(session,"Independent",0)==0);
    job=daw_begin_save(session,releasedPath.c_str());CHECK(job);daw_release_save(job);daw_destroy(session);
    bool releasedComplete=false;for(int i=0;i<1000&&!releasedComplete;++i){try{releasedComplete=daw::readDraft(releasedPath).tracks[0].name=="Independent";}catch(...){std::this_thread::sleep_for(std::chrono::milliseconds(5));}}
    CHECK(releasedComplete&&daw::backgroundJobsInFlight()==0);
    daw_save_status bad{};CHECK(daw_poll_save(nullptr,&bad)==1);daw_release_save(nullptr);
    std::cout<<"PASS: crash atomicity, immutable snapshots, bounded job saturation, explicit busy, released-handle/session lifetime independence\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
