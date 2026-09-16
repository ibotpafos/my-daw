// e2e: undo/redo history as the app drives it (mixed-subsystem script).
//
// A 30-command edit touching every subsystem the ABI exposes (audio import,
// clip edit/split/fades/gain, track rename/gain/pan/mute/solo/color/remove,
// buses, sends, all four automation lanes, tempo, signature, markers, master
// gain, MIDI clip + transpose) must rewind to a pristine project, replay to
// the exact same project, honor the documented 128-entry history bound
// (tests/core_tests.cpp: undos==128 after 256 adds), keep optimistic
// revisions honest, die on open ("Successful open replaces state and clears
// undo/redo", daw.h), and interlock with automation gestures ("Mutations/
// Undo are rejected while it is active", "consumes exactly one project
// revision at end").
//
// Revision honesty: undo/redo are themselves revisioned events in this
// domain (core_tests: s.undo(3) leaves revision 4), so a full rewind can
// never return revision 0 — the strongest honest claim is field-for-field
// equality against a BRAND-NEW session for every durable field except the
// monotonic revision counter (ids included: Session::undo restores the whole
// prior State snapshot, so nextID rewinds with it), plus exact expected
// revision numbers at every checkpoint.
#include "e2e.hpp"

#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

// Every durable field of the dump except the revision counter; the counter
// is asserted separately at each step because it must move monotonically.
bool sameProject(const e2e::Dump& a, const e2e::Dump& b) {
    return std::tie(a.masterGain, a.tracks, a.buses, a.masterInserts, a.masterAutomation, a.tempo, a.signatures,
                    a.markers) ==
           std::tie(b.masterGain, b.tracks, b.buses, b.masterInserts, b.masterAutomation, b.tempo, b.signatures,
                    b.markers);
}

} // namespace

int main() {
    try {
        using namespace e2e;
        TempRoot root("undo");

        Bridge session;
        daw_session* const s = session.get();
        CHECK(rev(s) == 0);
        CHECK(snapshotOf(s).can_undo == 0 && snapshotOf(s).can_redo == 0);

        // 0. Nothing happened yet: an empty history rejects both directions
        //    with a human-readable reason.
        CHECK_REJ(s, daw_undo(s, 0));
        CHECK_REJ(s, daw_redo(s, 0));

        // 1. A deterministic 48 kHz stereo fixture (cross-platform backbone).
        const auto tonePath = root / "tone.wav";
        writeWavFixture(tonePath, sineInterleaved(kProjectRate, kProjectRate, 440.0, 0.5, 2), kProjectRate, 2, "f32");

        // Committed step: one revision, one undo entry, recorded dump.
        uint64_t committed = 0;
        std::vector<Dump> history;
        auto step = [&](auto&& call) {
            call();
            ++committed;
            CHECK(rev(s) == committed); // monotonic single-revision rule
            history.push_back(dumpOf(s));
        };
        // Silent no-op: accepted (rc 0) but must NOT bump the revision.
        auto silent = [&](auto&& call) {
            call();
            CHECK(rev(s) == committed);
        };

        // 2. The mixed script: 30 committed commands, 4 silent no-ops.
        step([&] { CHECK_OK(s, daw_import_wav(s, tonePath.string().c_str(), "Lead", rev(s))); }); // step 1
        const uint64_t lead = trackById(s, 1).id;
        uint64_t drums = 0, bus = 0, synth = 0;
        step([&] { drums = addTrack(s, "Drums"); });                       // step 2
        step([&] { CHECK_OK(s, daw_rename_track(s, drums, "Percussion", rev(s))); });   // step 3
        step([&] { CHECK_OK(s, daw_set_gain(s, drums, -6.0, rev(s))); });               // step 4
        step([&] { CHECK_OK(s, daw_set_pan(s, drums, 0.35, rev(s))); });                // step 5
        step([&] { CHECK_OK(s, daw_set_mute(s, drums, 1, rev(s))); });                  // step 6
        silent([&] { CHECK_OK(s, daw_set_mute(s, drums, 1, rev(s))); });                // no-op: identical mute
        step([&] { CHECK_OK(s, daw_set_mute(s, drums, 0, rev(s))); });                  // step 7
        step([&] { CHECK_OK(s, daw_set_solo(s, drums, 1, rev(s))); });                  // step 8
        step([&] { CHECK_OK(s, daw_set_track_color(s, drums, 0x00FF88u, rev(s))); });   // step 9
        silent([&] { CHECK_OK(s, daw_set_pan(s, drums, 0.35, rev(s))); });              // no-op: identical pan
        step([&] { CHECK_OK(s, daw_edit_clip(s, lead, 0, 0, 0, 3 * kProjectRate / 4, rev(s))); });   // step 10
        step([&] { CHECK_OK(s, daw_split_clip(s, lead, 0, kProjectRate / 2, rev(s))); });            // step 11
        step([&] { CHECK_OK(s, daw_set_clip_fades(s, lead, 0, 100, 200, rev(s))); });                // step 12
        step([&] { CHECK_OK(s, daw_set_clip_gain(s, lead, 1, -3.0, rev(s))); });                     // step 13
        step([&] { bus = addBus(s, "Mix Bus"); });                                                   // step 14
        step([&] { CHECK_OK(s, daw_set_track_output(s, lead, bus, rev(s))); });                      // step 15
        step([&] { CHECK_OK(s, daw_upsert_send(s, drums, bus, -6.0, 1, rev(s))); });                 // step 16
        step([&] { CHECK_OK(s, daw_remove_send(s, drums, bus, rev(s))); });                          // step 17
        step([&] { CHECK_OK(s, daw_upsert_track_volume_automation_point(s, lead, 0, -12.0, rev(s))); }); // step 18
        step([&] { CHECK_OK(s, daw_upsert_track_pan_automation_point(s, lead, kProjectRate / 2, -0.5, rev(s))); }); // step 19
        step([&] { CHECK_OK(s, daw_upsert_bus_gain_automation_point(s, bus, 4800, 2.0, rev(s))); });  // step 20
        step([&] { CHECK_OK(s, daw_upsert_master_gain_automation_point(s, 9600, -1.0, rev(s))); });   // step 21
        step([&] { CHECK_OK(s, daw_set_tempo(s, kProjectRate, 96.0, rev(s))); });                     // step 22
        step([&] { CHECK_OK(s, daw_set_time_signature(s, 0, 3, 4, rev(s))); });                       // step 23: replaces the 4/8 anchor
        silent([&] { CHECK_OK(s, daw_set_time_signature(s, 0, 3, 4, rev(s))); });                     // no-op: identical value
        step([&] { CHECK_OK(s, daw_add_marker(s, 2 * kProjectRate, "Drop", rev(s))); });              // step 24
        step([&] { CHECK_OK(s, daw_rename_marker(s, 2 * kProjectRate, "Chorus", rev(s))); });         // step 25
        silent([&] { CHECK_OK(s, daw_rename_marker(s, 2 * kProjectRate, "Chorus", rev(s))); });       // no-op: identical name
        step([&] { CHECK_OK(s, daw_set_master_gain(s, -2.0, rev(s))); });                             // step 26
        step([&] { synth = addTrack(s, "Synth"); });                                                  // step 27
        {
            auto clip = abi<daw_midi_clip>();
            clip.version = DAW_MIDI_CLIP_VERSION;
            clip.start = 0;
            clip.length = kProjectRate / 2;
            clip.lane = 0;
            clip.note_count = 3;
            daw_midi_note notes[3] = {
                {sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 0, kProjectRate / 4, 60, 0, 100},
                {sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 0, kProjectRate / 4, 64, 0, 90},
                {sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 0, kProjectRate / 4, 67, 0, 80}};
            step([&] { CHECK_OK(s, daw_add_midi_clip(s, synth, &clip, notes, 3, rev(s))); });         // step 28
        }
        step([&] { CHECK_OK(s, daw_transpose_midi_clip(s, synth, 0, 2, rev(s))); });                  // step 29
        step([&] { CHECK_OK(s, daw_remove_track(s, drums, rev(s))); });                               // step 30

        CHECK(committed == 30);
        CHECK(rev(s) == 30);
        const Dump built = history.back();
        CHECK(built.tracks.size() == 2 && built.buses.size() == 1);

        // 3. Optimistic revisions on normal commands: a stale expected value
        //    rejects and changes nothing (dump compared bit-exactly, revision
        //    included).
        CHECK_REJ(s, daw_set_gain(s, lead, -42.0, 29));
        CHECK_REJ(s, daw_rename_track(s, lead, "Ghost", 0));
        CHECK(dumpOf(s) == built);

        // 4. Stale undo/redo are rejected too, and the rejection is inert.
        CHECK_REJ(s, daw_undo(s, 29));
        CHECK_REJ(s, daw_redo(s, 999));
        CHECK(dumpOf(s) == built);
        CHECK(snapshotOf(s).can_undo == 1 && snapshotOf(s).can_redo == 0); // flags are boolean, not depths

        // 5. Full rewind: always pass the CURRENT revision — expect_revision
        //    is a moving target because each undo is itself an event.
        uint64_t undos = 0;
        for (;;) {
            const auto snap = snapshotOf(s);
            if (!snap.can_undo) break;
            CHECK_OK(s, daw_undo(s, snap.revision));
            ++undos;
            CHECK(rev(s) == 30 + undos);
        }
        CHECK(undos == 30);
        CHECK(rev(s) == 60);
        CHECK(snapshotOf(s).can_undo == 0);
        CHECK(snapshotOf(s).track_count == 0 && snapshotOf(s).bus_count == 0);
        CHECK_REJ(s, daw_undo(s, rev(s))); // 31st undo: nothing left, honest error

        // 6. The rewound domain must be field-for-field pristine: compare
        //    every durable field against a BRAND-NEW session (ids included —
        //    undo restores whole State snapshots). The revision counter is
        //    the documented exception: undo/redo bump it (core_tests
        //    s.undo(3) -> revision 4), so exact equality there is replaced by
        //    the checkpoint rev == 2*committed asserted above.
        Bridge pristine;
        const Dump fresh = dumpOf(pristine.get());
        const Dump rewound = dumpOf(s);
        if (!sameProject(fresh, rewound))
            throw std::runtime_error("full rewind is not pristine:\nFRESH:\n" + describe(fresh) +
                                     "\nREWOUND:\n" + describe(rewound));
        CHECK(rewound.tempo == fresh.tempo);         // frame-0 120 BPM anchor only
        CHECK(rewound.signatures == fresh.signatures); // frame-0 meter anchor restored (the script's 3/4 undoed)
        CHECK(rewound.revision == 60 && fresh.revision == 0);

        // 7. Full replay: can_redo drives the loop, dumps compared against
        //    every 5th recorded step (not just endpoints), all equal except
        //    the monotonic revision counter, which is checked exactly.
        uint64_t redos = 0;
        for (;;) {
            const auto snap = snapshotOf(s);
            if (!snap.can_redo) break;
            CHECK_OK(s, daw_redo(s, snap.revision));
            ++redos;
            CHECK(rev(s) == 60 + redos);
            if (redos % 5 == 0) {
                const Dump replayed = dumpOf(s);
                if (!sameProject(history[redos - 1], replayed))
                    throw std::runtime_error("replay diverged after redo #" + std::to_string(redos) +
                                             ":\nEXPECTED:\n" + describe(history[redos - 1]) +
                                             "\nACTUAL:\n" + describe(replayed));
            }
        }
        CHECK(redos == 30);
        CHECK(rev(s) == 90);
        CHECK_REJ(s, daw_redo(s, rev(s))); // nothing left to redo
        CHECK(sameProject(history.back(), dumpOf(s)));
        CHECK(snapshotOf(s).can_undo == 1 && snapshotOf(s).can_redo == 0); // flags are boolean, not depths

        // 8. Two-step partial rewind, then optimistic checks WHILE a redo
        //    branch exists: both stale directions reject and leave state
        //    untouched.
        CHECK_OK(s, daw_undo(s, rev(s)));
        CHECK_OK(s, daw_undo(s, rev(s)));
        CHECK(snapshotOf(s).can_redo == 1 && snapshotOf(s).can_undo == 1);
        const Dump partial = dumpOf(s);
        CHECK_REJ(s, daw_redo(s, partial.revision + 7)); // stale target, branch exists
        CHECK_REJ(s, daw_undo(s, 0));                    // stale from below
        CHECK(dumpOf(s) == partial);

        // 9. A new command truncates the redo branch. (After undo/redo the
        // revision baseline moved, so raw checks replace the step() helper.)
        const uint64_t truncatedAt = partial.revision;
        CHECK_OK(s, daw_set_mute(s, lead, 1, truncatedAt));
        CHECK(snapshotOf(s).can_redo == 0);
        CHECK_REJ(s, daw_redo(s, rev(s))); // future cleared by the new commit
        CHECK(rev(s) == truncatedAt + 1);

        // 10. Save mid-history, reopen elsewhere: revision survives, history
        //     dies ("Successful open replaces state and clears undo/redo").
        const Dump midSave = dumpOf(s);
        const auto draft = root / "mid-history.mydawdraft";
        saveDraftAndWait(s, draft);
        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), draft.string().c_str()));
        CHECK(dumpOf(reopened.get()) == midSave); // exact, revision included
        CHECK(snapshotOf(reopened.get()).can_undo == 0);
        CHECK(snapshotOf(reopened.get()).can_redo == 0);
        // Opening in place does the same: the live session's stacks are gone
        // and editing continues from the loaded revision.
        CHECK(snapshotOf(s).can_undo > 0);
        CHECK_OK(s, daw_open_draft(s, draft.string().c_str()));
        CHECK(snapshotOf(s).can_undo == 0 && snapshotOf(s).can_redo == 0);
        CHECK(dumpOf(s) == midSave);
        CHECK_OK(s, daw_rename_track(s, lead, "Lead 2", rev(s)));
        CHECK(snapshotOf(s).can_undo == 1);

        // 11. Gesture interplay: undo is rejected while a gesture is active,
        //     ending it consumes exactly one revision, and undo then works.
        const uint64_t base = rev(s);
        const Dump beforeGesture = dumpOf(s);
        CHECK_REJ(s, daw_begin_automation_gesture(s, DAW_AUTOMATION_TRACK_VOLUME, lead, DAW_AUTOMATION_TOUCH, base + 42));
        CHECK_OK(s, daw_begin_automation_gesture(s, DAW_AUTOMATION_TRACK_VOLUME, lead, DAW_AUTOMATION_TOUCH, base));
        CHECK(rev(s) == base); // begin is not a project event
        CHECK(snapshotOf(s).can_undo == 1);
        CHECK_OK(s, daw_write_automation_gesture(s, 1000, -6.0));
        CHECK_OK(s, daw_write_automation_gesture(s, 2000, -18.0));
        CHECK(rev(s) == base); // buffered samples consume nothing
        CHECK_REJ(s, daw_undo(s, base));           // "Mutations/Undo are rejected while it is active"
        CHECK_REJ(s, daw_set_gain(s, lead, -9.0, base)); // and so are mutations
        CHECK(dumpOf(s) == beforeGesture);
        CHECK_OK(s, daw_end_automation_gesture(s, 3000, base));
        CHECK(rev(s) == base + 1); // exactly one revision at end
        CHECK(snapshotOf(s).can_undo == 1);
        const Points touched{{0, -12.0}, {1000, -6.0}, {2000, -18.0}}; // touch mode: only written samples
        CHECK(volumePoints(s, lead) == touched);
        CHECK_OK(s, daw_undo(s, base + 1)); // undo works again and reverts the whole gesture
        CHECK(rev(s) == base + 2);
        const Dump afterGestureUndo = dumpOf(s);
        if (!sameProject(beforeGesture, afterGestureUndo))
            throw std::runtime_error("gesture undo did not restore the pre-gesture project:\nBEFORE:\n" +
                                     describe(beforeGesture) + "\nAFTER:\n" + describe(afterGestureUndo));
        // An unwritten gesture is a documented no-op: ending it costs no revision.
        CHECK_OK(s, daw_begin_automation_gesture(s, DAW_AUTOMATION_MASTER_GAIN, 0, DAW_AUTOMATION_LATCH, base + 2));
        CHECK_OK(s, daw_end_automation_gesture(s, 5000, base + 2));
        CHECK(rev(s) == base + 2);

        // 12. History bound (mirrors tests/core_tests.cpp 'undos==128'): the
        //     undo ring is 128 entries deep; the oldest states fall off.
        Bridge bounded;
        daw_session* b = bounded.get();
        for (uint64_t i = 1; i <= 256; ++i) {
            const std::string name = "T" + std::to_string(i);
            CHECK_OK(b, daw_add_track(b, name.c_str(), rev(b)));
            CHECK(rev(b) == i);
        }
        CHECK(snapshotOf(b).track_count == 256);
        CHECK_REJ(b, daw_add_track(b, "Overflow", rev(b))); // capacity rule intact
        uint64_t boundedUndos = 0;
        for (;;) {
            const auto snap = snapshotOf(b);
            if (!snap.can_undo) break;
            CHECK_OK(b, daw_undo(b, snap.revision));
            ++boundedUndos;
        }
        CHECK(boundedUndos == 128);                 // exactly the documented ring depth
        CHECK(rev(b) == 256 + 128);
        CHECK(snapshotOf(b).track_count == 128);
        CHECK_REJ(b, daw_undo(b, rev(b)));          // the 129th undo fails
        uint64_t boundedRedos = 0;
        for (;;) {
            const auto snap = snapshotOf(b);
            if (!snap.can_redo) break;
            CHECK_OK(b, daw_redo(b, snap.revision));
            ++boundedRedos;
        }
        CHECK(boundedRedos == 128);                 // redo count matches
        CHECK(rev(b) == 256 + 256);
        CHECK(snapshotOf(b).track_count == 256);
        CHECK_REJ(b, daw_redo(b, rev(b)));          // and no more

        std::cout << "PASS: e2e_undo_history — 30-command mixed script rewinds pristine, replays bit-exact, honors the 128-entry ring, optimistic revisions and the gesture lock\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}