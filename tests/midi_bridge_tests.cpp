#include "bridge/daw.h"
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("Failed: " #x); } while (false)

static_assert(sizeof(daw_midi_note)==32,"note ABI");
static_assert(sizeof(daw_midi_clip)==40,"clip ABI");

int main() { try {
    std::unique_ptr<daw_session,decltype(&daw_destroy)> session(daw_create(),daw_destroy);
    CHECK(session!=nullptr);
    auto* s=session.get();
    auto revision=[&]{ daw_snapshot snap{};snap.struct_size=sizeof(snap); CHECK(daw_get_snapshot(s,&snap)==0); return snap.revision; };
    auto note=[](uint64_t start,uint64_t length,uint8_t pitch,uint8_t channel,uint8_t velocity){ daw_midi_note n{}; n.struct_size=sizeof(n); n.version=DAW_MIDI_NOTE_VERSION; n.start=start; n.length=length; n.pitch=pitch; n.channel=channel; n.velocity=velocity; return n; };
    auto meta=[](uint64_t start,uint64_t length,uint32_t lane,uint32_t note_count){ daw_midi_clip c{}; c.struct_size=sizeof(c); c.version=DAW_MIDI_CLIP_VERSION; c.start=start; c.length=length; c.lane=lane; c.note_count=note_count; return c; };
    auto clipCount=[&]{ uint32_t count=0; CHECK(daw_get_midi_clip_count(s,1,&count)==0); return count; };
    daw_midi_note read[4]; uint32_t written=0;

    CHECK(daw_add_track(s,"Midi bridge",revision())==0);
    CHECK(clipCount()==0);
    daw_midi_note empty; uint32_t emptyWritten=7;
    CHECK(daw_get_midi_clip_count(s,999,&emptyWritten)!=0); // absent track

    // add three notes (starts are clip-relative)
    daw_midi_note notes[3]={note(100,2400,60,0,90),note(3000,1800,64,15,127),note(47000,999,72,9,1)};
    auto clip=meta(0,48000,7,3);
    CHECK(daw_add_midi_clip(s,1,&clip,notes,3,revision())==0);
    CHECK(clipCount()==1);
    auto out=meta(0,0,0,0);
    CHECK(daw_get_midi_clip(s,1,0,&out,0,read,4,&written)==0);
    CHECK(out.start==0&&out.length==48000&&out.lane==7&&out.note_count==3);
    CHECK(written==3&&read[0].start==100&&read[1].pitch==64&&read[1].channel==15&&read[2].velocity==1); // relative starts preserved
    out=meta(0,0,0,0);
    CHECK(daw_get_midi_clip(s,1,0,&out,0,nullptr,0,&written)==0); // metadata-only page
    CHECK(out.note_count==3&&written==0);
    CHECK(daw_get_midi_clip(s,1,0,&out,2,read,1,&written)==0&&written==1&&read[0].start==47000); // paging
    CHECK(daw_get_midi_clip(s,1,0,&out,5,read,1,&written)!=0); // offset past the end
    CHECK(daw_get_midi_clip(s,1,1,&out,0,nullptr,0,&written)!=0); // index out of range

    // replace notes with two
    daw_midi_note two[2]={note(0,500,60,0,15),note(500,400,62,15,9)};
    CHECK(daw_set_midi_notes(s,1,0,two,2,revision())==0);
    out=meta(0,0,0,0); CHECK(daw_get_midi_clip(s,1,0,&out,0,read,4,&written)==0&&out.note_count==2&&written==2);

    // move
    CHECK(daw_move_midi_clip(s,1,0,10000,revision())==0);
    out=meta(0,0,0,0); CHECK(daw_get_midi_clip(s,1,0,&out,0,nullptr,0,&written)==0&&out.start==10000);
    const uint64_t beforeNoop=revision();
    CHECK(daw_move_midi_clip(s,1,0,10000,revision())==0); // no-op keeps the revision
    CHECK(revision()==beforeNoop);

    // split: a note crossing the cut is rejected without mutation
    CHECK(daw_split_midi_clip(s,1,0,10100,revision())!=0);
    CHECK(clipCount()==1);
    // split between notes: right side keeps note 1 at relative 0
    CHECK(daw_split_midi_clip(s,1,0,10500,revision())==0);
    CHECK(clipCount()==2);
    out=meta(0,0,0,0); CHECK(daw_get_midi_clip(s,1,0,&out,0,read,4,&written)==0);
    CHECK(out.start==10000&&out.length==500&&out.note_count==1&&written==1&&read[0].start==0&&read[0].pitch==60);
    out=meta(0,0,0,0); CHECK(daw_get_midi_clip(s,1,1,&out,0,read,4,&written)==0);
    CHECK(out.start==10500&&out.length==47500&&out.note_count==1&&read[0].start==0&&read[0].pitch==62);

    // trim drops notes that no longer fit whole
    CHECK(daw_trim_midi_clip(s,1,1,10500,100,revision())==0);
    out=meta(0,0,0,0); CHECK(daw_get_midi_clip(s,1,1,&out,0,nullptr,0,&written)==0&&out.note_count==0&&out.length==100);

    // undo restores the trimmed clip and its note; redo trims again
    CHECK(daw_undo(s,revision())==0);
    out=meta(0,0,0,0); CHECK(daw_get_midi_clip(s,1,1,&out,0,read,4,&written)==0&&out.note_count==1&&out.length==47500&&written==1);
    CHECK(daw_redo(s,revision())==0);
    out=meta(0,0,0,0); CHECK(daw_get_midi_clip(s,1,1,&out,0,nullptr,0,&written)==0&&out.note_count==0);

    // remove both
    CHECK(daw_remove_midi_clip(s,1,0,revision())==0);
    CHECK(clipCount()==1);
    CHECK(daw_remove_midi_clip(s,1,0,revision())==0);
    CHECK(clipCount()==0);

    // stale revision fails without mutation
    clip=meta(0,1000,0,0);
    CHECK(daw_add_midi_clip(s,1,&clip,nullptr,0,revision()+7)!=0);
    CHECK(clipCount()==0);

    // ABI rejections
    clip=meta(0,48000,0,0);
    auto badSize=clip; badSize.struct_size=sizeof(clip)-4;
    CHECK(daw_add_midi_clip(s,1,&badSize,nullptr,0,revision())!=0);
    auto badVersion=clip; badVersion.version=DAW_MIDI_CLIP_VERSION+1;
    CHECK(daw_add_midi_clip(s,1,&badVersion,nullptr,0,revision())!=0);
    CHECK(daw_add_midi_clip(s,1,&clip,notes,2,revision())!=0); // note_count != supplied count
    CHECK(daw_add_midi_clip(s,1,&clip,nullptr,3,revision())!=0); // missing array
    const uint32_t big=DAW_MIDI_NOTES_PER_CALL+1;
    out=meta(0,0,0,0);
    CHECK(daw_get_midi_clip(s,1,0,&out,0,reinterpret_cast<daw_midi_note*>(1),big,&written)!=0); // oversized capacity, checked before track lookup
    auto badPitch=note(0,100,128,0,100); CHECK(daw_add_midi_clip(s,1,&clip,&badPitch,1,revision())!=0);
    auto badVelocity=note(0,100,60,0,0); CHECK(daw_add_midi_clip(s,1,&clip,&badVelocity,1,revision())!=0);
    auto badChannel=note(0,100,60,16,100); CHECK(daw_add_midi_clip(s,1,&clip,&badChannel,1,revision())!=0);
    CHECK(clipCount()==0);

    // domain keeps timeline truth: note beyond the clip end is rejected
    clip=meta(0,100,0,1); auto overflow=note(50,100,60,0,100);
    CHECK(daw_add_midi_clip(s,1,&clip,&overflow,1,revision())!=0);
    CHECK(clipCount()==0);
    // lane is the editor row and must be non-negative
    clip=meta(0,100,-1,0);
    CHECK(daw_add_midi_clip(s,1,&clip,nullptr,0,revision())!=0);

    std::cout<<"PASS: MIDI bridge commands/queries (add,set,move,trim,split,remove,paged get,undo/redo,ABI+revision guards)"<<std::endl;
    return 0;
} catch (const std::exception& error) { std::cerr<<error.what()<<std::endl; return 1; } }