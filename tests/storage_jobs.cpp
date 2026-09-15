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
static bool sameClip(const std::shared_ptr<const daw::Clip>& left,const std::shared_ptr<const daw::Clip>& right){
    return (!left||!right)?left==right:left->samples()==right->samples();
}
static bool sameTrack(const daw::Track& left,const daw::Track& right){
    if(left.id!=right.id||left.name!=right.name||left.gain!=right.gain||!sameClip(left.audio,right.audio)||left.regions!=right.regions||left.pan!=right.pan||left.muted!=right.muted||left.solo!=right.solo||left.baseStart!=right.baseStart||left.outputBus!=right.outputBus||left.sends!=right.sends||left.volumeAutomation!=right.volumeAutomation||left.panAutomation!=right.panAutomation||left.inserts!=right.inserts||left.takes.size()!=right.takes.size())return false;
    for(size_t index=0;index<left.takes.size();++index)if(left.takes[index].name!=right.takes[index].name||left.takes[index].start!=right.takes[index].start||!sameClip(left.takes[index].audio,right.takes[index].audio))return false;
    return true;
}
static bool sameTracks(const std::vector<daw::Track>& left,const std::vector<daw::Track>& right){
    if(left.size()!=right.size())return false;for(size_t index=0;index<left.size();++index)if(!sameTrack(left[index],right[index]))return false;return true;
}
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

    // Deleting a track is a complete State snapshot transition. The snapshot
    // writer must retain every durable field of survivors and omit every row
    // owned by the removed track; Undo then has to restore the exact state
    // before the next Save.
    const auto removeClip=std::make_shared<daw::Clip>(std::vector<float>(960,0.125f));
    const auto keepClip=std::make_shared<daw::Clip>(std::vector<float>(1440,-0.25f));
    const auto removeTake=std::make_shared<daw::Clip>(std::vector<float>(480,0.5f));
    const auto keepTake=std::make_shared<daw::Clip>(std::vector<float>(720,-0.5f));
    daw::Session deleteProject;
    deleteProject.import("Delete me",removeClip,0);
    deleteProject.import("Keep me",keepClip,1);
    deleteProject.addTake(1,"Deleted take",removeTake,24,2);
    deleteProject.addTake(2,"Kept take",keepTake,48,3);
    deleteProject.upsertTrackVolumeAutomation(1,120,-3,4);
    deleteProject.upsertTrackPanAutomation(1,120,-0.3,5);
    deleteProject.addTrackInsert(1,{0,1,2,3,"Deleted insert",false,8,{1,2,3}},6);
    deleteProject.upsertPluginParameterAutomation(daw::PluginOwner::Track,1,3,7,"Deleted parameter",120,0.25,7);
    deleteProject.upsertTrackVolumeAutomation(2,240,-6,8);
    deleteProject.upsertTrackPanAutomation(2,240,0.4,9);
    deleteProject.addTrackInsert(2,{0,4,5,6,"Kept insert",true,16,{4,5,6}},10);
    deleteProject.upsertPluginParameterAutomation(daw::PluginOwner::Track,2,4,9,"Kept parameter",240,0.75,11);
    const auto beforeDelete=deleteProject.state();
    CHECK(beforeDelete.tracks.size()==2&&beforeDelete.nextID==5);
    deleteProject.removeTrack(1,12);
    CHECK(deleteProject.state().revision==13&&deleteProject.state().tracks.size()==1&&sameTrack(deleteProject.state().tracks[0],beforeDelete.tracks[1]));
    const auto deletedPath=(directory/"delete-track.mydawdraft").string();
    daw::writeDraft(deleteProject.state(),deletedPath);
    const auto persistedDelete=daw::readDraft(deletedPath);
    CHECK(persistedDelete.revision==13&&persistedDelete.nextID==beforeDelete.nextID&&persistedDelete.tracks.size()==1&&sameTrack(persistedDelete.tracks[0],beforeDelete.tracks[1]));
    deleteProject.undo(13);
    CHECK(deleteProject.state().revision==14&&sameTracks(deleteProject.state().tracks,beforeDelete.tracks)&&deleteProject.state().nextID==beforeDelete.nextID);
    daw::writeDraft(deleteProject.state(),deletedPath);
    const auto persistedUndo=daw::readDraft(deletedPath);
    CHECK(persistedUndo.revision==14&&persistedUndo.nextID==beforeDelete.nextID&&sameTracks(persistedUndo.tracks,beforeDelete.tracks));

    // Track ordering is durable project state. Moving an owning Track must
    // carry its complete subtree and SQLite position with it, while Undo
    // restores the previous ordering before a subsequent save.
    const auto firstClip=std::make_shared<daw::Clip>(std::vector<float>(960,0.1f));
    const auto secondClip=std::make_shared<daw::Clip>(std::vector<float>(960,0.2f));
    const auto thirdClip=std::make_shared<daw::Clip>(std::vector<float>(960,0.3f));
    const auto firstTake=std::make_shared<daw::Clip>(std::vector<float>(480,-0.1f));
    const auto secondTake=std::make_shared<daw::Clip>(std::vector<float>(480,-0.2f));
    const auto thirdTake=std::make_shared<daw::Clip>(std::vector<float>(480,-0.3f));
    daw::Session reorderProject;
    reorderProject.import("First",firstClip,0);
    reorderProject.import("Second",secondClip,1);
    reorderProject.import("Third",thirdClip,2);
    reorderProject.addTake(1,"First take",firstTake,10,3);
    reorderProject.addTake(2,"Second take",secondTake,20,4);
    reorderProject.addTake(3,"Third take",thirdTake,30,5);
    reorderProject.upsertTrackVolumeAutomation(1,100,-1,6);
    reorderProject.upsertTrackVolumeAutomation(2,100,-2,7);
    reorderProject.upsertTrackVolumeAutomation(3,100,-3,8);
    reorderProject.upsertTrackPanAutomation(1,100,-0.2,9);
    reorderProject.upsertTrackPanAutomation(2,100,0,10);
    reorderProject.upsertTrackPanAutomation(3,100,0.2,11);
    reorderProject.addTrackInsert(1,{0,1,2,3,"First insert",false,4,{1}},12);
    reorderProject.addTrackInsert(2,{0,4,5,6,"Second insert",false,5,{2}},13);
    reorderProject.addTrackInsert(3,{0,7,8,9,"Third insert",true,6,{3}},14);
    reorderProject.upsertPluginParameterAutomation(daw::PluginOwner::Track,1,4,1,"First parameter",100,0.1,15);
    reorderProject.upsertPluginParameterAutomation(daw::PluginOwner::Track,2,5,2,"Second parameter",100,0.2,16);
    reorderProject.upsertPluginParameterAutomation(daw::PluginOwner::Track,3,6,3,"Third parameter",100,0.3,17);
    const auto beforeReorder=reorderProject.state();
    CHECK(beforeReorder.tracks.size()==3&&beforeReorder.nextID==7);
    reorderProject.moveTrack(3,0,18);
    CHECK(reorderProject.state().revision==19&&reorderProject.state().nextID==beforeReorder.nextID&&sameTrack(reorderProject.state().tracks[0],beforeReorder.tracks[2])&&sameTrack(reorderProject.state().tracks[1],beforeReorder.tracks[0])&&sameTrack(reorderProject.state().tracks[2],beforeReorder.tracks[1]));
    const auto reorderedPath=(directory/"reorder-track.mydawdraft").string();
    daw::writeDraft(reorderProject.state(),reorderedPath);
    const auto persistedReorder=daw::readDraft(reorderedPath);
    CHECK(persistedReorder.revision==19&&persistedReorder.nextID==beforeReorder.nextID&&sameTracks(persistedReorder.tracks,reorderProject.state().tracks));
    reorderProject.undo(19);
    CHECK(reorderProject.state().revision==20&&reorderProject.state().nextID==beforeReorder.nextID&&sameTracks(reorderProject.state().tracks,beforeReorder.tracks));
    daw::writeDraft(reorderProject.state(),reorderedPath);
    const auto persistedReorderUndo=daw::readDraft(reorderedPath);
    CHECK(persistedReorderUndo.revision==20&&persistedReorderUndo.nextID==beforeReorder.nextID&&sameTracks(persistedReorderUndo.tracks,beforeReorder.tracks));
    std::cout<<"PASS: crash atomicity, immutable snapshots, bounded job saturation, explicit busy, released-handle/session lifetime independence, delete-track storage roundtrip and undo restore, track-reorder storage roundtrip and undo restore\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
