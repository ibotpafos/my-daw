// e2e: durability of everything we claim to put on disk.
//
// Journey: build a mid-size project (audio + automation + MIDI + bus/routing;
// AUs are deliberately absent because hosting is Apple-only and belongs to
// other suites) → async save semantics (captured revision, sequential same
// path, failed destination) → corrupt-file preservation (a rejected open must
// not touch the live session) → .mydawzip interchange (package, extract,
// reopen, and export bit-identically against the original draft) → DAWproject
// export success/empty/cancel → process-crash-during-save honesty (child
// processes start a save and die without polling; the target is ALWAYS absent
// or fully valid, never half-written) → .mydawtake recovery negatives.
//
// Take RECOVERY via the ABI needs a real .mydawtake; the only writer is the
// record path which requires an input device, and fabricating the binary
// format would mean including engine internals — explicitly forbidden for
// e2e. So this scenario asserts the honest negative surface only (missing +
// garbage files reject and leave the revision untouched), matching
// tests/recording_recovery.cpp's corrupt-file expectations.
#include "e2e.hpp"

#include <algorithm>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <vector>

extern char** environ;

namespace {

std::vector<unsigned char> readBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) throw std::runtime_error("cannot read " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream.good()) throw std::runtime_error("cannot write " + path.string());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream.good()) throw std::runtime_error("short write " + path.string());
}

bool startsWithZip(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    char magic[4] = {0, 0, 0, 0};
    stream.read(magic, 2);
    return stream.gcount() == 2 && magic[0] == 'P' && magic[1] == 'K';
}

bool containsBytes(const std::vector<unsigned char>& haystack, const std::string& needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

// Polls any save job to a TERMINAL outcome without judging it (the shared
// waitSave helper throws on failure, which is exactly what some steps here
// need to observe).
daw_save_status waitSaveOutcome(daw_save_job* job, int timeoutMs = 30000) {
    daw_save_status status = e2e::abi<daw_save_status>();
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        CHECK(daw_poll_save(job, &status) == 0);
        if (status.status != 0) return status;
        if (std::chrono::steady_clock::now() > end) throw std::runtime_error("timeout waiting for save outcome");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Child mode for the crash-during-save loop: build a project through the ABI,
// start a save, and die ~20 ms later without polling, releasing, or
// destroying anything. The sleep is the crash window itself (fault
// injection), not logic: the parent's promise is a disjunction — file absent
// OR fully openable — so it holds whichever side of the rename the kill
// lands on.
int saveCrashChild(char** argv) {
    e2e::Bridge kid;
    CHECK(kid.get() != nullptr);
    CHECK_OK(kid.get(), daw_import_wav(kid.get(), argv[2], argv[4], 0));
    CHECK_OK(kid.get(), daw_add_track(kid.get(), argv[4], 1));
    daw_save_job* job = daw_begin_save(kid.get(), argv[3]);
    if (job) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ::_exit(job ? 0 : 3);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--save-crash-child") return saveCrashChild(argv);
    try {
        using namespace e2e;
        TempRoot root("persist");
        const char* self = argc > 0 ? argv[0] : "./e2e_persistence_recovery";

        // 1. Deterministic 48 kHz stereo fixtures — the cross-platform backbone.
        const auto tonePath = root / "tone.wav";
        const auto padPath = root / "pad.wav";
        writeWavFixture(tonePath, sineInterleaved(2 * kProjectRate, kProjectRate, 440.0, 0.5, 2), kProjectRate, 2, "f32");
        writeWavFixture(padPath, sineInterleaved(3 * kProjectRate, kProjectRate, 110.0, 0.4, 2), kProjectRate, 2, "f32");

        // 2. A mid-size project touching every durable subsystem.
        Bridge session;
        daw_session* const s = session.get();
        CHECK_OK(s, daw_import_wav(s, tonePath.string().c_str(), "Lead", rev(s)));
        const auto lead = trackById(s, 1).id;
        CHECK_OK(s, daw_import_wav(s, padPath.string().c_str(), "Pad", rev(s)));
        const auto pad = trackById(s, 2).id;
        const auto bus = addBus(s, "Drum Bus");
        CHECK_OK(s, daw_set_track_output(s, pad, bus, rev(s)));
        CHECK_OK(s, daw_upsert_send(s, lead, bus, -6.0, 0, rev(s)));
        CHECK_OK(s, daw_upsert_track_volume_automation_point(s, lead, 0, -6.0, rev(s)));
        CHECK_OK(s, daw_upsert_track_volume_automation_point(s, lead, kProjectRate, 0.0, rev(s)));
        CHECK_OK(s, daw_upsert_track_pan_automation_point(s, lead, kProjectRate / 2, 0.5, rev(s)));
        CHECK_OK(s, daw_upsert_bus_gain_automation_point(s, bus, 4800, 1.5, rev(s)));
        CHECK_OK(s, daw_upsert_master_gain_automation_point(s, 9600, -1.0, rev(s)));
        const auto synth = addTrack(s, "Synth");
        {
            auto clip = abi<daw_midi_clip>();
            clip.version = DAW_MIDI_CLIP_VERSION;
            clip.start = 0;
            clip.length = kProjectRate;
            clip.lane = 0;
            clip.note_count = 4;
            daw_midi_note notes[4] = {
                {sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 0, kProjectRate / 8, 48, 0, 99},
                {sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, kProjectRate / 4, kProjectRate / 8, 55, 1, 98},
                {sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, kProjectRate / 2, kProjectRate / 8, 62, 2, 97},
                {sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 3 * kProjectRate / 4, kProjectRate / 8, 69, 3, 96}};
            CHECK_OK(s, daw_add_midi_clip(s, synth, &clip, notes, 4, rev(s)));
        }
        CHECK_OK(s, daw_set_tempo(s, kProjectRate, 128.0, rev(s)));
        CHECK_OK(s, daw_add_marker(s, 0, "Top", rev(s)));
        CHECK_OK(s, daw_set_master_gain(s, -2.0, rev(s)));
        const Dump built = dumpOf(s);
        CHECK(built.tracks.size() == 3 && built.buses.size() == 1);

        // 3. Async save semantics. The job captures the revision at begin and
        //    reports exactly that on completion (docs/20: "завершение отмечает
        //    сохранённой только захваченную revision").
        const auto draft = root / "project.mydawdraft";
        const uint64_t savedRev = rev(s);
        auto firstSave = saveDraftAndWait(s, draft);
        CHECK(firstSave.status == 1);
        CHECK(firstSave.revision == savedRev);
        {
            Bridge probe;
            CHECK_OK(probe.get(), daw_open_draft(probe.get(), draft.string().c_str()));
            CHECK(dumpOf(probe.get()) == built); // exact, revision included
        }

        // 4. Sequential saves to the SAME path are fine (the ABI header only
        //    requires the caller to serialize per destination; the "one
        //    manual-save job" rule is app policy, not engine policy).
        CHECK_OK(s, daw_rename_track(s, lead, "Lead vox", rev(s)));
        const Dump built2 = dumpOf(s);
        auto secondSave = saveDraftAndWait(s, draft);
        CHECK(secondSave.status == 1 && secondSave.revision == built2.revision);
        {
            Bridge probe;
            CHECK_OK(probe.get(), daw_open_draft(probe.get(), draft.string().c_str()));
            CHECK(dumpOf(probe.get()) == built2);
        }

        // 5. ABI shape gates: NULL job/status or wrong struct_size reject as
        //    pure-c-bridge rejections (rc 1, no session error text required).
        {
            daw_save_status bad = abi<daw_save_status>();
            bad.struct_size = sizeof(daw_save_status) - 4;
            CHECK(daw_poll_save(nullptr, &bad) == 1);
            CHECK(daw_poll_save(nullptr, nullptr) == 1);
            CHECK(daw_poll_save(nullptr, &bad) == 1);
        }

        // 6. A destination whose directory does not exist must FAIL THE JOB
        //    (status 2 + error text) and leave no target behind.
        {
            const auto nowhere = root / "no-such-dir" / "doomed.mydawdraft";
            auto* job = daw_begin_save(s, nowhere.string().c_str());
            CHECK(job != nullptr); // begin itself is accepted; failure is async per docs/20
            const auto failed = waitSaveOutcome(job);
            CHECK(failed.status == 2);
            CHECK(failed.error[0] != '\0');
            daw_release_save(job);
            std::error_code ec;
            CHECK(!std::filesystem::exists(nowhere, ec));
            CHECK(!std::filesystem::exists(root / "no-such-dir", ec));
            CHECK(dumpOf(s) == built2); // a failed job cannot touch the session
        }

        // 7. Corrupt-file preservation (PRJ-02 adjacent): a rejected open
        //    leaves the LIVE session byte-identical, and the same session can
        //    still open a good draft afterwards.
        const auto corruptPath = root / "corrupt.mydawdraft";
        saveDraftAndWait(s, corruptPath);
        {
            const size_t good = std::filesystem::file_size(corruptPath);
            CHECK(good > 200);
            // Simplest robust corruption the readers must reject: truncate to
            // half and append garbage (keeps the SQLite magic, breaks pages).
            std::filesystem::resize_file(corruptPath, good / 2);
            {
                std::ofstream junk(corruptPath, std::ios::binary | std::ios::app);
                for (size_t i = 0; i < 8192; ++i) junk.put(static_cast<char>((i * 7 + 13) & 0xFF));
                CHECK(junk.good());
            }
            const Dump before = dumpOf(s);
            CHECK_REJ(s, daw_open_draft(s, corruptPath.string().c_str()));
            CHECK(dumpOf(s) == before); // state AND revision untouched by a failed open
            CHECK(snapshotOf(s).revision == before.revision);
            // Recovery: the good draft still opens in this very session.
            CHECK_OK(s, daw_open_draft(s, draft.string().c_str()));
            CHECK(dumpOf(s) == built2); // open replaces state + clears history...
            CHECK(snapshotOf(s).can_undo == 0 && snapshotOf(s).can_redo == 0);
        }

        // 8. .mydawzip interchange.
        const auto zip = root / "exchange.mydawzip";
        CHECK_OK(s, daw_package_project(s, draft.string().c_str(), zip.string().c_str()));
        CHECK(startsWithZip(zip));
        CHECK(std::filesystem::file_size(zip) > 0);
        const auto extracted = root / "restored.mydawdraft";
        CHECK_OK(s, daw_extract_package(s, zip.string().c_str(), extracted.string().c_str()));
        {
            Bridge viaZip;
            CHECK_OK(viaZip.get(), daw_open_draft(viaZip.get(), extracted.string().c_str()));
            CHECK(dumpOf(viaZip.get()) == built2); // full fidelity through the archive
        }
        // docs/77 promises the target is PUBLISHED ATOMICALLY (temp+rename),
        // and tests/project_package_tests.cpp proves a second extraction over
        // an existing target republishes cleanly — the engine itself has no
        // non-collision requirement; only the UI picks unique names.
        CHECK_OK(s, daw_extract_package(s, zip.string().c_str(), extracted.string().c_str()));
        {
            Bridge again;
            CHECK_OK(again.get(), daw_open_draft(again.get(), extracted.string().c_str()));
            CHECK(dumpOf(again.get()) == built2);
        }
        // Negative gates (all must leave readable error text behind).
        {
            const auto neverZip = root / "never.zip";
            CHECK_REJ(s, daw_package_project(s, (root / "never-written.mydawdraft").string().c_str(), neverZip.string().c_str()));
            CHECK(bridgeError(s).find("not found") != std::string::npos); // mirrors the white-box gate
            std::error_code ec;
            CHECK(!std::filesystem::exists(neverZip, ec));

            const auto absentTarget = root / "from-absent-zip.mydawdraft";
            CHECK_REJ(s, daw_extract_package(s, (root / "absent.mydawzip").string().c_str(), absentTarget.string().c_str()));
            CHECK(!std::filesystem::exists(absentTarget, ec));

            const auto notZipTarget = root / "from-nonzip.mydawdraft";
            CHECK_REJ(s, daw_extract_package(s, draft.string().c_str(), notZipTarget.string().c_str())); // a SQLite file is not a ZIP
            CHECK(bridgeError(s).find("central directory") != std::string::npos);
            CHECK(!std::filesystem::exists(notZipTarget, ec));

            const auto crcZipBytes = readBytes(zip);
            auto damaged = crcZipBytes;
            damaged[200] ^= 0x40; // one bit inside the stored draft entry
            const auto damagedZip = root / "damaged.mydawzip";
            {
                std::ofstream out(damagedZip, std::ios::binary);
                out.write(reinterpret_cast<const char*>(damaged.data()), static_cast<std::streamsize>(damaged.size()));
                CHECK(out.good());
            }
            const auto crcTarget = root / "from-damaged.mydawdraft";
            CHECK_REJ(s, daw_extract_package(s, damagedZip.string().c_str(), crcTarget.string().c_str()));
            CHECK(bridgeError(s).find("CRC mismatch") != std::string::npos);
            CHECK(!std::filesystem::exists(crcTarget, ec));
        }

        // 9. DAWproject export: success surface on the mid-size project.
        const auto dpPath = root / "out.dawproject";
        {
            auto* job = daw_begin_dawproject_export(s, dpPath.string().c_str(), 128.0, 4, 4, "Persistence Suite");
            CHECK(job != nullptr);
            const auto status = waitDawproject(job);
            daw_release_dawproject_export(job);
            CHECK(status.status == 1);
            CHECK(status.revision == built2.revision); // frozen at begin (docs/41)
            CHECK(status.total_entries >= 4);
            CHECK(status.completed_entries == status.total_entries);
            // warning/info counters are readable through the versioned status.
            CHECK(status.error[0] == '\0');
            CHECK(startsWithZip(dpPath));
            CHECK(std::filesystem::file_size(dpPath) > 200);
            const auto bytes = readBytes(dpPath);
            for (const std::string marker : {"project.xml", "metadata.xml", "loss-report.json", "audio/"})
                CHECK(containsBytes(bytes, marker));
        }
        // An empty-but-valid project exports too.
        {
            Bridge empty;
            const auto emptyPath = root / "empty.dawproject";
            auto* job = daw_begin_dawproject_export(empty.get(), emptyPath.string().c_str(), 120.0, 4, 4, "Nothing");
            CHECK(job != nullptr);
            const auto status = waitDawproject(job);
            daw_release_dawproject_export(job);
            CHECK(status.status == 1 && status.revision == 0);
            CHECK(status.completed_entries == status.total_entries);
            CHECK(startsWithZip(emptyPath));
        }
        // The cancel path publishes NOTHING: a pre-existing file at the
        // destination survives byte-for-byte.
        {
            const auto cancelPath = root / "cancelled.dawproject";
            const std::string sentinel = "SENTINEL-MUST-SURVIVE-CANCEL";
            writeText(cancelPath, sentinel);
            auto* job = daw_begin_dawproject_export(s, cancelPath.string().c_str(), 128.0, 4, 4, "Cancelled");
            CHECK(job != nullptr);
            daw_cancel_dawproject_export(job);
            const auto status = waitDawproject(job, 3); // poll loop accepts canceled(3)
            daw_release_dawproject_export(job);
            CHECK(status.status == 3);
            const std::vector<unsigned char> want(sentinel.begin(), sentinel.end());
            CHECK(readBytes(cancelPath) == want); // byte-for-byte untouched
        }

        // 10. Crash-during-save honesty: five successive children each build
        //     a fresh project through the ABI and start a save over the SAME
        //     path, then die mid-write. After every kill the draft is either
        //     absent or a fully openable SQLite file — never half-broken.
        const auto crashFixture = root / "crash-tone.wav";
        writeWavFixture(crashFixture, sineInterleaved(5 * kProjectRate, kProjectRate, 220.0, 0.5, 2), kProjectRate, 2, "f32");
        const auto crashDraft = root / "crash.mydawdraft";
        for (int iteration = 1; iteration <= 5; ++iteration) {
            const std::string name = "Killed " + std::to_string(iteration);
            std::vector<std::string> strings = {self, "--save-crash-child", crashFixture.string(), crashDraft.string(), name};
            std::vector<char*> args;
            for (auto& value : strings) args.push_back(value.data());
            args.push_back(nullptr);
            pid_t child = -1;
            CHECK(::posix_spawn(&child, self, nullptr, nullptr, args.data(), environ) == 0);
            int waitStatus = 0;
            CHECK(::waitpid(child, &waitStatus, 0) == child);
            CHECK(WIFEXITED(waitStatus) && WEXITSTATUS(waitStatus) == 0);
            std::error_code ec;
            if (std::filesystem::exists(crashDraft, ec)) {
                Bridge probe;
                CHECK_OK(probe.get(), daw_open_draft(probe.get(), crashDraft.string().c_str()));
            }
        }
        // And the calm path: a COMPLETED save over that same repeatedly-killed
        // destination always reopens with full fidelity (dumpOf equality).
        {
            Bridge writer;
            CHECK_OK(writer.get(), daw_import_wav(writer.get(), crashFixture.string().c_str(), "Reliable", 0));
            CHECK_OK(writer.get(), daw_add_track(writer.get(), "Survivor", 1));
            const Dump final = dumpOf(writer.get());
            auto status = saveDraftAndWait(writer.get(), crashDraft);
            CHECK(status.status == 1 && status.revision == final.revision);
            Bridge reader;
            CHECK_OK(reader.get(), daw_open_draft(reader.get(), crashDraft.string().c_str()));
            CHECK(dumpOf(reader.get()) == final);
        }

        // 11. Take-recovery honesty through the ABI only. Producing a real
        //     .mydawtake requires daw_record_* (input hardware) or the engine
        //     RecordingWriter (internal header — forbidden here), so the
        //     positive path is documented-skipped; the negative surface is
        //     asserted instead, mirroring tests/recording_recovery.cpp's
        //     "corrupt rejection" and "missing file" gates.
        {
            const Dump before = dumpOf(s);
            CHECK_REJ(s, daw_recover_take(s, (root / "absent.mydawtake").string().c_str(), "Ghost", before.revision));
            CHECK(dumpOf(s) == before);
            const auto garbageTake = root / "garbage.mydawtake";
            writeText(garbageTake, "bad"); // same 3-byte pattern the white-box test uses
            CHECK_REJ(s, daw_recover_take(s, garbageTake.string().c_str(), "Garbage", before.revision));
            CHECK(dumpOf(s) == before); // rejected: revision and project untouched
        }

        // 12. Close the interchange loop: the packaged-and-extracted draft and
        //     the original draft must render bit-identically in fresh
        //     sessions (float32 export = no dither, so equality is exact).
        {
            Bridge original;
            CHECK_OK(original.get(), daw_open_draft(original.get(), draft.string().c_str()));
            const auto wavA = exportProject(original.get(), root / "from-draft.wav");
            Bridge package;
            CHECK_OK(package.get(), daw_open_draft(package.get(), extracted.string().c_str()));
            const auto wavB = exportProject(package.get(), root / "from-package.wav");
            CHECK(wavA.sampleRate == kProjectRate && wavB.sampleRate == kProjectRate);
            CHECK(wavA.frames() == wavB.frames() && wavA.channels == wavB.channels);
            CHECK(wavA.frames() >= 2 * kProjectRate);
            CHECK(channelPeak(wavA, 0, static_cast<uint32_t>(wavA.frames()), 0) > 0.05); // non-trivial guard
            for (size_t i = 0; i < wavA.samples.size(); ++i)
                if (wavA.samples[i] != wavB.samples[i])
                    throw std::runtime_error("packaged render differs at frame " + std::to_string(i / wavA.channels));
        }

        std::cout << "PASS: e2e_persistence_recovery — async save, failed/corrupt/openable honesty, .mydawzip round trip, DAWproject success+empty+cancel, 5x crash-during-save, take negatives, bit-identical interchange render\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}