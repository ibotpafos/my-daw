#include "domain/session.hpp"
#include "audio/export.hpp"
#include "audio/renderer.hpp"
#include "plugins/plugin_descriptor.hpp"
#include "daw.h"
#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <thread>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while(false)
template<class Fn> void rejects(Fn fn) { bool rejected=false; try { fn(); } catch (...) { rejected=true; } CHECK(rejected); }
static void put16(std::vector<unsigned char>& bytes,uint16_t value){bytes.push_back(static_cast<unsigned char>(value));bytes.push_back(static_cast<unsigned char>(value>>8));}
static void put32(std::vector<unsigned char>& bytes,uint32_t value){for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<unsigned char>(value>>shift));}
static uint32_t get32(const std::vector<unsigned char>& bytes,size_t offset){CHECK(offset+4<=bytes.size());return static_cast<uint32_t>(bytes[offset])|(static_cast<uint32_t>(bytes[offset+1])<<8)|(static_cast<uint32_t>(bytes[offset+2])<<16)|(static_cast<uint32_t>(bytes[offset+3])<<24);}
static void writeFixtureWav(const std::filesystem::path& path,uint32_t sampleRate,bool nanSample=false){
    constexpr uint16_t channels=2;const uint16_t bits=nanSample?32:16;const uint16_t format=nanSample?3:1;constexpr uint32_t frames=441;
    const uint32_t bytesPerSample=bits/8;const uint32_t dataBytes=frames*channels*bytesPerSample;
    std::vector<unsigned char> bytes;bytes.reserve(44+dataBytes);
    bytes.insert(bytes.end(),{'R','I','F','F'});put32(bytes,36+dataBytes);bytes.insert(bytes.end(),{'W','A','V','E'});
    bytes.insert(bytes.end(),{'f','m','t',' '});put32(bytes,16);put16(bytes,format);put16(bytes,channels);put32(bytes,sampleRate);put32(bytes,sampleRate*channels*bytesPerSample);put16(bytes,channels*bytesPerSample);put16(bytes,bits);
    bytes.insert(bytes.end(),{'d','a','t','a'});put32(bytes,dataBytes);
    for(uint32_t frame=0;frame<frames;++frame)for(uint16_t channel=0;channel<channels;++channel){
        if(nanSample){const float sample=frame==0&&channel==0?std::numeric_limits<float>::quiet_NaN():0.125f;uint32_t encoded=0;std::memcpy(&encoded,&sample,sizeof(encoded));put32(bytes,encoded);}
        else put16(bytes,static_cast<uint16_t>((static_cast<int>(frame%64)-32)*768));
    }
    std::ofstream stream(path,std::ios::binary);CHECK(stream.good());stream.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));CHECK(stream.good());
}
static daw_import_status waitForImport(daw_import_job* job){
    daw_import_status status{};status.struct_size=sizeof(status);
    for(int attempt=0;attempt<5000;++attempt){
        CHECK(daw_poll_import(job,&status)==0);
        if(status.status!=DAW_IMPORT_RUNNING)return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Timed out waiting for async import");
}
int main() { try {
    daw::Session s;
    s.add("Вокал 🎙",0); s.gain(1,-6,1); s.rename(1,"Lead",2);
    rejects([&]{s.gain(1,0,1);}); CHECK(s.state().revision==3);
    rejects([&]{s.gain(1,std::numeric_limits<double>::quiet_NaN(),3);});
    rejects([&]{s.gain(99,0,3);}); rejects([&]{s.rename(1,"",3);});
    rejects([&]{s.rename(1,"\xED\xA0\x80",3);}); rejects([&]{s.rename(1,"\xC0\xAF",3);});
    CHECK(s.state().revision==3); s.undo(3); CHECK(s.state().tracks[0].name=="Вокал 🎙");
    s.undo(4); CHECK(s.state().tracks[0].gain==0); s.redo(5); CHECK(s.state().tracks[0].gain==-6);
    s.add("Double",6); CHECK(!s.canRedo()); s.undo(7); s.add("New double",8); CHECK(s.state().tracks.back().id==3);
    daw::Session mix;mix.add("Mix",0);mix.pan(1,-0.25,1);mix.mute(1,true,2);mix.solo(1,true,3);mix.masterGain(-6,4);
    CHECK(mix.state().revision==5&&mix.state().tracks[0].pan==-0.25&&mix.state().tracks[0].muted&&mix.state().tracks[0].solo&&mix.state().masterGain==-6);
    mix.pan(1,-0.25,5);CHECK(mix.state().revision==5);rejects([&]{mix.pan(1,1.01,5);});rejects([&]{mix.masterGain(std::numeric_limits<double>::infinity(),5);});
    mix.undo(5);CHECK(mix.state().masterGain==0);mix.redo(6);CHECK(mix.state().masterGain==-6);
    // Deleting a track is one atomic snapshot mutation. Its complete owned
    // strip disappears from the render graph, while Undo restores the exact
    // immutable media-backed Track without consuming another ID.
    daw::Session deletedTrack;
    auto deleteClip=std::make_shared<daw::Clip>(std::vector<float>{0.5f,0.5f,0.25f,0.25f,-0.25f,-0.25f,-0.5f,-0.5f});
    deletedTrack.import("Disposable audio",deleteClip,0);
    deletedTrack.addBus("Disposable bus",1);
    deletedTrack.addTake(1,"Alternate take",deleteClip,0,2);
    deletedTrack.compRange(1,1,0,2,3);
    deletedTrack.upsertSend(1,2,-9,true,4);
    deletedTrack.upsertTrackVolumeAutomation(1,0,-3,5);
    deletedTrack.upsertTrackPanAutomation(1,0,0.25,6);
    deletedTrack.addTrackInsert(1,{0,1,2,3,"Disposable insert",false,0,{}},7);
    deletedTrack.upsertPluginParameterAutomation(daw::PluginOwner::Track,1,3,42,"Cutoff",0,0.5,8);
    auto silentClip=std::make_shared<daw::Clip>(std::vector<float>(8,0));
    deletedTrack.import("Other strip",silentClip,9);
    const auto deletedOriginal=deletedTrack.state().tracks[0];const auto retainedBus=deletedTrack.state().buses[0];const auto deletedNextID=deletedTrack.state().nextID;
    daw::Renderer beforeDeleteRenderer;beforeDeleteRenderer.prepare(deletedTrack.state());beforeDeleteRenderer.playing.store(true);
    std::array<float,4> renderedLeft{},renderedRight{};beforeDeleteRenderer.render(renderedLeft.data(),renderedRight.data(),4);CHECK(std::any_of(renderedLeft.begin(),renderedLeft.end(),[](float value){return value!=0;}));
    deletedTrack.removeTrack(1,10);CHECK(deletedTrack.state().revision==11&&deletedTrack.state().tracks.size()==1&&deletedTrack.state().tracks[0].id==4&&deletedTrack.state().buses[0]==retainedBus&&deletedTrack.state().nextID==deletedNextID);
    daw::Renderer deletedRenderer;deletedRenderer.prepare(deletedTrack.state());deletedRenderer.playing.store(true);renderedLeft.fill(1);renderedRight.fill(1);deletedRenderer.render(renderedLeft.data(),renderedRight.data(),4);CHECK(std::all_of(renderedLeft.begin(),renderedLeft.end(),[](float value){return value==0;})&&std::all_of(renderedRight.begin(),renderedRight.end(),[](float value){return value==0;}));
    deletedTrack.undo(11);CHECK(deletedTrack.state().tracks.size()==2&&deletedTrack.state().tracks[0]==deletedOriginal&&deletedTrack.state().buses[0]==retainedBus&&deletedTrack.state().nextID==deletedNextID);
    daw::Renderer restoredRenderer;restoredRenderer.prepare(deletedTrack.state());restoredRenderer.playing.store(true);renderedLeft.fill(0);renderedRight.fill(0);restoredRenderer.render(renderedLeft.data(),renderedRight.data(),4);CHECK(std::any_of(renderedLeft.begin(),renderedLeft.end(),[](float value){return value!=0;}));
    deletedTrack.redo(12);CHECK(deletedTrack.state().tracks.size()==1&&deletedTrack.state().tracks[0].id==4&&deletedTrack.state().buses[0]==retainedBus&&deletedTrack.state().nextID==deletedNextID);
    // Reordering has final-index semantics: moving a track to index N places
    // it at N in the resulting order and preserves its complete owner subtree.
    daw::Session reorderedTrack;
    auto reorderClip=std::make_shared<daw::Clip>(std::vector<float>{0.75f,0.5f,0.25f,0.0f,-0.25f,-0.5f,-0.75f,-1.0f});
    reorderedTrack.import("Detailed strip",reorderClip,0); // track 1, revision 1
    reorderedTrack.addBus("Reorder bus",1); // bus 2, revision 2
    reorderedTrack.addTake(1,"Reorder alternate",reorderClip,0,2);
    reorderedTrack.upsertSend(1,2,-9,true,3);
    reorderedTrack.upsertTrackVolumeAutomation(1,0,-3,4);
    reorderedTrack.upsertTrackPanAutomation(1,0,0.25,5);
    reorderedTrack.addTrackInsert(1,{0,1,2,3,"Reorder insert",false,0,{}},6);
    reorderedTrack.add("Middle strip",7); // track 4
    reorderedTrack.add("Last strip",8); // track 5
    const auto reorderOriginal=reorderedTrack.state();
    daw::Renderer beforeReorderRenderer;beforeReorderRenderer.prepare(reorderOriginal);beforeReorderRenderer.playing.store(true);
    std::array<float,4> beforeReorderLeft{},beforeReorderRight{};beforeReorderRenderer.render(beforeReorderLeft.data(),beforeReorderRight.data(),4);
    reorderedTrack.moveTrack(1,2,9);
    CHECK(reorderedTrack.state().revision==10&&reorderedTrack.state().nextID==reorderOriginal.nextID&&reorderedTrack.state().tracks.size()==3);
    CHECK(reorderedTrack.state().tracks[0].id==4&&reorderedTrack.state().tracks[1].id==5&&reorderedTrack.state().tracks[2]==reorderOriginal.tracks[0]);
    daw::Renderer afterReorderRenderer;afterReorderRenderer.prepare(reorderedTrack.state());afterReorderRenderer.playing.store(true);
    std::array<float,4> afterReorderLeft{},afterReorderRight{};afterReorderRenderer.render(afterReorderLeft.data(),afterReorderRight.data(),4);
    CHECK(beforeReorderLeft==afterReorderLeft&&beforeReorderRight==afterReorderRight);
    rejects([&]{reorderedTrack.moveTrack(1,0,9);}); // stale
    rejects([&]{reorderedTrack.moveTrack(99,0,10);});
    rejects([&]{reorderedTrack.moveTrack(1,3,10);});
    reorderedTrack.moveTrack(1,2,10);CHECK(reorderedTrack.state().revision==10); // same-position no-op
    reorderedTrack.undo(10);CHECK(reorderedTrack.state().revision==11&&reorderedTrack.state().tracks==reorderOriginal.tracks&&reorderedTrack.state().nextID==reorderOriginal.nextID);
    reorderedTrack.redo(11);CHECK(reorderedTrack.state().revision==12&&reorderedTrack.state().tracks[2]==reorderOriginal.tracks[0]&&reorderedTrack.state().nextID==reorderOriginal.nextID);
    daw::Session routing;routing.add("Lead",0);routing.addBus("Vocal Bus",1);routing.addBus("FX Return",2);routing.routeTrack(1,2,3);routing.routeBus(2,3,4);routing.upsertSend(1,3,-12,false,5);
    CHECK(routing.state().revision==6&&routing.state().tracks[0].outputBus==2&&routing.state().tracks[0].sends==std::vector<daw::Send>({{3,-12,false}}));
    rejects([&]{routing.routeBus(3,2,6);});CHECK(routing.state().revision==6&&routing.state().buses[1].outputBus==0);
    routing.busGain(2,-3,6);routing.busPan(3,0.25,7);routing.busMute(3,true,8);CHECK(routing.state().revision==9);routing.undo(9);CHECK(!routing.state().buses[1].muted);routing.redo(10);CHECK(routing.state().buses[1].muted);
    rejects([&]{routing.upsertSend(1,99,0,false,11);});rejects([&]{routing.routeTrack(1,99,11);});
    routing.addMasterInsert({0,1,2,3,"Test effect",false,128,{1,2,3}},11);CHECK(routing.state().revision==12&&routing.state().masterInserts[0].id==4);routing.bypassMasterInsert(4,true,12);CHECK(routing.state().masterInserts[0].bypassed);routing.undo(13);CHECK(!routing.state().masterInserts[0].bypassed);routing.redo(14);CHECK(routing.state().masterInserts[0].bypassed);
    auto before=s.state();
    std::string pattern=(std::filesystem::temp_directory_path()/"mydaw-tests-XXXXXX").string();
    std::vector<char> bytes(pattern.begin(),pattern.end()); bytes.push_back(0);
    char* dir=mkdtemp(bytes.data()); CHECK(dir);
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::filesystem::remove_all(path);} } cleanup{dir};
    auto path=(cleanup.path/"Сессия.mydawdraft").string();
    auto routingPath=(cleanup.path/"Routing.mydawdraft").string();daw::writeDraft(routing.state(),routingPath);auto loadedRouting=daw::readDraft(routingPath);CHECK(loadedRouting.tracks==routing.state().tracks&&loadedRouting.buses==routing.state().buses&&loadedRouting.masterInserts==routing.state().masterInserts);
    auto mixPath=(cleanup.path/"Микс.mydawdraft").string();daw::writeDraft(mix.state(),mixPath);auto loadedMix=daw::readDraft(mixPath);CHECK(loadedMix.tracks==mix.state().tracks&&loadedMix.masterGain==-6);
    sqlite3* mixDb=nullptr;CHECK(sqlite3_open(mixPath.c_str(),&mixDb)==SQLITE_OK);CHECK(sqlite3_exec(mixDb,"PRAGMA user_version=6;",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(mixDb);auto version6=daw::readDraft(mixPath);CHECK(version6.masterGain==0&&version6.tracks[0].pan==0&&!version6.tracks[0].muted&&!version6.tracks[0].solo);
    daw::writeDraft(mix.state(),mixPath);CHECK(sqlite3_open(mixPath.c_str(),&mixDb)==SQLITE_OK);CHECK(sqlite3_exec(mixDb,"UPDATE metadata SET master_gain=25",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(mixDb);rejects([&]{daw::readDraft(mixPath);});
    daw::writeDraft(mix.state(),mixPath);CHECK(sqlite3_open(mixPath.c_str(),&mixDb)==SQLITE_OK);CHECK(sqlite3_exec(mixDb,"UPDATE tracks SET pan=2 WHERE position=0",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(mixDb);rejects([&]{daw::readDraft(mixPath);});
    daw::writeDraft(s.state(),path); auto loaded=daw::readDraft(path);
    CHECK(loaded.tracks==before.tracks && loaded.revision==before.revision && loaded.nextID==before.nextID);

    // v15 persists hosting policy for every insert owner.  The v14 reader
    // deliberately ignores the new column, so old projects always reopen as
    // in-process even when they contain the otherwise identical plugin rows.
    daw::Session hosting;hosting.add("Hosting track",0);hosting.addBus("Hosting bus",1);
    hosting.addTrackInsert(1,{0,1,2,3,"Track AU",false,0,{1}},2);
    hosting.addBusInsert(2,{0,4,5,6,"Bus AU",false,0,{2}},3);
    hosting.addMasterInsert({0,7,8,9,"Master AU",false,0,{3}},4);
    hosting.setTrackInsertHostingMode(1,3,daw::PluginHostingMode::OutOfProcess,5);
    hosting.setBusInsertHostingMode(2,4,daw::PluginHostingMode::OutOfProcess,6);
    hosting.setMasterInsertHostingMode(5,daw::PluginHostingMode::OutOfProcess,7);
    auto hostingPath=(cleanup.path/"hosting-v15.mydawdraft").string();daw::writeDraft(hosting.state(),hostingPath);
    sqlite3* hostingDb=nullptr;CHECK(sqlite3_open(hostingPath.c_str(),&hostingDb)==SQLITE_OK);sqlite3_stmt* versionStatement=nullptr;CHECK(sqlite3_prepare_v2(hostingDb,"PRAGMA user_version",-1,&versionStatement,nullptr)==SQLITE_OK);CHECK(sqlite3_step(versionStatement)==SQLITE_ROW&&sqlite3_column_int(versionStatement,0)==17);sqlite3_finalize(versionStatement);sqlite3_close(hostingDb);
    auto hostingRoundTrip=daw::readDraft(hostingPath);CHECK(hostingRoundTrip.tracks[0].inserts[0].hostingMode==daw::PluginHostingMode::OutOfProcess&&hostingRoundTrip.buses[0].inserts[0].hostingMode==daw::PluginHostingMode::OutOfProcess&&hostingRoundTrip.masterInserts[0].hostingMode==daw::PluginHostingMode::OutOfProcess);
    auto legacyHostingPath=(cleanup.path/"hosting-v14.mydawdraft").string();daw::writeDraft(hosting.state(),legacyHostingPath);CHECK(sqlite3_open(legacyHostingPath.c_str(),&hostingDb)==SQLITE_OK);CHECK(sqlite3_exec(hostingDb,"BEGIN; ALTER TABLE channel_plugins RENAME TO channel_plugins_v15; CREATE TABLE channel_plugins(owner_kind INTEGER NOT NULL CHECK(owner_kind IN(1,2,3)), owner_id INTEGER NOT NULL, position INTEGER NOT NULL, id INTEGER UNIQUE NOT NULL, type INTEGER NOT NULL, subtype INTEGER NOT NULL, manufacturer INTEGER NOT NULL, name TEXT NOT NULL, bypassed INTEGER NOT NULL CHECK(bypassed IN(0,1)), latency_frames INTEGER NOT NULL, state BLOB NOT NULL, PRIMARY KEY(owner_kind,owner_id,position)); INSERT INTO channel_plugins SELECT owner_kind,owner_id,position,id,type,subtype,manufacturer,name,bypassed,latency_frames,state FROM channel_plugins_v15; DROP TABLE channel_plugins_v15; PRAGMA user_version=14; COMMIT;",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(hostingDb);
    auto legacyHosting=daw::readDraft(legacyHostingPath);CHECK(legacyHosting.tracks[0].inserts[0].hostingMode==daw::PluginHostingMode::InProcess&&legacyHosting.buses[0].inserts[0].hostingMode==daw::PluginHostingMode::InProcess&&legacyHosting.masterInserts[0].hostingMode==daw::PluginHostingMode::InProcess);

    // The bridge must report policy per format and reject unavailable isolation
    // requests without advancing the durable project revision.
    daw::Session bridgeHosting;bridgeHosting.add("Bridge hosting",0);
    bridgeHosting.addMasterInsert({0,11,12,13,"Bridge AU",false,0,{4}},1);
    daw::Vst3StateEnvelope vst3Envelope;vst3Envelope.descriptor.format=daw::PluginFormat::VST3;vst3Envelope.descriptor.vst3ClassFuid[0]=1;vst3Envelope.descriptor.modulePath="/fixture/Bridge.vst3";vst3Envelope.descriptor.vendor="Fixture";vst3Envelope.descriptor.version="1";vst3Envelope.descriptor.fingerprint="fixture";
    bridgeHosting.addMasterInsert({0,0,0,0,"Bridge VST3",false,0,daw::encodeVst3StateEnvelope(vst3Envelope)},2);
    auto bridgeHostingPath=(cleanup.path/"bridge-hosting.mydawdraft").string();daw::writeDraft(bridgeHosting.state(),bridgeHostingPath);
    std::unique_ptr<daw_session,decltype(&daw_destroy)> hostingBridge(daw_create(),daw_destroy);CHECK(hostingBridge&&daw_open_draft(hostingBridge.get(),bridgeHostingPath.c_str())==0);
    daw_snapshot hostingSnapshot{};hostingSnapshot.struct_size=sizeof(hostingSnapshot);CHECK(daw_get_snapshot(hostingBridge.get(),&hostingSnapshot)==0&&hostingSnapshot.revision==3);
    daw_insert_hosting_status auHosting{};auHosting.struct_size=sizeof(auHosting);CHECK(daw_get_insert_hosting_status(hostingBridge.get(),DAW_INSERT_OWNER_MASTER,0,2,&auHosting)==0&&auHosting.version==DAW_INSERT_HOSTING_STATUS_VERSION&&auHosting.format==DAW_INSERT_HOSTING_FORMAT_AUV2&&auHosting.selected_mode==DAW_INSERT_HOSTING_MODE_IN_PROCESS&&auHosting.supported_modes==DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS);
    CHECK(daw_set_insert_hosting_mode(hostingBridge.get(),DAW_INSERT_OWNER_MASTER,0,2,DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS,hostingSnapshot.revision)==1);daw_snapshot unchangedHostingSnapshot{};unchangedHostingSnapshot.struct_size=sizeof(unchangedHostingSnapshot);CHECK(daw_get_snapshot(hostingBridge.get(),&unchangedHostingSnapshot)==0&&unchangedHostingSnapshot.revision==hostingSnapshot.revision);
    daw_insert_hosting_status vst3Hosting{};vst3Hosting.struct_size=sizeof(vst3Hosting);CHECK(daw_get_insert_hosting_status(hostingBridge.get(),DAW_INSERT_OWNER_MASTER,0,3,&vst3Hosting)==0&&vst3Hosting.version==DAW_INSERT_HOSTING_STATUS_VERSION&&vst3Hosting.format==DAW_INSERT_HOSTING_FORMAT_VST3&&vst3Hosting.selected_mode==DAW_INSERT_HOSTING_MODE_IN_PROCESS);
    static_assert(sizeof(daw_insert_hosting_status)==20);static_assert(sizeof(daw_insert_runtime_status)==20);
    daw_insert_runtime_status runtimeBad{};runtimeBad.struct_size=sizeof(runtimeBad)-1;CHECK(daw_get_insert_runtime_status(hostingBridge.get(),DAW_INSERT_OWNER_MASTER,0,3,&runtimeBad)==1);
    daw_insert_runtime_status runtime{};runtime.struct_size=sizeof(runtime);CHECK(daw_get_insert_runtime_status(hostingBridge.get(),DAW_INSERT_OWNER_MASTER,0,3,&runtime)==0&&runtime.version==DAW_INSERT_RUNTIME_STATUS_VERSION&&runtime.state==DAW_INSERT_RUNTIME_UNPREPARED&&runtime.fault_code==DAW_INSERT_RUNTIME_FAULT_RESTART_REQUIRED&&runtime.extra_pipeline_latency_frames==0);daw_snapshot runtimeSnapshot{};runtimeSnapshot.struct_size=sizeof(runtimeSnapshot);CHECK(daw_get_snapshot(hostingBridge.get(),&runtimeSnapshot)==0&&runtimeSnapshot.revision==hostingSnapshot.revision);
    daw_insert_hosting_status unchangedHosting{};unchangedHosting.struct_size=sizeof(unchangedHosting);CHECK(daw_get_insert_hosting_status(hostingBridge.get(),DAW_INSERT_OWNER_MASTER,0,3,&unchangedHosting)==0&&unchangedHosting.format==vst3Hosting.format&&unchangedHosting.selected_mode==vst3Hosting.selected_mode&&unchangedHosting.supported_modes==vst3Hosting.supported_modes);
    const bool vst3Oop=(vst3Hosting.supported_modes&DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS)!=0;const auto runtimeRevision=unchangedHostingSnapshot.revision;const int runtimeSet=daw_set_insert_hosting_mode(hostingBridge.get(),DAW_INSERT_OWNER_MASTER,0,3,DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS,runtimeRevision);if(vst3Oop){CHECK(runtimeSet==0);unchangedHostingSnapshot.struct_size=sizeof(unchangedHostingSnapshot);CHECK(daw_get_snapshot(hostingBridge.get(),&unchangedHostingSnapshot)==0&&unchangedHostingSnapshot.revision==runtimeRevision+1);}else{CHECK(runtimeSet==1);unchangedHostingSnapshot.struct_size=sizeof(unchangedHostingSnapshot);CHECK(daw_get_snapshot(hostingBridge.get(),&unchangedHostingSnapshot)==0&&unchangedHostingSnapshot.revision==runtimeRevision);}
    // A persisted AU OOP policy has no remote parameter proxy. The generic
    // API must reject it instead of attempting an in-process AU instantiation.
    CHECK(daw_open_draft(hostingBridge.get(),hostingPath.c_str())==0);uint32_t isolatedAuParameterCount=0;CHECK(daw_get_insert_parameter_count(hostingBridge.get(),DAW_INSERT_OWNER_TRACK,1,3,&isolatedAuParameterCount)==1);daw_snapshot isolatedAuSnapshot{};isolatedAuSnapshot.struct_size=sizeof(isolatedAuSnapshot);CHECK(daw_get_snapshot(hostingBridge.get(),&isolatedAuSnapshot)==0&&isolatedAuSnapshot.revision==hosting.state().revision);
    sqlite3* legacyDb=nullptr; CHECK(sqlite3_open(path.c_str(),&legacyDb)==SQLITE_OK);
    CHECK(sqlite3_exec(legacyDb,"PRAGMA user_version=3; ALTER TABLE tracks ADD COLUMN clip_start INTEGER NOT NULL DEFAULT 0; ALTER TABLE tracks ADD COLUMN source_offset INTEGER NOT NULL DEFAULT 0; ALTER TABLE tracks ADD COLUMN clip_length INTEGER NOT NULL DEFAULT 0;",nullptr,nullptr,nullptr)==SQLITE_OK); sqlite3_close(legacyDb);
    auto legacyEmpty=daw::readDraft(path); CHECK(legacyEmpty.tracks.size()==2 && legacyEmpty.tracks[0].regions.empty());
    daw::writeDraft(s.state(),path);
    s.gain(1,-12,s.state().revision); daw::writeDraft(s.state(),path); CHECK(daw::readDraft(path).tracks[0].gain==-12);
    rejects([&]{daw::writeDraft(s.state(),(cleanup.path/"missing"/"file").string());});
    CHECK(daw::readDraft(path).tracks[0].gain==-12);
    std::unique_ptr<daw_session,decltype(&daw_destroy)> bridge(daw_create(),daw_destroy); CHECK(bridge);
    CHECK(daw_open_draft(bridge.get(),path.c_str())==0);
    daw_snapshot snap{}; snap.struct_size=sizeof(snap); CHECK(daw_get_snapshot(bridge.get(),&snap)==0);
    CHECK(snap.revision==s.state().revision && !snap.can_undo);
    daw_export_options automaticTail{};automaticTail.struct_size=sizeof(automaticTail);automaticTail.version=DAW_EXPORT_OPTIONS_VERSION;automaticTail.tail_mode=DAW_EXPORT_TAIL_AUTOMATIC;
    daw_export_tail_summary tailSummary{};tailSummary.struct_size=sizeof(tailSummary);CHECK(daw_get_export_tail_summary(bridge.get(),&automaticTail,&tailSummary)==1);
    daw_snapshot tailSnapshot{};tailSnapshot.struct_size=sizeof(tailSnapshot);CHECK(daw_get_snapshot(bridge.get(),&tailSnapshot)==0&&tailSnapshot.revision==snap.revision);
    daw_export_options noTail=automaticTail;noTail.tail_mode=DAW_EXPORT_TAIL_NONE;tailSummary={};tailSummary.struct_size=sizeof(tailSummary);CHECK(daw_get_export_tail_summary(bridge.get(),&noTail,&tailSummary)==1);
    daw_export_options manualTail=automaticTail;manualTail.tail_mode=DAW_EXPORT_TAIL_MANUAL_LIMIT;manualTail.manual_tail_frames=48000;tailSummary={};tailSummary.struct_size=sizeof(tailSummary);CHECK(daw_get_export_tail_summary(bridge.get(),&manualTail,&tailSummary)==1);
    daw_export_options badTail=automaticTail;badTail.tail_mode=99;CHECK(daw_get_export_tail_summary(bridge.get(),&badTail,&tailSummary)==1);badTail=manualTail;badTail.manual_tail_frames=48000*30+1;CHECK(daw_get_export_tail_summary(bridge.get(),&badTail,&tailSummary)==1);badTail=automaticTail;badTail.struct_size=sizeof(badTail)-1;CHECK(daw_get_export_tail_summary(bridge.get(),&badTail,&tailSummary)==1);tailSummary.struct_size=sizeof(tailSummary)-1;CHECK(daw_get_export_tail_summary(bridge.get(),&automaticTail,&tailSummary)==1);tailSummary.struct_size=sizeof(tailSummary);tailSnapshot={};tailSnapshot.struct_size=sizeof(tailSnapshot);CHECK(daw_get_snapshot(bridge.get(),&tailSnapshot)==0&&tailSnapshot.revision==snap.revision);
    CHECK(daw_set_gain(bridge.get(),1,100,snap.revision)==1);
    char error[512]; daw_error(bridge.get(),error,sizeof(error)); CHECK(error[0]);
    CHECK(daw_open_draft(bridge.get(),"/nonexistent/mydawdraft")==1);
    CHECK(daw_get_snapshot(bridge.get(),&snap)==0 && snap.track_count==2);
    daw_track t{}; t.struct_size=sizeof(t); CHECK(daw_get_track(bridge.get(),0,&t)==0 && t.gain_db==-12);
    CHECK(daw_set_pan(bridge.get(),1,0.5,snap.revision)==0);CHECK(daw_set_mute(bridge.get(),1,1,snap.revision+1)==0);CHECK(daw_set_solo(bridge.get(),1,1,snap.revision+2)==0);CHECK(daw_set_master_gain(bridge.get(),-3,snap.revision+3)==0);
    snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0&&snap.master_gain_db==-3);t.struct_size=sizeof(t);CHECK(daw_get_track(bridge.get(),0,&t)==0&&t.pan==0.5&&t.muted==1&&t.solo==1);CHECK(daw_set_mute(bridge.get(),1,2,snap.revision)==1);
    CHECK(daw_add_bus(bridge.get(),"Mix Bus",snap.revision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0&&snap.bus_count==1);daw_bus bus{};bus.struct_size=sizeof(bus);CHECK(daw_get_bus(bridge.get(),0,&bus)==0&&std::string(bus.name)=="Mix Bus");
    CHECK(daw_set_track_output(bridge.get(),1,bus.id,snap.revision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0);CHECK(daw_upsert_send(bridge.get(),1,bus.id,-9,0,snap.revision)==0);t.struct_size=sizeof(t);CHECK(daw_get_track(bridge.get(),0,&t)==0&&t.output_bus_id==bus.id&&t.send_count==1);daw_send send{};send.struct_size=sizeof(send);CHECK(daw_get_send(bridge.get(),1,0,&send)==0&&send.bus_id==bus.id&&send.gain_db==-9);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0);CHECK(daw_set_bus_output(bridge.get(),bus.id,bus.id,snap.revision)==1);
#ifdef __APPLE__
    uint32_t auCount=0;CHECK(daw_scan_supported_au(bridge.get(),&auCount)==0&&auCount==3);daw_au_component component{};component.struct_size=sizeof(component);CHECK(daw_get_supported_au(bridge.get(),0,&component)==0&&component.manufacturer);CHECK(daw_add_master_au(bridge.get(),component.type,component.subtype,component.manufacturer,snap.revision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0&&snap.master_insert_count==1);daw_plugin plugin{};plugin.struct_size=sizeof(plugin);CHECK(daw_get_master_insert(bridge.get(),0,&plugin)==0&&plugin.id&&std::string(plugin.name).find("Apple")!=std::string::npos);CHECK(daw_set_master_insert_bypass(bridge.get(),plugin.id,1,snap.revision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0);CHECK(daw_remove_master_insert(bridge.get(),plugin.id,snap.revision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0&&snap.master_insert_count==0);
    // The approved catalogue is machine-independent but an AUv3 component is
    // not.  For every available native component, enforce the public policy:
    // only an AUv3 may advertise and accept the isolated-hosting flag.
    for(uint32_t index=0;index<auCount;++index){component={};component.struct_size=sizeof(component);CHECK(daw_get_supported_au(bridge.get(),index,&component)==0);CHECK(daw_add_master_au(bridge.get(),component.type,component.subtype,component.manufacturer,snap.revision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0);plugin={};plugin.struct_size=sizeof(plugin);CHECK(daw_get_master_insert(bridge.get(),snap.master_insert_count-1,&plugin)==0);daw_insert_hosting_status status{};status.struct_size=sizeof(status);CHECK(daw_get_insert_hosting_status(bridge.get(),DAW_INSERT_OWNER_MASTER,0,plugin.id,&status)==0&&status.version==DAW_INSERT_HOSTING_STATUS_VERSION);const bool auv3=status.format==DAW_INSERT_HOSTING_FORMAT_AUV3;CHECK((((status.supported_modes&DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS)!=0)==auv3));if(auv3){const auto beforeRevision=snap.revision;CHECK(daw_set_insert_hosting_mode(bridge.get(),DAW_INSERT_OWNER_MASTER,0,plugin.id,DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS,beforeRevision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0&&snap.revision==beforeRevision+1);status={};status.struct_size=sizeof(status);CHECK(daw_get_insert_hosting_status(bridge.get(),DAW_INSERT_OWNER_MASTER,0,plugin.id,&status)==0&&status.selected_mode==DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS);CHECK(daw_set_insert_hosting_mode(bridge.get(),DAW_INSERT_OWNER_MASTER,0,plugin.id,DAW_INSERT_HOSTING_MODE_IN_PROCESS,snap.revision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0);}else{const auto beforeRevision=snap.revision;CHECK(daw_set_insert_hosting_mode(bridge.get(),DAW_INSERT_OWNER_MASTER,0,plugin.id,DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS,beforeRevision)==1);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0&&snap.revision==beforeRevision);}CHECK(daw_remove_master_insert(bridge.get(),plugin.id,snap.revision)==0);snap.struct_size=sizeof(snap);CHECK(daw_get_snapshot(bridge.get(),&snap)==0);}
#endif
    daw_track bad{}; CHECK(daw_get_track(bridge.get(),0,&bad)==1);
    CHECK(daw_add_track(bridge.get(),nullptr,snap.revision)==1);
    sqlite3* db=nullptr; CHECK(sqlite3_open(path.c_str(),&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"UPDATE tracks SET gain=200 WHERE position=0",nullptr,nullptr,nullptr)==SQLITE_OK); sqlite3_close(db);
    rejects([&]{daw::readDraft(path);}); CHECK(daw_open_draft(bridge.get(),path.c_str())==1);
    CHECK(daw_get_track(bridge.get(),0,&t)==0 && t.gain_db==-12);

    // The C bridge preserves the optimistic revision contract: failed stale
    // or missing deletes cannot invalidate output, while a successful delete
    // produces one revision and remains exactly undoable/redoable.
    std::unique_ptr<daw_session,decltype(&daw_destroy)> deleteBridge(daw_create(),daw_destroy);CHECK(deleteBridge);
    daw_snapshot deleteSnapshot{};deleteSnapshot.struct_size=sizeof(deleteSnapshot);CHECK(daw_get_snapshot(deleteBridge.get(),&deleteSnapshot)==0&&deleteSnapshot.revision==0);
    CHECK(daw_add_track(deleteBridge.get(),"Remove me",0)==0);CHECK(daw_add_track(deleteBridge.get(),"Keep me",1)==0);
    daw_track removable{};removable.struct_size=sizeof(removable);daw_track retained{};retained.struct_size=sizeof(retained);CHECK(daw_get_track(deleteBridge.get(),0,&removable)==0&&daw_get_track(deleteBridge.get(),1,&retained)==0);
    CHECK(daw_remove_track(deleteBridge.get(),removable.id,1)==1);CHECK(daw_remove_track(deleteBridge.get(),999,2)==1);
    deleteSnapshot.struct_size=sizeof(deleteSnapshot);CHECK(daw_get_snapshot(deleteBridge.get(),&deleteSnapshot)==0&&deleteSnapshot.revision==2&&deleteSnapshot.track_count==2);
    CHECK(daw_remove_track(deleteBridge.get(),removable.id,2)==0);deleteSnapshot.struct_size=sizeof(deleteSnapshot);CHECK(daw_get_snapshot(deleteBridge.get(),&deleteSnapshot)==0&&deleteSnapshot.revision==3&&deleteSnapshot.track_count==1);
    retained={};retained.struct_size=sizeof(retained);CHECK(daw_get_track(deleteBridge.get(),0,&retained)==0&&retained.id==2&&std::string(retained.name)=="Keep me");
    CHECK(daw_undo(deleteBridge.get(),3)==0);deleteSnapshot.struct_size=sizeof(deleteSnapshot);CHECK(daw_get_snapshot(deleteBridge.get(),&deleteSnapshot)==0&&deleteSnapshot.revision==4&&deleteSnapshot.track_count==2);
    removable={};removable.struct_size=sizeof(removable);CHECK(daw_get_track(deleteBridge.get(),0,&removable)==0&&removable.id==1&&std::string(removable.name)=="Remove me");
    CHECK(daw_redo(deleteBridge.get(),4)==0);deleteSnapshot.struct_size=sizeof(deleteSnapshot);CHECK(daw_get_snapshot(deleteBridge.get(),&deleteSnapshot)==0&&deleteSnapshot.revision==5&&deleteSnapshot.track_count==1);
    CHECK(daw_add_track(deleteBridge.get(),"New ID",5)==0);retained={};retained.struct_size=sizeof(retained);CHECK(daw_get_track(deleteBridge.get(),1,&retained)==0&&retained.id==3);

    // The C bridge exposes the same optimistic final-index contract, including
    // a successful no-op that leaves the session revision unchanged.
    std::unique_ptr<daw_session,decltype(&daw_destroy)> moveBridge(daw_create(),daw_destroy);CHECK(moveBridge);
    CHECK(daw_add_track(moveBridge.get(),"First",0)==0);CHECK(daw_add_track(moveBridge.get(),"Second",1)==0);CHECK(daw_add_track(moveBridge.get(),"Third",2)==0);
    daw_track first{};first.struct_size=sizeof(first);daw_track second{};second.struct_size=sizeof(second);daw_track third{};third.struct_size=sizeof(third);
    CHECK(daw_get_track(moveBridge.get(),0,&first)==0&&daw_get_track(moveBridge.get(),1,&second)==0&&daw_get_track(moveBridge.get(),2,&third)==0);
    const auto firstID=first.id,secondID=second.id,thirdID=third.id;
    CHECK(daw_move_track(moveBridge.get(),first.id,2,2)==1); // stale revision
    CHECK(daw_move_track(moveBridge.get(),first.id,3,3)==1); // final index must exist
    CHECK(daw_move_track(moveBridge.get(),99,0,3)==1);
    CHECK(daw_move_track(moveBridge.get(),first.id,2,3)==0);
    daw_snapshot moveSnapshot{};moveSnapshot.struct_size=sizeof(moveSnapshot);CHECK(daw_get_snapshot(moveBridge.get(),&moveSnapshot)==0&&moveSnapshot.revision==4&&moveSnapshot.track_count==3);
    first={};first.struct_size=sizeof(first);second={};second.struct_size=sizeof(second);third={};third.struct_size=sizeof(third);CHECK(daw_get_track(moveBridge.get(),0,&first)==0&&daw_get_track(moveBridge.get(),1,&second)==0&&daw_get_track(moveBridge.get(),2,&third)==0&&first.id==secondID&&second.id==thirdID&&third.id==firstID);
    CHECK(daw_move_track(moveBridge.get(),firstID,2,4)==0);moveSnapshot.struct_size=sizeof(moveSnapshot);CHECK(daw_get_snapshot(moveBridge.get(),&moveSnapshot)==0&&moveSnapshot.revision==4);
    CHECK(daw_undo(moveBridge.get(),4)==0);moveSnapshot.struct_size=sizeof(moveSnapshot);CHECK(daw_get_snapshot(moveBridge.get(),&moveSnapshot)==0&&moveSnapshot.revision==5);first={};first.struct_size=sizeof(first);CHECK(daw_get_track(moveBridge.get(),0,&first)==0&&std::string(first.name)=="First");
    CHECK(daw_redo(moveBridge.get(),5)==0);moveSnapshot.struct_size=sizeof(moveSnapshot);CHECK(daw_get_snapshot(moveBridge.get(),&moveSnapshot)==0&&moveSnapshot.revision==6);first={};first.struct_size=sizeof(first);CHECK(daw_get_track(moveBridge.get(),2,&first)==0&&std::string(first.name)=="First");

    // v35 accepts the supported 44.1 kHz PCM WAV family at the public C ABI
    // boundary, converts it once into the fixed 48 kHz project representation,
    // and never partially mutates a session when validation fails.
    const auto rate441Path=cleanup.path/"44k1-fixture.wav";
    const auto zeroRatePath=cleanup.path/"zero-rate.wav";
    const auto nanPath=cleanup.path/"nan-fixture.wav";
    writeFixtureWav(rate441Path,44100);writeFixtureWav(zeroRatePath,0);writeFixtureWav(nanPath,44100,true);
    std::unique_ptr<daw_session,decltype(&daw_destroy)> rateBridge(daw_create(),daw_destroy);CHECK(rateBridge);
    daw_snapshot rateSnapshot{};rateSnapshot.struct_size=sizeof(rateSnapshot);CHECK(daw_get_snapshot(rateBridge.get(),&rateSnapshot)==0&&rateSnapshot.revision==0&&rateSnapshot.track_count==0);
#ifdef __APPLE__
    CHECK(daw_import_wav(rateBridge.get(),rate441Path.c_str(),"44.1 base",rateSnapshot.revision)==0);
    rateSnapshot.struct_size=sizeof(rateSnapshot);CHECK(daw_get_snapshot(rateBridge.get(),&rateSnapshot)==0&&rateSnapshot.revision==1&&rateSnapshot.track_count==1);
    daw_track rateTrack{};rateTrack.struct_size=sizeof(rateTrack);CHECK(daw_get_track(rateBridge.get(),0,&rateTrack)==0&&rateTrack.audio_frames==480&&rateTrack.clip_count==1&&rateTrack.take_count==1);
    daw_clip rateBaseClip{};rateBaseClip.struct_size=sizeof(rateBaseClip);CHECK(daw_get_clip(rateBridge.get(),rateTrack.id,0,&rateBaseClip)==0&&rateBaseClip.start==0&&rateBaseClip.source_offset==0&&rateBaseClip.length==480&&rateBaseClip.take_index==0);
    CHECK(daw_import_take_wav(rateBridge.get(),rateTrack.id,rate441Path.c_str(),"44.1 take",240,rateSnapshot.revision)==0);
    rateSnapshot.struct_size=sizeof(rateSnapshot);CHECK(daw_get_snapshot(rateBridge.get(),&rateSnapshot)==0&&rateSnapshot.revision==2);
    rateTrack={};rateTrack.struct_size=sizeof(rateTrack);CHECK(daw_get_track(rateBridge.get(),0,&rateTrack)==0&&rateTrack.audio_frames==480&&rateTrack.take_count==2);
    daw_take convertedTake{};convertedTake.struct_size=sizeof(convertedTake);CHECK(daw_get_take(rateBridge.get(),rateTrack.id,1,&convertedTake)==0&&convertedTake.start==240&&convertedTake.frames==480&&std::string(convertedTake.name)=="44.1 take");
    CHECK(daw_comp_range(rateBridge.get(),rateTrack.id,1,240,720,rateSnapshot.revision)==0);
    rateSnapshot.struct_size=sizeof(rateSnapshot);CHECK(daw_get_snapshot(rateBridge.get(),&rateSnapshot)==0&&rateSnapshot.revision==3);
    rateTrack={};rateTrack.struct_size=sizeof(rateTrack);CHECK(daw_get_track(rateBridge.get(),0,&rateTrack)==0&&rateTrack.clip_count==2&&rateTrack.take_count==2);
    daw_clip compLeft{};compLeft.struct_size=sizeof(compLeft);daw_clip compRight{};compRight.struct_size=sizeof(compRight);CHECK(daw_get_clip(rateBridge.get(),rateTrack.id,0,&compLeft)==0&&daw_get_clip(rateBridge.get(),rateTrack.id,1,&compRight)==0&&compLeft.start==0&&compLeft.length==240&&compLeft.take_index==0&&compRight.start==240&&compRight.length==480&&compRight.take_index==1);
    const auto beforeRejectedRevision=rateSnapshot.revision;const auto beforeRejectedTrackCount=rateSnapshot.track_count;
    CHECK(daw_import_wav(rateBridge.get(),zeroRatePath.c_str(),"invalid rate",beforeRejectedRevision)==1);
    rateSnapshot.struct_size=sizeof(rateSnapshot);CHECK(daw_get_snapshot(rateBridge.get(),&rateSnapshot)==0&&rateSnapshot.revision==beforeRejectedRevision&&rateSnapshot.track_count==beforeRejectedTrackCount);
    CHECK(daw_import_take_wav(rateBridge.get(),rateTrack.id,nanPath.c_str(),"invalid nan",0,beforeRejectedRevision)==1);
    rateSnapshot.struct_size=sizeof(rateSnapshot);CHECK(daw_get_snapshot(rateBridge.get(),&rateSnapshot)==0&&rateSnapshot.revision==beforeRejectedRevision&&rateSnapshot.track_count==beforeRejectedTrackCount);
    rateTrack={};rateTrack.struct_size=sizeof(rateTrack);CHECK(daw_get_track(rateBridge.get(),0,&rateTrack)==0&&rateTrack.audio_frames==480&&rateTrack.clip_count==2&&rateTrack.take_count==2);
    daw_clip unchangedLeft{};unchangedLeft.struct_size=sizeof(unchangedLeft);daw_clip unchangedRight{};unchangedRight.struct_size=sizeof(unchangedRight);CHECK(daw_get_clip(rateBridge.get(),rateTrack.id,0,&unchangedLeft)==0&&daw_get_clip(rateBridge.get(),rateTrack.id,1,&unchangedRight)==0&&unchangedLeft.start==compLeft.start&&unchangedLeft.length==compLeft.length&&unchangedLeft.take_index==compLeft.take_index&&unchangedRight.start==compRight.start&&unchangedRight.length==compRight.length&&unchangedRight.take_index==compRight.take_index);
    const auto rateDraftPath=(cleanup.path/"converted-rate.mydawdraft").string();CHECK(daw_save_draft(rateBridge.get(),rateDraftPath.c_str())==0);const auto rateRoundTrip=daw::readDraft(rateDraftPath);CHECK(rateRoundTrip.tracks.size()==1&&rateRoundTrip.tracks[0].audio&&rateRoundTrip.tracks[0].audio->frames()==480&&rateRoundTrip.tracks[0].takes.size()==1&&rateRoundTrip.tracks[0].takes[0].audio->frames()==480&&rateRoundTrip.tracks[0].regions.size()==2&&rateRoundTrip.tracks[0].regions[1].take==1);
    const auto rateExportPath=(cleanup.path/"converted-rate.wav").string();daw::writeWav(rateRoundTrip,rateExportPath,daw::WavFormat::Float32);std::ifstream exported(rateExportPath,std::ios::binary);CHECK(exported.good());std::vector<unsigned char> exportedHeader(44);exported.read(reinterpret_cast<char*>(exportedHeader.data()),static_cast<std::streamsize>(exportedHeader.size()));CHECK(exported.gcount()==static_cast<std::streamsize>(exportedHeader.size())&&get32(exportedHeader,24)==48000);
#else
    CHECK(daw_import_wav(rateBridge.get(),rate441Path.c_str(),"unsupported non-Apple rate",rateSnapshot.revision)==1);
    rateSnapshot.struct_size=sizeof(rateSnapshot);CHECK(daw_get_snapshot(rateBridge.get(),&rateSnapshot)==0&&rateSnapshot.revision==0&&rateSnapshot.track_count==0);
#endif
#ifdef __APPLE__
    // v36 keeps expensive decode/resampling off the owner thread. Begin is
    // read-only; a ready job applies once against an explicit fresh revision.
    std::unique_ptr<daw_session,decltype(&daw_destroy)> asyncBridge(daw_create(),daw_destroy);CHECK(asyncBridge);
    daw_snapshot asyncSnapshot{};asyncSnapshot.struct_size=sizeof(asyncSnapshot);CHECK(daw_get_snapshot(asyncBridge.get(),&asyncSnapshot)==0&&asyncSnapshot.revision==0&&asyncSnapshot.track_count==0);
    daw_import_job* trackImport=daw_begin_import_wav(asyncBridge.get(),rate441Path.c_str(),"Async 44.1",asyncSnapshot.revision);CHECK(trackImport);
    daw_import_status asyncStatus{};asyncStatus.struct_size=sizeof(asyncStatus);CHECK(daw_poll_import(trackImport,&asyncStatus)==0&&asyncStatus.base_revision==0);
    asyncSnapshot.struct_size=sizeof(asyncSnapshot);CHECK(daw_get_snapshot(asyncBridge.get(),&asyncSnapshot)==0&&asyncSnapshot.revision==0&&asyncSnapshot.track_count==0);
    asyncStatus=waitForImport(trackImport);CHECK(asyncStatus.status==DAW_IMPORT_READY&&asyncStatus.version==DAW_IMPORT_STATUS_VERSION&&asyncStatus.phase==DAW_IMPORT_PHASE_READY&&asyncStatus.progress==100&&asyncStatus.source_sample_rate==44100&&asyncStatus.source_channels==2&&asyncStatus.source_frames==441&&asyncStatus.output_frames==480);
    CHECK(daw_add_track(asyncBridge.get(),"Edit while importing",asyncSnapshot.revision)==0);
    asyncSnapshot.struct_size=sizeof(asyncSnapshot);CHECK(daw_get_snapshot(asyncBridge.get(),&asyncSnapshot)==0&&asyncSnapshot.revision==1&&asyncSnapshot.track_count==1);
    CHECK(daw_apply_import(asyncBridge.get(),trackImport,0)==1);
    asyncStatus=waitForImport(trackImport);CHECK(asyncStatus.status==DAW_IMPORT_READY);
    CHECK(daw_apply_import(asyncBridge.get(),trackImport,asyncSnapshot.revision)==0);
    asyncStatus=waitForImport(trackImport);CHECK(asyncStatus.status==DAW_IMPORT_APPLIED&&asyncStatus.output_frames==480);
    asyncSnapshot.struct_size=sizeof(asyncSnapshot);CHECK(daw_get_snapshot(asyncBridge.get(),&asyncSnapshot)==0&&asyncSnapshot.revision==2&&asyncSnapshot.track_count==2);
    CHECK(daw_apply_import(asyncBridge.get(),trackImport,asyncSnapshot.revision)==1);daw_release_import(trackImport);
    daw_track asyncTrack{};asyncTrack.struct_size=sizeof(asyncTrack);CHECK(daw_get_track(asyncBridge.get(),1,&asyncTrack)==0&&asyncTrack.audio_frames==480&&asyncTrack.take_count==1);
    daw_import_job* takeImport=daw_begin_import_take_wav(asyncBridge.get(),asyncTrack.id,rate441Path.c_str(),"Async take",120,asyncSnapshot.revision);CHECK(takeImport);
    asyncStatus=waitForImport(takeImport);CHECK(asyncStatus.status==DAW_IMPORT_READY);
    std::unique_ptr<daw_session,decltype(&daw_destroy)> wrongSession(daw_create(),daw_destroy);CHECK(wrongSession&&daw_apply_import(wrongSession.get(),takeImport,0)==1);
    asyncStatus=waitForImport(takeImport);CHECK(asyncStatus.status==DAW_IMPORT_READY);
    CHECK(daw_apply_import(asyncBridge.get(),takeImport,asyncSnapshot.revision)==0);daw_release_import(takeImport);
    asyncSnapshot.struct_size=sizeof(asyncSnapshot);CHECK(daw_get_snapshot(asyncBridge.get(),&asyncSnapshot)==0&&asyncSnapshot.revision==3);
    asyncTrack={};asyncTrack.struct_size=sizeof(asyncTrack);CHECK(daw_get_track(asyncBridge.get(),1,&asyncTrack)==0&&asyncTrack.take_count==2);

    // A ready take import holds no implicit claim on its target: deleting the
    // target before apply rejects the job and leaves the new state untouched.
    daw_import_job* deletedTargetImport=daw_begin_import_take_wav(asyncBridge.get(),asyncTrack.id,rate441Path.c_str(),"Deleted target",0,asyncSnapshot.revision);CHECK(deletedTargetImport);
    asyncStatus=waitForImport(deletedTargetImport);CHECK(asyncStatus.status==DAW_IMPORT_READY);
    CHECK(daw_remove_track(asyncBridge.get(),asyncTrack.id,asyncSnapshot.revision)==0);
    daw_snapshot deletedTargetSnapshot{};deletedTargetSnapshot.struct_size=sizeof(deletedTargetSnapshot);CHECK(daw_get_snapshot(asyncBridge.get(),&deletedTargetSnapshot)==0&&deletedTargetSnapshot.revision==4&&deletedTargetSnapshot.track_count==1);
    CHECK(daw_apply_import(asyncBridge.get(),deletedTargetImport,deletedTargetSnapshot.revision)==1);
    deletedTargetSnapshot.struct_size=sizeof(deletedTargetSnapshot);CHECK(daw_get_snapshot(asyncBridge.get(),&deletedTargetSnapshot)==0&&deletedTargetSnapshot.revision==4&&deletedTargetSnapshot.track_count==1);daw_release_import(deletedTargetImport);

    // Failed and canceled jobs are terminal and cannot touch domain state.
    daw_import_job* failedImport=daw_begin_import_wav(asyncBridge.get(),zeroRatePath.c_str(),"Bad async",deletedTargetSnapshot.revision);CHECK(failedImport);
    asyncStatus=waitForImport(failedImport);CHECK(asyncStatus.status==DAW_IMPORT_FAILED&&asyncStatus.error[0]);CHECK(daw_apply_import(asyncBridge.get(),failedImport,deletedTargetSnapshot.revision)==1);daw_release_import(failedImport);
    daw_import_job* canceledImport=daw_begin_import_wav(asyncBridge.get(),rate441Path.c_str(),"Canceled async",deletedTargetSnapshot.revision);CHECK(canceledImport);daw_cancel_import(canceledImport);
    asyncStatus=waitForImport(canceledImport);CHECK(asyncStatus.status==DAW_IMPORT_CANCELED);CHECK(daw_apply_import(asyncBridge.get(),canceledImport,deletedTargetSnapshot.revision)==1);daw_release_import(canceledImport);
    daw_snapshot unchangedAsync{};unchangedAsync.struct_size=sizeof(unchangedAsync);CHECK(daw_get_snapshot(asyncBridge.get(),&unchangedAsync)==0&&unchangedAsync.revision==4&&unchangedAsync.track_count==1);

    // Opening a draft replaces the project epoch even when a source job is
    // already ready. The opaque handle is also safe after session destruction.
    daw_import_job* epochImport=daw_begin_import_wav(asyncBridge.get(),rate441Path.c_str(),"Old project",unchangedAsync.revision);CHECK(epochImport);asyncStatus=waitForImport(epochImport);CHECK(asyncStatus.status==DAW_IMPORT_READY);
    const auto epochDraft=(cleanup.path/"async-epoch.mydawdraft").string();daw::Session epochState;epochState.add("Replacement",0);daw::writeDraft(epochState.state(),epochDraft);CHECK(daw_open_draft(asyncBridge.get(),epochDraft.c_str())==0);
    daw_snapshot epochSnapshot{};epochSnapshot.struct_size=sizeof(epochSnapshot);CHECK(daw_get_snapshot(asyncBridge.get(),&epochSnapshot)==0&&epochSnapshot.revision==1&&epochSnapshot.track_count==1);CHECK(daw_apply_import(asyncBridge.get(),epochImport,epochSnapshot.revision)==1);asyncStatus=waitForImport(epochImport);CHECK(asyncStatus.status==DAW_IMPORT_READY);daw_release_import(epochImport);
    daw_session* destroyedSession=daw_create();CHECK(destroyedSession);daw_snapshot destroyedSnapshot{};destroyedSnapshot.struct_size=sizeof(destroyedSnapshot);CHECK(daw_get_snapshot(destroyedSession,&destroyedSnapshot)==0);daw_import_job* orphanImport=daw_begin_import_wav(destroyedSession,rate441Path.c_str(),"Orphan",destroyedSnapshot.revision);CHECK(orphanImport);daw_destroy(destroyedSession);daw_release_import(orphanImport);
#endif
    daw::Session bounded; for(int i=0;i<256;++i) bounded.add("Track",bounded.state().revision);
    rejects([&]{bounded.add("Overflow",bounded.state().revision);});
    int undos=0; while(bounded.canUndo()){ bounded.undo(bounded.state().revision); ++undos; } CHECK(undos==128);
    while(bounded.canRedo()) bounded.redo(bounded.state().revision); CHECK(bounded.state().tracks.size()==256);
    std::cout << "PASS: domain invariants, revision conflicts, UTF-8, undo/redo, bounded history, atomic snapshot replacement, SQLite roundtrip, invalid-load preservation, C ABI\n";
    return 0;
} catch(const std::exception& e){ std::cerr<<e.what()<<'\n'; return 1; } }
