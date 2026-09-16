// e2e: the whole session in one arc.
//
// A user's "day one" journey, start to finish, through the public C ABI only:
//   import audio → build tracks/buses/sends → mix → automate → edit clips →
//   lay down MIDI → set the tempo map → drop markers → save the draft →
//   reopen it in a fresh session → export the mix, and prove the reopened
//   project renders bit-for-bit the same mix as the original.
//
// If any durable field is lost between the ABI, the domain, SQLite, or the
// renderer, this scenario goes red even when every narrow CTest stays green.
#include "e2e.hpp"

int main() {
    try {
        using namespace e2e;
        TempRoot root("lifecycle");

        // 1. A deterministic instrument: a 440 Hz stereo tone, one second.
        const auto tonePath = root / "tone.wav";
        writeWavFixture(tonePath, sineInterleaved(kProjectRate, kProjectRate, 440.0, 0.5, 2), kProjectRate, 2, "f32");

        Bridge session;
        CHECK(snapshotOf(session.get()).revision == 0);

        // 2. Import the tone onto a new track.
        CHECK_OK(session.get(), daw_import_wav(session.get(), tonePath.string().c_str(), "Lead", rev(session.get())));
        const auto lead = trackById(session.get(), 1);
        CHECK(lead.clip_count == 1 && lead.take_count == 1 && lead.audio_frames == kProjectRate);

        // 3. Second strip with its own gain/pan, plus a bus the lead feeds.
        const auto harmony = addTrack(session.get(), "Harmony");
        CHECK_OK(session.get(), daw_set_gain(session.get(), harmony, -6.0, rev(session.get())));
        CHECK_OK(session.get(), daw_set_pan(session.get(), harmony, 0.4, rev(session.get())));
        const auto bus = addBus(session.get(), "Mix Bus");
        CHECK_OK(session.get(), daw_set_track_output(session.get(), lead.id, bus, rev(session.get())));
        CHECK_OK(session.get(), daw_upsert_send(session.get(), harmony, bus, -3.0, /*pre_fader=*/1, rev(session.get())));
        CHECK_OK(session.get(), daw_set_bus_gain(session.get(), bus, -1.0, rev(session.get())));
        CHECK_OK(session.get(), daw_set_master_gain(session.get(), -3.0, rev(session.get())));

        // 4. Volume automation on the lead: fade in over the first quarter second.
        CHECK_OK(session.get(), daw_upsert_track_volume_automation_point(session.get(), lead.id, 0, -24.0, rev(session.get())));
        CHECK_OK(session.get(), daw_upsert_track_volume_automation_point(session.get(), lead.id, kProjectRate / 4, 0.0, rev(session.get())));

        // 5. Clip-level edit: split the lead region at the exact half-second mark.
        auto region = abi<daw_clip>();
        CHECK_OK(session.get(), daw_get_clip(session.get(), lead.id, 0, &region));
        CHECK_OK(session.get(), daw_split_clip(session.get(), lead.id, 0, kProjectRate / 2, rev(session.get())));
        CHECK(trackById(session.get(), lead.id).clip_count == 2);

        // 6. MIDI strip: a one-bar C major voicing.
        const auto synth = addTrack(session.get(), "Synth");
        daw_midi_clip clip = abi<daw_midi_clip>();
        clip.version = DAW_MIDI_CLIP_VERSION;
        clip.start = 0;
        clip.length = kProjectRate / 2; // half a second at 120 BPM 4/4
        clip.lane = 0;
        clip.note_count = 3; // the bridge cross-checks it against the array
        daw_midi_note notes[3] = {};
        for (auto& note : notes) note.struct_size = sizeof(daw_midi_note);
        notes[0] = daw_midi_note{sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 0, kProjectRate / 4, 60, 0, 100};
        notes[1] = daw_midi_note{sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 0, kProjectRate / 4, 64, 0, 90};
        notes[2] = daw_midi_note{sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 0, kProjectRate / 4, 67, 0, 80};
        CHECK_OK(session.get(), daw_add_midi_clip(session.get(), synth, &clip, notes, 3, rev(session.get())));

        // 7. Tempo map + a marker, so both auxiliary lanes are round-tripped too.
        CHECK_OK(session.get(), daw_set_tempo(session.get(), kProjectRate, 96.0, rev(session.get())));
        CHECK_OK(session.get(), daw_add_marker(session.get(), 0, "Intro", rev(session.get())));

        const auto built = dumpOf(session.get());
        CHECK(built.tracks.size() == 3 && built.buses.size() == 1);

        // 8. Export BEFORE saving: this is the reference rendering.
        const auto mixBefore = exportProject(session.get(), root / "before.wav", 2);
        CHECK(mixBefore.frames() >= kProjectRate);
        // A 440 Hz tone must dominate the master bus at audible level.
        CHECK(channelPeak(mixBefore, 0, static_cast<uint32_t>(mixBefore.frames()), 0) > 0.05);
        CHECK(toneMagnitude(mixBefore, kProjectRate / 4, static_cast<uint32_t>(mixBefore.frames()), 0, 440.0) > 0.05);

        // 9. Save the durable draft, then reopen it in a brand-new session.
        const auto draft = root / "session.mydawdraft";
        saveDraftAndWait(session.get(), draft);
        CHECK(fileNonEmpty(draft));

        // 9b. The synchronous save API the app uses for quick persists:
        // argument/bad-directory rejections carry error text and change
        // nothing, and a successful sync save round-trips identically.
        const auto revisionBeforeSync = rev(session.get());
        CHECK_REJ(session.get(), daw_save_draft(session.get(), nullptr));
        CHECK_REJ(session.get(), daw_save_draft(session.get(), ""));
        const auto badDir = root / "no-such-dir" / "x.mydawdraft";
        CHECK_REJ(session.get(), daw_save_draft(session.get(), badDir.string().c_str()));
        CHECK(!std::filesystem::exists(root / "no-such-dir")); // rejected saves create no scaffolding
        CHECK(rev(session.get()) == revisionBeforeSync);      // and never mutate the session
        const auto syncDraft = root / "sync.mydawdraft";
        CHECK_OK(session.get(), daw_save_draft(session.get(), syncDraft.string().c_str()));
        CHECK(fileNonEmpty(syncDraft));

        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), draft.string().c_str()));
        const auto restored = dumpOf(reopened.get());
        if (!(restored == built))
            throw std::runtime_error("round-trip changed the project:\nBEFORE:\n" + describe(built) +
                                     "\nAFTER:\n" + describe(restored));

        // The synchronous draft is the same project, byte for byte in model terms.
        Bridge syncProbe;
        CHECK_OK(syncProbe.get(), daw_open_draft(syncProbe.get(), syncDraft.string().c_str()));
        const auto syncRestored = dumpOf(syncProbe.get());
        if (!(syncRestored == built))
            throw std::runtime_error("synchronous save lost the project:\nBEFORE:\n" + describe(built) +
                                     "\nAFTER:\n" + describe(syncRestored));

        // 10. Portable paths (PRJ-01): the draft embeds its media, so the
        // reopened project must still render with the source file deleted.
        // The mix math is deterministic, so samples must be bit-identical.
        std::filesystem::remove(tonePath);
        CHECK(!std::filesystem::exists(tonePath));
        const auto mixAfter = exportProject(reopened.get(), root / "after.wav", 2);
        CHECK(mixAfter.frames() == mixBefore.frames());
        CHECK(mixAfter.channels == mixBefore.channels);
        for (size_t i = 0; i < mixBefore.samples.size(); ++i)
            if (mixBefore.samples[i] != mixAfter.samples[i])
                throw std::runtime_error("reopened render differs at frame " + std::to_string(i / mixBefore.channels));

        std::cout << "PASS: e2e_project_lifecycle — import, mix, automate, edit, MIDI, tempo, marker, save, reopen, export\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}
