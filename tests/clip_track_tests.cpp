#include "domain/session.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
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
    auto dir=std::filesystem::temp_directory_path()/("mydaw-clip-track-"+std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    struct Cleanup { std::filesystem::path p; ~Cleanup(){ std::error_code ig; std::filesystem::remove_all(p,ig);} } cleanup{dir};

    // ---- Track property commands: rev-checked, silent no-op, undo/redo ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.0f)); // 2s stereo
        s.importAt("Audio",clip,0,0);                                   // track id 1, rev 1, one region
        CHECK(s.state().tracks.size()==1&&s.state().tracks[0].id==1);
        s.setTrackMuted(1,true,1);                                      // rev 2
        CHECK(s.state().tracks[0].muted==true);
        s.setTrackMuted(1,true,2);                                      // identical: silent no-op
        CHECK(s.state().revision==2);
        s.setTrackSolo(1,true,2);                                       // rev 3
        s.setTrackColor(1,0x1234ABu,3);                                // rev 4
        CHECK(s.state().tracks[0].solo==true&&s.state().tracks[0].color==0x1234ABu);
        s.setTrackGain(1,-6.0,4);                                       // rev 5
        CHECK(std::abs(s.state().tracks[0].gain-(-6.0))<1e-9);
        s.setTrackGain(1,-6.0,5);                                       // identical: silent no-op
        CHECK(s.state().revision==5);
        rejectsMessage("Track not found",[&]{s.setTrackMuted(999,true,5);});
        rejectsMessage("Track gain outside -60..12 dB",[&]{s.setTrackGain(1,50.0,5);});
        rejectsMessage("Track gain outside -60..12 dB",[&]{s.setTrackGain(1,-99.0,5);});
        // undo/redo restores the whole property set (undo to the imported default, then redo back)
        uint64_t r=s.state().revision;                                  // 5
        for(int i=0;i<4;++i){const uint64_t cur=s.state().revision; s.undo(cur); (void)r;}
        CHECK(s.state().tracks[0].muted==false&&s.state().tracks[0].solo==false&&s.state().tracks[0].color==0&&std::abs(s.state().tracks[0].gain)<1e-9);
        for(int i=0;i<4;++i){const uint64_t cur=s.state().revision; s.redo(cur);}
        CHECK(s.state().tracks[0].muted==true&&s.state().tracks[0].solo==true&&s.state().tracks[0].color==0x1234ABu&&std::abs(s.state().tracks[0].gain+6.0)<1e-9);
    }

    // ---- Clip (region) property commands: color + gain in dB ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.25f));
        s.importAt("Audio",clip,0,0);                                   // track 1, region 0, rev 1
        s.setClipColor(1,0,0x123456u,1);                                // rev 2
        CHECK(s.state().tracks[0].regions[0].color==0x123456u);
        s.setClipColor(1,0,0x123456u,2);                                // identical no-op
        CHECK(s.state().revision==2);
        s.setClipGain(1,0,-3.0,2);                                      // rev 3
        CHECK(std::abs(s.state().tracks[0].regions[0].gain-(-3.0))<1e-9);
        s.setClipGain(1,0,-3.0,3);                                      // identical no-op
        CHECK(s.state().revision==3);
        rejectsMessage("Audio clip not found",[&]{s.setClipColor(1,5,0x1u,3);});
        rejectsMessage("Audio clip not found",[&]{s.setClipColor(999,0,0x1u,3);});
        rejectsMessage("Clip gain outside -60..12 dB",[&]{s.setClipGain(1,0,20.0,3);});
        for(int i=0;i<2;++i){const uint64_t cur=s.state().revision; s.undo(cur);}
        CHECK(s.state().tracks[0].regions[0].gain==0.0&&s.state().tracks[0].regions[0].color==0);
        for(int i=0;i<2;++i){const uint64_t cur=s.state().revision; s.redo(cur);}
        CHECK(s.state().tracks[0].regions[0].color==0x123456u&&std::abs(s.state().tracks[0].regions[0].gain+3.0)<1e-9);
    }

    // ---- duplicateTrack: new id, copied regions + midi, name + " copy", undo ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2,0.0f));
        s.importAt("Drums",clip,0,0);                                   // track 1, rev 1
        s.setClipColor(1,0,0xABCDEFu,1);                               // rev 2
        MidiClip mc; mc.track=0; mc.start=0; mc.length=48000*4; mc.notes.push_back({0,480,60,0,100});
        s.addMidiClip(1,mc,2);                                          // rev 3
        const uint64_t id=s.duplicateTrack(1,3);                       // rev 4
        CHECK(id!=1);
        CHECK(s.state().tracks.size()==2);
        const auto& src=s.state().tracks[0];
        const auto& dup=s.state().tracks[1];
        CHECK(dup.id==id&&dup.name=="Drums copy");
        CHECK(dup.regions.size()==src.regions.size()&&dup.regions[0].color==0xABCDEFu);
        CHECK(dup.midiClips.size()==1&&dup.midiClips[0].notes.size()==1);
        rejectsMessage("Track not found",[&]{s.duplicateTrack(999,4);});
        const uint64_t cur=s.state().revision; s.undo(cur);            // rev 5
        CHECK(s.state().tracks.size()==1);
        const uint64_t cur2=s.state().revision; s.redo(cur2);          // rev 6
        CHECK(s.state().tracks.size()==2&&s.state().tracks[1].id==id);
    }

    // ---- MIDI clip: color, transpose (clamped), quantize ----
    {   Session s; s.add("MIDI",0);                                     // track 1, rev 1
        MidiClip mc; mc.track=0; mc.start=0; mc.length=48000*8;
        mc.notes.push_back({0,480,60,0,100});       // C4 at beat 0 (on grid)
        mc.notes.push_back({36000,480,64,0,100});   // E4 at 0.75 beat (off grid, snaps up to 1 beat)
        mc.notes.push_back({96000,480,127,0,100});  // high pitch for clamp test
        s.addMidiClip(1,mc,1);                                       // rev 2
        s.setMidiClipColor(1,0,0x0F0F0Fu,2);                        // rev 3
        CHECK(s.state().tracks[0].midiClips[0].color==0x0F0F0Fu);
        s.setMidiClipColor(1,0,0x0F0F0Fu,3);                        // identical no-op
        CHECK(s.state().revision==3);
        s.transposeMidiClip(1,0,2,3);                               // rev 4: +2 semitones
        { const auto& n=s.state().tracks[0].midiClips[0].notes;
          CHECK(n[0].pitch==62&&n[1].pitch==66&&n[2].pitch==127); }
        s.transposeMidiClip(1,0,0,4);                               // semitones==0: silent no-op
        CHECK(s.state().revision==4);
        s.transposeMidiClip(1,0,-127,4);                            // rev 5: -127 clamps every pitch down to 0
        { const auto& n=s.state().tracks[0].midiClips[0].notes;
          CHECK(n[0].pitch==0&&n[1].pitch==0&&n[2].pitch==0); }
        s.quantizeMidiClip(1,0,1.0,5);                              // rev 6: snap to whole beats (48000 f at 120bpm)
        { const auto& n=s.state().tracks[0].midiClips[0].notes;
          CHECK(n[0].start==0);            // already on grid
          CHECK(n[1].start==48000);        // 24000 -> snapped to beat (round 0.5 -> 1)
          CHECK(n[2].start==96000); }
        s.quantizeMidiClip(1,0,0.0,6);                              // grid<=0: silent no-op
        CHECK(s.state().revision==6);
        rejectsMessage("MIDI clip not found",[&]{s.setMidiClipColor(999,0,1u,6);});
        rejectsMessage("MIDI clip not found",[&]{s.setMidiClipColor(1,9,1u,6);});
        // undo fully back to the freshly-added clip, then redo forward
        for(int i=0;i<4;++i){const uint64_t c=s.state().revision; s.undo(c);}   // -> rev 2
        CHECK(s.state().tracks[0].midiClips[0].color==0);
        for(int i=0;i<4;++i){const uint64_t c=s.state().revision; s.redo(c);}   // -> rev 6
        CHECK(s.state().tracks[0].midiClips[0].color==0x0F0F0Fu&&s.state().tracks[0].midiClips[0].notes[1].start==48000);
    }

    // ---- Storage v19 round trip: clip/track color + per-region gain persist ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2,0.0f));
        s.importAt("Persist",clip,0,0);                             // track 1, rev 1
        s.setTrackColor(1,0x112233u,1);                            // rev 2
        s.setClipColor(1,0,0x445566u,2);                          // rev 3
        s.setClipGain(1,0,-9.0,3);                              // rev 4
        const auto path=dir/"cliptrack-roundtrip.mydawdraft";
        writeDraft(s.state(),path.string());
        const auto loaded=readDraft(path.string());
        CHECK(loaded.tracks.size()==1);
        CHECK(loaded.tracks[0].color==0x112233u);
        CHECK(loaded.tracks[0].regions.size()==1);
        CHECK(loaded.tracks[0].regions[0].color==0x445566u);
        CHECK(std::abs(loaded.tracks[0].regions[0].gain-(-9.0))<1e-9);
        auto db=openDb(path);
        sqlite3_stmt* version=nullptr;
        CHECK(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&version,nullptr)==SQLITE_OK);
        CHECK(sqlite3_step(version)==SQLITE_ROW&&sqlite3_column_int(version,0)==19);
        sqlite3_finalize(version);
        sqlite3_close(db);
    }

    std::cout << "PASS: clip/track editing: track mute/solo/color/gain + duplicateTrack, per-region clip color + gain (dB), MIDI clip color/transpose(clamped)/quantize, all revision-checked with silent no-ops and snapshot undo/redo, and schema v19 round trip persisting track/region color and per-region gain" << std::endl;
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}}
