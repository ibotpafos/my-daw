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
daw_clipboard_info board(daw_session *s) {
    auto value = abi<daw_clipboard_info>(); CHECK_OK(s, daw_get_clipboard(s, &value)); return value;
}
daw_clip audio(daw_session *s, uint64_t track, uint32_t index = 0) {
    auto value = abi<daw_clip>(); CHECK_OK(s, daw_get_clip(s, track, index, &value)); return value;
}
daw_midi_clip midi(daw_session *s, uint64_t track, uint32_t index = 0) {
    auto value = abi<daw_midi_clip>();
    CHECK_OK(s, daw_get_midi_clip(s, track, index, &value, 0, nullptr, 0, nullptr)); return value;
}
uint32_t midiCount(daw_session *s, uint64_t track) {
    uint32_t count = 0; CHECK_OK(s, daw_get_midi_clip_count(s, track, &count)); return count;
}
void addMidi(daw_session *s, uint64_t track, uint64_t start, uint64_t length = 24000) {
    auto clip = abi<daw_midi_clip>(); auto note = abi<daw_midi_note>();
    clip.version = DAW_MIDI_CLIP_VERSION; clip.start = start; clip.length = length;
    clip.lane = 4; clip.color = 0x9977AA; clip.note_count = 1;
    note.version = DAW_MIDI_NOTE_VERSION; note.start = 1; note.length = 2;
    note.pitch = 67; note.channel = 3; note.velocity = 91;
    CHECK_OK(s, daw_add_midi_clip(s, track, &clip, &note, 1, rev(s)));
}
int capture(daw_session *s, const std::vector<daw_clip_selection_ref> &refs, bool cut = false) {
    return daw_capture_selection_clipboard(s, refs.data(), static_cast<uint32_t>(refs.size()), cut ? 1 : 0, rev(s));
}
bool sameContent(Dump a, const Dump &b) { a.revision = b.revision; return a == b; }
}
int main() {
    try {
        TempRoot root("mixed-clipboard");
        writeWavFixture(root / "voice.wav", constantInterleaved(48000, 2, .1f, .05f), 48000, 2, "f32");
        Bridge session; auto *s = session.get();
        CHECK_OK(s, daw_import_wav(s, (root / "voice.wav").c_str(), "Original audio", rev(s)));
        const auto sourceGap = addTrack(s, "Unselected source gap"), sourceM = addTrack(s, "Original MIDI");
        const auto targetA = addTrack(s, "Destination audio"), targetGap = addTrack(s, "Destination gap"), targetM = addTrack(s, "Destination MIDI");
        CHECK_OK(s, daw_edit_clip_full(s, 1, 0, 40000, 1000, 10001, 64, 96, rev(s)));
        CHECK_OK(s, daw_set_clip_gain(s, 1, 0, -6, rev(s)));
        CHECK_OK(s, daw_set_clip_pan(s, 1, 0, .5, rev(s)));
        CHECK_OK(s, daw_set_clip_color(s, 1, 0, 0xCC88AA, rev(s)));
        CHECK_OK(s, daw_copy_clip_to_track(s, 1, 0, 1, 80000, rev(s)));
        addMidi(s, sourceM, 12000);
        const auto bus = addBus(s, "Keep target routing");
        CHECK_OK(s, daw_set_track_output(s, targetA, bus, rev(s)));
        // Capture in a different order from time and rows; normalization is independent.
        std::vector<daw_clip_selection_ref> group{ref(sourceM, 0, true), ref(1, 1, false), ref(1, 0, false)};
        const auto beforeCopy = dumpOf(s);
        CHECK_OK(s, capture(s, group));
        CHECK(dumpOf(s) == beforeCopy && board(s).kind == 3 && board(s).clip_count == 3 && board(s).length == 78001);
        CHECK(board(s).reserved == 0);
        // Copies own content, not indices or current track order.
        CHECK_OK(s, daw_set_clip_muted(s, 1, 0, 1, rev(s)));
        CHECK_OK(s, daw_transpose_midi_clip(s, sourceM, 0, 12, rev(s)));
        CHECK_OK(s, daw_move_track(s, sourceM, 0, rev(s)));
        CHECK_OK(s, daw_remove_track(s, sourceM, rev(s)));
        CHECK_OK(s, daw_remove_track(s, 1, rev(s)));
        auto revision = rev(s);
        CHECK_OK(s, daw_paste_clipboard(s, targetA, 100000, revision));
        CHECK(rev(s) == revision + 1 && audio(s, targetA).start == 128000 && audio(s, targetA, 1).start == 168000);
        CHECK(midi(s, targetM).start == 100000 && midi(s, targetM).lane == 4 && midi(s, targetM).color == 0x9977AA);
        const auto pasted = audio(s, targetA);
        CHECK(pasted.source_offset == 1000 && pasted.length == 10001 && pasted.fade_in == 64 && pasted.fade_out == 96);
        CHECK(pasted.gain_db == -6 && pasted.pan == .5 && pasted.color == 0xCC88AA && pasted.muted == 0);
        CHECK(trackById(s, targetA).output_bus_id == bus && trackById(s, targetGap).clip_count == 0 && midiCount(s, targetGap) == 0);
        CHECK(trackById(s, sourceGap).clip_count == 0);
        auto clip = abi<daw_midi_clip>(); auto note = abi<daw_midi_note>(); uint32_t written = 0;
        CHECK_OK(s, daw_get_midi_clip(s, targetM, 0, &clip, 0, &note, 1, &written));
        CHECK(written == 1 && note.pitch == 67 && note.velocity == 91 && note.channel == 3);
        const auto firstPaste = dumpOf(s);
        CHECK_OK(s, daw_paste_clipboard(s, targetA, 178001, rev(s)));
        CHECK(audio(s, targetA, 2).start == 206001 && midi(s, targetM, 1).start == 178001);
        CHECK(board(s).kind == 3 && board(s).length == 78001);
        CHECK_OK(s, daw_undo(s, rev(s))); CHECK(sameContent(dumpOf(s), firstPaste));
        CHECK_OK(s, daw_redo(s, rev(s))); CHECK(trackById(s, targetA).clip_count == 4 && midiCount(s, targetM) == 2);
        auto before = dumpOf(s); revision = rev(s);
        CHECK_REJ(s, daw_paste_clipboard(s, targetM, 300000, revision)); // too few rows
        CHECK_REJ(s, daw_paste_clipboard(s, UINT64_MAX, 300000, revision));
        CHECK_REJ(s, daw_paste_clipboard(s, targetA, UINT64_MAX, revision));
        CHECK_REJ(s, daw_paste_clipboard(s, targetA, 300000, revision - 1));
        CHECK(dumpOf(s) == before && board(s).clip_count == 3);
        // Late MIDI overlap: candidate audio and its source attachment must be discarded.
        const auto lateA = addTrack(s, "Late rollback audio"), lateGap = addTrack(s, "Late rollback gap"), lateM = addTrack(s, "Late rollback MIDI");
        addMidi(s, lateM, 400000);
        before = dumpOf(s);
        CHECK_REJ(s, daw_paste_clipboard(s, lateA, 400000, rev(s)));
        CHECK(dumpOf(s) == before && trackById(s, lateA).audio_frames == 0 && board(s).kind == 3);
        CHECK(trackById(s, lateGap).clip_count == 0);
        // Late incompatible type is rejected, even when the early audio target is free.
        // Legacy transfer can create a hybrid row; do not change that contract.
        CHECK_OK(s, daw_copy_clip_to_track(s, targetA, 0, lateM, 500000, rev(s)));
        before = dumpOf(s);
        CHECK_REJ(s, daw_paste_clipboard(s, lateA, 700000, rev(s)));
        CHECK(dumpOf(s) == before && trackById(s, lateA).audio_frames == 0);
        CHECK_REJ(s, capture(s, {ref(lateM, 0, false), ref(lateM, 0, true)}, true));
        CHECK(dumpOf(s) == before && board(s).kind == 3);
        // Both capture and Cut validate ALL references before replacing the board.
        group = {ref(targetM, 0, true), ref(targetA, 1, false), ref(targetA, 0, false)};
        before = dumpOf(s); revision = rev(s);
        CHECK_REJ(s, daw_capture_selection_clipboard(s, nullptr, 3, 1, revision));
        CHECK_REJ(s, daw_capture_selection_clipboard(s, group.data(), UINT32_MAX, 1, revision));
        CHECK_REJ(s, daw_capture_selection_clipboard(s, group.data(), 0, 0, revision));
        CHECK_REJ(s, daw_capture_selection_clipboard(s, group.data(), 3, 2, revision));
        CHECK_REJ(s, daw_capture_selection_clipboard(s, group.data(), 3, 1, revision - 1));
        CHECK(daw_capture_selection_clipboard(nullptr, group.data(), 3, 0, revision) != 0);
        for (int fault = 0; fault < 6; ++fault) {
            auto bad = group;
            if (fault == 0) bad.push_back(bad.front());
            if (fault == 1) bad.back().version++;
            if (fault == 2) bad.back().struct_size--;
            if (fault == 3) bad.back().kind = 9;
            if (fault == 4) bad.back().clip_index = UINT32_MAX;
            if (fault == 5) bad.back().track_id = UINT64_MAX;
            CHECK_REJ(s, capture(s, bad, true));
            CHECK(dumpOf(s) == before && board(s).kind == 3 && board(s).clip_count == 3 && board(s).length == 78001);
        }
        CHECK_OK(s, capture(s, group, true));
        CHECK(rev(s) == revision + 1 && trackById(s, targetA).clip_count == 2 && midiCount(s, targetM) == 1);
        CHECK_OK(s, daw_undo(s, rev(s))); CHECK(sameContent(dumpOf(s), before) && board(s).kind == 3);
        CHECK_OK(s, daw_redo(s, rev(s))); CHECK(trackById(s, targetA).clip_count == 2 && board(s).clip_count == 3);
        CHECK_OK(s, daw_paste_clipboard(s, targetA, 600000, rev(s)));
        CHECK(audio(s, targetA, 2).start == 628000 && midi(s, targetM, 1).start == 600000);
        // Real stereo render and persistence, not just metadata assertions.
        const auto wav = exportProject(s, root / "before-save.wav");
        CHECK(channelPeak(wav, 601000, 620000, 0) == 0);
        CHECK(std::abs(channelRms(wav, 629000, 637000, 0) - .05 * std::pow(10., -6. / 20.)) < 1e-6);
        saveDraftAndWait(s, root / "clipboard.daw");
        Bridge reopened; CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), (root / "clipboard.daw").c_str()));
        CHECK(exportProject(reopened.get(), root / "after-open.wav").samples == wav.samples);
        CHECK_REJ(s, daw_open_draft(s, (root / "missing.daw").c_str())); CHECK(board(s).kind == 3);
        CHECK_OK(s, daw_open_draft(s, (root / "clipboard.daw").c_str())); CHECK(board(s).clip_count == 0);
        // Old and new capture APIs share ONE document clipboard slot.
        const uint32_t index = 0;
        CHECK_OK(s, daw_capture_clipboard(s, targetM, 1, &index, 1, 0, rev(s)));
        CHECK(board(s).kind == 2 && board(s).clip_count == 1);
        CHECK_OK(s, daw_clear_clipboard(s)); CHECK(board(s).kind == 0);

        // Mixed capacity failure on the MIDI row cannot leave an extra audio
        // insertion in the first row, nor consume an Undo entry.
        {
            Bridge capacity; auto *c = capacity.get();
            CHECK_OK(c, daw_import_wav(c, (root / "voice.wav").c_str(), "Short audio", rev(c)));
            CHECK_OK(c, daw_edit_clip_full(c, 1, 0, 0, 0, 8, 0, 0, rev(c)));
            const auto cm = addTrack(c, "Short MIDI"), ca = addTrack(c, "Capacity audio"), ct = addTrack(c, "Capacity MIDI");
            addMidi(c, cm, 0, 8);
            CHECK_OK(c, capture(c, {ref(1, 0, false), ref(cm, 0, true)}));
            for (uint64_t i = 0; i < 64; ++i) CHECK_OK(c, daw_paste_clipboard(c, ca, 16 * i, rev(c)));
            const auto full = dumpOf(c);
            CHECK_REJ(c, daw_paste_clipboard(c, ca, 1024, rev(c)));
            CHECK(dumpOf(c) == full && board(c).kind == 3 && board(c).clip_count == 2);
            CHECK_OK(c, daw_undo(c, rev(c)));
            CHECK(trackById(c, ca).clip_count == 63 && midiCount(c, ct) == 63);
        }
        // Exact selection ceiling: one immutable 256-region, 16-row snapshot.
        Bridge many; auto *m = many.get(); std::vector<daw_clip_selection_ref> refs;
        std::vector<uint64_t> tracks;
        for (uint32_t row = 0; row < 16; ++row) {
            const auto id = addTrack(m, "Row " + std::to_string(row)); tracks.push_back(id);
            for (uint32_t i = 0; i < 16; ++i) { addMidi(m, id, 16 * i, 8); refs.push_back(ref(id, i, true)); }
        }
        const auto content = dumpOf(m); revision = rev(m);
        CHECK_OK(m, capture(m, refs, true));
        CHECK(rev(m) == revision + 1 && board(m).kind == 2 && board(m).clip_count == 256);
        for (const auto id : tracks) CHECK(midiCount(m, id) == 0);
        CHECK_OK(m, daw_paste_clipboard(m, tracks.front(), 0, rev(m)));
        CHECK(sameContent(dumpOf(m), content));
        for (uint64_t start : {256, 512, 768}) CHECK_OK(m, daw_paste_clipboard(m, tracks.front(), start, rev(m)));
        before = dumpOf(m);
        CHECK_REJ(m, daw_paste_clipboard(m, tracks.front(), 1024, rev(m))); // 64 MIDI regions per target
        CHECK(dumpOf(m) == before && board(m).clip_count == 256);
        CHECK_OK(m, daw_undo(m, rev(m))); // rejection consumed no history entry
        for (const auto id : tracks) CHECK(midiCount(m, id) == 48);
        CHECK(board(m).clip_count == 256);
        std::cout << "PASS: mixed clipboard capture/cut/repeat paste, row gaps, deleted sources, late rollback, 256 clips, ABI guards, Undo/Redo and real WAV persistence\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
