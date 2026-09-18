#pragma once
#include "domain/session.hpp"

namespace daw {
// A bounded, immutable document clipboard. Audio samples are shared with the
// same const Clip objects as Undo; MIDI and region properties are value copies.
// It contains no track IDs/indices that could be rebound by later edits.
class ClipClipboard {
  public:
    bool midi() const noexcept {
        return audioClips_.empty() && !midiClips_.empty();
    }
    uint32_t count() const noexcept {
        return static_cast<uint32_t>(midiClips_.size() + audioClips_.size());
    }
    // Existing kind values stay stable; 3 identifies mixed audio/MIDI content.
    uint32_t kind() const noexcept {
        return count() == 0 ? 0 : (audioClips_.empty() ? 2 : (midiClips_.empty() ? 1 : 3));
    }
    uint64_t length() const noexcept {
        return length_;
    }

  private:
    friend class Session;
    struct AudioEntry {
        Region region;
        std::shared_ptr<const Clip> source;
        std::string name;
        uint64_t sourceStart = 0;
        size_t rowOffset = 0;
    };
    struct MidiEntry {
        MidiClip clip;
        size_t rowOffset = 0;
    };
    // Offsets are captured in project track order, including unselected gaps.
    // No source track ID is needed after Copy/Cut, Undo or source deletion.
    size_t rowSpan_ = 1;
    bool arrangementTypes_ = true;
    uint64_t length_ = 0;
    std::vector<AudioEntry> audioClips_;
    std::vector<MidiEntry> midiClips_;
    void pasteInto(State &state, uint64_t target, uint64_t start) const;
};
}
