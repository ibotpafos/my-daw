#include "domain/session.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while (false)
template <class Fn> void rejects(Fn&& fn) { bool rejected = false; try { fn(); } catch (...) { rejected = true; } CHECK(rejected); }
using namespace daw;

static MidiNote note(uint64_t start,uint64_t length,uint8_t pitch=60,uint8_t channel=0,uint8_t velocity=100){return {start,length,pitch,channel,velocity};}
static sqlite3* openDb(const std::filesystem::path& path){sqlite3* db=nullptr;if(sqlite3_open(path.c_str(),&db)!=SQLITE_OK)throw std::runtime_error("Cannot open draft db");return db;}

int main(){try{
    auto dir=std::filesystem::temp_directory_path()/("mydaw-midi-model-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    struct Cleanup { std::filesystem::path p; ~Cleanup(){ std::error_code ig; std::filesystem::remove_all(p,ig);} } cleanup{dir};

    // ---- Happy-path command flow with revision bookkeeping ----
    Session s; s.add("Piano",0);                                                              // rev 1
    MidiClip clip; clip.start=0; clip.length=48000; clip.track=0; clip.notes={note(0,480),note(960,480,64),note(2400,1200,67)};
    s.addMidiClip(1,clip,1);                                                                  // rev 2
    CHECK(s.state().tracks[0].midiClips.size()==1&&s.state().tracks[0].midiClips[0].notes.size()==3);
    auto extended=clip.notes; extended.push_back(note(4000,400,72));
    s.setMidiNotes(1,0,extended,2);                                                           // rev 3
    CHECK(s.state().tracks[0].midiClips[0].notes.size()==4);
    s.setMidiNotes(1,0,extended,3);                                                           // no-op
    CHECK(s.state().revision==3);
    s.moveMidiClip(1,0,96000,3);                                                              // rev 4
    CHECK(s.state().tracks[0].midiClips[0].start==96000);
    s.addMidiClip(1,MidiClip{240000,48000,{note(0,480)},1},4);                                // rev 5
    s.trimMidiClip(1,1,250000,20000,5);                                                       // rev 6; note falls outside the window
    CHECK(s.state().tracks[0].midiClips[1].start==250000&&s.state().tracks[0].midiClips[1].length==20000&&s.state().tracks[0].midiClips[1].notes.empty());
    s.trimMidiClip(1,0,90000,60000,6);                                                        // rev 7; window grows left, notes shift
    CHECK(s.state().tracks[0].midiClips[0].notes[0].start==6000);
    s.splitMidiClip(1,0,90000+5000,7);                                                        // rev 8; inside gap between note 0 and 1
    CHECK(s.state().tracks[0].midiClips.size()==3&&s.state().tracks[0].midiClips[0].notes.empty()&&s.state().tracks[0].midiClips[1].start==95000&&s.state().tracks[0].midiClips[1].notes.size()==4);
    s.removeMidiClip(1,2,8);                                                                  // rev 9
    CHECK(s.state().tracks[0].midiClips.size()==2);

    // ---- Rejections (revision is consumed only by committed commands) ----
    const auto rev=s.state().revision;
    rejects([&]{s.addMidiClip(1,MidiClip{500000,48000,{note(0,480,128)},0},rev);});          // pitch 128
    rejects([&]{s.addMidiClip(1,MidiClip{500000,48000,{note(0,480,60,0,0)},0},rev);});       // velocity 0
    rejects([&]{s.addMidiClip(1,MidiClip{500000,48000,{note(0,480,60,16,1)},0},rev);});      // channel 16
    rejects([&]{s.addMidiClip(1,MidiClip{100000,48000,{note(0,480)},0},rev);});              // overlap
    rejects([&]{s.addMidiClip(1,MidiClip{500000,1000,{note(900,200)},0},rev);});             // note past clip end
    rejects([&]{s.addMidiClip(1,MidiClip{500000,0,{},0},rev);});                             // zero-length clip
    rejects([&]{s.addMidiClip(1,MidiClip{500000,48000,{},-1},rev);});                        // negative lane
    rejects([&]{s.addMidiClip(1,MidiClip{500000,kMaxMidiFrame+1,{note(0,480)},0},rev);});    // clip length over timeline limit
    rejects([&]{s.addMidiClip(1,MidiClip{500000,5000,{note(0,kMaxMidiNoteLength+1)},0},rev);});// note length limit
    CHECK(s.state().revision==rev);
    s.add("Extra",rev);                                                                       // rev 10

    // ---- Project-wide note limit: 65536 accepted, 65537 rejected ----
    {   Session l; l.add("Dense",0);
        std::vector<MidiNote> half; half.reserve(kMaxMidiNotesPerProject/2);
        for(uint64_t i=0;i<kMaxMidiNotesPerProject/2;++i)half.push_back({i*480,480,60,0,100});
        const uint64_t span=(kMaxMidiNotesPerProject/2)*480;
        l.addMidiClip(1,MidiClip{0,span,half,0},1);
        l.addMidiClip(1,MidiClip{span+48000,span,half,1},2);
        CHECK(l.state().revision==3);
        rejects([&]{l.addMidiClip(1,MidiClip{2*span+96000,480,{note(0,480)},2},3);});
        CHECK(l.state().revision==3);
    }
    // ---- Per-track clip limit: 64 accepted, 65th rejected ----
    {   Session c; c.add("Crowded",0);
        for(uint64_t i=0;i<64;++i)c.addMidiClip(1,MidiClip{i*4800,4800,{note(0,480)},static_cast<int>(i)},1+i);
        CHECK(c.state().tracks[0].midiClips.size()==64);
        rejects([&]{c.addMidiClip(1,MidiClip{64*4800,4800,{note(0,480)},0},65);});
    }

    // ---- Undo/redo preserves MIDI exactly ----
    {   Session u; u.add("Keys",0);
        MidiClip c; c.start=0; c.length=48000; c.track=3; c.notes={note(0,480),note(4800,960,62,5,90)};
        u.addMidiClip(1,c,1);                                                                 // rev 2
        const auto before=u.state();
        u.moveMidiClip(1,0,12000,2);                                                          // rev 3
        CHECK(u.state().tracks[0].midiClips[0].start==12000);
        u.undo(3);                                                                            // rev 4
        CHECK(u.state().tracks==before.tracks&&u.state().tracks[0].midiClips[0].start==0);
        u.redo(4);                                                                            // rev 5
        CHECK(u.state().tracks[0].midiClips[0].start==12000&&u.state().tracks[0].midiClips[0].notes==c.notes);

        // ---- writeDraft/readDraft round trip (draft files retain MIDI) ----
        const auto path=dir/"midi-roundtrip.mydawdraft";
        writeDraft(u.state(),path.string());
        const auto loaded=readDraft(path.string());
        CHECK(loaded.tracks==u.state().tracks&&loaded.revision==u.state().revision&&loaded.nextID==u.state().nextID);
        CHECK(loaded.tracks[0].midiClips.size()==1&&loaded.tracks[0].midiClips[0].track==3&&loaded.tracks[0].midiClips[0].notes==c.notes);

        // ---- Legacy v15 drafts (no MIDI tables) still open with empty clips ----
        const auto legacy=dir/"midi-legacy.mydawdraft";
        writeDraft(u.state(),legacy.string());
        auto downgraded=openDb(legacy);
        CHECK(sqlite3_exec(downgraded,"PRAGMA user_version=15;",nullptr,nullptr,nullptr)==SQLITE_OK);
        sqlite3_close(downgraded);
        const auto legacyState=readDraft(legacy.string());
        auto withoutMidi=u.state();for(auto& t:withoutMidi.tracks)t.midiClips.clear();
        CHECK(legacyState.tracks==withoutMidi.tracks&&legacyState.tracks[0].midiClips.empty());

        // ---- Corrupted MIDI payloads are rejected by the reader ----
        const auto bad=dir/"midi-bad.mydawdraft";
        writeDraft(u.state(),bad.string());
        auto damaged=openDb(bad);
        CHECK(sqlite3_exec(damaged,"UPDATE midi_notes SET pitch=128 WHERE position=0;",nullptr,nullptr,nullptr)==SQLITE_OK);
        sqlite3_close(damaged);
        rejects([&]{(void)readDraft(bad.string());});
    }
    std::cout << "PASS: MIDI model: clips/notes validated, six revision-checked commands, undo/redo and writeDraft/readDraft round trip at schema v16 with v15 fallback" << std::endl;
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}}
