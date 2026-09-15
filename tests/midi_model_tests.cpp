#include "audio/recording.hpp"
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
// One poll() drain of a capture source, converted to recorder events and fed
// as the control thread would: a contiguous batch, oldest event first.
static void feedAll(MidiRecorder& recorder,const std::vector<RecordedMidiEvent>& events){recorder.feed(events.data(),events.size());}

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
    // ---- appendMidiNotes: the record-to-clip commit command ----
    {   Session a; a.add("Keys",0);                                                           // rev 1
        a.addMidiClip(1,MidiClip{0,48000,{note(0,480)},0},1);                                 // rev 2
        a.appendMidiNotes(1,0,{},2);                                                          // empty batch is a no-op
        CHECK(a.state().revision==2&&a.state().tracks[0].midiClips[0].notes.size()==1);
        a.appendMidiNotes(1,0,{note(1000,240,64,2,90),note(900,47100,67,0,1)},2);              // rev 3; unsorted on purpose
        const auto recorded=a.state().tracks[0].midiClips[0].notes;
        CHECK(recorded.size()==3&&recorded[0]==note(0,480)&&recorded[1]==note(1000,240,64,2,90));
        CHECK(recorded[2]==note(900,47100,67,0,1));                                           // capture order is kept verbatim
        const auto after=a.state();
        rejects([&]{a.appendMidiNotes(1,0,{note(48000,480)},3);});                            // starts on the clip end frame
        rejects([&]{a.appendMidiNotes(1,0,{note(47000,2000)},3);});                           // runs over the end; the clip never grows
        rejects([&]{a.appendMidiNotes(1,0,{note(0,480,60,0,0)},3);});                         // velocity 0
        rejects([&]{a.appendMidiNotes(1,0,{note(0,480,128,0,100)},3);});                      // pitch 128
        rejects([&]{a.appendMidiNotes(1,0,{note(0,480,60,16,100)},3);});                      // channel 16
        rejects([&]{a.appendMidiNotes(1,0,{note(0,0)},3);});                                  // zero-length note
        rejects([&]{a.appendMidiNotes(1,0,{note(0,kMaxMidiNoteLength+1)},3);});               // note length limit
        rejects([&]{a.appendMidiNotes(1,0,{note(2000,480),note(48000,480)},3);});             // one bad note fails the batch
        rejects([&]{a.appendMidiNotes(1,0,{note(0,480)},2);});                                // stale revision
        rejects([&]{a.appendMidiNotes(1,1,{note(0,480)},3);});                                // clip position not found
        rejects([&]{a.appendMidiNotes(9,0,{note(0,480)},3);});                                // track not found
        CHECK(a.state().revision==3&&a.state().tracks==after.tracks);
        a.undo(3);                                                                            // rev 4; one append is one undo step
        CHECK(a.state().revision==4&&a.state().tracks[0].midiClips[0].notes.size()==1);
        a.redo(4);                                                                            // rev 5
        CHECK(a.state().revision==5&&a.state().tracks==after.tracks);
        const auto appendPath=dir/"midi-append.mydawdraft";                                   // appended notes are ordinary stored notes
        writeDraft(a.state(),appendPath.string());
        const auto reloaded=readDraft(appendPath.string());
        CHECK(reloaded.tracks==a.state().tracks&&reloaded.tracks[0].midiClips[0].notes==recorded);
        CHECK(reloaded.revision==a.state().revision&&reloaded.nextID==a.state().nextID);
    }

    // ---- append limits: one batch is bounded, the project budget stays final ----
    {   Session l; l.add("Dense",0);                                                          // rev 1
        const uint64_t span=static_cast<uint64_t>(kMaxMidiNotesPerProject)*480;
        l.addMidiClip(1,MidiClip{0,2*span,{},0},1);                                           // rev 2; a window wide enough for a whole batch
        std::vector<MidiNote> full; full.reserve(kMaxMidiNotesPerProject);
        for(uint64_t i=0;i<kMaxMidiNotesPerProject;++i)full.push_back({i*480,480,60,0,100});
        std::vector<MidiNote> tooMany=full; tooMany.push_back({0,480,61,0,100});
        rejects([&]{l.appendMidiNotes(1,0,tooMany,2);});                                      // 65537 notes in one batch
        CHECK(l.state().revision==2&&l.state().tracks[0].midiClips[0].notes.empty());
        l.appendMidiNotes(1,0,full,2);                                                        // rev 3; the project is now exactly full
        CHECK(l.state().tracks[0].midiClips[0].notes.size()==kMaxMidiNotesPerProject);
        rejects([&]{l.appendMidiNotes(1,0,{note(1,480)},3);});                                // no budget left to append into
        rejects([&]{l.addMidiClip(1,MidiClip{2*span+48000,480,{note(0,480)},1},3);});         // nor to add a note in a new clip
        CHECK(l.state().revision==3);
        l.undo(3);                                                                            // rev 4; the take leaves and the budget returns
        CHECK(l.state().revision==4&&l.state().tracks[0].midiClips[0].notes.empty());
        l.addMidiClip(1,MidiClip{2*span+48000,960,{note(0,480),note(1,480)},1},4);            // rev 5
        CHECK(l.state().tracks[0].midiClips.size()==2);
    }

    // ---- MidiRecorder: a live event stream becomes a commit-ready batch ----
    {   MidiRecorder rec;
        // an armed take with nothing fed is simply empty, and stop() consumes it
        rec.arm(1,0); CHECK(rec.armed()&&rec.trackID()==1&&rec.clipIndex()==0);
        CHECK(rec.stop(48000).empty()&&!rec.armed()&&rec.dropped()==0&&rec.unmatched()==0);
        // held from frame 1000 to 5000, i.e. length is off minus on
        rec.arm(1,0);
        rec.feed({1000,60,0,100,false});
        CHECK(rec.openNotes()==1&&rec.recordedNotes()==0);                                    // nothing is committable while the key is down
        rec.feed({5000,60,0,0,true});                                                         // 0x90 velocity 0 arrives normalised to an off
        CHECK(rec.openNotes()==0&&rec.recordedNotes()==1);
        auto batch=rec.stop(6000);
        CHECK(batch.size()==1&&batch[0]==note(1000,4000,60,0,100));
        // a key still held down at stop() ends with the take instead of vanishing
        rec.arm(2,1);
        rec.feed({1000,64,3,70,false});
        batch=rec.stop(5000);
        CHECK(batch.size()==1&&batch[0]==note(1000,4000,64,3,70)&&rec.trackID()==2&&rec.clipIndex()==1);
        // batch order is close order, not attack order
        rec.arm(1,0);
        feedAll(rec,{{0,60,0,100,false},{100,61,0,101,false},{200,61,0,0,true},{300,60,0,0,true}});
        batch=rec.stop(300);
        CHECK(batch.size()==2&&batch[0]==note(100,100,61,0,101)&&batch[1]==note(0,300,60,0,100));
        // one pitch held twice closes last-in/first-out
        rec.arm(1,0);
        feedAll(rec,{{0,61,0,100,false},{1000,61,0,110,false},{2000,61,0,0,true},{3000,61,0,0,true}});
        batch=rec.stop(3000);
        CHECK(batch.size()==2&&batch[0]==note(1000,1000,61,0,110)&&batch[1]==note(0,3000,61,0,100));
        // events that could only produce an invalid note are refused at the door
        rec.arm(1,0);
        feedAll(rec,{{0,60,0,0,false},{100,128,0,100,false},{200,60,16,100,false},{300,60,0,128,false},{kMaxMidiFrame,60,0,100,false}});
        CHECK(rec.dropped()==5&&rec.openNotes()==0&&rec.recordedNotes()==0);
        rec.feed({1000,60,5,100,false});
        rec.feed({1500,60,6,0,true});                                                         // an off on another channel matches nothing
        CHECK(rec.unmatched()==1&&rec.openNotes()==1);
        batch=rec.stop(2000);
        CHECK(batch.size()==1&&batch[0]==note(1000,1000,60,5,100));
        // out-of-order frames are clamped forward: a note can be one frame, never zero
        rec.arm(1,0);
        rec.feed({5000,62,0,100,false});
        rec.feed({1000,62,0,0,true});
        batch=rec.stop(5000);
        CHECK(batch.size()==1&&batch[0]==note(5000,1,62,0,100));
        // feeding outside a take records nothing
        MidiRecorder idle;
        idle.feed({0,60,0,100,false});
        CHECK(idle.recordedNotes()==0&&idle.openNotes()==0&&idle.dropped()==0);
        idle.arm(1,0); idle.feed({0,60,0,100,false}); idle.feed({500,60,0,0,true}); idle.stop(500);
        idle.feed({600,61,0,100,false});
        CHECK(!idle.armed()&&idle.openNotes()==0&&idle.recordedNotes()==0&&idle.stop(700).empty());
        // re-arming abandons the previous take; a take is never an accumulator
        idle.arm(1,0); idle.feed({0,60,0,100,false}); idle.feed({480,60,0,0,true});
        CHECK(idle.recordedNotes()==1);
        idle.arm(1,1);
        CHECK(idle.recordedNotes()==0&&idle.openNotes()==0&&idle.clipIndex()==1&&idle.stop(0).empty());
        idle.arm(1,0); feedAll(idle,{{0,60,0,100,false},{480,60,0,0,true}}); idle.disarm();
        CHECK(!idle.armed()&&idle.recordedNotes()==0&&idle.openNotes()==0);
        // a take cannot outgrow the note budget the model allows
        MidiRecorder dense; dense.arm(1,0);
        for(uint64_t i=0;i<MidiRecorder::kMaxTakeNotes;++i)dense.feed({2*i,60,0,100,false});
        CHECK(dense.openNotes()==MidiRecorder::kMaxTakeNotes&&dense.dropped()==0);
        dense.feed({2*MidiRecorder::kMaxTakeNotes,60,0,100,false});
        CHECK(dense.dropped()==1&&dense.openNotes()==MidiRecorder::kMaxTakeNotes);
        auto fullTake=dense.stop(48000*3);
        CHECK(fullTake.size()==kMaxMidiNotesPerProject);
        MidiClip wide; wide.start=0; wide.length=48000*3; wide.track=0;
        CHECK(midiRecordBatchFits(wide,fullTake));
    }

    // ---- midiRecordBatchFits predicts the command for the live path ----
    {   MidiClip clip; clip.start=0; clip.length=4800; clip.track=0;
        CHECK(midiRecordBatchFits(clip,{}));                                                  // nothing to append still fits
        CHECK(midiRecordBatchFits(clip,{note(0,4800)}));                                      // flush with the end is inside
        CHECK(midiRecordBatchFits(clip,{note(0,480),note(4000,800),note(100,4700)}));         // overlapping lanes are legal
        CHECK(!midiRecordBatchFits(clip,{note(4800,1)}));                                     // starts past the end
        CHECK(!midiRecordBatchFits(clip,{note(4700,200)}));                                   // runs over the end
        CHECK(!midiRecordBatchFits(clip,{note(0,0)}));                                        // zero length
        CHECK(!midiRecordBatchFits(clip,{note(0,kMaxMidiNoteLength+1)}));                     // over the note length limit
        CHECK(!midiRecordBatchFits(clip,{note(0,480,60,0,0)}));                               // velocity 0
        CHECK(!midiRecordBatchFits(clip,{note(0,480,128,0,100)}));                            // pitch out of range
        CHECK(!midiRecordBatchFits(clip,{note(0,480,60,16,100)}));                            // channel out of range
        Session p; p.add("Keys",0); p.addMidiClip(1,MidiClip{0,4800,{},0},1);                 // rev 2
        const std::vector<MidiNote> overflow{note(0,480),note(4700,200)};
        CHECK(!midiRecordBatchFits(p.state().tracks[0].midiClips[0],overflow));
        rejects([&]{p.appendMidiNotes(1,0,overflow,2);});                                     // refused before any state is copied
        CHECK(p.state().revision==2&&p.state().tracks[0].midiClips[0].notes.empty());
        const std::vector<MidiNote> fits{note(0,480),note(4700,100)};
        CHECK(midiRecordBatchFits(p.state().tracks[0].midiClips[0],fits));
        p.appendMidiNotes(1,0,fits,2);                                                        // rev 3
        CHECK(p.state().revision==3&&p.state().tracks[0].midiClips[0].notes==fits);
    }

    // ---- end to end: a live take is recorded, committed, saved and undone ----
    {   Session live; live.add("Keys",0);                                                     // rev 1
        live.addMidiClip(1,MidiClip{96000,48000,{},0},1);                                     // rev 2; the clip starts mid-timeline
        MidiRecorder rec; rec.arm(1,0);
        // transport is already running, so the caller feeds clip-relative frames
        feedAll(rec,{{0,60,0,100,false},{1000,64,0,95,false},{2000,60,0,0,true},{2400,64,0,0,true},
                     {5000,67,1,70,false},{8000,67,1,0,true},{9000,62,0,88,false}});
        CHECK(rec.recordedNotes()==3&&rec.openNotes()==1&&rec.unmatched()==0&&rec.dropped()==0);
        const auto take=rec.stop(10000);                                                      // the C6 still down ends with the take
        CHECK(take.size()==4&&take[0]==note(0,2000,60,0,100)&&take[1]==note(1000,1400,64,0,95));
        CHECK(take[2]==note(5000,3000,67,1,70)&&take[3]==note(9000,1000,62,0,88));
        const auto& target=live.state().tracks[0].midiClips[0];
        CHECK(target.notes.empty()&&midiRecordBatchFits(target,take));
        live.appendMidiNotes(1,0,take,2);                                                     // rev 3
        CHECK(live.state().tracks[0].midiClips[0].notes==take);
        CHECK(live.state().tracks[0].midiClips[0].start==96000&&live.state().tracks[0].midiClips[0].length==48000);
        const auto takePath=dir/"midi-take.mydawdraft";
        writeDraft(live.state(),takePath.string());
        CHECK(readDraft(takePath.string()).tracks==live.state().tracks);
        live.undo(3);                                                                         // rev 4
        CHECK(live.state().tracks[0].midiClips[0].notes.empty());
        live.redo(4);                                                                         // rev 5
        CHECK(live.state().tracks[0].midiClips[0].notes==take);
    }
    std::cout << "PASS: MIDI model: clips/notes validated, seven revision-checked commands (appendMidiNotes keeps capture order and never grows a clip), undo/redo and writeDraft/readDraft round trip at schema v18 with v15/v16/v17 fallback, MidiRecorder takes and the batch fit predicate" << std::endl;
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}}
