#pragma once
#include "audio/clip.hpp"
#include "domain/session.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace daw {
struct RecoveredTake {
    uint64_t startFrame=0;
    std::shared_ptr<const Clip> clip;
};

// One producer (audio callback), one consumer (writer thread). The producer
// allocates nothing, locks nothing and performs no file I/O.
class RecordingWriter {
    std::string path_;
    uint64_t startFrame_=0, capacityFrames_=0;
    uint64_t skipFrames_=0; // pre-roll: drop this many leading captured frames
    std::vector<float> ring_;
    std::atomic<uint64_t> read_{0}, written_{0}, accepted_{0}, committed_{0};
    std::atomic<bool> stopping_{false}, overflow_{false}, failed_{false};
    std::thread worker_;
    int fd_=-1;
    void run() noexcept;
    void closeFile() noexcept;
public:
    RecordingWriter(std::string path,uint64_t startFrame,uint64_t capacityFrames,uint64_t ringFrames=48000*2,uint64_t skipFrames=0);
    ~RecordingWriter();
    RecordingWriter(const RecordingWriter&)=delete;
    RecordingWriter& operator=(const RecordingWriter&)=delete;
    void writeMono(const float* input,uint32_t frames) noexcept;
    std::shared_ptr<const Clip> finish();
    void stopPreserving() noexcept;
    void discard() noexcept;
    uint64_t frames() const noexcept { return accepted_.load(std::memory_order_acquire); }
    uint64_t committedFrames() const noexcept { return committed_.load(std::memory_order_acquire); }
    bool overflowed() const noexcept { return overflow_.load(std::memory_order_acquire) || failed_.load(std::memory_order_acquire); }
    const std::string& path() const noexcept { return path_; }
};

RecoveredTake recoverTake(const std::string& path);
std::vector<std::shared_ptr<const Clip>> splitLoopPasses(const Clip& recording,uint64_t loopFrames);

// ---------------------------------------------------------------------------
// MIDI record-to-clip (v0 capture controller)
// ---------------------------------------------------------------------------

// One normalised note event on the fixed 48 kHz project timeline, expressed in
// frames relative to the left edge of the clip being recorded into. This POD is
// the recorder abstraction over platform capture: engine/audio never includes
// engine/platform, so the caller converts a captured MidiCapturedEvent into it
// and performs the two normalisations there — host time to the nearest project
// frame (framesFromHostTime minus the clip start) and a 0x90 with velocity 0 to
// noteOff=true per the MIDI 1.0 convention, which is why velocity stays a
// note-on-only field. Feed events in non-decreasing frame order.
struct RecordedMidiEvent {
    uint64_t frame=0;
    uint8_t pitch=0, channel=0, velocity=0;
    bool noteOff=false;
    bool operator==(const RecordedMidiEvent&) const = default;
};

// Turns a live MIDI stream into a batch Session::appendMidiNotes can commit —
// the job RecordingWriter does for audio, without the file. It owns no Session
// and never touches storage, so the session control thread stays the only
// writer of the model. It is not lock-free and not for the audio callback: the
// capture ring already absorbs the event rate and poll() is drained on the
// control thread.
//
// Quantization is v0-identity: frames are recorded exactly as fed, with no beat
// snap. Tempo arithmetic lives in the domain beat converters and snapping is a
// later UI-layer pass over the finished batch, so repeating either here would
// create a second source of truth.
class MidiRecorder {
public:
    // One take holds at most the project-wide note budget, so a batch that
    // records successfully is never rejected merely for its size.
    static constexpr uint64_t kMaxTakeNotes=kMaxMidiNotesPerProject;

    // Starts a take aimed at the MIDI clip at clipIndex within trackID clip
    // vector — the same positional index Session and storage both call
    // "position". The recorder keeps the target only so the caller can verify
    // it is still recording where it means to commit; re-arming abandons
    // whatever the previous take held and resets its counters.
    void arm(uint64_t trackID,uint32_t clipIndex) noexcept;
    // Abandons the take without producing a batch.
    void disarm() noexcept;
    bool armed() const noexcept { return armed_; }
    uint64_t trackID() const noexcept { return trackID_; }
    uint32_t clipIndex() const noexcept { return clipIndex_; }

    // Feeds events oldest first — one poll() drain of a capture source,
    // converted to RecordedMidiEvent. A note on opens a pending note at its
    // frame; a note off closes the newest pending note with the same pitch and
    // channel, with length = off frame - on frame. Nothing reaches the model
    // from here, so an event that could only yield an invalid note is refused
    // at the door instead of poisoning the whole take: a malformed note on
    // (anything outside the wire ranges velocity 1…127, pitch 0…127, channel
    // 0…15), a past-timeline frame and an event over the take budget all raise
    // dropped(). An off that matches no pending note — the normal result of the
    // ring dropping events — is discarded and raises unmatched(). Events are
    // never reordered: a frame
    // earlier than the last one captured is clamped forward, and an event that
    // is refused leaves the capture clock where it was.
    void feed(const RecordedMidiEvent* events,uint64_t count);
    void feed(RecordedMidiEvent event) { feed(&event,1); }

    // Ends the take and returns its batch in note-off order. Every pending note
    // closes at stopFrame, so a key still held down when recording stops becomes
    // a note that ends there rather than an invalid zero-length one. stop()
    // consumes the take whether or not it produced notes; an armed recorder with
    // nothing fed returns an empty batch, which the caller may still hand to
    // appendMidiNotes as the no-op it is.
    std::vector<MidiNote> stop(uint64_t stopFrame);

    // Notes closed by feed() and not yet taken by stop().
    uint64_t recordedNotes() const noexcept { return static_cast<uint64_t>(committed_.size()); }
    // Keys currently held down, i.e. the pending notes this take still has open.
    uint64_t openNotes() const noexcept { return static_cast<uint64_t>(open_.size()); }
    uint64_t dropped() const noexcept { return dropped_; }
    uint64_t unmatched() const noexcept { return unmatched_; }

private:
    struct OpenNote { uint64_t start=0; uint8_t pitch=0, channel=0, velocity=0; };

    bool armed_=false;
    uint64_t trackID_=0, lastFrame_=0, dropped_=0, unmatched_=0;
    uint32_t clipIndex_=0;
    std::vector<OpenNote> open_;
    std::vector<MidiNote> committed_;
    // Closes one pending note at end, honouring the domain length limits. Named
    // to stay clear of the POSIX close() this translation unit also uses.
    void closeNote(const OpenNote& held,uint64_t end) noexcept;
};

// True when every note of a recorded batch is a note the target clip can accept
// today: the same per-note rules validate() applies to a MIDI clip, including
// "inside the clip window". This is the live path pre-flight check — recording
// past the end of a clip is normal, appendMidiNotes rejects the entire batch for
// one note that no longer fits, and the caller uses this predicate to cut the
// take short instead of losing it. A false result only predicts rejection, not
// the reverse: the project-wide budget still belongs to the command.
bool midiRecordBatchFits(const MidiClip& clip,const std::vector<MidiNote>& batch);
}
