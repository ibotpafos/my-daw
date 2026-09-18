// e2e: MIDI-01 + TEMPO-01 + MARKER-01 — the three data lanes end to end.
//
// MIDI clips, the tempo/time-signature maps and marker locators are driven
// purely through the public C ABI: geometry/ABI rejection matrices, paged note
// reads over the per-call limit, batch-append atomicity, window edits
// (move/trim/split), grid quantization on the default 120 BPM map,
// one-revision-per-command accounting, undo/redo of note arrays, capacity
// boundaries (64 tempo points, 64 signatures, 256 markers) and a final
// save/reopen whose dumpOf() equality proves every lane round-trips together.
// MIDI is data-only (docs/68: no audible render is asserted). Tempo rules
// follow docs/69 (frame-0 anchors 120 BPM and 4/8, bpm in (20,999]); markers
// follow docs/74 (1-120 codepoints, unique frames, 256 cap).
#include "e2e.hpp"

namespace {

// Note coordinates are clip-relative; every call cross-checks ABI shape.
daw_midi_note noteAt(uint64_t start, uint64_t length, uint8_t pitch, uint8_t channel, uint8_t velocity) {
    daw_midi_note value{};
    value.struct_size = sizeof(value);
    value.version = DAW_MIDI_NOTE_VERSION;
    value.start = start;
    value.length = length;
    value.pitch = pitch;
    value.channel = channel;
    value.velocity = velocity;
    return value;
}

daw_midi_clip clipMeta(uint64_t start, uint64_t length, int32_t lane, uint32_t color, uint32_t noteCount) {
    daw_midi_clip value{};
    value.struct_size = sizeof(value);
    value.version = DAW_MIDI_CLIP_VERSION;
    value.start = start;
    value.length = length;
    value.lane = lane;
    value.color = color;
    value.note_count = noteCount;
    return value;
}

// Takes the clip by value so call sites can pass freshly built metadata.
int addClip(daw_session* session, uint64_t track, daw_midi_clip clip, const daw_midi_note* notes, uint32_t count,
            uint64_t revision) {
    return daw_add_midi_clip(session, track, &clip, notes, count, revision);
}

// Reads every note of one clip through the documented paging protocol.
std::vector<e2e::DumpNote> readAllNotes(daw_session* session, uint64_t trackId, uint32_t clipIndex) {
    auto clip = e2e::abi<daw_midi_clip>();
    clip.version = DAW_MIDI_CLIP_VERSION;
    std::vector<daw_midi_note> page(DAW_MIDI_NOTES_PER_CALL);
    std::vector<e2e::DumpNote> notes;
    uint32_t written = 0;
    uint32_t offset = 0;
    CHECK_OK(session, daw_get_midi_clip(session, trackId, clipIndex, &clip, 0, nullptr, 0, &written));
    CHECK(written == 0); // metadata-only probe: note_count is the full total
    while (offset < clip.note_count) {
        CHECK_OK(session, daw_get_midi_clip(session, trackId, clipIndex, &clip, offset, page.data(),
                                            static_cast<uint32_t>(page.size()), &written));
        if (written == 0) throw std::runtime_error("note paging stalled");
        for (uint32_t index = 0; index < written; ++index) {
            e2e::DumpNote entry;
            entry.start = page[index].start;
            entry.length = page[index].length;
            entry.pitch = page[index].pitch;
            entry.channel = page[index].channel;
            entry.velocity = page[index].velocity;
            notes.push_back(entry);
        }
        offset += written;
    }
    return notes;
}

e2e::DumpNote dumpNote(uint64_t start, uint64_t length, uint8_t pitch, uint8_t channel, uint8_t velocity) {
    e2e::DumpNote value;
    value.start = start;
    value.length = length;
    value.pitch = pitch;
    value.channel = channel;
    value.velocity = velocity;
    return value;
}

std::vector<uint64_t> noteStarts(const std::vector<e2e::DumpNote>& notes) {
    std::vector<uint64_t> starts;
    starts.reserve(notes.size());
    for (const auto& note : notes) starts.push_back(note.start);
    return starts;
}

using TempoLane = std::vector<std::pair<uint64_t, double>>;
using MeterLane = std::vector<std::tuple<uint64_t, uint32_t, uint32_t>>;
using MarkerL = std::vector<std::pair<uint64_t, std::string>>;

TempoLane tempoLane(daw_session* session) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_tempo_count(session, &count));
    TempoLane lane;
    for (uint32_t index = 0; index < count; ++index) {
        auto point = e2e::abi<daw_tempo_point>();
        point.version = DAW_TEMPO_POINT_VERSION;
        CHECK_OK(session, daw_get_tempo_point(session, index, &point));
        lane.emplace_back(point.frame, point.bpm);
    }
    return lane;
}

MeterLane signatureLane(daw_session* session) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_time_signature_count(session, &count));
    MeterLane lane;
    for (uint32_t index = 0; index < count; ++index) {
        auto point = e2e::abi<daw_time_signature_point>();
        point.version = DAW_TIME_SIGNATURE_POINT_VERSION;
        CHECK_OK(session, daw_get_time_signature_point(session, index, &point));
        lane.emplace_back(point.frame, point.numerator, point.denominator);
    }
    return lane;
}

MarkerL markerLane(daw_session* session) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_marker_count(session, &count));
    MarkerL lane;
    for (uint32_t index = 0; index < count; ++index) {
        auto marker = e2e::abi<daw_marker>();
        marker.version = DAW_MARKER_VERSION;
        CHECK_OK(session, daw_get_marker(session, index, &marker));
        lane.emplace_back(marker.frame, std::string(marker.name));
    }
    return lane;
}

uint32_t midiClipCount(daw_session* session, uint64_t trackId) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_midi_clip_count(session, trackId, &count));
    return count;
}

} // namespace

int main() {
    try {
        using namespace e2e;
        TempRoot root("miditempo");

        // =========================================================================
        // 1. MIDI-01 — clip commands on data-only tracks.
        // =========================================================================
        Bridge synthBridge;
        daw_session* s = synthBridge.get();
        const uint64_t synth = addTrack(s, "Synth");
        CHECK(rev(s) == 1);
        CHECK(midiClipCount(s, synth) == 0);

        // 1a. Pure ABI-shape rejections: plain rc=1 (struct_size/NULL/capacity gates).
        {
            auto clip = clipMeta(0, 48000, 0, 0, 0);
            auto badSize = clip;
            badSize.struct_size = sizeof(daw_midi_clip) - 4;
            CHECK(daw_add_midi_clip(s, synth, &badSize, nullptr, 0, rev(s)) == 1);
            auto badVersion = clip;
            badVersion.version = DAW_MIDI_CLIP_VERSION + 1;
            CHECK(daw_add_midi_clip(s, synth, &badVersion, nullptr, 0, rev(s)) == 1);
            CHECK(daw_add_midi_clip(s, synth, nullptr, nullptr, 0, rev(s)) == 1);
            auto out = clipMeta(0, 0, 0, 0, 0);
            daw_midi_note probe{};
            uint32_t shapeWritten = 7;
            CHECK(daw_get_midi_clip(s, synth, 0, nullptr, 0, nullptr, 0, &shapeWritten) == 1);
            CHECK(daw_get_midi_clip(s, synth, 0, &out, 0, nullptr, DAW_MIDI_NOTES_PER_CALL + 1, &shapeWritten) == 1);
            out.struct_size = sizeof(daw_midi_clip) - 4;
            CHECK(daw_get_midi_clip(s, synth, 0, &out, 0, &probe, 1, &shapeWritten) == 1);
            CHECK(rev(s) == 1);
        }

        // 1b. Semantic rejection matrix — each must leave human-readable text and
        //     consume no revision (aligned with midi_bridge/midi_model tests).
        {
            const uint64_t held = rev(s);
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 0, 0, 0, 0), nullptr, 0, held)); // zero-length clip
            auto one = noteAt(0, 480, 60, 0, 100);
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, 0, 0, 3), &one, 2, held)); // note_count != array length
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, 0, 0, 1), nullptr, 0, held)); // claims a note, no array
            auto zeroVelocity = noteAt(0, 480, 60, 0, 0);
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, 0, 0, 1), &zeroVelocity, 1, held));
            auto maxVelocity = noteAt(0, 480, 60, 0, 128);
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, 0, 0, 1), &maxVelocity, 1, held));
            auto badPitch = noteAt(0, 480, 128, 0, 100);
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, 0, 0, 1), &badPitch, 1, held));
            auto badChannel = noteAt(0, 480, 60, 16, 100);
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, 0, 0, 1), &badChannel, 1, held));
            auto badNoteAbi = noteAt(0, 480, 60, 0, 100);
            badNoteAbi.struct_size = sizeof(daw_midi_note) + 4;
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, 0, 0, 1), &badNoteAbi, 1, held));
            auto zeroLengthNote = noteAt(0, 0, 60, 0, 100);
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, 0, 0, 1), &zeroLengthNote, 1, held));
            auto longNote = noteAt(0, 480001, 60, 0, 100); // domain note-length cap is 480000 frames
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 480002, 0, 0, 1), &longNote, 1, held));
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, 48000, -1, 0, 0), nullptr, 0, held)); // negative lane
            CHECK_REJ(s, addClip(s, synth, clipMeta(0, (1ULL << 40) + 1, 0, 0, 0), nullptr, 0, held)); // past the 2^40-frame timeline limit
            auto beyondClip = noteAt(50, 60, 60, 0, 100); // ends at 110 inside a 100-frame clip
            CHECK_REJ(s, addClip(s, synth, clipMeta(500000, 100, 0, 0, 1), &beyondClip, 1, held));
            CHECK(rev(s) == held);
            CHECK(midiClipCount(s, synth) == 0);
        }

        // 1c. The first real clip: three notes, lane 7, v2 color.
        const std::vector<DumpNote> original = {
            dumpNote(0, 480, 60, 0, 100), dumpNote(9600, 1920, 64, 5, 90), dumpNote(24000, 2400, 67, 15, 1)};
        {
            daw_midi_note notes[3] = {noteAt(0, 480, 60, 0, 100), noteAt(9600, 1920, 64, 5, 90),
                                      noteAt(24000, 2400, 67, 15, 1)};
            CHECK_OK(s, addClip(s, synth, clipMeta(0, 48000, 7, 0x1A2B3C, 3), notes, 3, rev(s)));
            CHECK(rev(s) == 2); // one add = one revision
        }
        auto meta0 = clipMeta(0, 0, 0, 0, 0);
        uint32_t written = 7;
        CHECK_OK(s, daw_get_midi_clip(s, synth, 0, &meta0, 0, nullptr, 0, &written)); // metadata-only
        CHECK(meta0.start == 0 && meta0.length == 48000 && meta0.lane == 7 && meta0.color == 0x1A2B3C);
        CHECK(meta0.note_count == 3 && written == 0);
        CHECK(readAllNotes(s, synth, 0) == original);

        // 1d. Reader bounds (paged protocol per daw.h): unknown clip index is a
        //     semantic rejection; offset/NULL/capacity guards return rc=1.
        {
            auto out = clipMeta(0, 0, 0, 0, 0);
            daw_midi_note one{};
            CHECK_REJ(s, daw_get_midi_clip(s, synth, 1, &out, 0, nullptr, 0, &written));
            CHECK(daw_get_midi_clip(s, synth, 0, &out, 4, &one, 1, &written) != 0); // offset past note_count
            CHECK_OK(s, daw_get_midi_clip(s, synth, 0, &out, 3, nullptr, 0, &written)); // offset == note_count: legal, empty page
            CHECK(written == 0);
            CHECK(daw_get_midi_clip(s, synth, 0, &out, 0, nullptr, 4, &written) != 0); // capacity without buffer
            CHECK_OK(s, daw_get_midi_clip(s, synth, 0, &out, 2, &one, 1, &written)); // last note via offset paging
            CHECK(written == 1 && one.start == 24000 && one.pitch == 67);
        }

        // 1e. Clip color (v2): change = one revision, identical = silent no-op.
        CHECK_OK(s, daw_set_midi_clip_color(s, synth, 0, 0x0F1E2D, rev(s)));
        CHECK(rev(s) == 3);
        {
            const uint64_t held = rev(s);
            CHECK_OK(s, daw_set_midi_clip_color(s, synth, 0, 0x0F1E2D, held));
            CHECK(rev(s) == held);
        }
        meta0 = clipMeta(0, 0, 0, 0, 0);
        CHECK_OK(s, daw_get_midi_clip(s, synth, 0, &meta0, 0, nullptr, 0, &written));
        CHECK(meta0.color == 0x0F1E2D);

        // 1f. A clip with MORE notes than DAW_MIDI_NOTES_PER_CALL: 8192 written in
        //     one add (add/append batches are capped at 8192) + 808 appended in
        //     one revision = 9000, then paged fully back through note_offset.
        const uint64_t dense = addTrack(s, "Dense");
        {
            std::vector<daw_midi_note> notes(DAW_MIDI_NOTES_PER_CALL);
            for (uint64_t index = 0; index < notes.size(); ++index)
                notes[index] = noteAt(index, 1, static_cast<uint8_t>(index % 128), static_cast<uint8_t>(index % 16),
                                      static_cast<uint8_t>(1 + index % 127));
            const uint64_t beforeAdd = rev(s);
            CHECK_OK(s, addClip(s, dense, clipMeta(0, 480000, 2, 0, DAW_MIDI_NOTES_PER_CALL), notes.data(),
                                static_cast<uint32_t>(notes.size()), beforeAdd));
            CHECK(rev(s) == beforeAdd + 1);
            std::vector<daw_midi_note> tail(808);
            for (uint64_t index = 0; index < tail.size(); ++index) {
                const uint64_t global = notes.size() + index;
                tail[index] = noteAt(global, 1, static_cast<uint8_t>(global % 128), static_cast<uint8_t>(global % 16),
                                     static_cast<uint8_t>(1 + global % 127));
            }
            CHECK_OK(s, daw_append_midi_notes(s, dense, 0, tail.data(), static_cast<uint32_t>(tail.size()), rev(s)));
            CHECK(rev(s) == beforeAdd + 2); // the 808-note batch is ONE revision
            auto out = clipMeta(0, 0, 0, 0, 0);
            CHECK_OK(s, daw_get_midi_clip(s, dense, 0, &out, 0, nullptr, 0, &written));
            CHECK(out.note_count == 9000 && out.lane == 2);
            const auto all = readAllNotes(s, dense, 0); // 8192 + 808 page reads
            CHECK(all.size() == 9000);
            for (uint64_t index = 0; index < all.size(); ++index) {
                CHECK(all[index].start == index && all[index].length == 1);
                CHECK(all[index].pitch == static_cast<uint8_t>(index % 128));
                CHECK(all[index].channel == static_cast<uint8_t>(index % 16));
                CHECK(all[index].velocity == static_cast<uint8_t>(1 + index % 127));
            }
            // A piano-roll commit replaces this whole clip in ONE command,
            // even when reading it required more than one page.
            std::vector<daw_midi_note> replacement;
            replacement.reserve(all.size());
            for (const auto& note : all)
                replacement.push_back(noteAt(note.start, note.length, note.pitch, note.channel, 100));
            const auto beforeReplace = rev(s);
            CHECK_OK(s, daw_set_midi_notes(s, dense, 0, replacement.data(),
                     static_cast<uint32_t>(replacement.size()), beforeReplace));
            CHECK(rev(s) == beforeReplace + 1);
            const auto replaced = readAllNotes(s, dense, 0);
            CHECK(replaced.size() == all.size());
            for (const auto& note : replaced) CHECK(note.velocity == 100);
            CHECK_OK(s, daw_undo(s, rev(s)));
            const auto restored = readAllNotes(s, dense, 0);
            CHECK(restored.size() == all.size());
            for (size_t i = 0; i < all.size(); ++i) {
                CHECK(restored[i].velocity == all[i].velocity);
                CHECK(restored[i].start == all[i].start && restored[i].length == all[i].length);
            }
        }

        // 1g. Overlapping clips reject (timeline truth stays domain-owned).
        {
            const uint64_t held = rev(s);
            CHECK_REJ(s, addClip(s, dense, clipMeta(47999, 48000, 3, 0, 0), nullptr, 0, held));
            CHECK(rev(s) == held);
            CHECK(midiClipCount(s, dense) == 1);
        }

        // 1h. set_midi_notes replaces the full array; identical arrays are silent
        //     no-ops; an empty array clears the notes but keeps the window.
        const std::vector<DumpNote> pair = {dumpNote(0, 500, 60, 0, 15), dumpNote(500, 400, 62, 15, 9)};
        {
            daw_midi_note notes[2] = {noteAt(0, 500, 60, 0, 15), noteAt(500, 400, 62, 15, 9)};
            const uint64_t beforeSet = rev(s);
            CHECK_OK(s, daw_set_midi_notes(s, synth, 0, notes, 2, beforeSet));
            CHECK(rev(s) == beforeSet + 1);
            CHECK_OK(s, daw_set_midi_notes(s, synth, 0, notes, 2, rev(s))); // identical -> silent no-op
            CHECK(rev(s) == beforeSet + 1);
            // undo/redo restore the note array exactly (one command = one step).
            CHECK_OK(s, daw_undo(s, beforeSet + 1));
            CHECK(readAllNotes(s, synth, 0) == original);
            CHECK_OK(s, daw_redo(s, beforeSet + 2));
            CHECK(rev(s) == beforeSet + 3);
            CHECK(readAllNotes(s, synth, 0) == pair);
            CHECK_OK(s, daw_set_midi_notes(s, synth, 0, nullptr, 0, rev(s))); // empty clears, window survives
            auto out = clipMeta(0, 0, 0, 0, 0);
            CHECK_OK(s, daw_get_midi_clip(s, synth, 0, &out, 0, nullptr, 0, &written));
            CHECK(out.note_count == 0 && out.start == 0 && out.length == 48000 && out.lane == 7 &&
                  out.color == 0x0F1E2D);
            daw_midi_note back[3] = {noteAt(0, 480, 60, 0, 100), noteAt(9600, 1920, 64, 5, 90),
                                      noteAt(24000, 2400, 67, 15, 1)};
            CHECK_OK(s, daw_set_midi_notes(s, synth, 0, back, 3, rev(s))); // restore for the edit section
            CHECK(readAllNotes(s, synth, 0) == original);
        }

        // 1i. append: ONE revision per batch; one non-fitting note refuses the
        //     WHOLE batch (revision and note arrays untouched).
        {
            const uint64_t beforeAppend = rev(s);
            daw_midi_note poisoned[2] = {noteAt(0, 480, 70, 0, 50), noteAt(47900, 200, 71, 0, 60)};
            CHECK_REJ(s, daw_append_midi_notes(s, synth, 0, poisoned, 2, beforeAppend)); // #2 runs past the clip end
            CHECK(rev(s) == beforeAppend);
            CHECK(readAllNotes(s, synth, 0) == original);
            daw_midi_note good[2] = {noteAt(40000, 240, 72, 0, 50), noteAt(3000, 240, 73, 0, 60)};
            CHECK_OK(s, daw_append_midi_notes(s, synth, 0, good, 2, beforeAppend));
            CHECK(rev(s) == beforeAppend + 1);
            const std::vector<DumpNote> five = {
                dumpNote(0, 480, 60, 0, 100), dumpNote(9600, 1920, 64, 5, 90), dumpNote(24000, 2400, 67, 15, 1),
                dumpNote(40000, 240, 72, 0, 50), dumpNote(3000, 240, 73, 0, 60)};
            CHECK(readAllNotes(s, synth, 0) == five); // capture order kept verbatim at the tail
            daw_midi_note extra[1] = {noteAt(0, 480, 60, 0, 100)};
            const uint64_t emptyHeld = rev(s);
            CHECK_OK(s, daw_append_midi_notes(s, synth, 0, extra, 0, emptyHeld)); // empty batch = silent no-op
            CHECK(rev(s) == emptyHeld);
        }

        // 1j. Move: overlap rejects with text; a committed move costs one
        //     revision; an identical move is a silent no-op. White-box note:
        //     Session::moveMidiClip keeps the clip at its vector POSITION (it does
        //     not re-sort; timeline order is protected by the overlap rule).
        {
            const uint64_t beforeMarker = rev(s);
            CHECK_OK(s, addClip(s, synth, clipMeta(100000, 1000, 0, 0, 0), nullptr, 0, beforeMarker));
            CHECK(rev(s) == beforeMarker + 1); // one add = one revision
            CHECK_REJ(s, daw_move_midi_clip(s, synth, 0, 90000, rev(s))); // 90000..138000 overlaps 100000..101000
            CHECK(rev(s) == beforeMarker + 1);
            CHECK_OK(s, daw_move_midi_clip(s, synth, 0, 48000, rev(s))); // free slot, touches nothing
            CHECK(rev(s) == beforeMarker + 2);
            const uint64_t movedHeld = rev(s);
            CHECK_OK(s, daw_move_midi_clip(s, synth, 0, 48000, movedHeld)); // identical move = silent no-op
            CHECK(rev(s) == movedHeld);
            auto out = clipMeta(0, 0, 0, 0, 0);
            CHECK_OK(s, daw_get_midi_clip(s, synth, 0, &out, 0, nullptr, 0, &written));
            CHECK(out.start == 48000 && out.note_count == 5); // still vector position 0 (no re-sort happened)
        }

        // 1k. Trim: growth shifts clip-relative notes with the window; a shrink
        //     drops notes that no longer fit WHOLE. Surviving set asserted exactly.
        const std::vector<DumpNote> shrunk = {dumpNote(0, 480, 60, 0, 100), dumpNote(9600, 1920, 64, 5, 90),
                                              dumpNote(3000, 240, 73, 0, 60)};
        {
            const uint64_t beforeTrim = rev(s);
            CHECK_OK(s, daw_trim_midi_clip(s, synth, 0, 45000, 51000, beforeTrim)); // grow 3000 left
            CHECK(rev(s) == beforeTrim + 1);
            const std::vector<uint64_t> shifted = {3000, 12600, 27000, 43000, 6000};
            CHECK(noteStarts(readAllNotes(s, synth, 0)) == shifted); // every note rode the window
            const uint64_t grownHeld = rev(s);
            CHECK_OK(s, daw_trim_midi_clip(s, synth, 0, 45000, 51000, grownHeld)); // identical trim: silent no-op
            CHECK(rev(s) == grownHeld);
            CHECK_REJ(s, daw_trim_midi_clip(s, synth, 0, 45000, 0, grownHeld)); // zero length rejected
            CHECK_OK(s, daw_trim_midi_clip(s, synth, 0, 48000, 15000, grownHeld)); // shrink back
            CHECK(rev(s) == grownHeld + 1);
            CHECK(readAllNotes(s, synth, 0) == shrunk); // exactly A, B and E survive whole
        }

        // 1l. Split: a crossing note is cut in two; edges reject; a split keeps
        //     both parts within bounds with exactly the right notes each side.
        {
            const uint64_t beforeCrossing = rev(s);
            CHECK_OK(s, daw_split_midi_clip(s, synth, 0, 51100, beforeCrossing));
            CHECK(rev(s) == beforeCrossing + 1 && midiClipCount(s, synth) == 3);
            const auto cutLeft = readAllNotes(s, synth, 0);
            const auto cutRight = readAllNotes(s, synth, 1);
            CHECK(cutLeft.back() == dumpNote(3000, 100, 73, 0, 60));
            CHECK(cutRight.back() == dumpNote(0, 140, 73, 0, 60));
            CHECK_OK(s, daw_undo(s, rev(s)));
            CHECK(readAllNotes(s, synth, 0) == shrunk);
            const uint64_t beforeSplit = rev(s);
            CHECK_REJ(s, daw_split_midi_clip(s, synth, 0, 48000, beforeSplit));  // on the clip start
            CHECK_REJ(s, daw_split_midi_clip(s, synth, 0, 63000, beforeSplit));   // on the clip end
            CHECK(rev(s) == beforeSplit && midiClipCount(s, synth) == 2);
            CHECK_OK(s, daw_split_midi_clip(s, synth, 0, 49500, beforeSplit));
            CHECK(rev(s) == beforeSplit + 1); // one split = one revision
            CHECK(midiClipCount(s, synth) == 3);
            auto left = clipMeta(0, 0, 0, 0, 0);
            auto right = clipMeta(0, 0, 0, 0, 0);
            CHECK_OK(s, daw_get_midi_clip(s, synth, 0, &left, 0, nullptr, 0, &written));
            CHECK_OK(s, daw_get_midi_clip(s, synth, 1, &right, 0, nullptr, 0, &written));
            CHECK(left.start == 48000 && left.length == 1500);
            CHECK(right.start == 49500 && right.length == 13500); // parts touch and stay in bounds
            const std::vector<DumpNote> leftNotes = {dumpNote(0, 480, 60, 0, 100)};
            const std::vector<DumpNote> rightNotes = {dumpNote(8100, 1920, 64, 5, 90), dumpNote(1500, 240, 73, 0, 60)};
            CHECK(readAllNotes(s, synth, 0) == leftNotes);
            CHECK(readAllNotes(s, synth, 1) == rightNotes); // relative starts rebased; order kept
        }

        // 1m. Transpose clamps each pitch to 0..127; out-of-range requests reject.
        {
            const uint64_t beforeTranspose = rev(s);
            CHECK_REJ(s, daw_transpose_midi_clip(s, synth, 0, 128, beforeTranspose)); // beyond +/-127
            CHECK_REJ(s, daw_transpose_midi_clip(s, synth, 0, -128, beforeTranspose));
            CHECK(rev(s) == beforeTranspose);
            CHECK_OK(s, daw_transpose_midi_clip(s, synth, 0, 70, beforeTranspose)); // 60 -> 130 -> clamps to 127
            CHECK(rev(s) == beforeTranspose + 1);
            CHECK(readAllNotes(s, synth, 0)[0].pitch == 127);
            const uint64_t clampedHeld = rev(s);
            CHECK_OK(s, daw_transpose_midi_clip(s, synth, 0, 5, clampedHeld)); // 127 -> 127: nothing changed
            CHECK(rev(s) == clampedHeld);
            CHECK_OK(s, daw_transpose_midi_clip(s, synth, 0, 0, clampedHeld)); // zero semitones: silent no-op
            CHECK(rev(s) == clampedHeld);
            CHECK_OK(s, daw_transpose_midi_clip(s, synth, 0, -127, clampedHeld)); // 127 -> 0
            CHECK(readAllNotes(s, synth, 0)[0].pitch == 0);
            const uint64_t zeroHeld = rev(s);
            CHECK_OK(s, daw_transpose_midi_clip(s, synth, 0, -6, zeroHeld)); // 0 -> 0: clamped, no change
            CHECK(rev(s) == zeroHeld);
        }

        // 1n. Quantize at the default 120 BPM map: grid_beats 0.25 == 6000
        //     frames; starts snap to the NEAREST grid frame and lengths survive.
        const uint64_t grid = addTrack(s, "Grid");
        {
            daw_midi_note notes[4] = {noteAt(100, 240, 60, 0, 100), noteAt(6300, 240, 61, 0, 100),
                                      noteAt(23900, 240, 62, 0, 100), noteAt(9001, 240, 63, 0, 100)};
            CHECK_OK(s, addClip(s, grid, clipMeta(0, 48000, 0, 0, 4), notes, 4, rev(s)));
            const uint64_t beforeQuantize = rev(s);
            CHECK_REJ(s, daw_quantize_midi_clip(s, grid, 0, 0.0, beforeQuantize)); // grid must be > 0
            CHECK_REJ(s, daw_quantize_midi_clip(s, grid, 0, -0.25, beforeQuantize));
            CHECK_REJ(s, daw_quantize_midi_clip(s, grid, 0, std::nan(""), beforeQuantize));
            CHECK_REJ(s, daw_quantize_midi_clip(s, grid, 0, 64.5, beforeQuantize)); // grid capped at 64 beats
            CHECK(rev(s) == beforeQuantize);
            CHECK_OK(s, daw_quantize_midi_clip(s, grid, 0, 0.25, beforeQuantize)); // 1/16 note grid
            CHECK(rev(s) == beforeQuantize + 1);
            const auto snapped = readAllNotes(s, grid, 0);
            const std::vector<uint64_t> expected{0, 6000, 24000, 12000}; // nearest multiple of 6000 each
            CHECK(noteStarts(snapped) == expected);
            for (const auto start : noteStarts(snapped)) CHECK(start % 6000 == 0);
            CHECK(snapped[1].length == 240 && snapped[3].length == 240); // only starts move
            const uint64_t snappedHeld = rev(s);
            CHECK_OK(s, daw_quantize_midi_clip(s, grid, 0, 0.25, snappedHeld)); // already on the grid: no-op
            CHECK(rev(s) == snappedHeld);
            CHECK_OK(s, daw_quantize_midi_clip(s, grid, 0, 1.0, snappedHeld)); // beat grid re-snap
            const std::vector<uint64_t> beatGrid{0, 0, 24000, 24000};
            CHECK(noteStarts(readAllNotes(s, grid, 0)) == beatGrid);
        }

        // 1o. Inter-track transfer: copy clones the window AND notes at the
        //     chosen start (source untouched), move transfers them; each costs
        //     one revision, rejects overlaps/unknown ids with text, and undo
        //     treats the transfer as a single command.
        const uint64_t cymbals = addTrack(s, "Cymbals");
        {
            const uint64_t beforeCopy = rev(s);
            CHECK_OK(s, daw_copy_midi_clip_to_track(s, synth, 1, cymbals, 300000, beforeCopy));
            CHECK(rev(s) == beforeCopy + 1);
            CHECK(midiClipCount(s, synth) == 3 && midiClipCount(s, cymbals) == 1);
            auto clone = clipMeta(0, 0, 0, 0, 0);
            CHECK_OK(s, daw_get_midi_clip(s, cymbals, 0, &clone, 0, nullptr, 0, &written));
            CHECK(clone.start == 300000 && clone.length == 13500 && clone.note_count == 2 && clone.lane == 7 &&
                  clone.color == 0x0F1E2D); // metadata rides along; start is the caller's
            const std::vector<DumpNote> rightNotes = {dumpNote(8100, 1920, 64, 5, 90),
                                                      dumpNote(1500, 240, 73, 0, 60)};
            CHECK(readAllNotes(s, cymbals, 0) == rightNotes); // notes verbatim ...
            CHECK(readAllNotes(s, synth, 1) == rightNotes);   // ... and the source untouched
            const uint64_t held = rev(s);
            CHECK_REJ(s, daw_copy_midi_clip_to_track(s, synth, 0, cymbals, 300000, held)); // overlap on the target
            CHECK_REJ(s, daw_copy_midi_clip_to_track(s, synth, 0, synth, 48000, held));    // same-track copy overlapping the original
            CHECK_REJ(s, daw_move_midi_clip_to_track(s, synth, 0, cymbals, 300500, held)); // lands on 300000..313500
            CHECK_REJ(s, daw_copy_midi_clip_to_track(s, synth, 9, cymbals, 400000, held)); // unknown source index
            CHECK_REJ(s, daw_copy_midi_clip_to_track(s, 4242, 0, cymbals, 400000, held));  // unknown source track
            CHECK_REJ(s, daw_move_midi_clip_to_track(s, synth, 0, 4242, 400000, held));    // unknown target track
            CHECK_REJ(s, daw_move_midi_clip_to_track(s, synth, 2, cymbals, 400000, held + 1)); // stale revision
            CHECK(rev(s) == held && midiClipCount(s, synth) == 3 && midiClipCount(s, cymbals) == 1);
            CHECK_OK(s, daw_copy_midi_clip_to_track(s, synth, 0, synth, 200000, held)); // a real same-track duplicate passes
            CHECK(rev(s) == held + 1);
            CHECK(midiClipCount(s, synth) == 4);
            CHECK_OK(s, daw_remove_midi_clip(s, synth, 3, rev(s))); // tidy up again
            CHECK(midiClipCount(s, synth) == 3);
            // Move with undo: the transfer reverts as one command.
            const auto synthBefore = midiOf(s, synth);
            const auto cymbalsBefore = midiOf(s, cymbals);
            const uint64_t beforeMove = rev(s);
            CHECK_OK(s, daw_move_midi_clip_to_track(s, synth, 2, cymbals, 400000, beforeMove));
            CHECK(rev(s) == beforeMove + 1);
            CHECK(midiClipCount(s, synth) == 2 && midiClipCount(s, cymbals) == 2);
            auto moved = clipMeta(0, 0, 0, 0, 0);
            CHECK_OK(s, daw_get_midi_clip(s, cymbals, 1, &moved, 0, nullptr, 0, &written));
            CHECK(moved.start == 400000 && moved.length == 1000 && moved.note_count == 0);
            CHECK_OK(s, daw_undo(s, rev(s))); // back at the source, target released again
            CHECK(midiOf(s, synth) == synthBefore && midiOf(s, cymbals) == cymbalsBefore);
            CHECK_OK(s, daw_redo(s, rev(s))); // ... and across again with redo
            CHECK(midiClipCount(s, synth) == 2 && midiClipCount(s, cymbals) == 2);
            CHECK_OK(s, daw_undo(s, rev(s))); // leave the lane as the remove section expects
            CHECK(rev(s) == beforeMove + 4);
        }

        // 1p. Remove = one revision each way; unknown indices reject; undo
        //     restores the deleted clip with its notes intact.
        {
            const auto beforeDeletes = midiOf(s, synth);
            const uint64_t beforeRemove = rev(s);
            CHECK_OK(s, daw_remove_midi_clip(s, synth, 2, beforeRemove)); // drop the marker clip
            CHECK(rev(s) == beforeRemove + 1);
            CHECK(midiClipCount(s, synth) == 2);
            CHECK_REJ(s, daw_remove_midi_clip(s, synth, 9, rev(s))); // index past the end
            CHECK(rev(s) == beforeRemove + 1);
            CHECK_REJ(s, daw_remove_midi_clip(s, 999, 0, rev(s))); // unknown track
            CHECK_OK(s, daw_undo(s, rev(s))); // the clip and its notes come back exactly
            CHECK(rev(s) == beforeRemove + 2);
            CHECK(midiOf(s, synth) == beforeDeletes);
            CHECK_OK(s, daw_redo(s, beforeRemove + 2));
            CHECK(rev(s) == beforeRemove + 3);
            CHECK(midiClipCount(s, synth) == 2);
            CHECK_OK(s, daw_remove_midi_clip(s, synth, 0, rev(s))); // then the split halves,
            CHECK_OK(s, daw_remove_midi_clip(s, synth, 0, rev(s)));
            CHECK_OK(s, daw_remove_midi_clip(s, dense, 0, rev(s))); // the 9000-note clip,
            CHECK_OK(s, daw_remove_midi_clip(s, grid, 0, rev(s)));  // and the grid clip
            CHECK(midiClipCount(s, synth) == 0 && midiClipCount(s, dense) == 0 && midiClipCount(s, grid) == 0);
            uint32_t junk = 0;
            CHECK_REJ(s, daw_get_midi_clip_count(s, 4242, &junk)); // unknown track is semantic
            CHECK(daw_get_midi_clip_count(s, 4242, nullptr) == 1); // NULL output shape gate
        }

        // =========================================================================
        // 2. TEMPO-01 — the tempo map (docs/69).
        // =========================================================================
        Bridge tempoBridge;
        daw_session* t = tempoBridge.get();
        const TempoLane anchor{{0, 120.0}};
        CHECK(tempoLane(t) == anchor); // default anchor exists
        {
            const uint64_t held = rev(t);
            CHECK_REJ(t, daw_set_tempo(t, 48000, std::nan(""), held));  // NaN
            CHECK_REJ(t, daw_set_tempo(t, 48000, 0.0, held));           // zero
            CHECK_REJ(t, daw_set_tempo(t, 48000, 20.0, held));          // floor is exclusive
            CHECK_REJ(t, daw_set_tempo(t, 48000, 1000.0, held));        // 999 is the inclusive ceiling
            CHECK_REJ(t, daw_set_tempo(t, 48000, 1.0e300, held));       // absurdly huge
            CHECK_REJ(t, daw_set_tempo(t, (1ULL << 40), 120.0, held));  // frame limit exclusive
            CHECK(tempoLane(t) == anchor); // rejections changed nothing
            CHECK(rev(t) == held);
        }
        {
            const uint64_t base = rev(t);
            CHECK_OK(t, daw_set_tempo(t, 48000, 60.0, base)); // new point
            CHECK(rev(t) == base + 1);
            CHECK_OK(t, daw_set_tempo(t, 48000, 60.0, rev(t))); // identical upsert: silent no-op
            CHECK(rev(t) == base + 1);
            CHECK_OK(t, daw_set_tempo(t, 48000, 90.0, rev(t))); // same frame replaces (upsert)
            CHECK(rev(t) == base + 2);
            CHECK_OK(t, daw_set_tempo(t, 24000, 100.0, rev(t))); // new point between the others
            CHECK(rev(t) == base + 3);
            const TempoLane sorted{{0, 120.0}, {24000, 100.0}, {48000, 90.0}};
            CHECK(tempoLane(t) == sorted); // frame order preserved
            CHECK_OK(t, daw_remove_tempo(t, 24000, rev(t))); // removing a non-anchor works
            CHECK(rev(t) == base + 4);
            const uint64_t held = rev(t);
            CHECK_REJ(t, daw_remove_tempo(t, 0, held));              // the frame-0 anchor cannot go
            CHECK_REJ(t, daw_remove_tempo(t, 777777, held));         // unknown frame
            CHECK_REJ(t, daw_set_tempo(t, 48000, 999.0, held + 5));  // stale revision
            CHECK(rev(t) == held);
            CHECK_OK(t, daw_set_tempo(t, 96000, 999.0, held));      // boundary value 999 is legal
            const std::pair<uint64_t, double> ceiling{96000, 999.0};
            CHECK(tempoLane(t)[2] == ceiling);
            // ABI-shape gates on the enumeration: plain rc=1.
            auto undersized = abi<daw_tempo_point>();
            undersized.struct_size = sizeof(daw_tempo_point) - 1;
            CHECK(daw_get_tempo_point(t, 0, &undersized) == 1);
            CHECK(daw_get_tempo_point(t, 0, nullptr) == 1);
            CHECK(daw_get_tempo_count(t, nullptr) == 1);
            undersized.struct_size = sizeof(daw_tempo_point);
            CHECK_REJ(t, daw_get_tempo_point(t, 99, &undersized)); // index out of range, with text
            uint32_t tempoCount = 0;
            CHECK_OK(t, daw_get_tempo_count(t, &tempoCount));
            CHECK(tempoCount == 3);
            // undo restores the removed point exactly.
            const uint64_t beforeUndo = rev(t);
            CHECK_OK(t, daw_remove_tempo(t, 48000, beforeUndo));
            CHECK_OK(t, daw_undo(t, beforeUndo + 1));
            CHECK(rev(t) == beforeUndo + 2);
            const TempoLane restored{{0, 120.0}, {48000, 90.0}, {96000, 999.0}};
            CHECK(tempoLane(t) == restored);
        }

        // =========================================================================
        // 3. Time signatures. White-box truth (tempo_model_tests + docs/69): the
        //    default anchor is numerator 4, denominator 8 — a 4/8 bar of two
        //    quarter beats — even though the daw.h comment says "4/4".
        // =========================================================================
        Bridge sigBridge;
        daw_session* g = sigBridge.get();
        const MeterLane defaultMeter{{0, 4, 8}};
        CHECK(signatureLane(g) == defaultMeter);
        {
            const uint64_t held = rev(g);
            CHECK_REJ(g, daw_set_time_signature(g, 150000, 0, 4, held));  // numerator 0
            CHECK_REJ(g, daw_set_time_signature(g, 150000, 33, 4, held));  // numerator past 32
            CHECK_REJ(g, daw_set_time_signature(g, 150000, 4, 5, held));   // non-power-of-two denominator
            CHECK_REJ(g, daw_set_time_signature(g, 150000, 4, 3, held));   // denominator 3
            CHECK_REJ(g, daw_set_time_signature(g, 150000, 4, 64, held));  // denominator past 32
            CHECK_REJ(g, daw_set_time_signature(g, (1ULL << 40), 4, 8, held)); // frame limit exclusive
            CHECK(rev(g) == held);
            const uint64_t base = rev(g);
            CHECK_OK(g, daw_set_time_signature(g, 96000, 6, 16, base)); // new point
            CHECK(rev(g) == base + 1);
            CHECK_OK(g, daw_set_time_signature(g, 96000, 6, 16, rev(g))); // identical: silent no-op
            CHECK(rev(g) == base + 1);
            CHECK_OK(g, daw_set_time_signature(g, 96000, 3, 4, rev(g))); // same frame replaces
            CHECK(rev(g) == base + 2);
            CHECK_OK(g, daw_set_time_signature(g, 48000, 7, 8, rev(g))); // inserted BEFORE the 96000 point
            const MeterLane meters{{0, 4, 8}, {48000, 7, 8}, {96000, 3, 4}};
            CHECK(signatureLane(g) == meters); // ordering preserved
            CHECK_OK(g, daw_remove_time_signature(g, 48000, rev(g))); // non-anchor removal works
            CHECK(rev(g) == base + 4);
            const uint64_t held2 = rev(g);
            CHECK_REJ(g, daw_remove_time_signature(g, 0, held2));       // frame-0 anchor cannot go
            CHECK_REJ(g, daw_remove_time_signature(g, 777777, held2));  // unknown frame
            CHECK(rev(g) == held2);
            CHECK_OK(g, daw_set_time_signature(g, 0, 1, 1, held2)); // the anchor may be retuned; 1/1 is legal
            CHECK(rev(g) == held2 + 1);
            const std::tuple<uint64_t, uint32_t, uint32_t> retuned{0, 1, 1};
            CHECK(signatureLane(g)[0] == retuned);
            auto undersized = abi<daw_time_signature_point>();
            undersized.struct_size = sizeof(daw_time_signature_point) + 1;
            CHECK(daw_get_time_signature_point(g, 0, &undersized) == 1); // ABI shape gate
            uint32_t sigCount = 0;
            CHECK_OK(g, daw_get_time_signature_count(g, &sigCount));
            CHECK(sigCount == 2);
        }

        // =========================================================================
        // 4. MARKER-01 — locator lane (docs/74).
        // =========================================================================
        Bridge markerBridge;
        daw_session* m = markerBridge.get();
        CHECK(markerLane(m).empty()); // a fresh project carries no locators
        {
            const uint64_t base = rev(m);
            CHECK_OK(m, daw_add_marker(m, 96000, "Chorus", base)); // rev+1
            CHECK(rev(m) == base + 1);
            CHECK_OK(m, daw_add_marker(m, 0, "Start", rev(m))); // front insert stays legal
            CHECK(rev(m) == base + 2);
            CHECK_OK(m, daw_add_marker(m, 48000, "Verse", rev(m))); // middle insert
            CHECK(rev(m) == base + 3);
            const MarkerL locators{{0, "Start"}, {48000, "Verse"}, {96000, "Chorus"}};
            CHECK(markerLane(m) == locators); // frame-ordered enumeration
            CHECK_OK(m, daw_rename_marker(m, 48000, "Bridge", rev(m))); // rename = one revision
            CHECK(rev(m) == base + 4);
            const uint64_t held = rev(m);
            CHECK_OK(m, daw_rename_marker(m, 48000, "Bridge", held)); // identical rename: silent no-op
            CHECK(rev(m) == held);
            CHECK_REJ(m, daw_add_marker(m, 48000, "Dup", held));       // duplicate frame rejects
            CHECK_REJ(m, daw_add_marker(m, 96000, "Dup", held));       // against a stored frame
            CHECK_REJ(m, daw_remove_marker(m, 12345, held));           // remove missing rejects
            CHECK_REJ(m, daw_rename_marker(m, 12345, "Ghost", held));  // rename missing rejects
            CHECK_REJ(m, daw_add_marker(m, 7, "", held));              // empty name
            CHECK_REJ(m, daw_add_marker(m, 7, std::string(121, 'A').c_str(), held)); // one over the 120-codepoint cap
            CHECK_REJ(m, daw_add_marker(m, 7, "\xFF\xFE", held));            // invalid UTF-8 lead byte
            CHECK_REJ(m, daw_add_marker(m, 7, "\xC2", held));                 // truncated sequence
            CHECK_REJ(m, daw_add_marker(m, 7, "\xED\xA0\x80", held));       // UTF-16 surrogate
            CHECK_REJ(m, daw_add_marker(m, 7, "Bad\x01", held));               // control character
            CHECK_REJ(m, daw_add_marker(m, (1ULL << 40), "Edge", held));        // frame limit exclusive
            CHECK(daw_add_marker(m, 7, nullptr, held) == 1);                    // NULL name: shape gate
            CHECK(rev(m) == held); // none of that consumed a revision
            CHECK_OK(m, daw_add_marker(m, 7, std::string(120, 'A').c_str(), held)); // exactly 120 is legal
            CHECK_OK(m, daw_add_marker(m, 240000, "Take \xF0\x9F\x8E\xB5", rev(m))); // emoji passes strict UTF-8
            const std::pair<uint64_t, std::string> take{240000, std::string("Take \xF0\x9F\x8E\xB5")};
            CHECK(markerLane(m)[4] == take);
            CHECK_OK(m, daw_rename_marker(m, 0, "Intro \xF0\x9F\x8E\xB5", rev(m))); // rename to emoji too
            CHECK(markerLane(m)[0].second == "Intro \xF0\x9F\x8E\xB5");
            // First-marker semantics: frame 0 is an ordinary marker (no anchor),
            // unlike the tempo map — so it removes fine.
            const uint64_t beforeRemove = rev(m);
            CHECK_OK(m, daw_remove_marker(m, 0, beforeRemove));
            CHECK(rev(m) == beforeRemove + 1);
            CHECK(markerLane(m).front().first == 7); // the lane shifted but stayed sorted
            // undo brings it right back.
            CHECK_OK(m, daw_undo(m, beforeRemove + 1));
            const std::pair<uint64_t, std::string> intro{0, std::string("Intro \xF0\x9F\x8E\xB5")};
            CHECK(markerLane(m)[0] == intro);
            auto tiny = abi<daw_marker>();
            tiny.struct_size = sizeof(daw_marker) - 4;
            CHECK(daw_get_marker(m, 0, &tiny) == 1); // ABI shape gate
            uint32_t markerCount = 0;
            CHECK_OK(m, daw_get_marker_count(m, &markerCount));
            CHECK(markerCount == 5);
        }

        // =========================================================================
        // 5. Capacity boundaries: 64 tempo points, 64 signatures, 256 markers.
        // =========================================================================
        Bridge capBridge;
        daw_session* c = capBridge.get();
        {
            for (uint64_t index = 1; index < 64; ++index)
                CHECK_OK(c, daw_set_tempo(c, index * 1000, 30.0 + static_cast<double>(index), rev(c)));
            CHECK(tempoLane(c).size() == 64); // exactly the cap: 63 new + the anchor
            const uint64_t held = rev(c);
            CHECK_REJ(c, daw_set_tempo(c, 64000, 190.0, held)); // one over is refused atomically
            CHECK(rev(c) == held);
            for (uint64_t index = 1; index < 64; ++index)
                CHECK_OK(c, daw_set_time_signature(c, index * 1000, 3, 8, rev(c)));
            CHECK(signatureLane(c).size() == 64);
            const uint64_t sigHeld = rev(c);
            CHECK_REJ(c, daw_set_time_signature(c, 64000, 5, 8, sigHeld));
            CHECK(rev(c) == sigHeld);
            for (uint64_t index = 0; index < 256; ++index)
                CHECK_OK(c, daw_add_marker(c, 1000000 + index, ("M" + std::to_string(index)).c_str(), rev(c)));
            CHECK(markerLane(c).size() == 256); // exactly the documented 256 cap (docs/74)
            const uint64_t full = rev(c);
            CHECK_REJ(c, daw_add_marker(c, 2000000, "One too many", full)); // the 257th refuses
            CHECK(rev(c) == full);
            CHECK_OK(c, daw_remove_marker(c, 1000255, rev(c))); // a freed slot refits the cap
            CHECK_OK(c, daw_add_marker(c, 2000000, "Fits again", rev(c)));
            CHECK(markerLane(c).size() == 256);
        }

        // =========================================================================
        // 6. Persistence: one project carrying MIDI + tempo + signatures +
        //    markers saves, reopens in a fresh session and dumps back equal.
        // =========================================================================
        Bridge lifeBridge;
        daw_session* p = lifeBridge.get();
        {
            const uint64_t keys = addTrack(p, "Keys");
            daw_midi_note notes[2] = {noteAt(0, 480, 60, 0, 100), noteAt(4800, 960, 62, 5, 90)};
            CHECK_OK(p, addClip(p, keys, clipMeta(0, 48000, 3, 0x0A0B0C, 2), notes, 2, rev(p)));
            daw_midi_note tail = noteAt(0, 480, 72, 0, 50);
            CHECK_OK(p, addClip(p, keys, clipMeta(96000, 48000, 1, 0, 1), &tail, 1, rev(p)));
            const uint64_t brass = addTrack(p, "Brass");
            daw_midi_note longNote = noteAt(100, 479900, 64, 1, 127); // ends flush with the clip window (480000)
            CHECK_OK(p, addClip(p, brass, clipMeta(480000, 480000, 0, 0xFF00AA, 1), &longNote, 1, rev(p)));
            CHECK_OK(p, daw_set_tempo(p, 48000, 60.0, rev(p)));
            CHECK_OK(p, daw_set_tempo(p, 96000, 150.0, rev(p)));
            CHECK_OK(p, daw_set_tempo(p, 240000, 999.0, rev(p)));
            CHECK_OK(p, daw_set_time_signature(p, 96000, 6, 16, rev(p)));
            CHECK_OK(p, daw_set_time_signature(p, 192000, 3, 4, rev(p)));
            CHECK_OK(p, daw_add_marker(p, 0, "Intro \xF0\x9F\x8E\xB5", rev(p)));
            CHECK_OK(p, daw_add_marker(p, 48000, "\xCE\x97\xCF\x87\xCE\xB1\xCE\xBF\xCF\x85\xCF\x83", rev(p))); // Greek, multi-byte
            CHECK_OK(p, daw_add_marker(p, 1440000, std::string(120, 'A').c_str(), rev(p)));
        }
        const auto built = dumpOf(p);
        CHECK(built.tracks.size() == 2 && built.tracks[0].midi.size() == 2 && built.tracks[1].midi.size() == 1);
        CHECK(built.tempo.size() == 4 && built.signatures.size() == 3 && built.markers.size() == 3);
        const auto draft = root / "miditempo.mydawdraft";
        saveDraftAndWait(p, draft);
        CHECK(fileNonEmpty(draft));
        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), draft.string().c_str()));
        const auto restoredDump = dumpOf(reopened.get());
        if (!(restoredDump == built))
            throw std::runtime_error("MIDI/tempo/marker round-trip changed the project:\nBEFORE:\n" + describe(built) +
                                     "\nAFTER:\n" + describe(restoredDump));
        // The reopened clip reads back note-perfect through the paging protocol.
        const std::vector<DumpNote> keysHead = {dumpNote(0, 480, 60, 0, 100), dumpNote(4800, 960, 62, 5, 90)};
        CHECK(readAllNotes(reopened.get(), built.tracks[0].id, 0) == keysHead);

        std::cout << "PASS: e2e_midi_tempo — MIDI clip ABI lifecycle (paged notes, atomic batches, trim/split/quantize/"
                     "transpose, undo), tempo anchor rules, meter rules, marker name/capacity rules, combined save/reopen dumpOf\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}