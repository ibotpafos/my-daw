#pragma once
#include "domain/session.hpp"

namespace daw {
// A bounded, immutable document clipboard. Audio samples are shared with the
// same const Clip objects as Undo; MIDI and region properties are value copies.
// It contains no track IDs/indices that could be rebound by later edits.
class ClipClipboard {
  public:
    bool midi() const noexcept {
        return midi_;
    }
    uint32_t count() const noexcept {
        return static_cast<uint32_t>(midi_ ? midiClips_.size() : audioClips_.size());
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
    };
    bool midi_ = false;
    uint64_t length_ = 0;
    std::vector<AudioEntry> audioClips_;
    std::vector<MidiClip> midiClips_;
    void pasteInto(State &state, uint64_t target, uint64_t start) const;
};
}
