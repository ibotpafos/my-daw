#include "domain/session.hpp"
#include "bridge/daw.h"
#include <sqlite3.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <unistd.h>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while (false)
template <class Fn> void rejects(Fn&& fn) { bool rejected = false; try { fn(); } catch (...) { rejected = true; } CHECK(rejected); }
using namespace daw;

static sqlite3* openDb(const std::filesystem::path& path){sqlite3* db=nullptr;if(sqlite3_open(path.c_str(),&db)!=SQLITE_OK)throw std::runtime_error("Cannot open draft db");return db;}

static_assert(sizeof(daw_tempo_point)==24,"tempo ABI");
static_assert(sizeof(daw_time_signature_point)==24,"time signature ABI");

int main(){try{
    auto dir=std::filesystem::temp_directory_path()/("mydaw-tempo-model-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    struct Cleanup { std::filesystem::path p; ~Cleanup(){ std::error_code ig; std::filesystem::remove_all(p,ig);} } cleanup{dir};

    // ---- Default state carries the frame-0 anchors; commands upsert on top ----
    Session s;
    CHECK(s.state().tempo.size()==1&&s.state().tempo[0]==(TempoPoint{0,120.0}));
    CHECK(s.state().timeSignatures.size()==1&&s.state().timeSignatures[0]==(TimeSignaturePoint{0,4,8}));
    s.add("Timeline",0);                                                              // rev 1
    s.setTempoAt(48000,60.0,1);                                                       // rev 2
    CHECK(s.state().tempo.size()==2&&s.state().tempo[1]==(TempoPoint{48000,60.0}));
    s.setTempoAt(48000,60.0,2);                                                       // identical upsert: silent no-op
    CHECK(s.state().revision==2);
    s.setTempoAt(48000,90.0,2);                                                       // rev 3; same frame replaced
    CHECK(s.state().tempo.size()==2&&s.state().tempo[1]==(TempoPoint{48000,90.0}));
    s.setTempoAt(0,130.0,3);                                                          // rev 4; the anchor may be retuned
    CHECK(s.state().tempo.size()==2&&s.state().tempo[0]==(TempoPoint{0,130.0}));
    s.setTimeSignatureAt(96000,3,4,4);                                                // rev 5
    CHECK(s.state().timeSignatures.size()==2&&s.state().timeSignatures[1]==(TimeSignaturePoint{96000,3,4}));
    s.removeTempo(48000,5);                                                           // rev 6
    CHECK(s.state().tempo.size()==1&&s.state().tempo[0].bpm==130.0);
    s.removeTimeSignature(96000,6);                                                   // rev 7
    CHECK(s.state().timeSignatures.size()==1&&s.state().timeSignatures[0]==(TimeSignaturePoint{0,4,8}));

    // ---- Rejections never consume a revision ----
    {   const uint64_t rev=s.state().revision;                                        // 7
        rejects([&]{s.setTempoAt(48000,19.0,rev);});                                  // below the (20,999] floor
        rejects([&]{s.setTempoAt(48000,20.0,rev);});                                  // floor is exclusive
        rejects([&]{s.setTempoAt(48000,1000.0,rev);});
        rejects([&]{s.setTempoAt(48000,std::numeric_limits<double>::quiet_NaN(),rev);});
        rejects([&]{s.setTempoAt(kMaxMidiFrame,120.0,rev);});                         // frame limit is exclusive
        rejects([&]{s.setTimeSignatureAt(150000,0,4,rev);});                          // numerator 0
        rejects([&]{s.setTimeSignatureAt(150000,33,4,rev);});                         // numerator beyond 32
        rejects([&]{s.setTimeSignatureAt(150000,4,5,rev);});                          // non-power-of-two denominator
        rejects([&]{s.setTimeSignatureAt(150000,4,64,rev);});                         // denominator beyond 32
        rejects([&]{s.removeTempo(0,rev);});                                          // anchor cannot be removed
        rejects([&]{s.removeTempo(777777,rev);});                                     // unknown frame
        rejects([&]{s.removeTimeSignature(0,rev);});
        rejects([&]{s.setTempoAt(48000,999.0,rev+5);});                               // revision conflict
        s.setTempoAt(48000,999.0,rev);                                                // boundary value 999 is legal  // rev 8
        CHECK(s.state().revision==8);
    }

    // ---- Beats <-> frames: exact pairs and a constant invertible round trip ----
    {   Session b; b.add("Beats",0);
        CHECK(b.state().bpmAtFrame(0)==120.0&&b.state().bpmAtFrame(kMaxMidiFrame-1)==120.0);
        CHECK(b.state().beatsAtFrame(0)==0.0&&b.state().frameAtBeats(0.0)==0);
        CHECK(b.state().beatsAtFrame(24000)==1.0&&b.state().frameAtBeats(1.0)==24000); // 120 BPM: one beat is 24000 frames
        b.setTempoAt(48000,60.0,1);                                                    // rev 2; half tempo from one second on
        CHECK(b.state().bpmAtFrame(47999)==120.0&&b.state().bpmAtFrame(48000)==60.0);
        CHECK(b.state().beatsAtFrame(96000)==3.0);                                     // 48000/24000 + 48000/48000 = 3.0 exactly
        CHECK(b.state().frameAtBeats(2.0)==48000&&b.state().frameAtBeats(3.0)==96000);
        CHECK(b.state().frameAtBeats(0.5)==12000&&b.state().frameAtBeats(2.5)==72000);
        for(const uint64_t f:{0ULL,1ULL,23999ULL,47999ULL,48000ULL,48001ULL,96000ULL,5000000ULL,1000000000ULL})
            CHECK(b.state().frameAtBeats(b.state().beatsAtFrame(f))==f);
        rejects([&]{(void)b.state().frameAtBeats(-1.0);});
        rejects([&]{(void)b.state().frameAtBeats(std::numeric_limits<double>::infinity());});
        rejects([&]{(void)b.state().frameAtBeats(1.0e300);});                          // far past the timeline limit
        // Long-double accumulation holds at the 2^40-frame edge (~36.5 hours): the
        // round trip may drift by at most the final rounding to the nearest frame.
        const uint64_t nearEdge=kMaxMidiFrame-24000;
        const auto hugeBeats=b.state().beatsAtFrame(nearEdge);
        CHECK(hugeBeats>2.2e7&&hugeBeats<2.4e7); // ≈2^40/48000 beats at the dominant 60 BPM tempo
        CHECK(std::llabs(static_cast<long long>(b.state().frameAtBeats(hugeBeats))-static_cast<long long>(nearEdge))<=1);
    }

    // ---- Capacity: exactly 64 points of each kind, the 65th is rejected ----
    {   Session m; m.add("Busy",0);
        for(uint64_t i=1;i<64;++i)m.setTempoAt(i*1000,120.0+i,i);                      // rev 1..63 commit to 64
        CHECK(m.state().tempo.size()==64&&m.state().revision==64);
        rejects([&]{m.setTempoAt(64000,190.0,64);});
        CHECK(m.state().revision==64);
        for(uint64_t i=1;i<64;++i)m.setTimeSignatureAt(i*1000,3,8,64+i-1);
        CHECK(m.state().timeSignatures.size()==64&&m.state().revision==127);
        rejects([&]{m.setTimeSignatureAt(64000,5,8,127);});
        CHECK(m.state().revision==127);
    }

    // ---- Undo/redo restores the maps exactly, one revision per command ----
    {   Session u; u.add("Keys",0);
        u.setTempoAt(48000,60.0,1);                                                    // rev 2
        u.setTimeSignatureAt(96000,6,8,2);                                             // rev 3
        const auto before=u.state();
        u.setTempoAt(48000,70.0,3);                                                    // rev 4
        CHECK(u.state().tempo[1].bpm==70.0);
        u.undo(4);                                                                     // rev 5
        CHECK(u.state().tempo==before.tempo&&u.state().timeSignatures==before.timeSignatures);
        u.redo(5);                                                                     // rev 6
        CHECK(u.state().tempo[1].bpm==70.0&&u.state().revision==6);
        u.undo(6);u.undo(7);u.undo(8);                                                 // back to the fresh anchors
        CHECK(u.state().tempo.size()==1&&u.state().tempo[0].bpm==120.0&&u.state().timeSignatures[0]==(TimeSignaturePoint{0,4,8}));
    }

    // ---- Storage: v19 round trip, v16/v17/v18 fallback to defaults, corrupt maps rejected ----
    {   Session w; w.add("Wired",0);
        w.setTempoAt(48000,60.0,1);
        w.setTimeSignatureAt(96000,7,8,2);                                             // rev 3
        const auto path=dir/"tempo-roundtrip.mydawdraft";
        writeDraft(w.state(),path.string());
        const auto loaded=readDraft(path.string());
        CHECK(loaded.tempo==w.state().tempo&&loaded.timeSignatures==w.state().timeSignatures);
        CHECK(loaded.tracks==w.state().tracks&&loaded.revision==w.state().revision&&loaded.nextID==w.state().nextID);
        auto stamped=openDb(path);
        sqlite3_stmt* version=nullptr;
        CHECK(sqlite3_prepare_v2(stamped,"PRAGMA user_version",-1,&version,nullptr)==SQLITE_OK);
        CHECK(sqlite3_step(version)==SQLITE_ROW&&sqlite3_column_int(version,0)==22);                                      // draft v20 stamps the write
        sqlite3_finalize(version);
        CHECK(sqlite3_exec(stamped,"PRAGMA user_version=16;",nullptr,nullptr,nullptr)==SQLITE_OK);
        sqlite3_close(stamped);
        const auto legacy=readDraft(path.string());                                    // v16 gate ignores the tables
        CHECK(legacy.tempo==(std::vector<TempoPoint>{{0,120.0}}));
        CHECK(legacy.timeSignatures==(std::vector<TimeSignaturePoint>{{0,4,8}}));
        CHECK(legacy.tracks==w.state().tracks);

        const auto bad=dir/"tempo-corrupt.mydawdraft";
        const auto damage=[&](const char* statement){
            writeDraft(w.state(),bad.string());
            auto db=openDb(bad);
            CHECK(sqlite3_exec(db,statement,nullptr,nullptr,nullptr)==SQLITE_OK);
            sqlite3_close(db);
            rejects([&]{(void)readDraft(bad.string());});
        };
        damage("UPDATE tempo_points SET bpm=5.0 WHERE frame=48000;");                  // out-of-range tempo
        damage("UPDATE time_signature_points SET denominator=5 WHERE frame=96000;");  // illegal meter
        damage("DELETE FROM tempo_points WHERE frame=0;");                             // missing anchor
        damage("DELETE FROM time_signature_points;");                                  // empty map
    }

    // ---- Bridge smoke: four commands, count/point queries, ABI negatives ----
    {   auto* session=daw_create();
        CHECK(session!=nullptr);
        auto revision=[&]{ daw_snapshot snap{};snap.struct_size=sizeof(snap); CHECK(daw_get_snapshot(session,&snap)==0); return snap.revision; };
        uint32_t count=0;
        CHECK(daw_get_tempo_count(session,&count)==0&&count==1);                       // default anchor
        CHECK(daw_get_time_signature_count(session,&count)==0&&count==1);
        CHECK(daw_add_track(session,"Bridge",revision())==0);                          // rev 1
        CHECK(daw_set_tempo(session,48000,60.0,1)==0);                                 // rev 2
        CHECK(daw_set_tempo(session,48000,60.0,2)==0&&revision()==2);                  // identical value stays a no-op
        CHECK(daw_get_tempo_count(session,&count)==0&&count==2);
        daw_tempo_point point{};point.struct_size=sizeof(daw_tempo_point);
        CHECK(daw_get_tempo_point(session,1,&point)==0&&point.version==DAW_TEMPO_POINT_VERSION&&point.frame==48000&&point.bpm==60.0);
        daw_tempo_point undersized{};undersized.struct_size=sizeof(daw_tempo_point)-1;
        CHECK(daw_get_tempo_point(session,0,&undersized)!=0);                          // ABI guard
        daw_tempo_point beyond{};beyond.struct_size=sizeof(daw_tempo_point);
        CHECK(daw_get_tempo_point(session,2,&beyond)!=0);                              // index out of range
        CHECK(daw_get_tempo_count(session,nullptr)!=0);
        char message[512]={};daw_error(session,message,sizeof(message));
        CHECK(message[0]!='\0');
        CHECK(daw_set_time_signature(session,96000,3,4,2)==0);                         // rev 3
        CHECK(daw_get_time_signature_count(session,&count)==0&&count==2);
        daw_time_signature_point signature{};signature.struct_size=sizeof(daw_time_signature_point);
        CHECK(daw_get_time_signature_point(session,1,&signature)==0&&signature.version==DAW_TIME_SIGNATURE_POINT_VERSION&&signature.frame==96000&&signature.numerator==3&&signature.denominator==4);
        daw_time_signature_point oversized{};oversized.struct_size=sizeof(daw_time_signature_point)+1;
        CHECK(daw_get_time_signature_point(session,0,&oversized)!=0);
        const uint64_t held=revision();CHECK(held==3);
        CHECK(daw_set_tempo(session,48000,19.0,3)!=0);                                 // bpm below the floor
        CHECK(daw_set_tempo(session,48000,1000.0,3)!=0);
        CHECK(daw_set_time_signature(session,120000,0,4,3)!=0);                        // numerator 0
        CHECK(daw_set_time_signature(session,120000,4,5,3)!=0);                        // denominator 5
        CHECK(daw_remove_tempo(session,0,3)!=0);                                       // the frame-0 anchor stays
        CHECK(daw_remove_time_signature(session,0,3)!=0);
        CHECK(revision()==3);
        CHECK(daw_remove_tempo(session,48000,3)==0);                                   // rev 4
        CHECK(daw_get_tempo_count(session,&count)==0&&count==1);
        CHECK(daw_undo(session,4)==0);                                                 // rev 5; undo restores the map
        CHECK(daw_get_tempo_count(session,&count)==0&&count==2);
        daw_tempo_point restored{};restored.struct_size=sizeof(daw_tempo_point);
        CHECK(daw_get_tempo_point(session,1,&restored)==0&&restored.bpm==60.0);
        const auto saved=dir/"tempo-bridge.mydawdraft";
        CHECK(daw_save_draft(session,saved.string().c_str())==0);
        CHECK(daw_open_draft(session,saved.string().c_str())==0);
        CHECK(daw_get_tempo_count(session,&count)==0&&count==2&&daw_get_time_signature_count(session,&count)==0&&count==2);
        daw_destroy(session);
    }
    std::cout << "PASS: tempo model: frame-0 anchored tempo and time-signature maps, four revision-checked commands, invertible beat/frame conversion, undo/redo, writeDraft/readDraft round trip at schema v19 with v16/v17/v18 default fallback and bridge smoke" << std::endl;
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}}
