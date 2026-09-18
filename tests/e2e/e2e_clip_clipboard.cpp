#include "e2e.hpp"

using namespace e2e;
namespace {
daw_clip clip(daw_session *s, uint64_t track, uint32_t index) {
    auto value = abi<daw_clip>();
    CHECK_OK(s, daw_get_clip(s, track, index, &value));
    return value;
}
daw_clipboard_info clipboard(daw_session *s) {
    auto value = abi<daw_clipboard_info>();
    CHECK_OK(s, daw_get_clipboard(s, &value));
    return value;
}
daw_midi_clip midi(daw_session *s, uint64_t track, uint32_t index) {
    auto value = abi<daw_midi_clip>();
    CHECK_OK(s, daw_get_midi_clip(s, track, index, &value, 0, nullptr, 0, nullptr));
    return value;
}
}
int main() {
    try {
        TempRoot root("clip-clipboard");
        writeWavFixture(root / "voice.wav", constantInterleaved(48000, 2, .1f, .05f), 48000, 2,
                        "f32");
        writeWavFixture(root / "other.wav", constantInterleaved(48000, 2, .2f, .15f), 48000, 2,
                        "f32");
        Bridge session;
        auto *s = session.get();
        const uint32_t one[] = {0}, group[] = {2, 1, 2}, bad[] = {0, 999};
        CHECK_REJ(s, daw_paste_clipboard(s, 1, 0, rev(s)));
        CHECK(clipboard(s).clip_count == 0);
        CHECK_OK(s, daw_import_wav(s, (root / "voice.wav").c_str(), "Voice", rev(s)));
        CHECK_OK(s, daw_import_wav(s, (root / "other.wav").c_str(), "Other source", rev(s)));
        CHECK_OK(s, daw_edit_clip_full(s, 1, 0, 12000, 1000, 10000, 64, 96, rev(s)));
        CHECK_OK(s, daw_set_clip_gain(s, 1, 0, -6.020599913, rev(s)));
        CHECK_OK(s, daw_set_clip_pan(s, 1, 0, .5, rev(s)));
        CHECK_OK(s, daw_set_clip_color(s, 1, 0, 0xCC77AA, rev(s)));
        CHECK_OK(s, daw_set_clip_looped(s, 1, 0, 1, rev(s)));
        const auto original = clip(s, 1, 0);
        auto before = rev(s);
        CHECK_OK(s, daw_capture_clipboard(s, 1, 0, one, 1, 0, before));
        CHECK(rev(s) == before && clipboard(s).kind == 1 && clipboard(s).length == 10000);
        CHECK_OK(s, daw_set_clip_gain(s, 1, 0, 3, rev(s)));
        CHECK_OK(s, daw_set_clip_muted(s, 1, 0, 1, rev(s)));
        CHECK_OK(s, daw_delete_clip(s, 1, 0, rev(s)));
        CHECK(trackById(s, 1).clip_count == 0 && trackById(s, 1).audio_frames == 48000);
        before = rev(s);
        CHECK_OK(s, daw_paste_clipboard(s, 2, 96000, before));
        CHECK(rev(s) == before + 1);
        auto pasted = clip(s, 2, 1);
        CHECK(pasted.start == 96000 && pasted.source_offset == original.source_offset &&
              pasted.length == original.length);
        CHECK(pasted.gain_db == original.gain_db && pasted.pan == original.pan &&
              pasted.color == original.color);
        CHECK(pasted.muted == 0 && pasted.looped == 1 && pasted.fade_in == 64 &&
              pasted.fade_out == 96);
        CHECK(pasted.take_index == 1 && trackById(s, 2).take_count == 2);
        CHECK_OK(s, daw_paste_clipboard(s, 2, 144000, rev(s)));
        CHECK(clipboard(s).clip_count == 1 && trackById(s, 2).take_count == 2);
        CHECK(clip(s, 2, 2).take_index == 1);
        auto wav = exportProject(s, root / "pasted.wav");
        CHECK(std::abs(channelRms(wav, 97000, 104000, 0) - .025) < 1e-5);
        CHECK(std::abs(channelRms(wav, 97000, 104000, 1) - .025) < 1e-5);
        CHECK(channelPeak(wav, 50000, 90000, 0) == 0); // no resurrected base waveform
        before = rev(s);
        CHECK_REJ(s, daw_paste_clipboard(s, 2, 97000, before));
        CHECK_REJ(s, daw_paste_clipboard(s, 2, UINT64_MAX, before));
        CHECK_REJ(s, daw_paste_clipboard(s, 999, 0, before));
        CHECK(rev(s) == before && trackById(s, 2).clip_count == 3 &&
              trackById(s, 2).take_count == 2);
        CHECK_REJ(s, daw_capture_clipboard(s, 2, 0, bad, 2, 1, before));
        CHECK_REJ(s, daw_capture_clipboard(s, 2, 2, one, 1, 0, before));
        CHECK_REJ(s, daw_capture_clipboard(s, 2, 0, one, 1, 2, before));
        CHECK_REJ(s, daw_capture_clipboard(s, 2, 0, nullptr, 1, 0, before));
        CHECK_REJ(s, daw_capture_clipboard(s, 2, 0, one, UINT32_MAX, 0, before));
        CHECK_REJ(s, daw_capture_clipboard(s, 2, 0, one, 0, 0, before));
        CHECK_REJ(s, daw_capture_clipboard(s, 2, 0, one, 1, 0, before - 1));
        CHECK(rev(s) == before && clipboard(s).length == 10000); // failed capture keeps old buffer
        auto wrongABI = abi<daw_clipboard_info>();
        wrongABI.struct_size--;
        CHECK_REJ(s, daw_get_clipboard(s, &wrongABI));
        CHECK_REJ(s, daw_get_clipboard(s, nullptr));
        CHECK_OK(s, daw_begin_mixer_gesture(s, DAW_MIXER_TRACK_GAIN, 2, 0, rev(s)));
        CHECK_REJ(s, daw_capture_clipboard(s, 2, 0, one, 1, 0, rev(s)));
        CHECK_REJ(s, daw_paste_clipboard(s, 2, 192000, rev(s)));
        daw_cancel_mixer_gesture(s);
        CHECK(clipboard(s).length == 10000);

        // True cut: remove now, retain original sources for Undo and clipboard.
        before = rev(s);
        CHECK_OK(s, daw_capture_clipboard(s, 2, 0, group, 3, 1, before));
        CHECK(rev(s) == before + 1 && trackById(s, 2).clip_count == 1);
        CHECK(clipboard(s).clip_count == 2 && clipboard(s).length == 58000);
        CHECK_OK(s, daw_undo(s, rev(s)));
        CHECK(trackById(s, 2).clip_count == 3);
        CHECK_OK(s, daw_redo(s, rev(s)));
        CHECK(trackById(s, 2).clip_count == 1);
        const auto target = addTrack(s, "Fresh target");
        CHECK_OK(s, daw_paste_clipboard(s, target, 240000, rev(s)));
        CHECK(trackById(s, target).clip_count == 2 && trackById(s, target).take_count == 1);
        CHECK(clip(s, target, 0).start == 240000 && clip(s, target, 1).start == 288000);
        CHECK(clip(s, target, 0).take_index == 0 && clip(s, target, 0).looped);
        saveDraftAndWait(s, root / "snapshot.daw");
        CHECK_REJ(s, daw_open_draft(s, (root / "missing.daw").c_str()));
        CHECK(clipboard(s).clip_count == 2); // failed Open leaves the current document intact
        CHECK_OK(s, daw_open_draft(s, (root / "snapshot.daw").c_str()));
        CHECK(clipboard(s).clip_count == 0); // successful Open changes document identity
        CHECK(trackById(s, 1).clip_count == 0 && trackById(s, 1).audio_frames == 48000);
        CHECK(trackById(s, target).clip_count == 2 &&
              clip(s, target, 0).gain_db == original.gain_db);
        wav = exportProject(s, root / "reopened.wav");
        CHECK(std::abs(channelRms(wav, 241000, 248000, 0) - .025) < 1e-5);
        CHECK(std::abs(channelRms(wav, 289000, 296000, 1) - .025) < 1e-5);

        // Existing transfer API now adopts sources on empty/unrelated tracks.
        const auto transferTarget = addTrack(s, "Drag target");
        CHECK_OK(s, daw_copy_clip_to_track(s, target, 0, transferTarget, 0, rev(s)));
        CHECK(trackById(s, transferTarget).clip_count == 1);
        CHECK_OK(s, daw_move_clip_to_track(s, transferTarget, 0, 1, 192000, rev(s)));
        CHECK(trackById(s, transferTarget).clip_count == 0 && clip(s, 1, 0).start == 192000);

        // MIDI snapshots copy notes, velocities, channel, lane and color by value.
        const auto keys = addTrack(s, "Keys"), keysTarget = addTrack(s, "Keys target");
        auto mc = abi<daw_midi_clip>();
        mc.version = DAW_MIDI_CLIP_VERSION;
        mc.start = 24000;
        mc.length = 48000;
        mc.lane = 4;
        mc.color = 0x123456;
        mc.note_count = 1;
        auto note = abi<daw_midi_note>();
        note.version = DAW_MIDI_NOTE_VERSION;
        note.start = 6000;
        note.length = 12000;
        note.pitch = 65;
        note.channel = 3;
        note.velocity = 81;
        CHECK_OK(s, daw_add_midi_clip(s, keys, &mc, &note, 1, rev(s)));
        mc.start = 120000;
        note.pitch = 72;
        CHECK_OK(s, daw_add_midi_clip(s, keys, &mc, &note, 1, rev(s)));
        const uint32_t midiGroup[] = {0, 1};
        before = rev(s);
        CHECK_OK(s, daw_capture_clipboard(s, keys, 1, midiGroup, 2, 1, before));
        CHECK(rev(s) == before + 1 && clipboard(s).kind == 2 && clipboard(s).clip_count == 2);
        uint32_t count = 99;
        CHECK_OK(s, daw_get_midi_clip_count(s, keys, &count));
        CHECK(count == 0);
        for (uint64_t frame : {0ULL, 240000ULL, 480000ULL})
            CHECK_OK(s, daw_paste_clipboard(s, keysTarget, frame, rev(s)));
        CHECK_OK(s, daw_get_midi_clip_count(s, keysTarget, &count));
        CHECK(count == 6);
        auto restored = midi(s, keysTarget, 4);
        uint32_t written = 0;
        auto readNote = abi<daw_midi_note>();
        CHECK_OK(s, daw_get_midi_clip(s, keysTarget, 4, &restored, 0, &readNote, 1, &written));
        CHECK(restored.start == 480000 && restored.lane == 4 && restored.color == 0x123456);
        CHECK(written == 1 && readNote.pitch == 65 && readNote.channel == 3 &&
              readNote.velocity == 81 && readNote.start == 6000);
        CHECK(midi(s, keysTarget, 5).start == 576000);
        saveDraftAndWait(s, root / "midi.daw");
        Bridge reloaded;
        CHECK_OK(reloaded.get(), daw_open_draft(reloaded.get(), (root / "midi.daw").c_str()));
        CHECK(midi(reloaded.get(), keysTarget, 5).start == 576000);
        before = rev(s);
        CHECK_OK(s, daw_clear_clipboard(s));
        CHECK(clipboard(s).clip_count == 0 && rev(s) == before);
        CHECK_REJ(s, daw_paste_clipboard(s, keysTarget, 720000, before));
        CHECK(daw_clear_clipboard(nullptr) != 0);
        std::cout
            << "E2E clip clipboard PASS: value snapshots, repeated paste, cut/Undo/Redo, source remapping, persisted empty audio lanes and independently read exported stereo samples\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
