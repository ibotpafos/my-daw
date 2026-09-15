#include "platform/macos/midi_input.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#define CHECK(x) do { if(!(x)) throw std::runtime_error("Failed: " #x); } while(false)
template<class Fn> void rejects(Fn fn) { bool bad=false; try{fn();}catch(...){bad=true;} CHECK(bad); }
using namespace daw;
int main(){try{
    // ---- Note on/off with explicit status ----
    { const uint8_t b[]={0x90,0x3C,0x64}; uint8_t cache=0; MidiCapturedEvent e{}; size_t c=0;
      CHECK(parseMidiPacket(b,3,cache,e,&c)&&c==3&&e.kind==1&&e.pitch==0x3C&&e.velocity==0x64&&e.channel==0&&cache==0x90&&e.hostTimeNs==0); }
    { const uint8_t b[]={0x8F,0x40,0x7F}; uint8_t cache=0; MidiCapturedEvent e{}; size_t c=0;
      CHECK(parseMidiPacket(b,3,cache,e,&c)&&e.kind==2&&e.channel==15&&e.pitch==0x40&&e.velocity==0x7F&&c==3); }
    { const uint8_t b[]={0x91,0x30,0x00}; uint8_t cache=0; MidiCapturedEvent e{};
      CHECK(parseMidiPacket(b,3,cache,e,nullptr)&&e.kind==2&&e.velocity==0&&e.channel==1); } // vel-0 note on normalises to off
    // ---- Running status across a stream ----
    { const uint8_t b[]={0x90,0x60,0x64,0x61,0x64}; uint8_t cache=0; MidiCapturedEvent e{}; size_t c=0;
      CHECK(parseMidiPacket(b,5,cache,e,&c)&&e.pitch==0x60&&c==3);
      CHECK(parseMidiPacket(b+3,2,cache,e,&c)&&e.pitch==0x61&&e.velocity==0x64&&c==2&&e.kind==1);
      CHECK(!parseMidiPacket(b+3,1,cache,e,&c)&&c==0); } // lone data byte: incomplete
    // ---- Non-note channel messages are skipped with correct width ----
    { const uint8_t b[]={0xB0,0x07,0x64}; uint8_t cache=0; MidiCapturedEvent e{}; size_t c=0;
      CHECK(!parseMidiPacket(b,3,cache,e,&c)&&c==3&&cache==0xB0);
      CHECK(!parseMidiPacket(b+1,2,cache,e,&c)&&c==2); } // CC continues via cache
    { const uint8_t b[]={0xC2,0x05}; uint8_t cache=0; MidiCapturedEvent e{}; size_t c=0;
      CHECK(!parseMidiPacket(b,2,cache,e,&c)&&c==2&&cache==0xC2);
      CHECK(!parseMidiPacket(b+1,1,cache,e,&c)&&c==1); } // prog change: one data byte
    { const uint8_t b[]={0xE0,0x40,0x7F}; uint8_t cache=0; MidiCapturedEvent e{}; size_t c=0;
      CHECK(!parseMidiPacket(b,3,cache,e,&c)&&c==3); } // pitch bend skipped
    // ---- System messages: consume width, keep cache; sysex stops the walk ----
    { uint8_t b[]={0xF8}; uint8_t cache=0x90; MidiCapturedEvent e{}; size_t c=0;
      CHECK(!parseMidiPacket(b,1,cache,e,&c)&&c==1&&cache==0x90); } // clock is invisible to running status
    { uint8_t b[]={0xF2,0x00,0x20}; uint8_t cache=0x90; MidiCapturedEvent e{}; size_t c=0;
      CHECK(!parseMidiPacket(b,3,cache,e,&c)&&c==3&&cache==0x90); } // song position: 3 bytes
    { uint8_t b[]={0xF0,0x7E,0x00}; uint8_t cache=0x90; MidiCapturedEvent e{}; size_t c=0;
      CHECK(!parseMidiPacket(b,3,cache,e,&c)&&c==0); } // sysex: no consumption, caller drops remainder
    // ---- Corrupted and truncated streams ----
    { const uint8_t b[]={0x90,0x3C}; uint8_t cache=0; MidiCapturedEvent e{}; size_t c=0;
      CHECK(!parseMidiPacket(b,2,cache,e,&c)&&c==0); } // truncated note
    { uint8_t b[]={0x60,0x64}; uint8_t cache=0; MidiCapturedEvent e{};
      CHECK(!parseMidiPacket(b,2,cache,e,nullptr)); } // data byte with no cache
    { uint8_t b[]={0x60,0x64}; uint8_t cache=0xF8; MidiCapturedEvent e{};
      CHECK(!parseMidiPacket(b,2,cache,e,nullptr)); } // invalid cache is not usable
    { const uint8_t b[]={0x90,0x80,0x40,0x64}; uint8_t cache=0; MidiCapturedEvent e{}; size_t c=0;
      CHECK(!parseMidiPacket(b,4,cache,e,&c)&&c==1&&cache==0); // status inside data: drop one byte
      CHECK(parseMidiPacket(b+1,3,cache,e,&c)&&e.kind==2&&e.pitch==0x40&&e.velocity==0x64&&c==3); } // resynced
    // ---- Host time to project frame ----
    CHECK(framesFromHostTime(1000000000,0)==48000);
    CHECK(framesFromHostTime(20812500,0)==999);
    CHECK(framesFromHostTime(20833,0)==0); // sub-frame timestamps clamp to frame 0
    CHECK(framesFromHostTime(5,10)==0);
    CHECK(framesFromHostTime(1000000000+20833333,1000000000)==999);
    CHECK(framesFromHostTime(0,0)==0);
    // ---- Ring is fixed capacity, drop-oldest, order preserving ----
    { MidiRing ring; uint64_t evictions=0;
      for(uint64_t i=0;i<5000;++i){ MidiCapturedEvent e{}; e.kind=1; e.hostTimeNs=i; if(!ring.push(e)) ++evictions; }
      CHECK(ring.size()==MidiRing::capacity&&ring.dropped()==904&&evictions==904);
      MidiCapturedEvent e{}; uint64_t n=0;
      while(ring.pop(e)){ ++n; CHECK(e.hostTimeNs==903+n); }
      CHECK(n==4096);
      CHECK(!ring.pop(e));
      CHECK(ring.push(e)&&ring.push(e)&&ring.pop(e)&&ring.pop(e)&&!ring.pop(e)); }
    // ---- open() rejects without touching hardware ----
    rejects([]{MidiInput::open(0);});
    rejects([]{MidiInput::open(0xFFFFFFFFu);});
    // ---- Enumeration is headless safe and stable ----
    { const auto first=listMidiInputDevices(); const auto second=listMidiInputDevices();
      CHECK(first.size()==second.size());
      for(size_t i=0;i<first.size();++i){ CHECK(first[i].uniqueID==second[i].uniqueID); CHECK(first[i].uniqueID!=0); if(first[i].online) CHECK(!first[i].name.empty()); } }
    std::cout<<"PASS: CoreMIDI parse (running status, skips, resync, sysex stop), drop-oldest ring accounting, host-time framing, headless enumeration and open rejection\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
