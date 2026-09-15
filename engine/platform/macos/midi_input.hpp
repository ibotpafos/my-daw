#pragma once
// Core MIDI input capture (macOS only). This layer owns device discovery and
// event capture only: short note messages are decoded on CoreMIDI's high
// priority read thread into a fixed capacity ring, and the session control
// thread drains them with poll(). Nothing here runs on the audio render
// thread. open() requires a running main run loop for packet delivery, and
// close() must be serialized with open() on the session control thread;
// neither ever runs from the RT callback. MIDI 1.0 wire protocol v0: note
// on/off with running status; program change, CC, pitch bend and system
// messages are skipped without breaking stream resync, sysex is dropped.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace daw {

// A Core MIDI source entity as offered to the UI. uniqueID is the entity's
// kMIDIPropertyUniqueID — the value accepted by MidiInput::open. online is
// the offline flag of the owning device (virtual sources report online).
struct MidiInputDevice { uint32_t uniqueID=0; std::string name; bool online=false; };

// Enumerates all source entities (device entities first, then orphaned and
// virtual sources). Never throws; returns an empty list on headless systems.
std::vector<MidiInputDevice> listMidiInputDevices();

// One captured short message. kind: 1 = note on, 2 = note off. A 0x90 with
// velocity 0 is normalised to note off per the MIDI 1.0 convention.
// hostTimeNs is the packet timeStamp converted from mach absolute time to
// nanoseconds, or 0 when the packet carries no timestamp. The parser leaves
// hostTimeNs at 0; the capture thread stamps it.
struct MidiCapturedEvent { uint64_t hostTimeNs=0; uint8_t kind=0, channel=0, pitch=0, velocity=0; bool operator==(const MidiCapturedEvent&) const = default; };

// Pure, allocation free decoder for one message at the front of [bytes,size).
// A leading status byte 0x80..0xEF updates runningStatusCache; a leading data
// byte continues it, so running status streams decode with the cache carried
// across calls. System messages consume their fixed width without touching
// the cache and never match. Sysex (0xF0/0xF7) consumes nothing: the caller
// must stop parsing the packet, which v0 drops. Returns true only for note
// on/off; a complete non-note message still reports its width through
// consumed (when non-null) so callers skip it. Incomplete, absent-cache or
// corrupted (status byte inside the data field, which also clears the cache)
// input returns false; consumed is 0 except for the corrupted case, where it
// is 1 so the caller can resync at the offending status byte.
bool parseMidiPacket(const uint8_t* bytes, size_t size, uint8_t& runningStatusCache, MidiCapturedEvent& event, size_t* consumed=nullptr);

// Event host time to a project frame on the fixed 48 kHz timeline. Events
// older than the session start clamp to frame 0.
uint64_t framesFromHostTime(uint64_t eventNs, uint64_t sessionStartNs) noexcept;

// Fixed capacity drop-oldest event ring, preallocated at construction. It is
// the published test seam for the capture accounting: push/pop are not
// internally synchronised (MidiInput owns the lock); push returns false when
// the oldest event was evicted to make room, counted by dropped().
struct MidiRing {
    static constexpr uint64_t capacity=4096;
    bool push(const MidiCapturedEvent& event) noexcept;
    bool pop(MidiCapturedEvent& event) noexcept;
    uint64_t dropped() const noexcept { return dropped_; }
    uint64_t size() const noexcept { return count_; }
private:
    std::array<MidiCapturedEvent, capacity> slots_{};
    uint64_t head=0, tail=0, count_=0, dropped_=0;
};

// One connected Core MIDI source. open() resolves deviceUniqueID against the
// live source list, creates the shared client and an input port, and
// connects; it throws Error when the id is unknown or CoreMIDI refuses.
// Capture callbacks land in the ring; poll drains it without blocking.
class MidiInput {
public:
    static std::unique_ptr<MidiInput> open(uint32_t deviceUniqueID);
    ~MidiInput();
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // Idempotent. Disconnects the source; the input port is released when the
    // last MidiInput of the process closes (ports belong to the client).
    void close() noexcept;

    // Non-blocking drain: clears out, then moves at most maxEvents captured
    // events into it, oldest first, and returns how many were taken.
    uint64_t poll(std::vector<MidiCapturedEvent>& out, uint64_t maxEvents);
    uint64_t dropped() const noexcept;

    // Internal: called from the CoreMIDI read procedure only. Public so the
    // capture path is exercisable without hardware; do not call from UI code.
    void capture(const MidiCapturedEvent& event) noexcept;

private:
    MidiInput() = default;
    MidiRing ring_;
    mutable std::mutex mutex_;
    // Opaque CoreMIDI object references (MIDIPortRef/MIDIEndpointRef are
    // uint32 typedefs), kept as integers so this header needs no CoreMIDI
    // includes. 0 means unset. The input port is owned by the shared client
    // and is released with it; only the source connection needs undoing.
    // CoreMIDI object handles are UInt32 object references in this SDK.
    uint32_t clientHandle=0; // shared MIDIClientRef, owned via acquire/release
    uint32_t port=0, source=0, deviceID=0;
    bool connected=false;
};
}
