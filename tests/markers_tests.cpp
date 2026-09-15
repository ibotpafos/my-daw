#include "domain/session.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while (false)
template <class Fn> void rejects(Fn&& fn) { bool rejected = false; try { fn(); } catch (...) { rejected = true; } CHECK(rejected); }
template <class Fn> void rejectsMessage(const char* text,Fn&& fn) {
    try { fn(); }
    catch (const daw::Error& error) { CHECK(std::string(error.what())==text); return; }
    throw std::runtime_error("Expected rejection was not raised");
}
using namespace daw;

static sqlite3* openDb(const std::filesystem::path& path){sqlite3* db=nullptr;if(sqlite3_open(path.c_str(),&db)!=SQLITE_OK)throw std::runtime_error("Cannot open draft db");return db;}

int main(){try{
    auto dir=std::filesystem::temp_directory_path()/("mydaw-marker-model-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    struct Cleanup { std::filesystem::path p; ~Cleanup(){ std::error_code ig; std::filesystem::remove_all(p,ig);} } cleanup{dir};

    // ---- Empty lane is the valid default; a fresh project carries no locators ----
    {   Session s;
        CHECK(s.state().markers.empty());
        validate(s.state());                                                     // unlike tempo, no frame-0 anchor
        State one; one.markers.push_back({kMaxMidiFrame-1,"Edge"});
        validate(one);                                                           // the exclusive-edge frame is legal
    }

    // ---- add/rename/remove: sorted lane, expected revisions, silent no-ops ----
    Session s;
    CHECK(s.state().markers.empty());
    s.add("Timeline",0);                                                         // rev 1
    s.addMarker(96000,"Куплет",1);                                               // rev 2
    s.addMarker(0,"Начало",2);                                                   // rev 3; front insert
    s.addMarker(48000,"Припев",3);                                               // rev 4; middle insert
    CHECK(s.state().markers.size()==3);
    CHECK(s.state().markers[0]==(Marker{0,"Начало"}));
    CHECK(s.state().markers[1]==(Marker{48000,"Припев"}));
    CHECK(s.state().markers[2]==(Marker{96000,"Куплет"}));
    s.renameMarker(48000,"Бридж",4);                                             // rev 5; frame never moves
    CHECK(s.state().markers[1]==(Marker{48000,"Бридж"}));
    s.renameMarker(48000,"Бридж",5);                                             // identical name: silent no-op
    CHECK(s.state().revision==5&&s.state().markers.size()==3);
    s.removeMarker(48000,5);                                                     // rev 6
    CHECK(s.state().markers.size()==2&&s.state().markers[1].frame==96000);
    rejectsMessage("A marker already exists at this position",[&]{s.addMarker(96000,"Again",6);});
    rejectsMessage("Marker not found",[&]{s.removeMarker(48000,6);});
    rejectsMessage("Marker not found",[&]{s.renameMarker(48000,"Ghost",6);});
    CHECK(s.state().revision==6);

    // ---- Rejection matrix never consumes a revision ----
    {   const uint64_t rev=s.state().revision;                                   // 6
        rejects([&]{s.addMarker(0,"Dup",rev);});                                 // stored frames reject
        rejects([&]{s.addMarker(96000,"Dup",rev);});
        rejects([&]{s.addMarker(kMaxMidiFrame,"Edge",rev);});                    // frame limit is exclusive
        rejects([&]{s.addMarker(kMaxMidiFrame+1,"Beyond",rev);});
        rejects([&]{s.addMarker(123456,"",rev);});                               // empty name
        rejects([&]{s.addMarker(123456,std::string(121,'A'),rev);});             // one over the 120-character limit
        rejects([&]{s.addMarker(123456,std::string("Bad\x01"),rev);});          // control character
        rejects([&]{s.addMarker(123456,std::string("A\0B",3),rev);});           // embedded NUL
        rejects([&]{s.addMarker(123456,"\xFF\xFE",rev);});                     // invalid UTF-8 lead byte
        rejects([&]{s.addMarker(123456,"\xC2",rev);});                          // truncated sequence
        rejects([&]{s.addMarker(123456,"\xC0\x80",rev);});                     // overlong encoding
        rejects([&]{s.addMarker(123456,"\xED\xA0\x80",rev);});                // UTF-16 surrogate
        rejects([&]{s.renameMarker(0,"",rev);});
        rejects([&]{s.renameMarker(96000,std::string(121,'B'),rev);});
        rejects([&]{s.addMarker(123456,"Conflict",rev+9);});                     // revision conflict
        rejects([&]{s.removeMarker(123456,rev+9);});
        CHECK(s.state().revision==rev&&s.state().markers.size()==2);
        s.addMarker(123456,std::string(120,'A'),rev);                            // exactly 120 characters is legal  // rev 7
        CHECK(s.state().markers.size()==3&&s.state().markers.back().frame==123456);
    }

    // ---- Undo/redo through the shared snapshot stack on a three-command history ----
    {   Session u; u.add("Undo",0);                                              // rev 1
        u.addMarker(48000,"Intro",1);                                            // rev 2
        u.addMarker(96000,"Outro",2);                                            // rev 3
        u.renameMarker(48000,"Hook",3);                                          // rev 4
        const State committed=u.state();
        CHECK(committed.markers[0]==(Marker{48000,"Hook"})&&committed.markers[1]==(Marker{96000,"Outro"}));
        u.renameMarker(48000,"Hook",4);                                          // no-op leaves no undo entry
        CHECK(u.state().revision==4);
        u.undo(4);                                                               // rev 5; back behind the rename
        CHECK(u.state().markers[0]==(Marker{48000,"Intro"})&&u.state().revision==5);
        u.undo(5);u.undo(6);                                                     // rev 6, rev 7; lane fully unwound
        CHECK(u.state().markers.empty()&&u.state().revision==7);
        u.redo(7);u.redo(8);u.redo(9);                                           // rev 10 restores the committed lane
        CHECK(u.state().markers==committed.markers&&u.state().revision==10);
    }

    // ---- Capacity: exactly 256 locators; the 257th fails on every entry path ----
    {   Session m; m.add("Busy",0);
        for(uint64_t i=0;i<kMaxMarkersPerProject;++i)m.addMarker(i*100,"M"+std::to_string(i),1+i);
        CHECK(m.state().markers.size()==256&&m.state().revision==257);
        rejectsMessage("Too many markers",[&]{m.addMarker(25600,"One too many",257);});
        CHECK(m.state().revision==257&&m.state().markers.size()==256);
        m.removeMarker(25500,257);                                               // rev 258; freeing a slot refits the cap
        m.addMarker(25600,"Fits again",258);                                     // rev 259
        CHECK(m.state().markers.size()==256&&m.state().markers.back().frame==25600);
        State over=m.state(); over.markers.push_back({300000,"257th"});
        rejectsMessage("Too many markers",[&]{m.replace(over);});                // validate() owns the cap for replace()
        State unsorted=m.state(); std::swap(unsorted.markers[10],unsorted.markers[11]);
        rejects([&]{m.replace(unsorted);});                                      // sortedness is a domain invariant
        State edge; edge.markers.push_back({kMaxMidiFrame,"Edge"});
        rejects([&]{m.replace(edge);});                                          // 2^40 is exclusive for every path
        CHECK(m.state().revision==259&&m.state().markers.size()==256);
    }

    // ---- Storage: v18 round trip with RU names, v17 fallback, corruption ----
    {   Session w; w.add("Wired",0);
        w.addMarker(0,"Начало",1);
        w.addMarker(480000,"Куплет 1",2);
        w.addMarker(kMaxMidiFrame-1,"Финал на краю",3);                          // the int64 primary key holds the edge frame
        const auto path=dir/"markers-roundtrip.mydawdraft";
        writeDraft(w.state(),path.string());
        const auto loaded=readDraft(path.string());
        CHECK(loaded.markers==w.state().markers);
        CHECK(loaded.tracks==w.state().tracks&&loaded.revision==w.state().revision&&loaded.nextID==w.state().nextID);
        auto stamped=openDb(path);
        sqlite3_stmt* version=nullptr;
        CHECK(sqlite3_prepare_v2(stamped,"PRAGMA user_version",-1,&version,nullptr)==SQLITE_OK);
        CHECK(sqlite3_step(version)==SQLITE_ROW&&sqlite3_column_int(version,0)==18);
        sqlite3_finalize(version);
        CHECK(sqlite3_exec(stamped,"INSERT INTO markers VALUES(480000,'dup');",nullptr,nullptr,nullptr)!=SQLITE_OK);  // the key itself blocks a duplicate
        CHECK(sqlite3_exec(stamped,"PRAGMA user_version=17;",nullptr,nullptr,nullptr)==SQLITE_OK);
        sqlite3_close(stamped);
        const auto legacy=readDraft(path.string());                              // the v17 gate ignores the table
        CHECK(legacy.markers.empty());
        CHECK(legacy.tempo==w.state().tempo&&legacy.tracks==w.state().tracks);

        Session z; z.add("Quiet",0);                                             // an empty lane round-trips through v18
        const auto quietPath=dir/"markers-empty.mydawdraft";
        writeDraft(z.state(),quietPath.string());
        const auto quiet=readDraft(quietPath.string());
        CHECK(quiet.markers.empty()&&quiet.tracks==z.state().tracks);

        const auto bad=dir/"markers-corrupt.mydawdraft";
        const auto damage=[&](const char* statement){
            writeDraft(w.state(),bad.string());
            auto db=openDb(bad);
            CHECK(sqlite3_exec(db,statement,nullptr,nullptr,nullptr)==SQLITE_OK);
            sqlite3_close(db);
            rejects([&]{(void)readDraft(bad.string());});
        };
        damage("UPDATE markers SET name='' WHERE frame=0;");                      // empty name
        damage("UPDATE markers SET name=CAST(x'F09080' AS TEXT) WHERE frame=0;"); // truncated UTF-8
        damage("UPDATE markers SET name=x'C080' WHERE frame=0;");                 // non-text storage class
        damage("UPDATE markers SET frame=1099511627776 WHERE frame=480000;");     // exactly kMaxMidiFrame
        damage("UPDATE markers SET frame=-1 WHERE frame=480000;");                // negative rowid casts past the limit
        damage("BEGIN; CREATE TABLE markers_dup(frame INTEGER, name TEXT); INSERT INTO markers_dup SELECT frame,name FROM markers; INSERT INTO markers_dup VALUES(480000,'Куплет 2'); DROP TABLE markers; ALTER TABLE markers_dup RENAME TO markers; COMMIT;");  // duplicates survive only without the key; validate() catches them

        const auto shuffled=dir/"markers-shuffled.mydawdraft";                    // physical order is irrelevant
        writeDraft(w.state(),shuffled.string());
        {   auto db=openDb(shuffled);
            CHECK(sqlite3_exec(db,"BEGIN; CREATE TABLE markers_sorted(frame INTEGER PRIMARY KEY, name TEXT NOT NULL); INSERT INTO markers_sorted SELECT frame,name FROM markers ORDER BY frame DESC; DROP TABLE markers; ALTER TABLE markers_sorted RENAME TO markers; COMMIT;",nullptr,nullptr,nullptr)==SQLITE_OK);
            sqlite3_close(db);
        }
        const auto fixed=readDraft(shuffled.string());
        CHECK(fixed.markers==w.state().markers);                                  // the reader's ORDER BY normalises the lane
    }

    std::cout << "PASS: marker model: optional strictly-ordered locator lane reusing the 1-120 character name rule, three revision-checked commands with duplicate-frame rejects and silent no-op renames, undo/redo, writeDraft/readDraft round trip at schema v18 with v17 default-empty fallback, primary-key uniqueness and 256-marker caps" << std::endl;
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}}
