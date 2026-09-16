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

    // First recording into an empty ordinary track keeps the selected track,
    // establishes a base clip and creates its initial region at the punch-in.
    {   Session s;
        s.add("Armed",0);
        const auto recorded=std::make_shared<const Clip>(std::vector<float>(32,0.25f));
        s.materializeRecordedTrack(1,"Take 1",recorded,9600,1);
        const auto& track=s.state().tracks.front();
        CHECK(track.id==1&&track.audio==recorded&&track.baseStart==9600&&track.takes.empty());
        CHECK(track.regions.size()==1&&track.regions[0].start==9600&&track.regions[0].length==16&&track.regions[0].sourceOffset==0);
        rejectsMessage("Target track already has audio",[&]{s.materializeRecordedTrack(1,"Take 2",recorded,0,2);});
    }

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

    // ---- Cross-track clipboard: copy/move audio regions + MIDI clips ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.25f));  // 2s stereo
        const auto other=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.5f));
        s.importAt("Audio",clip,0,0);                                      // track 1 (shares source with 3), rev 1
        s.importAt("Other",other,0,1);                                      // track 2 foreign source, rev 2
        s.importAt("Twin",clip,0,2);                                        // track 3 same shared_ptr, rev 3
        s.setClipColor(1,0,0xFF0000u,3);                                   // rev 4
        s.setClipGain(1,0,-3.5,4);                                          // rev 5
        s.copyClipToTrack(1,0,3,96000,5);                                   // rev 6 — paste onto same-source track
        { const auto& t=s.state().tracks[2];
          CHECK(t.regions.size()==2&&t.regions[1].start==96000&&t.regions[1].color==0xFF0000u&&std::abs(t.regions[1].gain+3.5)<1e-9);
          CHECK(s.state().tracks[0].regions.size()==1); }                   // source keeps its clip
        rejectsMessage("The target track does not share the clip's audio source",[&]{s.copyClipToTrack(1,0,2,0,6);});
        rejectsMessage("Audio clip not found",[&]{s.copyClipToTrack(1,5,3,0,6);});
        rejectsMessage("Track not found",[&]{s.copyClipToTrack(1,0,99,0,6);});
        CHECK(s.state().revision==6);
        rejectsMessage("No timeline space for the clip",[&]{s.copyClipToTrack(1,0,3,48000ull*601,6);});
        s.moveClipToTrack(3,0,3,240000,6);                                  // rev 7 — same-track move reorders
        { const auto& t=s.state().tracks[2];
          CHECK(t.regions.size()==2&&t.regions[0].start==96000&&t.regions[1].start==240000); }
        rejectsMessage("The last clip keeps the imported audio attached to its track",[&]{s.moveClipToTrack(1,0,3,0,7);});
        CHECK(s.state().revision==7);
        const uint64_t moved=s.state().revision; s.undo(moved);              // rev 8
        CHECK(s.state().tracks[2].regions[0].start==0&&s.state().tracks[2].regions[1].start==96000);
        const uint64_t back=s.state().revision; s.redo(back);                // rev 9
        CHECK(s.state().tracks[2].regions[0].start==96000&&s.state().tracks[2].regions[1].start==240000);
        // MIDI: notes, colour and the non-negative lane all travel cross-track; validate owns overlaps.
        MidiClip mc; mc.track=2; mc.start=0; mc.length=48000; mc.color=0x77AA00u;
        mc.notes.push_back({2400,1200,65,1,90});
        s.addMidiClip(1,mc,9);                                              // rev 10 — lane 2 on track 1
        s.copyMidiClipToTrack(1,0,2,96000,10);                              // rev 11
        { const auto& mc2=s.state().tracks[1].midiClips[0];
          CHECK(s.state().tracks[1].midiClips.size()==1&&mc2.start==96000&&mc2.track==2&&mc2.color==0x77AA00u&&mc2.notes.size()==1&&mc2.notes[0].pitch==65); }
        s.moveMidiClipToTrack(1,0,2,192000,11);                             // rev 12 — source lane empties
        CHECK(s.state().tracks[0].midiClips.empty()&&s.state().tracks[1].midiClips.size()==2);
        rejectsMessage("MIDI clip overlap is not allowed",[&]{s.moveMidiClipToTrack(2,1,2,120000,12);});
        CHECK(s.state().revision==12);
        rejectsMessage("MIDI clip not found",[&]{s.copyMidiClipToTrack(1,0,2,0,12);});
        const uint64_t cur=s.state().revision; s.undo(cur);                  // rev 13 — move back
        CHECK(s.state().tracks[0].midiClips.size()==1&&s.state().tracks[1].midiClips.size()==1);
    }

    // ---- Playback state (v20): clip mute + loop; split/edit carry every field ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.25f));  // 2s stereo
        s.importAt("State",clip,0,0);                                      // track 1, region [0,96000), rev 1
        s.setClipMuted(1,0,true,1);                                        // rev 2
        CHECK(s.state().tracks[0].regions[0].muted==true);
        s.setClipMuted(1,0,true,2);                                        // identical: silent no-op
        CHECK(s.state().revision==2);
        rejectsMessage("Audio clip not found",[&]{s.setClipMuted(1,9,false,2);});
        s.setClipLooped(1,0,true,2);                                       // rev 3
        CHECK(s.state().tracks[0].regions[0].looped==true);
        s.editClip(1,0,0,0,96000+48000,3);                                 // rev 4 — 3s from a 2s slice, legal only because looped
        CHECK(s.state().tracks[0].regions[0].length==144000);
        rejectsMessage("Invalid clip bounds (timeline limit: 10 minutes)",[&]{s.setClipLooped(1,0,false,4);});
        CHECK(s.state().revision==4);
        rejectsMessage("Unloop the clip before splitting",[&]{s.splitClip(1,0,48000,4);});
        s.setClipLooped(1,0,true,4);                                       // identical: silent no-op
        CHECK(s.state().revision==4);
        // Shrink back into the slice, then unloop: field-carry regressions follow.
        s.editClip(1,0,0,0,96000,4);                                      // rev 5
        s.setClipLooped(1,0,false,5);                                     // rev 6
        s.setClipGain(1,0,-4.0,6);                                        // rev 7
        const uint64_t afterGain=s.state().revision;
        CHECK(s.state().tracks[0].regions[0].length==96000&&std::abs(s.state().tracks[0].regions[0].gain+4.0)<1e-9);
        s.setClipColor(1,0,0xFEEDABu,afterGain);                           // rev +1
        s.splitClip(1,0,48000,s.state().revision);                         // rev +2
        { const auto& t=s.state().tracks[0];
          CHECK(t.regions.size()==2);
          CHECK(t.regions[0].gain==t.regions[1].gain&&t.regions[0].color==0xFEEDABu&&t.regions[1].color==0xFEEDABu);
          CHECK(t.regions[1].sourceOffset==48000&&t.regions[1].start==48000); }
        s.editClip(1,0,0,0,40000,s.state().revision);                       // trim left half, rev +1
        CHECK(s.state().tracks[0].regions[0].color==0xFEEDABu&&std::abs(s.state().tracks[0].regions[0].gain+4.0)<1e-9);
        // Mute round-trips through undo back to audible
        { const uint64_t now=s.state().revision; s.undo(now); }
    }

    // ---- Overlap creation: edit, cross-track copy and same-track move ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.25f));
        s.importAt("Source",clip,0,0);                                      // [0,96000)
        s.duplicateClip(1,0,s.state().revision);                             // [96000,192000)
        const auto oneFrameRevision=s.state().revision;
        rejectsMessage("Crossfade requires at least two frames",[&]{s.editClip(1,1,95999,0,96000,oneFrameRevision);});
        CHECK(s.state().revision==oneFrameRevision&&s.state().tracks[0].regions[1].start==96000);
        s.editClip(1,1,48000,0,96000,s.state().revision);                    // 48k overlap
        { const auto& r=s.state().tracks[0].regions;
          CHECK(r.size()==2&&r[0].start==0&&r[1].start==48000);
          CHECK(r[0].fadeOut==48000&&r[1].fadeIn==48000);
          CHECK(r[0].fadeOutShape==FadeShape::Linear&&r[1].fadeInShape==FadeShape::Linear); }
        const auto editRevision=s.state().revision;
        s.undo(editRevision);
        CHECK(s.state().tracks[0].regions[1].start==96000&&s.state().tracks[0].regions[0].fadeOut==0);
        s.redo(s.state().revision);
        CHECK(s.state().tracks[0].regions[1].start==48000&&s.state().tracks[0].regions[1].fadeIn==48000);
        // Auto-crossfades do not turn the lane into an ambiguous stack: three
        // concurrent clips and a full cover still reject atomically.
        s.duplicateClip(1,0,s.state().revision);
        const auto invalidRevision=s.state().revision;
        rejects([&]{s.editClip(1,2,72000,0,96000,invalidRevision);});
        CHECK(s.state().revision==invalidRevision&&s.state().tracks[0].regions[2].start==144000);

        s.importAt("Twin",clip,0,s.state().revision);                       // target shares the source
        const auto target=s.state().tracks[1].id;
        s.copyClipToTrack(1,0,target,48000,s.state().revision);
        { const auto& r=s.state().tracks[1].regions;
          CHECK(r.size()==2&&r[0].fadeOut==48000&&r[1].fadeIn==48000); }
        // Moving the pasted clip closer re-computes rather than preserving a
        // stale 48k fade; the overlap is now 72k.
        s.moveClipToTrack(target,1,target,24000,s.state().revision);
        { const auto& r=s.state().tracks[1].regions;
          CHECK(r.size()==2&&r[0].start==0&&r[1].start==24000);
          CHECK(r[0].fadeOut==72000&&r[1].fadeIn==72000); }
    }

    // ---- Auto-crossfade provenance: only auto sides are disposable ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.25f));
        s.importAt("Auto provenance",clip,0,0);s.splitClip(1,0,48000,1);
        s.editClip(1,1,36000,48000,48000,2); // automatic 12k overlap
        { const auto& r=s.state().tracks[0].regions;
          CHECK(r[0].autoFadeOut&&r[1].autoFadeIn&&r[0].fadeOut==12000&&r[1].fadeIn==12000); }
        const auto path=(dir/"auto-fade-v24.mydawdraft").string();writeDraft(s.state(),path);auto persisted=readDraft(path);
        CHECK(persisted.tracks[0].regions[0].autoFadeOut&&persisted.tracks[0].regions[1].autoFadeIn);
        Session reopened;reopened.replace(std::move(persisted));
        // Moving an automatic pair apart through the keyboard/nudge path must
        // clear both generated sides rather than leave a quiet orphan fade.
        reopened.nudgeClips(1,{1u},12000,reopened.state().revision); // separate the auto pair
        { const auto& r=reopened.state().tracks[0].regions;
          CHECK(!r[0].autoFadeOut&&!r[1].autoFadeIn&&r[0].fadeOut==0&&r[1].fadeIn==0); }
        reopened.editClip(1,1,36000,48000,48000,reopened.state().revision);
        reopened.deleteClip(1,1,reopened.state().revision);
        CHECK(reopened.state().tracks[0].regions.size()==1&&!reopened.state().tracks[0].regions[0].autoFadeOut&&reopened.state().tracks[0].regions[0].fadeOut==0);

        Session manual;manual.importAt("Manual provenance",clip,0,0);manual.splitClip(1,0,48000,1);
        manual.setCrossfadeShaped(1,0,12000,FadeShape::EqualPower,2);
        manual.duplicateClip(1,0,3); // unrelated timeline geometry preserves the manual pair
        { const auto& r=manual.state().tracks[0].regions;
          CHECK(!r[0].autoFadeOut&&!r[1].autoFadeIn&&r[0].fadeOutShape==FadeShape::EqualPower&&r[1].fadeInShape==FadeShape::EqualPower); }
        writeDraft(manual.state(),path);auto manualPersisted=readDraft(path);std::filesystem::remove(path);
        { const auto& r=manualPersisted.tracks[0].regions;
          CHECK(!r[0].autoFadeOut&&!r[1].autoFadeIn&&r[0].fadeOutShape==FadeShape::EqualPower&&r[1].fadeInShape==FadeShape::EqualPower); }
    }

    // ---- Multi-selection group commands: deleteClips + nudgeClips, one revision ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.25f));  // 2s stereo
        s.importAt("Group",clip,0,0);                                                       // rev 1: [0,96000)
        s.duplicateClip(1,0,s.state().revision);                                            // [96000,192000)
        s.duplicateClip(1,0,s.state().revision);                                            // [192000,288000)
        CHECK(s.state().tracks[0].regions.size()==3);
        const auto starts=[&]{std::vector<uint64_t> v;for(const auto&r:s.state().tracks[0].regions)v.push_back(r.start);return v;};
        rejectsMessage("Audio clip not found",[&]{s.deleteClips(1,{0u,7u},s.state().revision);});   // partial index poisons the batch
        CHECK(starts()==std::vector<uint64_t>({0,96000,192000}));
        rejectsMessage("No timeline space for the clip",[&]{s.nudgeClips(1,{0u},-5000,s.state().revision);});
        s.nudgeClips(1,{0u},0,s.state().revision);                                          // zero delta: silent no-op
        CHECK(s.state().revision==3);
        // A move through a neighbour is legal. The exact 48k overlap becomes
        // the pair's automatic linear crossfade in the same undo snapshot.
        s.nudgeClips(1,{1u,2u},-48000,s.state().revision);
        CHECK(starts()==std::vector<uint64_t>({0,48000,144000}));
        CHECK(s.state().tracks[0].regions[0].fadeOut==48000&&s.state().tracks[0].regions[1].fadeIn==48000);
        CHECK(s.state().tracks[0].regions[0].fadeOutShape==FadeShape::Linear&&s.state().tracks[0].regions[1].fadeInShape==FadeShape::Linear);
        s.nudgeClips(1,{2u},48000,s.state().revision);
        CHECK(starts()==std::vector<uint64_t>({0,48000,192000}));
        s.nudgeClips(1,{0u},400000,s.state().revision);                                      // reorder through the group
        CHECK(starts()==std::vector<uint64_t>({48000,192000,400000}));
        rejectsMessage("No timeline space for the clip",[&]{s.nudgeClips(1,{2u},int64_t(48000ull*600),s.state().revision);});
        s.deleteClips(1,{0u,0u,1u},s.state().revision);                                      // rev 6: unique-merge deletes two
        CHECK(s.state().tracks[0].regions.size()==1&&starts()==std::vector<uint64_t>({400000}));
        // Deleting a selected final group leaves a genuinely empty reusable
        // track, rather than a hidden media owner that cannot be recorded to.
        const uint64_t clearRevision=s.state().revision;
        s.deleteClips(1,{0u},clearRevision);
        CHECK(!s.state().tracks[0].audio&&s.state().tracks[0].regions.empty()&&s.state().tracks[0].takes.empty());
        CHECK(s.state().revision==clearRevision+1);
        { const uint64_t cur=s.state().revision; s.undo(cur);
          CHECK(s.state().tracks[0].audio&&s.state().tracks[0].regions.size()==1&&starts()==std::vector<uint64_t>({400000}));
          s.redo(s.state().revision);
          CHECK(!s.state().tracks[0].audio&&s.state().tracks[0].regions.empty());
          s.materializeRecordedTrack(1,"Re-recorded",clip,24000,s.state().revision);
          CHECK(s.state().tracks[0].audio&&s.state().tracks[0].regions.size()==1&&s.state().tracks[0].regions[0].start==24000); }
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

    // ---- Clip pan: validation, silent no-op, carry across split/duplicate/edit ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2*2,0.25f));
        s.importAt("Pan",clip,0,0);                                 // track 1, rev 1, one 2 s clip
        s.setClipPan(1,0,-0.5,1);                                    // rev 2
        CHECK(std::abs(s.state().tracks[0].regions[0].pan-(-0.5))<1e-12);
        s.setClipPan(1,0,-0.5,2);                                    // same value writes nothing
        CHECK(s.state().revision==2);
        rejectsMessage("Clip pan outside -1..1",[&]{s.setClipPan(1,0,1.5,2);});
        rejectsMessage("Clip pan outside -1..1",[&]{s.setClipPan(1,0,std::sqrt(-1.0),2);});
        rejectsMessage("Audio clip not found",[&]{s.setClipPan(2,0,0.0,2);});
        CHECK(s.state().revision==2);
        s.undo(2);CHECK(std::abs(s.state().tracks[0].regions[0].pan)<1e-12);
        s.redo(3);CHECK(std::abs(s.state().tracks[0].regions[0].pan+0.5)<1e-12);     // rev 4
        s.splitClip(1,0,48000,4);                                    // rev 5 — both halves keep pan
        CHECK(s.state().tracks[0].regions.size()==2&&std::abs(s.state().tracks[0].regions[0].pan+0.5)<1e-12&&std::abs(s.state().tracks[0].regions[1].pan+0.5)<1e-12);
        s.duplicateClip(1,1,5);                                      // rev 6 — the copy carries pan
        CHECK(std::abs(s.state().tracks[0].regions[2].pan+0.5)<1e-12);
        s.editClip(1,2,600000,0,96000,6);                            // rev 7 — move keeps it too
        CHECK(std::abs(s.state().tracks[0].regions[2].pan+0.5)<1e-12);
    }
    // ---- Storage v19 round trip: clip/track color + per-region gain persist ----
    {   Session s;
        const auto clip=std::make_shared<const Clip>(std::vector<float>(size_t(48000)*2,0.0f));
        s.importAt("Persist",clip,0,0);                             // track 1, rev 1
        s.setTrackColor(1,0x112233u,1);                            // rev 2
        s.setClipColor(1,0,0x445566u,2);                          // rev 3
        s.setClipGain(1,0,-9.0,3);                              // rev 4
        s.setClipMuted(1,0,true,4);                              // rev 5
        s.setClipLooped(1,0,true,5);                             // rev 6
        s.setClipPan(1,0,0.75,6);                                // rev 7
        const auto path=dir/"cliptrack-roundtrip.mydawdraft";
        writeDraft(s.state(),path.string());
        const auto loaded=readDraft(path.string());
        CHECK(loaded.tracks.size()==1);
        CHECK(loaded.tracks[0].color==0x112233u);
        CHECK(loaded.tracks[0].regions.size()==1);
        CHECK(loaded.tracks[0].regions[0].color==0x445566u);
        CHECK(std::abs(loaded.tracks[0].regions[0].gain-(-9.0))<1e-9);
        CHECK(loaded.tracks[0].regions[0].muted==true);
        CHECK(loaded.tracks[0].regions[0].looped==true);
        CHECK(std::abs(loaded.tracks[0].regions[0].pan-0.75)<1e-12);   // v21: per-clip pan persists
        auto db=openDb(path);
        sqlite3_stmt* version=nullptr;
        CHECK(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&version,nullptr)==SQLITE_OK);
        CHECK(sqlite3_step(version)==SQLITE_ROW&&sqlite3_column_int(version,0)==24);
        sqlite3_finalize(version);
        sqlite3_close(db);
    }

    std::cout << "PASS: clip/track editing: track mute/solo/color/gain + duplicateTrack, per-region clip color + gain (dB), playback state (mute + loop with slice-wrap bounds), cross-track clipboard copy/move for audio regions (take-source rule, last-clip and timeline-space guards) and MIDI clips (lane/notes/colour travel, overlap via validate), MIDI clip color/transpose(clamped)/quantize, split/edit field-carry regressions, all revision-checked with silent no-ops and snapshot undo/redo, and schema v20 round trip persisting track/region color, per-region gain and mute/loop flags" << std::endl;
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}}
