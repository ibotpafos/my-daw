// e2e: live capture surfaces without any hardware (REC-01/MIDI-05 contracts).
//
// Real microphones, controllers and TCC prompts stay a manual gate — the
// acceptance policy says CTests must never open them. What CAN and must be
// proven headless is the full contract around those gates: enumeration is
// bounded, unknown sources reject cleanly, arming without an input changes
// nothing, the idle poll/stop loop is a safe no-op that consumes no revision,
// every counter reads zero outside a take, and the ABI shape gates hold.
// Calling daw_record_start itself is intentionally avoided: on a developer
// machine it would open the microphone (and macOS would ask). That path is
// covered by recording_crash_recovery + hardware_smoke and the manual gate.
#include "e2e.hpp"

int main() {
    try {
        using namespace e2e;
        TempRoot root("capture");
        Bridge session;

        const auto tonePath = root / "tone.wav";
        writeWavFixture(tonePath, sineInterleaved(kProjectRate / 2, kProjectRate, 300.0, 0.3, 2), kProjectRate, 2, "f32");
        CHECK_OK(session.get(), daw_import_wav(session.get(), tonePath.string().c_str(), "Source", rev(session.get())));
        const uint64_t revision = rev(session.get());
        const auto projectBefore = dumpOf(session.get());

        // 1. Recording telemetry outside any take: honest zeros, shape gates.
        auto recording = abi<daw_recording>();
        CHECK_OK(session.get(), daw_get_recording(session.get(), &recording));
        CHECK(recording.recording == 0 && recording.frames == 0 && recording.callbacks == 0 && recording.target_track_id == 0);
        float detailedRecordingPreview[DAW_RECORDING_PREVIEW_DETAIL_BINS]{};
        CHECK_OK(session.get(), daw_get_recording_preview_detail(session.get(), detailedRecordingPreview, DAW_RECORDING_PREVIEW_DETAIL_BINS));
        CHECK(detailedRecordingPreview[0] == 0.0f && detailedRecordingPreview[DAW_RECORDING_PREVIEW_DETAIL_BINS - 1] == 0.0f);
        CHECK_REJ(session.get(), daw_get_recording_preview_detail(session.get(), detailedRecordingPreview, DAW_RECORDING_PREVIEW_DETAIL_BINS - 1));
        CHECK_REJ(session.get(), daw_get_recording_preview_detail(session.get(), nullptr, DAW_RECORDING_PREVIEW_DETAIL_BINS));
        recording.struct_size = sizeof(daw_recording) - 4;
        CHECK_REJ(session.get(), daw_get_recording(session.get(), &recording)); // ABI gate
        CHECK_REJ(session.get(), daw_record_stop(session.get(), "Orphan", revision)); // never started
        CHECK_OK(session.get(), daw_record_cancel(session.get())); // cancel with no take: no-op
        CHECK_REJ(session.get(), daw_recover_take(session.get(), (root / "missing.mydawtake").string().c_str(),
                                                   "Ghost", revision)); // missing recovery file
        CHECK(rev(session.get()) == revision);

        // 1b. Recording into an empty armed track materializes it into an
        // audio track on stop. Therefore it must reserve the same eight-track
        // budget before opening the microphone, rather than bypassing the
        // policy merely because the target has no audio yet.
        {
            Bridge limited;
            for (uint32_t index = 0; index < 8; ++index) {
                CHECK_OK(limited.get(), daw_import_wav(limited.get(), tonePath.string().c_str(), "Audio", rev(limited.get())));
            }
            const uint64_t emptyTarget = addTrack(limited.get(), "Armed empty audio");
            const auto beforeLimit = snapshotOf(limited.get());
            CHECK_REJ(limited.get(), daw_record_start_take(limited.get(), emptyTarget, 0,
                                                           (root / "limit.mydawtake").string().c_str()));
            CHECK(bridgeError(limited.get()) == "Prototype supports at most 8 audio tracks");
            const auto afterLimit = snapshotOf(limited.get());
            CHECK(afterLimit.revision == beforeLimit.revision && afterLimit.track_count == beforeLimit.track_count);
            CHECK(trackById(limited.get(), emptyTarget).audio_frames == 0);
        }

        // 2. MIDI source enumeration is bounded: whatever the environment
        // reports, indexed reads must match the count and out-of-range rejects.
        uint32_t deviceCount = 0;
        CHECK_OK(session.get(), daw_get_midi_input_device_count(session.get(), &deviceCount));
        for (uint32_t index = 0; index < deviceCount; ++index) {
            auto device = abi<daw_midi_device>();
            device.version = DAW_MIDI_DEVICE_VERSION;
            CHECK_OK(session.get(), daw_get_midi_input_device(session.get(), index, &device));
            CHECK(device.uniqueID != 0);
        }
        auto beyond = abi<daw_midi_device>();
        CHECK_REJ(session.get(), daw_get_midi_input_device(session.get(), deviceCount, &beyond));

        // 3. Closing a source that was never opened is an idempotent success;
        // an unknown source id rejects and leaves the (empty) selection alone.
        CHECK_OK(session.get(), daw_set_midi_input(session.get(), 0));
        uint32_t activeId = 1;
        CHECK_OK(session.get(), daw_midi_input_active(session.get(), &activeId));
        CHECK(activeId == 0);
        CHECK_REJ(session.get(), daw_set_midi_input(session.get(), 0xFFFFFFFFu));
        CHECK_OK(session.get(), daw_midi_input_active(session.get(), &activeId));
        CHECK(activeId == 0);

        // 4. Arming requires an open input AND an existing clip. Build the
        // clip so the rejection is specifically about the missing input, then
        // confirm both arming failures consume no revision and mutate nothing.
        const auto trackId = trackById(session.get(), 1).id;
        daw_midi_clip clip = abi<daw_midi_clip>();
        clip.version = DAW_MIDI_CLIP_VERSION;
        clip.start = 0;
        clip.length = kProjectRate / 2;
        clip.note_count = 0;
        CHECK_OK(session.get(), daw_add_midi_clip(session.get(), trackId, &clip, nullptr, 0, rev(session.get())));
        const auto afterClip = rev(session.get());
        CHECK_REJ(session.get(), daw_midi_record_arm(session.get(), trackId, 0)); // no open input
        CHECK_REJ(session.get(), daw_midi_record_arm(session.get(), trackId, 99)); // no such clip (still no input)
        CHECK_REJ(session.get(), daw_midi_record_arm(session.get(), 9999, 0)); // no such track
        CHECK(rev(session.get()) == afterClip);
        CHECK(dumpOf(session.get()) == dumpOf(session.get())); // reads are stable

        // 5. The idle poll/stop loop must never commit, never consume, and
        // never report armed. This is what the 100 ms UI timer rides on.
        CHECK_OK(session.get(), daw_midi_record_poll(session.get()));
        CHECK_OK(session.get(), daw_midi_record_stop(session.get()));
        CHECK(rev(session.get()) == afterClip);
        auto status = abi<daw_midi_record_status_t>();
        status.version = DAW_MIDI_RECORD_STATUS_VERSION;
        CHECK_OK(session.get(), daw_midi_record_status(session.get(), &status));
        CHECK(status.armed == 0 && status.open_notes == 0 && status.recorded == 0 && status.dropped == 0 && status.unmatched == 0);
        status.struct_size = sizeof(status) - 4;
        CHECK_REJ(session.get(), daw_midi_record_status(session.get(), &status)); // ABI gate
        status.struct_size = sizeof(status);
        status.version = 99;
        CHECK_REJ(session.get(), daw_midi_record_status(session.get(), &status)); // version gate

        // 6. A captured-into take cannot be faked at the ABI: stop with an
        // armed-less recorder must not have appended anything to the clip.
        auto stored = abi<daw_midi_clip>();
        uint32_t written = 0;
        CHECK_OK(session.get(), daw_get_midi_clip(session.get(), trackId, 0, &stored, 0, nullptr, 0, &written));
        CHECK(stored.note_count == 0);

        // 7. Transport arming interlock: changing the loop while a take would
        // run is refused only WITH an active recorder; idle it stays legal.
        CHECK_OK(session.get(), daw_set_loop(session.get(), 1, 0, kProjectRate / 4));

        // 8. Ephemeral capture flags still obey session scoping across saves.
        CHECK_OK(session.get(), daw_set_record_preroll(session.get(), 2400));
        CHECK_OK(session.get(), daw_set_metronome(session.get(), 1));
        saveDraftAndWait(session.get(), root / "capture.mydawdraft");
        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), (root / "capture.mydawdraft").string().c_str()));
        uint64_t preroll = 999;
        CHECK_OK(reopened.get(), daw_get_record_preroll(reopened.get(), &preroll));
        CHECK(preroll == 0); // session-scoped, never persisted
        int metronome = 1;
        CHECK_OK(reopened.get(), daw_get_metronome(reopened.get(), &metronome));
        CHECK(metronome == 0);
        uint32_t reopenedActive = 7;
        CHECK_OK(reopened.get(), daw_midi_input_active(reopened.get(), &reopenedActive));
        CHECK(reopenedActive == 0); // no input survives a session swap

        std::cout << "PASS: e2e_capture_contracts — idle telemetry, bounded enumeration, input rejection, arming preconditions, no-commit poll/stop, flag scoping\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}
