#include "platform/macos/midi_input.hpp"
#include "domain/session.hpp"
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <string>

namespace daw {

bool MidiRing::push(const MidiCapturedEvent& event) noexcept {
    if(count_==capacity){
        // Drop-oldest: overwrite the slot the reader would pop next and
        // advance both cursors so order is always preserved.
        slots_[head]=event;
        head=(head+1)%capacity;
        tail=(tail+1)%capacity;
        ++dropped_;
        return false;
    }
    slots_[head]=event;
    head=(head+1)%capacity;
    ++count_;
    return true;
}

bool MidiRing::pop(MidiCapturedEvent& event) noexcept {
    if(!count_) return false;
    event=slots_[tail];
    tail=(tail+1)%capacity;
    --count_;
    return true;
}

uint64_t framesFromHostTime(uint64_t eventNs,uint64_t sessionStartNs) noexcept {
    const uint64_t elapsed=eventNs>sessionStartNs?eventNs-sessionStartNs:0;
    return (elapsed*48000)/1000000000;
}

namespace {

// Fixed data-field count for a MIDI 1.0 channel voice status byte.
unsigned channelVoiceDataBytes(uint8_t status) {
    const uint8_t high=status&0xF0;
    return (high==0xC0||high==0xD0)?1u:2u; // note/poly/CC/bend carry two, prog/pressure one
}

unsigned systemMessageBytes(uint8_t status) {
    switch(status){
        case 0xF1: case 0xF3: return 2u;
        case 0xF2: return 3u;
        default: return 1u; // remaining defined and undefined system bytes
    }
}

}

bool parseMidiPacket(const uint8_t* bytes,size_t size,uint8_t& runningStatusCache,MidiCapturedEvent& event,size_t* consumed) {
    if(consumed) *consumed=0;
    if(!bytes||!size) return false;
    const uint8_t first=bytes[0];
    uint8_t status=0;
    size_t have=0;
    if(first<0x80){
        // Running status: the cache must hold a channel voice status byte.
        if(runningStatusCache<0x80||runningStatusCache>=0xF0) return false;
        status=runningStatusCache;
    } else if(first<0xF0){
        status=first;
        runningStatusCache=first;
        have=1;
    } else {
        // System messages never match and never replace running status.
        // Sysex and standalone escape terminate v0 parsing: report no
        // consumption so the caller drops the remainder of the packet.
        if(first==0xF0||first==0xF7) return false;
        const unsigned width=systemMessageBytes(first);
        if(size<width) return false;
        if(consumed) *consumed=width;
        return false;
    }
    const unsigned data=channelVoiceDataBytes(status);
    if(size<have+data) return false; // incomplete message
    for(unsigned i=0;i<data;++i){
        if(bytes[have+i]>=0x80){
            // Status byte inside a data field: the stream is corrupted.
            // Discard one byte and clear the cache so the caller resyncs
            // starting from the offending byte.
            runningStatusCache=0;
            if(consumed) *consumed=1;
            return false;
        }
    }
    const uint8_t pitch=bytes[have];
    const uint8_t value=data>1?bytes[have+1]:0;
    if(consumed) *consumed=have+data;
    const uint8_t high=status&0xF0;
    if(high!=0x90&&high!=0x80) return false; // complete message, not a note
    event=MidiCapturedEvent{};
    event.channel=status&0x0F;
    event.pitch=pitch;
    event.velocity=value;
    // 0x90 with velocity 0 is the note-off convention; kind 2 covers both.
    event.kind=(high==0x80||value==0)?2:1;
    return true;
}

namespace {

void checkedCore(OSStatus status,const char* action) {
    if(status!=noErr) throw Error(std::string(action)+" (Core MIDI "+std::to_string(static_cast<int>(status))+")");
}

uint64_t machToNs(uint64_t ticks) {
    static const mach_timebase_info_data_t info=[](){
        mach_timebase_info_data_t base{1,1};
        if(mach_timebase_info(&base)!=0||base.denom==0){ base.numer=1; base.denom=1; }
        return base;
    }();
    return ticks*info.numer/info.denom;
}

int integerProperty(MIDIObjectRef object,CFStringRef key) {
    SInt32 value=0;
    if(MIDIObjectGetIntegerProperty(object,key,&value)!=noErr) return 0;
    return value;
}

std::string stringProperty(MIDIObjectRef object,CFStringRef key) {
    CFStringRef value=nullptr;
    if(MIDIObjectGetStringProperty(object,key,&value)!=noErr||!value) return {};
    char buffer[512];
    std::string result;
    if(CFStringGetCString(value,buffer,sizeof(buffer),kCFStringEncodingUTF8)) result=buffer;
    CFRelease(value);
    return result;
}

uint32_t uniqueID(MIDIObjectRef object) {
    const SInt32 id=integerProperty(object,kMIDIPropertyUniqueID);
    return id>0?static_cast<uint32_t>(id):0;
}

// One shared client for all inputs; CoreMIDI ports are owned by their client
// and are released with it, so closing an input only disconnects the source.
// open()/close() are serialised on the session control thread by contract;
// the mutex only guards client lifetime against re-entrant creation.
std::mutex clientMutex;
MIDIClientRef sharedClient=0;
int clientUsers=0;

MIDIClientRef acquireClient() {
    std::lock_guard<std::mutex> lock(clientMutex);
    if(!sharedClient)
        checkedCore(MIDIClientCreate(CFSTR("My DAW MIDI"),nullptr,nullptr,&sharedClient),"Create MIDI client");
    ++clientUsers;
    return sharedClient;
}

void releaseClient() noexcept {
    std::lock_guard<std::mutex> lock(clientMutex);
    if(--clientUsers<=0){
        if(sharedClient) MIDIClientDispose(sharedClient);
        sharedClient=0;
        clientUsers=0;
    }
}

// The MIDI 1.0 receive path (MIDIInputPortCreate + MIDIPacketList) carries
// deprecation attributes in recent SDKs. v0 deliberately captures the wire
// protocol already proven headlessly by parseMidiPacket; migrating capture
// to MIDIReceiveBlock/MIDIEventList is its own later arc. The diagnostic
// waiver is scoped to the legacy usage below, never to the whole target.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

void midiReadProc(const MIDIPacketList* packets,void* readRefCon,void*) {
    auto* input=static_cast<MidiInput*>(readRefCon);
    if(!packets||!input) return;
    try {
        const MIDIPacket* packet=&packets->packet[0];
        for(UInt32 p=0;p<packets->numPackets;++p){
            const uint64_t stamp=packet->timeStamp?machToNs(packet->timeStamp):0;
            uint8_t cache=0; // running status never crosses packet bounds
            size_t offset=0;
            while(offset<packet->length){
                MidiCapturedEvent event{};
                size_t consumed=0;
                const bool note=parseMidiPacket(packet->data+offset,packet->length-offset,cache,event,&consumed);
                if(!consumed) break; // truncated or sysex: drop packet remainder
                offset+=consumed;
                if(note){ event.hostTimeNs=stamp; input->capture(event); }
            }
            packet=MIDIPacketNext(packet);
        }
    } catch(...) {}
}

#pragma clang diagnostic pop

}

std::vector<MidiInputDevice> listMidiInputDevices() {
    std::vector<MidiInputDevice> devices;
    std::vector<uint32_t> seen;
    const ItemCount deviceTotal=MIDIGetNumberOfDevices();
    for(ItemCount d=0;d<deviceTotal;++d){
        const MIDIDeviceRef device=MIDIGetDevice(d);
        const bool online=integerProperty(device,kMIDIPropertyOffline)==0;
        const std::string deviceName=stringProperty(device,kMIDIPropertyDisplayName);
        const ItemCount entities=MIDIDeviceGetNumberOfEntities(device);
        for(ItemCount e=0;e<entities;++e){
            const MIDIEntityRef entity=MIDIDeviceGetEntity(device,e);
            if(!entity) continue;
            const ItemCount sources=MIDIEntityGetNumberOfSources(entity);
            for(ItemCount s=0;s<sources;++s){
                const MIDIEndpointRef source=MIDIEntityGetSource(entity,s);
                const uint32_t id=uniqueID(source);
                if(!id) continue;
                std::string name=stringProperty(source,kMIDIPropertyDisplayName);
                if(name.empty()) name=deviceName;
                devices.push_back({id,name,online});
                seen.push_back(id);
            }
        }
    }
    // Virtual and network sources may have no device parent; the endpoint
    // itself exposes the offline flag for those.
    const ItemCount sourceTotal=MIDIGetNumberOfSources();
    for(ItemCount i=0;i<sourceTotal;++i){
        const MIDIEndpointRef source=MIDIGetSource(i);
        const uint32_t id=uniqueID(source);
        if(!id||std::find(seen.begin(),seen.end(),id)!=seen.end()) continue;
        devices.push_back({id,stringProperty(source,kMIDIPropertyDisplayName),integerProperty(source,kMIDIPropertyOffline)==0});
    }
    return devices;
}

std::unique_ptr<MidiInput> MidiInput::open(uint32_t deviceUniqueID) {
    MIDIEndpointRef endpoint=0;
    if(!deviceUniqueID) throw Error("No MIDI device selected");
    {
        const ItemCount total=MIDIGetNumberOfSources();
        bool found=false;
        for(ItemCount i=0;i<total;++i){
            const MIDIEndpointRef candidate=MIDIGetSource(i);
            if(candidate&&uniqueID(candidate)==deviceUniqueID){ endpoint=candidate; found=true; break; }
        }
        if(!found) throw Error("MIDI device is no longer available (Core MIDI source "+std::to_string(deviceUniqueID)+")");
    }
    std::unique_ptr<MidiInput> input(new MidiInput());
    input->clientHandle=acquireClient();
    input->source=endpoint;
    input->deviceID=deviceUniqueID;
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    try {
        checkedCore(MIDIInputPortCreate(input->clientHandle,CFSTR("My DAW capture"),midiReadProc,input.get(),&input->port),"Create MIDI input port");
        checkedCore(MIDIPortConnectSource(input->port,endpoint,input.get()),"Connect MIDI source");
    } catch(...) {
        input->connected=false;
        input->port=0;
        input->source=0;
        const uint32_t client=input->clientHandle;
        input->clientHandle=0;
        if(client) releaseClient();
        throw;
    }
    #pragma clang diagnostic pop
    input->connected=true;
    return input;
}

void MidiInput::close() noexcept {
    if(connected&&port&&source){
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        MIDIPortDisconnectSource(port,source);
        #pragma clang diagnostic pop
    }
    connected=false;
    source=0;
    port=0;
    if(clientHandle){
        clientHandle=0;
        releaseClient();
    }
}

uint64_t MidiInput::poll(std::vector<MidiCapturedEvent>& out,uint64_t maxEvents) {
    out.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t taken=0;
    MidiCapturedEvent event{};
    while(taken<maxEvents&&ring_.pop(event)){ out.push_back(event); ++taken; }
    return taken;
}

uint64_t MidiInput::dropped() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ring_.dropped();
}

void MidiInput::capture(const MidiCapturedEvent& event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    ring_.push(event);
}

MidiInput::~MidiInput(){ close(); }

}
