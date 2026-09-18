#include "domain/clip_clipboard.hpp"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

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

void Session::editClipSelection(const std::vector<ClipSelectionRef> &selection,
                                ClipSelectionEdit action, int64_t delta, int32_t trackOffset,
                                uint64_t expected) {
    check(expected);
    if (selection.empty() || selection.size() > 256)
        throw Error("Select 1–256 clips for a group edit");
    if (action != ClipSelectionEdit::Move && action != ClipSelectionEdit::Copy &&
        action != ClipSelectionEdit::Delete)
        throw Error("Unknown clip selection action");
    if (action == ClipSelectionEdit::Delete && (delta != 0 || trackOffset != 0))
        throw Error("Delete does not accept a time or track offset");
    // Build every source batch against the same immutable revision BEFORE any
    // erase/insert/sort. Moving onto another selected track cannot rebind indices.
    std::map<std::pair<size_t, bool>, std::vector<uint32_t>> groups;
    std::set<std::tuple<uint64_t, bool, uint32_t>> unique;
    for (const auto &ref : selection) {
        if (!unique.emplace(ref.trackID, ref.midi, ref.index).second)
            throw Error("Duplicate clip selection reference");
        const auto found = std::find_if(current.tracks.begin(), current.tracks.end(),
                                        [&](const Track &t) { return t.id == ref.trackID; });
        if (found == current.tracks.end())
            throw Error("Selected track no longer exists");
        if (ref.index >= (ref.midi ? found->midiClips.size() : found->regions.size()))
            throw Error("Selected clip no longer exists");
        groups[{static_cast<size_t>(found - current.tracks.begin()), ref.midi}].push_back(ref.index);
    }
    if (action == ClipSelectionEdit::Move && delta == 0 && trackOffset == 0)
        return; // Validate references first; a stale no-op must still be rejected.
    struct Batch {
        size_t source = 0, target = 0;
        bool midi = false;
        uint64_t start = 0;
        std::vector<uint32_t> indices;
        ClipClipboard board;
    };
    std::vector<Batch> batches;
    for (auto &[key, indices] : groups) {
        std::sort(indices.begin(), indices.end());
        const auto [source, midi] = key;
        Batch batch;
        batch.source = source; batch.midi = midi; batch.indices = indices;
        if (action != ClipSelectionEdit::Delete) {
            const auto destination = static_cast<int64_t>(source) + trackOffset;
            if (destination < 0 || static_cast<uint64_t>(destination) >= current.tracks.size())
                throw Error("The entire selection must fit within existing tracks");
            batch.target = static_cast<size_t>(destination);
            const auto &track = current.tracks[source];
            uint64_t anchor = std::numeric_limits<uint64_t>::max();
            for (auto index : indices)
                anchor = std::min(anchor, midi ? track.midiClips[index].start : track.regions[index].start);
            // Avoid signed conversion/negation overflow, even for INT64_MIN.
            if (delta < 0) {
                const auto magnitude = static_cast<uint64_t>(-(delta + 1)) + 1;
                if (magnitude > anchor)
                    throw Error("The entire selection must remain after frame zero");
                batch.start = anchor - magnitude;
            } else {
                const auto limit = midi ? kMaxMidiFrame : audioLimit;
                if (static_cast<uint64_t>(delta) > limit - anchor)
                    throw Error("The entire selection must fit within the timeline");
                batch.start = anchor + static_cast<uint64_t>(delta);
            }
            batch.board = captureClips(track.id, indices, midi, false, expected);
        }
        batches.push_back(std::move(batch));
    }
    State next = current;
    if (action != ClipSelectionEdit::Copy) {
        for (const auto &batch : batches)
            erase(next.tracks[batch.source], batch.indices, batch.midi);
    }
    if (action != ClipSelectionEdit::Delete) {
        for (const auto &batch : batches) {
            auto &target = next.tracks[batch.target];
            // The current arrangement projects one type per row. Never insert
            // invisible MIDI under a retained audio source, or hide MIDI with audio.
            if ((batch.midi && target.audio) || (!batch.midi && !target.midiClips.empty()))
                throw Error("Destination track has an incompatible audio/MIDI type");
            batch.board.pasteInto(next, target.id, batch.start);
        }
    }
    // validate/commit owns overlaps, budgets and Undo. No clipboard is touched,
    // and failure in a later batch discards ALL earlier candidate changes.
    commit(std::move(next));
}
}
