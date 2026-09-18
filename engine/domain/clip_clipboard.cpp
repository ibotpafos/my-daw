#include "domain/clip_clipboard.hpp"
#include <algorithm>
#include <limits>

namespace daw {
namespace {
constexpr uint64_t audioLimit = 48000ULL * 600;
Track &findTrack(State &state, uint64_t id) {
    auto it = std::find_if(state.tracks.begin(), state.tracks.end(), [id](const Track &track) {
        return track.id == id;
    });
    if (it == state.tracks.end())
        throw Error("Track not found");
    return *it;
}
std::vector<uint32_t> checkedIndices(std::vector<uint32_t> indices, size_t size) {
    if (indices.empty() || indices.size() > 256)
        throw Error("Select 1–256 clips for the clipboard");
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.back() >= size)
        throw Error("Clipboard clip index is no longer valid");
    return indices;
}
void erase(Track &track, const std::vector<uint32_t> &indices, bool midi) {
    for (auto it = indices.rbegin(); it != indices.rend(); ++it) {
        if (midi)
            track.midiClips.erase(track.midiClips.begin() + *it);
        else
            track.regions.erase(track.regions.begin() + *it);
    }
}
}
ClipClipboard Session::captureClips(uint64_t id, std::vector<uint32_t> indices, bool midi, bool cut,
                                    uint64_t expected) {
    check(expected);
    const auto it =
        std::find_if(current.tracks.begin(), current.tracks.end(), [id](const Track &track) {
            return track.id == id;
        });
    if (it == current.tracks.end())
        throw Error("Track not found");
    indices = checkedIndices(std::move(indices), midi ? it->midiClips.size() : it->regions.size());
    ClipClipboard board;
    board.midi_ = midi;
    uint64_t anchor = std::numeric_limits<uint64_t>::max(), end = 0;
    for (const auto index : indices) {
        if (midi) {
            const auto &clip = it->midiClips[index];
            anchor = std::min(anchor, clip.start);
            end = std::max(end, clip.start + clip.length);
            board.midiClips_.push_back(clip);
        } else {
            const auto &region = it->regions[index];
            const auto *take = region.take == 0 ? nullptr : &it->takes.at(region.take - 1);
            anchor = std::min(anchor, region.start);
            end = std::max(end, region.start + region.length);
            board.audioClips_.push_back({region, take ? take->audio : it->audio,
                                         take ? take->name : it->name,
                                         take ? take->start : it->baseStart});
        }
    }
    board.length_ = end - anchor;
    for (auto &entry : board.audioClips_)
        entry.region.start -= anchor;
    for (auto &clip : board.midiClips_)
        clip.start -= anchor;
    // Allocate the complete replacement clipboard before spending an Undo step.
    // Returning/moving the result is noexcept; a rejected cut keeps the old board.
    if (cut) {
        State next = current;
        erase(findTrack(next, id), indices, midi);
        commit(std::move(next));
    }
    return board;
}
void ClipClipboard::pasteInto(State &state, uint64_t target, uint64_t start) const {
    if (count() == 0)
        throw Error("Clip clipboard is empty");
    const uint64_t limit = midi_ ? kMaxMidiFrame : audioLimit;
    if (start > limit || length_ > limit - start)
        throw Error("No timeline space for the clip");
    auto &track = findTrack(state, target);
    if (midi_) {
        if (midiClips_.size() > kMaxMidiClipsPerTrack - track.midiClips.size())
            throw Error("Track supports at most 64 MIDI clips");
        for (auto clip : midiClips_) {
            clip.start += start;
            track.midiClips.push_back(std::move(clip));
        }
        std::stable_sort(track.midiClips.begin(), track.midiClips.end(),
                         [](const auto &a, const auto &b) {
                             return a.start < b.start;
                         });
        return;
    }
    if (audioClips_.size() > 256 - track.regions.size())
        throw Error("Track supports at most 256 clips");
    for (const auto &entry : audioClips_) {
        auto region = entry.region;
        // Reuse the persisted per-track source table (base + takes). A foreign
        // source is attached once, not decoded, copied or written during a drag.
        // Original source timing is retained for the existing comping contract.
        if (!track.audio) {
            track.audio = entry.source;
            track.baseStart = entry.sourceStart;
            region.take = 0;
        } else if (track.audio == entry.source && track.baseStart == entry.sourceStart) {
            region.take = 0;
        } else {
            auto take = std::find_if(track.takes.begin(), track.takes.end(), [&](const Take &t) {
                return t.audio == entry.source && t.start == entry.sourceStart;
            });
            if (take == track.takes.end()) {
                if (track.takes.size() >= 15)
                    throw Error("Target track supports at most 16 audio sources/takes");
                track.takes.push_back({entry.name, entry.sourceStart, entry.source});
                region.take = static_cast<uint32_t>(track.takes.size());
            } else {
                region.take = static_cast<uint32_t>(take - track.takes.begin() + 1);
            }
        }
        region.start += start;
        track.regions.push_back(region);
    }
    std::stable_sort(track.regions.begin(), track.regions.end(),
                     [](const Region &a, const Region &b) {
                         return a.start < b.start;
                     });
}
void Session::pasteClips(const ClipClipboard &clipboard, uint64_t target, uint64_t start,
                         uint64_t expected) {
    check(expected);
    State next = current;
    clipboard.pasteInto(next, target, start);
    // All target capacity, memory, overlap and crossfade rules are checked
    // before publication. One paste of a group is exactly one history entry.
    commit(std::move(next));
}
void Session::transferClips(uint64_t source, std::vector<uint32_t> indices, bool midi,
                            uint64_t target, uint64_t start, bool copy, uint64_t expected) {
    const auto clipboard = captureClips(source, indices, midi, false, expected);
    State next = current;
    auto &from = findTrack(next, source);
    indices =
        checkedIndices(std::move(indices), midi ? from.midiClips.size() : from.regions.size());
    if (!copy)
        erase(from, indices, midi);
    clipboard.pasteInto(next, target, start);
    if (next.tracks == current.tracks)
        return;
    commit(std::move(next));
}
}
