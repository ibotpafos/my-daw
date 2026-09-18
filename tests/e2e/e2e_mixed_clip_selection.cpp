#include "e2e.hpp"
#include <limits>

using namespace e2e;
namespace {
daw_clip_selection_ref ref(uint64_t track, uint32_t index, bool midi) {
    auto value = abi<daw_clip_selection_ref>();
    value.version = DAW_CLIP_SELECTION_REF_VERSION;
    value.track_id = track; value.clip_index = index; value.kind = midi ? 2 : 1;
    return value;
}
daw_clip audio(daw_session *s, uint64_t track, uint32_t index = 0) {
    auto value = abi<daw_clip>();
    CHECK_OK(s, daw_get_clip(s, track, index, &value));
    return value;
}
daw_midi_clip midi(daw_session *s, uint64_t track, uint32_t index = 0) {
    auto value = abi<daw_midi_clip>();
    CHECK_OK(s, daw_get_midi_clip(s, track, index, &value, 0, nullptr, 0, nullptr));
    return value;
}
uint32_t midiCount(daw_session *s, uint64_t track) {
    uint32_t count = 0;
    CHECK_OK(s, daw_get_midi_clip_count(s, track, &count));
    return count;
}
void addMidi(daw_session *s, uint64_t track, uint64_t start) {
    auto clip = abi<daw_midi_clip>();
    clip.version = DAW_MIDI_CLIP_VERSION; clip.start = start; clip.length = 12000;
    clip.color = 0x8877AA; clip.lane = 4; clip.note_count = 1;
    auto note = abi<daw_midi_note>();
    note.version = DAW_MIDI_NOTE_VERSION; note.start = 3000; note.length = 6000;
    note.channel = 3; note.pitch = 67; note.velocity = 91;
    CHECK_OK(s, daw_add_midi_clip(s, track, &clip, &note, 1, rev(s)));
}
int edit(daw_session *s, const std::vector<daw_clip_selection_ref> &selection,
         uint32_t action, int64_t delta = 0, int32_t trackOffset = 0) {
    return daw_edit_clip_selection(s, selection.data(), static_cast<uint32_t>(selection.size()),
                                    action, delta, trackOffset, rev(s));
}
}
int main() {
    try {
        TempRoot root("mixed-clip-selection");
        writeWavFixture(root / "voice.wav", constantInterleaved(48000, 2, .1f, .05f), 48000, 2, "f32");
        Bridge session; auto *s = session.get();
        CHECK_OK(s, daw_import_wav(s, (root / "voice.wav").c_str(), "Audio", rev(s)));
        const auto midiSource = addTrack(s, "MIDI"), audioTarget = addTrack(s, "Audio destination"),
                   midiTarget = addTrack(s, "MIDI destination");
        CHECK_OK(s, daw_edit_clip_full(s, 1, 0, 12000, 1000, 12000, 64, 96, rev(s)));
        CHECK_OK(s, daw_set_clip_gain(s, 1, 0, -6, rev(s)));
        CHECK_OK(s, daw_set_clip_pan(s, 1, 0, .5, rev(s)));
        CHECK_OK(s, daw_set_clip_color(s, 1, 0, 0xCC88AA, rev(s)));
        addMidi(s, midiSource, 24000); addMidi(s, midiSource, 60000);
        const auto bus = addBus(s, "Destination routing");
        CHECK_OK(s, daw_set_track_output(s, audioTarget, bus, rev(s)));
        std::vector<daw_clip_selection_ref> original{ref(midiSource, 1, true), ref(1, 0, false), ref(midiSource, 0, true)};
        const auto originalAudio = audio(s, 1);
        const uint32_t one = 0;
        CHECK_OK(s, daw_capture_clipboard(s, 1, 0, &one, 1, 0, rev(s)));
        const auto before = rev(s);
        CHECK_OK(s, edit(s, original, DAW_CLIP_SELECTION_MOVE, 24000, 2));
        CHECK(rev(s) == before + 1 && trackById(s, 1).clip_count == 0 && midiCount(s, midiSource) == 0);
        CHECK(trackById(s, 1).audio_frames == 48000); // last clip removed, source still held
        const auto moved = audio(s, audioTarget);
        CHECK(moved.start == 36000 && moved.length == originalAudio.length && moved.source_offset == 1000);
        CHECK(moved.fade_in == 64 && moved.fade_out == 96 && moved.gain_db == -6 && moved.pan == .5 && moved.color == 0xCC88AA);
        CHECK(trackById(s, audioTarget).output_bus_id == bus); // route belongs to the target
        CHECK(midiCount(s, midiTarget) == 2 && midi(s, midiTarget).start == 48000 && midi(s, midiTarget, 1).start == 84000);
        auto clip = abi<daw_midi_clip>(); auto note = abi<daw_midi_note>(); uint32_t written = 0;
        CHECK_OK(s, daw_get_midi_clip(s, midiTarget, 0, &clip, 0, &note, 1, &written));
        CHECK(written == 1 && note.start == 3000 && note.length == 6000 && note.pitch == 67 &&
              note.channel == 3 && note.velocity == 91 && clip.lane == 4 && clip.color == 0x8877AA);
        auto board = abi<daw_clipboard_info>();
        CHECK_OK(s, daw_get_clipboard(s, &board)); CHECK(board.clip_count == 1 && board.kind == 1);
        auto wav = exportProject(s, root / "moved.wav");
        CHECK(channelPeak(wav, 12000, 24000, 0) == 0);
        CHECK(std::abs(channelRms(wav, 37000, 46000, 0) - .05 * std::pow(10., -6. / 20.)) < 1e-6);
        CHECK_OK(s, daw_undo(s, rev(s)));
        CHECK(audio(s, 1).start == 12000 && midiCount(s, midiSource) == 2 && trackById(s, audioTarget).audio_frames == 0);
        CHECK_OK(s, daw_redo(s, rev(s)));
        CHECK(audio(s, audioTarget).start == 36000 && midiCount(s, midiTarget) == 2);
        saveDraftAndWait(s, root / "mixed.daw");
        Bridge reloaded; CHECK_OK(reloaded.get(), daw_open_draft(reloaded.get(), (root / "mixed.daw").c_str()));
        CHECK(trackById(reloaded.get(), 1).clip_count == 0 && midiCount(reloaded.get(), midiTarget) == 2);
        auto reopened = exportProject(reloaded.get(), root / "reopened.wav");
        CHECK(wav.samples == reopened.samples); // independent WAV reader, full rendered output

        // Zero-motion gesture is truly read-only, but still validates refs/revision.
        std::vector<daw_clip_selection_ref> group{ref(audioTarget, 0, false), ref(midiTarget, 1, true), ref(midiTarget, 0, true)};
        auto revision = rev(s);
        CHECK_OK(s, edit(s, group, DAW_CLIP_SELECTION_MOVE)); CHECK(rev(s) == revision);
        CHECK_REJ(s, daw_edit_clip_selection(s, group.data(), 3, DAW_CLIP_SELECTION_MOVE, 0, 0, revision - 1));
        CHECK_REJ(s, edit(s, group, DAW_CLIP_SELECTION_MOVE, std::numeric_limits<int64_t>::min()));
        CHECK_REJ(s, edit(s, group, DAW_CLIP_SELECTION_MOVE, std::numeric_limits<int64_t>::max()));
        CHECK_REJ(s, edit(s, group, DAW_CLIP_SELECTION_MOVE, 0, std::numeric_limits<int32_t>::max()));
        CHECK_REJ(s, edit(s, group, DAW_CLIP_SELECTION_MOVE, 0, -100));
        CHECK_REJ(s, edit(s, group, DAW_CLIP_SELECTION_DELETE, 1));
        CHECK_REJ(s, edit(s, group, 999));
        CHECK_REJ(s, daw_edit_clip_selection(s, nullptr, 1, DAW_CLIP_SELECTION_MOVE, 0, 0, revision));
        CHECK_REJ(s, daw_edit_clip_selection(s, group.data(), UINT32_MAX, DAW_CLIP_SELECTION_MOVE, 0, 0, revision));
        CHECK(daw_edit_clip_selection(nullptr, group.data(), 3, DAW_CLIP_SELECTION_MOVE, 0, 0, revision) != 0);
        auto bad = group; bad.push_back(bad.front()); CHECK_REJ(s, edit(s, bad, DAW_CLIP_SELECTION_DELETE));
        bad = group; bad.back().kind = 42; CHECK_REJ(s, edit(s, bad, DAW_CLIP_SELECTION_MOVE));
        bad = group; bad.back().struct_size--; CHECK_REJ(s, edit(s, bad, DAW_CLIP_SELECTION_DELETE));
        bad = group; bad.back().version++; CHECK_REJ(s, edit(s, bad, DAW_CLIP_SELECTION_DELETE));
        bad = group; bad.back().clip_index = UINT32_MAX; CHECK_REJ(s, edit(s, bad, DAW_CLIP_SELECTION_DELETE));
        bad = group; bad.back().track_id = UINT64_MAX; CHECK_REJ(s, edit(s, bad, DAW_CLIP_SELECTION_DELETE));
        CHECK(rev(s) == revision && audio(s, audioTarget).start == 36000 && midiCount(s, midiTarget) == 2);

        CHECK_OK(s, daw_begin_mixer_gesture(s, DAW_MIXER_TRACK_GAIN, audioTarget, 0, rev(s)));
        CHECK_REJ(s, edit(s, group, DAW_CLIP_SELECTION_DELETE)); CHECK(rev(s) == revision);
        daw_cancel_mixer_gesture(s);
        // Entire mixed group duplicates after its span, retaining its gaps.
        CHECK_OK(s, edit(s, group, DAW_CLIP_SELECTION_COPY, 60000));
        CHECK(rev(s) == revision + 1 && audio(s, audioTarget, 1).start == 96000 && midiCount(s, midiTarget) == 4);
        CHECK(midi(s, midiTarget, 2).start == 108000 && midi(s, midiTarget, 3).start == 144000);
        CHECK_OK(s, daw_undo(s, rev(s))); CHECK(midiCount(s, midiTarget) == 2 && trackById(s, audioTarget).clip_count == 1);
        CHECK_OK(s, daw_redo(s, rev(s))); CHECK(midiCount(s, midiTarget) == 4);
        // A later target rejects the edit after the candidate audio copy exists.
        // No partial audio, sources, clipboard changes or Undo entry may escape.
        const auto emptyA = addTrack(s, "Rollback audio target"), emptyM = addTrack(s, "Rollback MIDI target");
        addMidi(s, emptyM, 48000);
        revision = rev(s);
        CHECK_REJ(s, edit(s, group, DAW_CLIP_SELECTION_COPY, 0, 2));
        CHECK(rev(s) == revision && trackById(s, emptyA).audio_frames == 0 && trackById(s, emptyA).clip_count == 0);
        CHECK(midiCount(s, emptyM) == 1 && trackById(s, audioTarget).clip_count == 2);
        CHECK_OK(s, daw_get_clipboard(s, &board)); CHECK(board.clip_count == 1 && board.kind == 1);
        // Earlier candidate audio is also discarded if a MIDI destination is audio-typed.
        CHECK_REJ(s, edit(s, group, DAW_CLIP_SELECTION_MOVE, 24000, -1)); CHECK(rev(s) == revision);
        std::vector<daw_clip_selection_ref> all{ref(audioTarget, 0, false), ref(audioTarget, 1, false),
            ref(midiTarget, 0, true), ref(midiTarget, 1, true), ref(midiTarget, 2, true), ref(midiTarget, 3, true)};
        CHECK_OK(s, edit(s, all, DAW_CLIP_SELECTION_DELETE));
        CHECK(rev(s) == revision + 1 && trackById(s, audioTarget).clip_count == 0 && midiCount(s, midiTarget) == 0);
        CHECK_OK(s, daw_undo(s, rev(s))); CHECK(trackById(s, audioTarget).clip_count == 2 && midiCount(s, midiTarget) == 4);
        CHECK_OK(s, daw_get_clipboard(s, &board)); CHECK(board.clip_count == 1);

        // Source/target chains: source B must be read before A moves onto B.
        Bridge chain; auto *c = chain.get();
        const auto a = addTrack(c, "A"), b = addTrack(c, "B"), d = addTrack(c, "C");
        addMidi(c, a, 0); addMidi(c, b, 24000);
        std::vector<daw_clip_selection_ref> links{ref(a, 0, true), ref(b, 0, true)};
        revision = rev(c); CHECK_OK(c, edit(c, links, DAW_CLIP_SELECTION_MOVE, 12000, 1));
        CHECK(rev(c) == revision + 1 && midiCount(c, a) == 0 && midi(c, b).start == 12000 && midi(c, d).start == 36000);
        CHECK_OK(c, daw_undo(c, rev(c))); CHECK(midi(c, a).start == 0 && midi(c, b).start == 24000 && midiCount(c, d) == 0);
        std::cout << "PASS: mixed audio/MIDI move/copy/delete, atomic rollback, ABI guards, Undo/Redo, WAV and persistence\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
