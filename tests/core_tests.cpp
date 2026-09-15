#include "domain/session.hpp"
#include "plugins/plugin_descriptor.hpp"
#include "daw.h"
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while(false)
template<class Fn> void rejects(Fn fn) { bool rejected=false; try { fn(); } catch (...) { rejected=true; } CHECK(rejected); }
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
    sqlite3* hostingDb=nullptr;CHECK(sqlite3_open(hostingPath.c_str(),&hostingDb)==SQLITE_OK);sqlite3_stmt* versionStatement=nullptr;CHECK(sqlite3_prepare_v2(hostingDb,"PRAGMA user_version",-1,&versionStatement,nullptr)==SQLITE_OK);CHECK(sqlite3_step(versionStatement)==SQLITE_ROW&&sqlite3_column_int(versionStatement,0)==15);sqlite3_finalize(versionStatement);sqlite3_close(hostingDb);
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
    daw::Session bounded; for(int i=0;i<256;++i) bounded.add("Track",bounded.state().revision);
    rejects([&]{bounded.add("Overflow",bounded.state().revision);});
    int undos=0; while(bounded.canUndo()){ bounded.undo(bounded.state().revision); ++undos; } CHECK(undos==128);
    while(bounded.canRedo()) bounded.redo(bounded.state().revision); CHECK(bounded.state().tracks.size()==256);
    std::cout << "PASS: domain invariants, revision conflicts, UTF-8, undo/redo, bounded history, atomic snapshot replacement, SQLite roundtrip, invalid-load preservation, C ABI\n";
    return 0;
} catch(const std::exception& e){ std::cerr<<e.what()<<'\n'; return 1; } }
