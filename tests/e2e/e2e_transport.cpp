// e2e: transport surface as the app drives it (TRN-01 / METRO-01 / DEV-01).
//
// Headless CI must stay green, so this scenario asserts the CONTRACT that
// holds in every environment: ephemeral transport state never touches the
// durable project, meters read honest zeros with no renderer, play without
// reachable hardware fails (or lands in a named failed state) without
// corrupting the session, and session-scoped flags (metronome, pre-roll,
// monitoring) behave per the header: settable, gettable, never persisted.
//
// On a machine WITH an output device, run with DAW_E2E_DEVICE=1 to also prove
// the live arc: play → callbacks advance → position moves → stop retains the
// position → loop wraps within range. The test skips that branch otherwise.
#include "e2e.hpp"

#include <cstdlib>

int main() {
    try {
        using namespace e2e;
        TempRoot root("transport");

        const auto tonePath = root / "tone.wav";
        // Modest level: this machine's speakers must not blast during a run.
        writeWavFixture(tonePath, sineInterleaved(2 * kProjectRate, kProjectRate, 220.0, 0.1, 2), kProjectRate, 2, "f32");

        Bridge session;
        CHECK_OK(session.get(), daw_import_wav(session.get(), tonePath.string().c_str(), "Tone", rev(session.get())));
        const uint64_t initialRevision = rev(session.get());

        // 1. Transport introspection works without any hardware.
        auto transport = abi<daw_transport>();
        CHECK_OK(session.get(), daw_get_transport(session.get(), &transport));
        CHECK(transport.playing == 0);
        CHECK(transport.frame == 0);
        CHECK(transport.duration == 2 * kProjectRate); // the imported region defines project length
        auto badShape = abi<daw_transport>();
        badShape.struct_size = sizeof(daw_transport) - 4;
        CHECK(daw_get_transport(session.get(), &badShape) == 1); // struct_size gate

        // 2. Seek is ephemeral: position moves, no revision, playback not required.
        CHECK_OK(session.get(), daw_seek_frame(session.get(), kProjectRate / 2));
        CHECK_OK(session.get(), daw_get_transport(session.get(), &transport));
        CHECK(transport.frame == kProjectRate / 2);
        CHECK(rev(session.get()) == initialRevision);
        CHECK_OK(session.get(), daw_seek_frame(session.get(), 0));
        CHECK_REJ(session.get(), daw_seek_frame(session.get(), 2 * kProjectRate + 1)); // past EOF rejects
        CHECK_OK(session.get(), daw_get_transport(session.get(), &transport));
        CHECK(transport.frame == 0); // a rejected seek must not move the playhead

        // 3. Loop is ephemeral half-open [start,end) over the transport.
        CHECK_REJ(session.get(), daw_set_loop(session.get(), 1, 1000, 999)); // end must exceed start
        CHECK_REJ(session.get(), daw_set_loop(session.get(), 1, 0, 2 * kProjectRate + 1)); // must fit the project
        CHECK_OK(session.get(), daw_set_loop(session.get(), 1, 1000, 2000));
        CHECK_OK(session.get(), daw_get_transport(session.get(), &transport));
        CHECK(transport.loop_enabled == 1 && transport.loop_start == 1000 && transport.loop_end == 2000);
        // Enabling a loop behind the playhead pulls the playhead to loop start.
        CHECK_OK(session.get(), daw_seek_frame(session.get(), 5000));
        CHECK_OK(session.get(), daw_set_loop(session.get(), 1, 1000, 2000));
        CHECK_OK(session.get(), daw_get_transport(session.get(), &transport));
        CHECK(transport.frame == 1000);
        CHECK_OK(session.get(), daw_set_loop(session.get(), 0, 0, 0));
        CHECK_OK(session.get(), daw_get_transport(session.get(), &transport));
        CHECK(transport.loop_enabled == 0);
        CHECK(rev(session.get()) == initialRevision);

        // 4. Meters and loudness report the documented no-renderer truth.
        auto meter = abi<daw_channel_meter>();
        CHECK_OK(session.get(), daw_get_channel_meter(session.get(), DAW_INSERT_OWNER_MASTER, 0, &meter));
        CHECK(meter.version == DAW_CHANNEL_METER_VERSION && meter.left_peak == 0.0f && meter.right_peak == 0.0f);
        auto loudness = abi<daw_master_loudness>();
        CHECK_OK(session.get(), daw_get_master_loudness(session.get(), &loudness));
        CHECK(loudness.live == 0 && loudness.momentary_lufs <= -199.0f && loudness.short_term_lufs <= -199.0f);

        // 5. Session-scoped flags: settable, readable, never durable.
        CHECK_OK(session.get(), daw_set_metronome(session.get(), 1));
        int metronome = 0;
        CHECK_OK(session.get(), daw_get_metronome(session.get(), &metronome));
        CHECK(metronome == 1);
        CHECK_OK(session.get(), daw_set_record_preroll(session.get(), 4800)); // 100 ms
        uint64_t preroll = 0;
        CHECK_OK(session.get(), daw_get_record_preroll(session.get(), &preroll));
        CHECK(preroll == 4800);
        CHECK_REJ(session.get(), daw_set_record_preroll(session.get(), 48000ull * 31)); // > 30 s cap
        CHECK_OK(session.get(), daw_set_record_monitor(session.get(), 1));
        int monitor = 0;
        CHECK_OK(session.get(), daw_get_record_monitor(session.get(), &monitor));
        CHECK(monitor == 1);
        CHECK_OK(session.get(), daw_set_auto_monitor_on_arm(session.get(), 0));
        int autoMonitor = 1;
        CHECK_OK(session.get(), daw_get_auto_monitor_on_arm(session.get(), &autoMonitor));
        CHECK(autoMonitor == 0);
        CHECK(rev(session.get()) == initialRevision); // flags are not project state

        const auto built = dumpOf(session.get());

        // 6. Play on a possibly device-less box: either it starts, lands in a
        // named failed/preparing state, or is rejected — but the session must
        // survive intact and remain editable either way.
        const int playRc = daw_play(session.get());
        auto started = abi<daw_transport>();
        daw_get_transport(session.get(), &started);
        if (playRc == 1) {
            // Rejection is clean if the device probe fails synchronously; a
            // bounded async preparation failure is reported via poll below.
            bool settled = false;
            for (int attempt = 0; attempt < 120 && !settled; ++attempt) {
                auto poll = abi<daw_transport>();
                CHECK_OK(session.get(), daw_get_transport(session.get(), &poll));
                auto out = abi<daw_output_status>();
                CHECK_OK(session.get(), daw_get_output_status(session.get(), &out));
                if (out.state != DAW_OUTPUT_PREPARING) settled = true;
            }
            CHECK(settled);
            auto out = abi<daw_output_status>();
            CHECK_OK(session.get(), daw_get_output_status(session.get(), &out));
            CHECK(out.state == DAW_OUTPUT_IDLE || out.state == DAW_OUTPUT_STOPPED ||
                  out.state == DAW_OUTPUT_PREPARATION_FAILED);
        }

        // Undo the transport intent, then prove the durable project is intact.
        daw_stop(session.get());
        auto afterPlay = abi<daw_transport>();
        CHECK_OK(session.get(), daw_get_transport(session.get(), &afterPlay));
        CHECK(afterPlay.playing == 0);
        CHECK(rev(session.get()) == initialRevision);
        CHECK(dumpOf(session.get()) == built);
        CHECK_OK(session.get(), daw_rename_track(session.get(), 1, "Renamed after transport", rev(session.get())));

        // 7. Ephemeral state must never leak into a saved draft or a reopen.
        CHECK_OK(session.get(), daw_set_loop(session.get(), 1, 100, 200));
        CHECK_OK(session.get(), daw_set_metronome(session.get(), 1));
        CHECK_OK(session.get(), daw_seek_frame(session.get(), 321));
        saveDraftAndWait(session.get(), root / "transport.mydawdraft");
        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), (root / "transport.mydawdraft").string().c_str()));
        auto reopenedTransport = abi<daw_transport>();
        CHECK_OK(reopened.get(), daw_get_transport(reopened.get(), &reopenedTransport));
        CHECK(reopenedTransport.frame == 0 && reopenedTransport.loop_enabled == 0 && reopenedTransport.playing == 0);
        int reopenedMetronome = 1;
        CHECK_OK(reopened.get(), daw_get_metronome(reopened.get(), &reopenedMetronome));
        CHECK(reopenedMetronome == 0);
        CHECK(rev(reopened.get()) == rev(session.get()));
        CHECK(snapshotOf(reopened.get()).can_undo == 0);

        // 8. Live hardware arc, manual gate only: opt in with DAW_E2E_DEVICE=1.
        if (const char* gate = std::getenv("DAW_E2E_DEVICE"); gate && gate[0] == '1') {
            CHECK_OK(session.get(), daw_seek_frame(session.get(), 0));
            CHECK_OK(session.get(), daw_set_loop(session.get(), 1, 0, 4800)); // wrap every 100 ms
            // The claimed graph reports playing before its first RT callback:
            // wait for a callback too, not just the flag.
            const auto wasPlaying = [&] {
                auto state = abi<daw_transport>();
                return daw_get_transport(session.get(), &state) == 0 && state.playing == 1 && state.callbacks > 0;
            };
            CHECK_OK(session.get(), daw_play(session.get()));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!wasPlaying() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto playing = abi<daw_transport>();
            CHECK_OK(session.get(), daw_get_transport(session.get(), &playing));
            CHECK(playing.playing == 1 && playing.callbacks > 0 && playing.loop_enabled == 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            auto advanced = abi<daw_transport>();
            CHECK_OK(session.get(), daw_get_transport(session.get(), &advanced));
            CHECK(advanced.callbacks > playing.callbacks);
            // Inside the loop the playhead wraps with the live block (2048) slack.
            CHECK(advanced.frame >= advanced.loop_start && advanced.frame < advanced.loop_end + 2048);
            // A metered master with a live 220 Hz tone reads above silence.
            auto liveMeter = abi<daw_channel_meter>();
            CHECK_OK(session.get(), daw_get_channel_meter(session.get(), DAW_INSERT_OWNER_MASTER, 0, &liveMeter));
            CHECK(liveMeter.left_peak > 0.0f && liveMeter.right_peak > 0.0f);
            auto liveLoudness = abi<daw_master_loudness>();
            CHECK_OK(session.get(), daw_get_master_loudness(session.get(), &liveLoudness));
            CHECK(liveLoudness.live == 1 && liveLoudness.momentary_lufs > -70.0f);
            CHECK_OK(session.get(), daw_stop(session.get()));
            auto stopped = abi<daw_transport>();
            CHECK_OK(session.get(), daw_get_transport(session.get(), &stopped));
            CHECK(stopped.playing == 0);
            // Stop must retain the playhead (still inside the loop window) and
            // callbacks must never run backwards across the session.
            CHECK(stopped.callbacks >= advanced.callbacks);
            CHECK(stopped.frame < advanced.loop_end + 2048);
            // Eof restart rule: parking at duration and playing resumes at 0.
            CHECK_OK(session.get(), daw_set_loop(session.get(), 0, 0, 0));
            CHECK_OK(session.get(), daw_seek_frame(session.get(), 2 * kProjectRate));
            CHECK_OK(session.get(), daw_play(session.get()));
            auto fromEof = abi<daw_transport>();
            const auto eofDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                CHECK_OK(session.get(), daw_get_transport(session.get(), &fromEof));
                if (fromEof.playing == 1) break;
                if (std::chrono::steady_clock::now() > eofDeadline) throw std::runtime_error("timeout waiting for playback start");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK(fromEof.frame < kProjectRate / 2); // restarted near zero, not stuck at EOF
            CHECK_OK(session.get(), daw_stop(session.get()));
        }

        std::cout << "PASS: e2e_transport — ephemeral state, meter zeros, flag persistence rules, device-less play contract"
                  << (std::getenv("DAW_E2E_DEVICE") ? " (+live hardware arc)" : "") << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}
