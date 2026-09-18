// Hardware is an explicit link-only fixture. All product actions below use
// the unmodified public bridge, actual callback processor, disk writer,
// SQLite, Undo and WAV exporter. This is not physical AUHAL/latency proof.
#include "e2e.hpp"
#include "../support/recording_device_fixture.h"
#include <array>
#include <limits>
using namespace e2e;

static daw_recording capture(daw_session* s) {
    auto result = abi<daw_recording>(); CHECK_OK(s, daw_get_recording(s, &result)); return result;
}
static daw_recording_progress progress(daw_session* s) {
    auto result = abi<daw_recording_progress>(); result.version = DAW_RECORDING_PROGRESS_VERSION;
    CHECK_OK(s, daw_get_recording_progress(s, &result)); return result;
}
static std::vector<float> pump(float value, uint32_t frames) {
    std::vector<float> in(frames, value), left(frames, -99), right(frames, -99);
    CHECK(recording_fixture_pump(in.data(), frames, left.data(), right.data()) == 0);
    CHECK(left == right); return left;
}
// Existing renderer start smoothing is part of playback/export, not captured PCM.
static bool sameContent(Dump a, const Dump& b) {
    a.revision = b.revision; return a == b;
}
static float fader(uint64_t processed) {
    float gain = 0;
    for (uint64_t n = 0; n < processed; ++n) gain += (1.0f - gain) * 0.004166667f;
    return gain;
}
int main() {
    try {
        TempRoot root("duplex");
        // 1. Record into an empty project; no dummy backing clip is needed.
        {
            Bridge session; auto* s = session.get(); const auto before = rev(s);
            auto idle = progress(s); CHECK(idle.capacity_frames == 0 && idle.limit_reached == 0);
            auto invalid = idle; invalid.version = 99;
            CHECK_REJ(s, daw_get_recording_progress(s, &invalid));
            invalid.version = DAW_RECORDING_PROGRESS_VERSION; --invalid.struct_size;
            CHECK_REJ(s, daw_get_recording_progress(s, &invalid));
            CHECK_REJ(s, daw_get_recording_progress(s, nullptr));
            const auto raw = (root / "empty.mydawtake").string();
            CHECK_OK(s, daw_record_start(s, 0, raw.c_str()));
            CHECK(recording_fixture_active() && rev(s) == before);
            const auto output = pump(0.25f, 1024);
            CHECK(std::all_of(output.begin(), output.end(), [](float v) { return v == 0; }));
            CHECK(capture(s).frames == 1024 && capture(s).loop_recording == 0 && capture(s).pass_count == 0);
            CHECK(progress(s).timeline_frame == 1024);
            CHECK_OK(s, daw_record_stop(s, "Vocal", before));
            CHECK(!recording_fixture_active() && rev(s) == before + 1 && snapshotOf(s).track_count == 1);
            CHECK(!std::filesystem::exists(raw));
            const auto recorded = dumpOf(s);
            const auto wave = exportProject(s, root / "empty.wav");
            CHECK(wave.frames() == 1024);
            for (size_t frame = 0; frame < wave.frames(); ++frame) {
                const float expected = 0.25f * fader(frame + 1);
                CHECK(std::abs(wave.samples[frame * 2] - expected) < 1e-7f);
                CHECK(wave.samples[frame * 2] == wave.samples[frame * 2 + 1]);
            }
            CHECK_OK(s, daw_undo(s, rev(s))); CHECK(snapshotOf(s).track_count == 0);
            CHECK_OK(s, daw_redo(s, rev(s))); CHECK(sameContent(dumpOf(s), recorded));
            saveDraftAndWait(s, root / "recorded.mydaw");
            Bridge reopened; CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), (root / "recorded.mydaw").c_str()));
            CHECK(sameContent(dumpOf(reopened.get()), recorded));
            CHECK(exportProject(reopened.get(), root / "reopen.wav").samples == wave.samples);
        }
        // 2. Backing playback + dry capture, pre-roll crossing a block boundary,
        // live MON on/off, input safety, and continuing beyond backing duration.
        {
            Bridge session; auto* s = session.get();
            const auto bedPath = root / "bed.wav";
            writeWavFixture(bedPath, std::vector<float>(1000 * 2, 0.125f), kProjectRate, 2, "f32");
            CHECK_OK(s, daw_import_wav(s, bedPath.c_str(), "Bed", rev(s)));
            const auto before = dumpOf(s); const auto revision = rev(s);
            CHECK_OK(s, daw_seek_frame(s, 55));
            CHECK_OK(s, daw_record_cancel(s)); // idle cancel must not seek
            auto idleTransport = abi<daw_transport>();
            CHECK_OK(s, daw_get_transport(s, &idleTransport)); CHECK(idleTransport.frame == 55);
            CHECK_REJ(s, daw_record_start(s, 137, nullptr));
            CHECK_REJ(s, daw_record_start(s, 137, ""));
            CHECK_OK(s, daw_set_record_preroll(s, 137));
            const auto raw = (root / "overdub.mydawtake").string();
            CHECK_OK(s, daw_record_start(s, 137, raw.c_str()));
            CHECK(progress(s).preroll_remaining_frames == 137);
            auto out = pump(0.25f, 64);
            CHECK(std::abs(out.front() - 0.125f * fader(1)) < 1e-7f && capture(s).frames == 0 && progress(s).timeline_frame == 64);
            out = pump(0.25f, 192);
            CHECK(std::abs(out.back() - 0.125f * fader(256)) < 1e-7f && capture(s).frames == 119 && progress(s).preroll_remaining_frames == 0);
            CHECK_REJ(s, daw_set_record_preroll(s, 1));
            CHECK_REJ(s, daw_seek_frame(s, 0)); CHECK_REJ(s, daw_set_loop(s, 1, 0, 1000));
            CHECK_REJ(s, daw_play(s)); CHECK_REJ(s, daw_stop(s));
            CHECK_REJ(s, daw_record_start(s, 0, (root / "nested").c_str()));
            CHECK_REJ(s, daw_midi_record_arm(s, 1, 0));
            CHECK_OK(s, daw_set_record_monitor(s, 1)); out = pump(0.25f, 512);
            CHECK(std::abs(out.back() - (0.125f * fader(768) + 0.25f)) < 0.000001f);
            CHECK_OK(s, daw_set_record_monitor(s, 0)); out = pump(0.25f, 512);
            CHECK(out.back() == 0); // clock keeps advancing beyond 1000-frame bed
            auto transport = abi<daw_transport>(); CHECK_OK(s, daw_get_transport(s, &transport));
            CHECK(transport.playing && transport.frame == 1280 && capture(s).frames == 1143);
            CHECK(dumpOf(s) == before && rev(s) == revision);
            CHECK_OK(s, daw_record_stop(s, "Overdub", revision));
            CHECK(snapshotOf(s).track_count == 2);
            const auto recordedID = snapshotOf(s).track_count; // IDs 1, 2 in this fixture
            CHECK_OK(s, daw_set_solo_exclusive(s, recordedID, 1, rev(s)));
            const auto audio = exportProject(s, root / "dry.wav");
            CHECK(audio.frames() == 1280);
            for (size_t frame = 0; frame < 1280; ++frame) {
                const float expected = frame < 137 ? 0 : 0.25f * fader(frame + 1);
                CHECK(std::abs(audio.samples[frame * 2] - expected) < 1e-7f && audio.samples[frame * 2] == audio.samples[frame * 2 + 1]);
            }
        }
        // 3. An ordinary take is ONE take, not splitLoopPasses(length=0).
        // Existing comp remains unchanged, and loop takes still batch atomically.
        {
            Bridge session; auto* s = session.get();
            writeWavFixture(root / "target.wav", std::vector<float>(4096 * 2, 0.1f), kProjectRate, 2, "f32");
            CHECK_OK(s, daw_import_wav(s, (root / "target.wav").c_str(), "Lead", rev(s)));
            const auto original = dumpOf(s);
            CHECK_OK(s, daw_record_start_take(s, 1, 100, (root / "single.mydawtake").c_str()));
            pump(0.5f, 512); CHECK(capture(s).target_track_id == 1 && !capture(s).loop_recording);
            CHECK_OK(s, daw_record_stop(s, "Take", rev(s)));
            auto take = abi<daw_take>(); CHECK_OK(s, daw_get_take(s, 1, 1, &take));
            CHECK(take.start == 100 && take.frames == 512);
            CHECK(dumpOf(s).tracks.front().clips == original.tracks.front().clips);
            CHECK_OK(s, daw_undo(s, rev(s))); CHECK(sameContent(dumpOf(s), original));
            CHECK_OK(s, daw_redo(s, rev(s)));
            const auto beforeLoop = dumpOf(s); const auto revision = rev(s);
            CHECK_OK(s, daw_set_loop(s, 1, 100, 356));
            CHECK_OK(s, daw_set_record_preroll(s, 37));
            CHECK_OK(s, daw_record_start_take(s, 1, 100, (root / "loop.mydawtake").c_str()));
            pump(0.5f, 640); // 37 pre-roll; 256 + 256 + 91 captured
            CHECK(capture(s).frames == 603 && capture(s).loop_recording && capture(s).pass_count == 3);
            CHECK(progress(s).timeline_frame == 191);
            CHECK_OK(s, daw_record_stop(s, "Loop", revision));
            CHECK(rev(s) == revision + 1 && trackById(s, 1).take_count == 5);
            CHECK_OK(s, daw_get_take(s, 1, 4, &take)); CHECK(take.start == 100 && take.frames == 91);
            CHECK_OK(s, daw_undo(s, rev(s))); CHECK(sameContent(dumpOf(s), beforeLoop));
        }
        // 4. No clip from pre-roll-only Stop; the intentional cap is not xrun.
        {
            Bridge session; auto* s = session.get(); auto revision = rev(s);
            CHECK_OK(s, daw_set_record_preroll(s, 1000));
            const auto raw = (root / "preroll.mydawtake").string();
            CHECK_OK(s, daw_record_start(s, 1000, raw.c_str())); pump(0.2f, 512);
            CHECK_OK(s, daw_record_stop(s, "No clip", revision));
            CHECK(rev(s) == revision && snapshotOf(s).track_count == 0 && !std::filesystem::exists(raw));
            CHECK_OK(s, daw_set_record_preroll(s, 0));
            constexpr uint64_t end = 48000ULL * 600;
            CHECK_OK(s, daw_record_start(s, end - 9, (root / "limit.mydawtake").c_str()));
            const auto out = pump(0.25f, 17);
            CHECK(capture(s).frames == 9 && !capture(s).overflowed);
            CHECK(progress(s).capacity_frames == 9 && progress(s).limit_reached && progress(s).timeline_frame == end);
            CHECK(pump(0.25f, 17) == std::vector<float>(17, 0));
            CHECK_OK(s, daw_record_stop(s, "Limited", revision));
        }
        // 5. Stale project, start error, device loss and cancel retain recovery.
        {
            Bridge session; auto* s = session.get();
            recording_fixture_failure(1);
            const auto raw = (root / "refused.mydawtake").string();
            CHECK_REJ(s, daw_record_start(s, 0, raw.c_str()));
            CHECK(!recording_fixture_active() && !capture(s).recording && !std::filesystem::exists(raw));
            recording_fixture_failure(0);
            const auto recover = (root / "recover.mydawtake").string();
            CHECK_OK(s, daw_record_start(s, 100, recover.c_str())); pump(0.25f, 512);
            CHECK_OK(s, daw_add_track(s, "External edit", rev(s)));
            CHECK_REJ(s, daw_record_stop(s, "Wrong revision", rev(s)));
            CHECK(capture(s).recording);
            CHECK_OK(s, daw_record_cancel(s)); CHECK(!recording_fixture_active() && std::filesystem::exists(recover));
            CHECK_OK(s, daw_recover_take(s, recover.c_str(), "Recovered", rev(s)));
            CHECK(trackById(s, 2).audio_frames == 512);
            const auto lost = (root / "lost.mydawtake").string();
            CHECK_OK(s, daw_record_start(s, 100, lost.c_str())); pump(0.25f, 512);
            recording_fixture_failure(2); auto status = abi<daw_recording>();
            CHECK_REJ(s, daw_get_recording(s, &status));
            CHECK_OK(s, daw_record_cancel(s)); CHECK(std::filesystem::exists(lost));
            recording_fixture_failure(0);
        }
        // 6. A discontinuity must never become a successful stopped take,
        // even if Stop is called directly before the UI's next status poll.
        for (int fault : {3, 4}) for (bool pollFirst : {false, true}) {
            Bridge session; auto* s = session.get();
            const auto original = dumpOf(s); const auto revision = rev(s);
            const auto raw = (root / "clock.mydawtake").string();
            CHECK_OK(s, daw_record_start(s, 100, raw.c_str())); pump(0.25f, 512);
            recording_fixture_failure(fault);
            CHECK(pump(0.75f, 64) == std::vector<float>(64, 0));
            recording_fixture_failure(0);
            CHECK(pump(0.75f, 64) == std::vector<float>(64, 0));
            CHECK(progress(s).timeline_frame == 612);
            if (pollFirst) { auto status = abi<daw_recording>(); CHECK_REJ(s, daw_get_recording(s, &status)); }
            else CHECK_REJ(s, daw_record_stop(s, "Must not commit", revision));
            CHECK_OK(s, daw_record_cancel(s));
            CHECK(!recording_fixture_active() && rev(s) == revision && sameContent(dumpOf(s), original));
            CHECK(std::filesystem::exists(raw));
            CHECK_OK(s, daw_recover_take(s, raw.c_str(), "Clock prefix", rev(s)));
            const auto recovered = dumpOf(s);
            CHECK(trackById(s, 1).audio_frames == 512);
            const auto audio = exportProject(s, root / "clock-prefix.wav");
            CHECK(audio.frames() == 612);
            for (size_t frame = 0; frame < 612; ++frame) {
                const float expected = frame < 100 ? 0 : 0.25f * fader(frame + 1);
                CHECK(std::abs(audio.samples[frame * 2] - expected) < 1e-7f);
                CHECK(audio.samples[frame * 2] == audio.samples[frame * 2 + 1]);
            }
            saveDraftAndWait(s, root / "clock-project.mydaw");
            CHECK_OK(s, daw_undo(s, rev(s))); CHECK(sameContent(dumpOf(s), original));
            CHECK_OK(s, daw_redo(s, rev(s))); CHECK(sameContent(dumpOf(s), recovered));
            CHECK_OK(s, daw_open_draft(s, (root / "clock-project.mydaw").c_str()));
            CHECK(sameContent(dumpOf(s), recovered));
        }
        std::cout << "PASS: duplex recording bridge, dry PCM, pre-roll, live MON, loop/normal commit, Undo/reopen/export, cap and recovery (simulated device, real core).\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
