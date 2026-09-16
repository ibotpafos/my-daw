// e2e: IMP-01 — getting external audio into a project, through the public ABI.
//
// A user's import journey, driven only the way the macOS app drives it
// (daw.h): synchronous WAV import onto a new track, import-take onto an
// existing track, the cancellable background job (begin -> poll -> edit ->
// apply), every honest failure (stale revision, cancel, corrupt file, vanished
// target track, replaced project), the cached peak reads the waveform view is
// built on, and finally save + reopen + export so the imported audio provably
// became durable project data.
//
// Every expectation comes from our OWN fixture reader (e2e.hpp) plus the
// documented contracts in docs/62, docs/63 and docs/67; the engine's codecs are
// never used to check themselves.
#include "e2e.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Raw byte access, so a scenario can hand the engine a file it corrupted
// itself. No engine decoder is involved in producing or reading these bytes.
std::vector<unsigned char> readFileBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) throw std::runtime_error("cannot read " + path.string());
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

void writeFileBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream.good()) throw std::runtime_error("cannot write " + path.string());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(stream.good());
}

// Independent replica of the documented waveform cache ("exactly 512 cached,
// linear, pre-gain stereo absolute peaks"): bin b covers
// [b*frames/512, (b+1)*frames/512) and holds the loudest of both channels,
// with a mono source duplicated to stereo by the decoder.
std::vector<float> expectedPeaks(const e2e::Wav& wav) {
    const uint64_t frames = wav.frames();
    std::vector<float> peaks(512, 0.0f);
    for (uint64_t bin = 0; bin < 512; ++bin) {
        const uint64_t begin = bin * frames / 512;
        const uint64_t end = (bin + 1) * frames / 512;
        for (uint64_t frame = begin; frame < end; ++frame) {
            for (uint16_t channel = 0; channel < 2; ++channel) {
                const uint16_t source = std::min(channel, static_cast<uint16_t>(wav.channels - 1));
                const float value = wav.samples[static_cast<size_t>(frame) * wav.channels + source];
                peaks[bin] = std::max(peaks[bin], std::abs(value));
            }
        }
    }
    return peaks;
}

// Commands that mint unpredictable ids are read back by their user-visible name.
uint64_t trackNamed(daw_session* session, const std::string& name) {
    const auto snap = e2e::snapshotOf(session);
    for (uint32_t index = 0; index < snap.track_count; ++index) {
        auto track = e2e::abi<daw_track>();
        CHECK_OK(session, daw_get_track(session, index, &track));
        if (name == track.name) return track.id;
    }
    throw std::runtime_error("no track named " + name);
}

uint32_t countNamedTracks(daw_session* session, const std::string& name) {
    const auto snap = e2e::snapshotOf(session);
    uint32_t found = 0;
    for (uint32_t index = 0; index < snap.track_count; ++index) {
        auto track = e2e::abi<daw_track>();
        CHECK_OK(session, daw_get_track(session, index, &track));
        if (name == track.name) ++found;
    }
    return found;
}

} // namespace

int main() {
    try {
        using namespace e2e;

        // -------------------------------------------------------------------
        // 1. Fixture material: fixed math, 48 kHz backbone, PCM16 containers.
        // -------------------------------------------------------------------
        TempRoot root("media-import");
        const auto tonePath = root / "tone-stereo.wav";
        writeWavFixture(tonePath, sineInterleaved(kProjectRate, kProjectRate, 440.0, 0.5, 2), kProjectRate, 2, "pcm16");
        const auto deskPath = root / "dc-mono.wav";
        writeWavFixture(deskPath, constantInterleaved(kProjectRate / 2, 1, 0.25f, 0.0f), kProjectRate, 1, "pcm16");
        const auto takePath = root / "take-stereo.wav";
        writeWavFixture(takePath, sineInterleaved(kProjectRate / 4, kProjectRate, 880.0, 0.25, 2), kProjectRate, 2, "pcm16");
        const auto backgroundPath = root / "background.wav";
        writeWavFixture(backgroundPath, constantInterleaved(kProjectRate / 4, 2, 0.5f, -0.5f), kProjectRate, 2, "pcm16");

        // Our writer and our reader agree; everything below is checked against
        // this independent view of the same bytes the engine is handed.
        const Wav toneFixture = readWavFixture(tonePath);
        CHECK(toneFixture.sampleRate == kProjectRate && toneFixture.channels == 2 && toneFixture.bits == 16);
        CHECK(!toneFixture.floating && toneFixture.frames() == kProjectRate);
        const Wav deskFixture = readWavFixture(deskPath);
        CHECK(deskFixture.channels == 1 && deskFixture.frames() == kProjectRate / 2);
        const Wav takeFixture = readWavFixture(takePath);
        CHECK(takeFixture.frames() == kProjectRate / 4);

        Bridge bridge;
        daw_session* session = bridge.get();
        CHECK(snapshotOf(session).revision == 0 && snapshotOf(session).track_count == 0);

        // -------------------------------------------------------------------
        // 2. daw_import_wav: one command creates track + base take + region,
        //    and the 48 kHz fast path must not add or drop a single frame.
        // -------------------------------------------------------------------
        CHECK_OK(session, daw_import_wav(session, tonePath.string().c_str(), "Tone", rev(session)));
        CHECK(rev(session) == 1);
        const auto tone = trackById(session, 1);
        CHECK(std::string(tone.name) == "Tone");
        CHECK(tone.clip_count == 1 && tone.take_count == 1);
        CHECK(tone.audio_frames == kProjectRate);
        auto region = abi<daw_clip>();
        CHECK_OK(session, daw_get_clip(session, 1, 0, &region));
        CHECK(region.start == 0 && region.source_offset == 0 && region.length == kProjectRate);
        CHECK(region.fade_in == 0 && region.fade_out == 0 && region.take_index == 0);
        CHECK(region.gain_db == 0.0 && region.pan == 0.0 && region.muted == 0 && region.looped == 0);

        // -------------------------------------------------------------------
        // 3. A mono source becomes the same canonical stereo clip, and the
        //    cached peaks the waveform draws are exact for a DC fixture.
        // -------------------------------------------------------------------
        CHECK_OK(session, daw_import_wav(session, deskPath.string().c_str(), "Desk", rev(session)));
        CHECK(rev(session) == 2);
        const auto desk = trackById(session, 2);
        CHECK(desk.audio_frames == kProjectRate / 2 && desk.clip_count == 1 && desk.take_count == 1);
        std::vector<float> peaks(512, -1.0f);
        CHECK_OK(session, daw_get_waveform(session, 2, peaks.data(), 512));
        const auto deskPeaks = expectedPeaks(deskFixture);
        for (size_t bin = 0; bin < 512; ++bin) CHECK(peaks[bin] == deskPeaks[bin] && peaks[bin] == 0.25f);

        // A sine's cache must match our own binning of the fixture, bin for bin.
        const auto tonePeaks = expectedPeaks(toneFixture);
        CHECK(tonePeaks[0] > 0.0f && tonePeaks[0] <= 0.5f);
        std::vector<float> readPeaks(512, 0.0f);
        CHECK_OK(session, daw_get_waveform(session, 1, readPeaks.data(), 512));
        for (size_t bin = 0; bin < 512; ++bin) CHECK(readPeaks[bin] == tonePeaks[bin]);

        // Waveform ABI: exactly 512 floats, a real buffer, an existing track.
        CHECK_REJ(session, daw_get_waveform(session, 1, peaks.data(), 511));
        CHECK_REJ(session, daw_get_waveform(session, 1, nullptr, 512));
        CHECK_REJ(session, daw_get_waveform(session, 9999, peaks.data(), 512));
        CHECK(rev(session) == 2); // reads never spend a revision

        // -------------------------------------------------------------------
        // 4. daw_import_take_wav: a new take placed at start_frame, no region.
        // -------------------------------------------------------------------
        CHECK_OK(session, daw_import_take_wav(session, 1, takePath.string().c_str(), "Tone take", kProjectRate / 2, rev(session)));
        CHECK(rev(session) == 3);
        const auto withTake = trackById(session, 1);
        CHECK(withTake.take_count == 2);  // base audio plus one alternate
        CHECK(withTake.clip_count == 1);  // an import take never mints a region by itself
        auto base = abi<daw_take>();
        CHECK_OK(session, daw_get_take(session, 1, 0, &base));
        CHECK(base.index == 0 && base.start == 0 && base.frames == kProjectRate && std::string(base.name) == "Tone");
        auto take = abi<daw_take>();
        CHECK_OK(session, daw_get_take(session, 1, 1, &take));
        CHECK(take.index == 1 && take.start == kProjectRate / 2 && take.frames == kProjectRate / 4);
        CHECK(std::string(take.name) == "Tone take");
        // Each take carries its own peak cache; take 0 is the track's base clip.
        std::vector<float> takePeaks(512, 0.0f);
        CHECK_OK(session, daw_get_take_waveform(session, 1, 1, takePeaks.data(), 512));
        const auto takeExpected = expectedPeaks(takeFixture);
        for (size_t bin = 0; bin < 512; ++bin) CHECK(takePeaks[bin] == takeExpected[bin]);
        std::vector<float> basePeaks(512, 0.0f);
        CHECK_OK(session, daw_get_take_waveform(session, 1, 0, basePeaks.data(), 512));
        for (size_t bin = 0; bin < 512; ++bin) CHECK(basePeaks[bin] == tonePeaks[bin]);
        CHECK_REJ(session, daw_get_take(session, 1, 2, &take));
        CHECK_REJ(session, daw_get_take_waveform(session, 1, 7, basePeaks.data(), 512));
        CHECK_REJ(session, daw_get_take_waveform(session, 1, 1, basePeaks.data(), 511));
        // struct_size gates are pure ABI shape: rc 1, no semantic error text.
        auto wrongSized = abi<daw_clip>();
        wrongSized.struct_size = sizeof(daw_clip) + 8;
        CHECK(daw_get_clip(session, 1, 0, &wrongSized) == 1);
        auto wrongTake = abi<daw_take>();
        wrongTake.struct_size = 4;
        CHECK(daw_get_take(session, 1, 0, &wrongTake) == 1);
        CHECK(rev(session) == 3);

        // -------------------------------------------------------------------
        // 5. Background import. begin captures intent only - no revision, no
        //    read - and the user keeps editing while the worker decodes.
        // -------------------------------------------------------------------
        const uint64_t importBase = rev(session);
        CHECK(daw_begin_import_wav(session, tonePath.string().c_str(), "Too late", importBase - 1) == nullptr);
        CHECK(!bridgeError(session).empty()); // begin reports why it refused
        CHECK(rev(session) == importBase);
        auto* job = daw_begin_import_wav(session, backgroundPath.string().c_str(), "Background", importBase);
        CHECK(job != nullptr);
        CHECK(rev(session) == importBase);            // begin mutates nothing
        CHECK_OK(session, daw_set_gain(session, 2, -6.0, rev(session))); // ... meanwhile, the user works
        CHECK(rev(session) == importBase + 1);
        const auto ready = waitImport(job, DAW_IMPORT_READY);
        CHECK(ready.status == DAW_IMPORT_READY);
        CHECK(ready.version == DAW_IMPORT_STATUS_VERSION);
        CHECK(ready.phase == DAW_IMPORT_PHASE_READY);
        CHECK(ready.progress == 100);
        CHECK(ready.base_revision == importBase);      // informational, captured at begin
        CHECK(ready.source_sample_rate == kProjectRate);
        CHECK(ready.source_channels == 2);
        CHECK(ready.source_frames == kProjectRate / 4);
        CHECK(ready.output_frames == kProjectRate / 4); // 48 kHz in, 48 kHz out
        CHECK(ready.error[0] == '\0');
        auto badStatus = abi<daw_import_status>();
        badStatus.struct_size = sizeof(daw_import_status) - 4;
        CHECK(daw_poll_import(job, &badStatus) == 1);   // versioned-struct gate
        CHECK(daw_poll_import(nullptr, &badStatus) == 1);

        // A stale optimistic revision rejects, spends nothing, and leaves the
        // ready result retryable - the explicit "add to current project" path.
        CHECK_REJ(session, daw_apply_import(session, job, importBase));
        CHECK(rev(session) == importBase + 1);
        CHECK(snapshotOf(session).track_count == 2);
        CHECK(waitImport(job, DAW_IMPORT_READY).status == DAW_IMPORT_READY);
        CHECK_OK(session, daw_apply_import(session, job, rev(session)));
        CHECK(rev(session) == importBase + 2);
        CHECK(waitImport(job, DAW_IMPORT_APPLIED).status == DAW_IMPORT_APPLIED);
        CHECK(snapshotOf(session).track_count == 3);
        const auto backgroundId = trackNamed(session, "Background");
        CHECK(trackById(session, backgroundId).audio_frames == kProjectRate / 4);
        CHECK(trackById(session, backgroundId).clip_count == 1);
        CHECK_REJ(session, daw_apply_import(session, job, rev(session))); // one apply per job
        CHECK(rev(session) == importBase + 2);
        daw_release_import(job);

        // -------------------------------------------------------------------
        // 6. Cancel is cooperative and never publishes a partial clip.
        // -------------------------------------------------------------------
        const auto beforeCancel = snapshotOf(session);
        auto* cancelJob = daw_begin_import_wav(session, tonePath.string().c_str(), "Never", beforeCancel.revision);
        CHECK(cancelJob != nullptr);
        daw_cancel_import(cancelJob);
        const auto canceled = waitImport(cancelJob, DAW_IMPORT_CANCELED);
        CHECK(canceled.status == DAW_IMPORT_CANCELED);
        CHECK(canceled.output_frames == 0);
        CHECK(snapshotOf(session).revision == beforeCancel.revision);
        CHECK(snapshotOf(session).track_count == beforeCancel.track_count);
        CHECK_REJ(session, daw_apply_import(session, cancelJob, rev(session)));
        daw_release_import(cancelJob);

        // -------------------------------------------------------------------
        // 7. Corrupt media fails honestly: a reason, no mutation, no partial
        //    clip. Each case is a file this scenario corrupted itself.
        // -------------------------------------------------------------------
        const auto pristine = readFileBytes(tonePath);
        const auto truncatedPath = root / "truncated.wav";
        writeFileBytes(truncatedPath, std::vector<unsigned char>(pristine.begin(), pristine.begin() + 4096));
        const auto garbagePath = root / "garbage.wav";
        {
            std::vector<unsigned char> junk(4096, 0);
            uint32_t state = 0x1234567u;
            for (auto& byte : junk) {
                state = state * 1664525u + 1013904223u;
                byte = static_cast<unsigned char>(state >> 13);
            }
            const char magic[4] = {'N', 'O', 'P', '!'}; // definitely not RIFF
            for (int index = 0; index < 4; ++index) junk[static_cast<size_t>(index)] = static_cast<unsigned char>(magic[index]);
            writeFileBytes(garbagePath, junk);
        }
        const auto nanPath = root / "non-finite.wav";
        writeWavFixture(nanPath, sineInterleaved(4800, kProjectRate, 440.0, 0.5, 2), kProjectRate, 2, "f32");
        {
            auto bytes = readFileBytes(nanPath);
            CHECK(bytes.size() > 48);
            const uint32_t quietNaN = 0x7fc00000u; // IEEE-754 binary32 NaN, little endian
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes[44 + shift / 8] = static_cast<unsigned char>((quietNaN >> shift) & 0xFF);
            writeFileBytes(nanPath, bytes);
        }
        for (const auto& corrupt : {truncatedPath, garbagePath, nanPath}) {
            const auto before = snapshotOf(session);
            auto* badJob = daw_begin_import_wav(session, corrupt.string().c_str(), "Corrupt", before.revision);
            CHECK(badJob != nullptr); // validating intent cannot see inside the file
            const auto failed = waitImport(badJob, DAW_IMPORT_FAILED);
            CHECK(failed.status == DAW_IMPORT_FAILED);
            CHECK(failed.error[0] != '\0'); // a human-readable reason reaches the app
            CHECK(failed.source_sample_rate == 0 && failed.source_channels == 0);
            CHECK(failed.source_frames == 0 && failed.output_frames == 0); // nothing partial published
            CHECK(snapshotOf(session).revision == before.revision);
            CHECK(snapshotOf(session).track_count == before.track_count);
            CHECK_REJ(session, daw_apply_import(session, badJob, before.revision));
            daw_release_import(badJob);
            // The synchronous path rejects the same file with the same honesty.
            CHECK_REJ(session, daw_import_wav(session, corrupt.string().c_str(), "Corrupt", before.revision));
            CHECK(snapshotOf(session).revision == before.revision);
            CHECK(snapshotOf(session).track_count == before.track_count);
        }
        CHECK_REJ(session, daw_import_wav(session, (root / "missing.wav").string().c_str(), "Ghost", rev(session)));
        CHECK(countNamedTracks(session, "Ghost") == 0);
        CHECK(countNamedTracks(session, "Corrupt") == 0);
        CHECK(countNamedTracks(session, "Never") == 0);

        // -------------------------------------------------------------------
        // 8. A ready take import whose target track was deleted rejects.
        // -------------------------------------------------------------------
        const auto doomedPath = root / "doomed.wav";
        writeWavFixture(doomedPath, constantInterleaved(4800, 2, 0.125f, -0.125f), kProjectRate, 2, "pcm16");
        CHECK_OK(session, daw_import_wav(session, doomedPath.string().c_str(), "Doomed", rev(session)));
        const auto doomed = trackNamed(session, "Doomed");
        auto* ghost = daw_begin_import_take_wav(session, doomed, takePath.string().c_str(), "Ghost take", 0, rev(session));
        CHECK(ghost != nullptr);
        CHECK(waitImport(ghost, DAW_IMPORT_READY).status == DAW_IMPORT_READY);
        CHECK_OK(session, daw_remove_track(session, doomed, rev(session)));
        const auto afterRemove = rev(session);
        CHECK_REJ(session, daw_apply_import(session, ghost, afterRemove));
        CHECK(rev(session) == afterRemove);            // the orphaned job spends nothing
        CHECK(trackById(session, 1).take_count == 2);  // and no stray take appeared elsewhere
        daw_release_import(ghost);

        // -------------------------------------------------------------------
        // 9. A replaced project invalidates every pending job: open_draft
        //    advances the session epoch while the revision stays identical.
        // -------------------------------------------------------------------
        Bridge epoch;
        CHECK_OK(epoch.get(), daw_import_wav(epoch.get(), tonePath.string().c_str(), "Epoch", rev(epoch.get())));
        const auto epochDraft = root / "epoch.mydawdraft";
        saveDraftAndWait(epoch.get(), epochDraft);
        CHECK_OK(epoch.get(), daw_open_draft(epoch.get(), epochDraft.string().c_str()));
        auto* pending = daw_begin_import_wav(epoch.get(), backgroundPath.string().c_str(), "After open", rev(epoch.get()));
        CHECK(pending != nullptr);
        CHECK(waitImport(pending, DAW_IMPORT_READY).status == DAW_IMPORT_READY);
        const auto revisionAtApply = rev(epoch.get());
        CHECK_OK(epoch.get(), daw_open_draft(epoch.get(), epochDraft.string().c_str()));
        CHECK(rev(epoch.get()) == revisionAtApply); // same revision: only the epoch moved
        CHECK_REJ(epoch.get(), daw_apply_import(epoch.get(), pending, rev(epoch.get())));
        CHECK(snapshotOf(epoch.get()).track_count == 1);
        daw_release_import(pending);

        // -------------------------------------------------------------------
        // 10. Platform import surface. macOS additionally converts foreign
        //     rates (docs/62) and reads AIFF/AIFC (docs/67); a non-Apple build
        //     has no converter and must say so instead of faking a pitch.
        // -------------------------------------------------------------------
        const auto narrowPath = root / "tone-441.wav";
        writeWavFixture(narrowPath, sineInterleaved(441, 44100, 440.0, 0.5, 2), 44100, 2, "pcm16");
        Bridge platform;
#ifdef __APPLE__
        {
            // 44.1 kHz -> 480 project frames for 441 source frames: the
            // documented round-to-nearest conversion law, through the job.
            auto* rateJob = daw_begin_import_wav(platform.get(), narrowPath.string().c_str(), "Converted", rev(platform.get()));
            CHECK(rateJob != nullptr);
            const auto converted = waitImport(rateJob, DAW_IMPORT_READY);
            CHECK(converted.source_sample_rate == 44100 && converted.source_frames == 441);
            CHECK(converted.output_frames == 480);
            CHECK_OK(platform.get(), daw_apply_import(platform.get(), rateJob, rev(platform.get())));
            const auto convertedId = trackNamed(platform.get(), "Converted");
            CHECK(trackById(platform.get(), convertedId).audio_frames == 480);
            auto convertedRegion = abi<daw_clip>();
            CHECK_OK(platform.get(), daw_get_clip(platform.get(), convertedId, 0, &convertedRegion));
            CHECK(convertedRegion.length == 480 && convertedRegion.source_offset == 0);
            daw_release_import(rateJob);

            // AIFF PCM16 stereo at the project rate: the same canonical clip
            // as WAV, direct fast path, so its peak cache matches our own
            // binning of the big-endian fixture bit for bit.
            const auto aiffPath = root / "tone.aif";
            const auto aiffSamples = sineInterleaved(kProjectRate / 4, kProjectRate, 880.0, 0.25, 2);
            writeAiffFixture(aiffPath, aiffSamples, kProjectRate, 2, "pcm16");
            CHECK_OK(platform.get(), daw_import_aiff(platform.get(), aiffPath.string().c_str(), "AIFF", rev(platform.get())));
            const auto aiffId = trackNamed(platform.get(), "AIFF");
            const auto aiffTrack = trackById(platform.get(), aiffId);
            CHECK(aiffTrack.audio_frames == kProjectRate / 4 && aiffTrack.clip_count == 1);
            CHECK(aiffTrack.take_count == 1 && aiffTrack.audio_frames == aiffSamples.size() / 2);
            Wav aiffWav; // what a 16-bit big-endian container must hold
            aiffWav.sampleRate = kProjectRate;
            aiffWav.channels = 2;
            aiffWav.bits = 16;
            aiffWav.samples.reserve(aiffSamples.size());
            for (const float value : aiffSamples)
                aiffWav.samples.push_back(static_cast<float>(std::lround(value * 32768.0f)) / 32768.0f);
            const auto aiffPeaks = expectedPeaks(aiffWav);
            std::vector<float> readAiff(512, 0.0f);
            CHECK_OK(platform.get(), daw_get_waveform(platform.get(), aiffId, readAiff.data(), 512));
            for (size_t bin = 0; bin < 512; ++bin) CHECK(readAiff[bin] == aiffPeaks[bin]);

            // AIFC fl32 mono: float samples arrive untouched, duplicated to R.
            const auto aifcPath = root / "tone.aifc";
            writeAiffFixture(aifcPath, constantInterleaved(kProjectRate / 8, 1, 0.125f, 0.0f), kProjectRate, 1, "f32");
            CHECK_OK(platform.get(), daw_import_aiff(platform.get(), aifcPath.string().c_str(), "AIFC", rev(platform.get())));
            const auto aifcId = trackNamed(platform.get(), "AIFC");
            CHECK(trackById(platform.get(), aifcId).audio_frames == kProjectRate / 8);
            std::vector<float> aifcPeaks(512, 0.0f);
            CHECK_OK(platform.get(), daw_get_waveform(platform.get(), aifcId, aifcPeaks.data(), 512));
            for (const float peak : aifcPeaks) CHECK(peak == 0.125f); // exact float round trip

            // An AIFF take joins the same import-take path as WAV.
            CHECK_OK(platform.get(), daw_import_take_aiff(platform.get(), aiffId, aifcPath.string().c_str(), "AIFF take", 6000, rev(platform.get())));
            auto aiffTake = abi<daw_take>();
            CHECK_OK(platform.get(), daw_get_take(platform.get(), aiffId, 1, &aiffTake));
            CHECK(aiffTake.start == 6000 && aiffTake.frames == kProjectRate / 8);
            CHECK(std::string(aiffTake.name) == "AIFF take");

            // The background form of that same command. begin_import_take_aiff
            // prepares a lane for a track that already exists: it mints no
            // revision, publishes nothing until apply, and a take never creates
            // a region - comping decides what is heard later (docs/21, 67).
            const auto takeAiff = root / "take.aif";
            writeAiffFixture(takeAiff, sineInterleaved(kProjectRate / 4, kProjectRate, 1760.0, 0.125, 2), kProjectRate, 2,
                             "pcm16");
            const auto lanesBefore = trackById(platform.get(), aiffId).take_count;
            const auto jobBase = rev(platform.get());
            auto* takeJob = daw_begin_import_take_aiff(platform.get(), aiffId, takeAiff.string().c_str(), "AIFF take job",
                                                       kProjectRate / 4, jobBase);
            CHECK(takeJob != nullptr);
            CHECK(rev(platform.get()) == jobBase); // preparing is not editing
            CHECK(trackById(platform.get(), aiffId).take_count == lanesBefore);
            const auto takeReady = waitImport(takeJob, DAW_IMPORT_READY);
            CHECK(takeReady.status == DAW_IMPORT_READY && takeReady.progress == 100);
            CHECK(takeReady.source_sample_rate == kProjectRate && takeReady.source_channels == 2);
            CHECK(takeReady.source_frames == kProjectRate / 4); // 48 kHz: the fast path, no conversion
            CHECK(takeReady.output_frames == takeReady.source_frames);
            CHECK(takeReady.error[0] == '\0');
            CHECK(trackById(platform.get(), aiffId).take_count == lanesBefore); // ready is still not applied
            daw_import_status raw = {}; // struct_size 0: a shape the ABI must refuse
            CHECK(daw_poll_import(takeJob, &raw) == 1);
            auto polled = abi<daw_import_status>();
            CHECK(daw_poll_import(takeJob, &polled) == 0 && polled.status == DAW_IMPORT_READY);
            CHECK(polled.base_revision == takeReady.base_revision && polled.source_frames == takeReady.source_frames);
            const auto regionCountBefore = trackById(platform.get(), aiffId).clip_count;
            CHECK_OK(platform.get(), daw_apply_import(platform.get(), takeJob, rev(platform.get())));
            CHECK(rev(platform.get()) == jobBase + 1); // one revision, spent at apply
            CHECK(waitImport(takeJob, DAW_IMPORT_APPLIED).status == DAW_IMPORT_APPLIED);
            CHECK(trackById(platform.get(), aiffId).take_count == lanesBefore + 1);
            CHECK(trackById(platform.get(), aiffId).clip_count == regionCountBefore); // no region appeared
            auto appliedTake = abi<daw_take>();
            CHECK_OK(platform.get(), daw_get_take(platform.get(), aiffId, lanesBefore, &appliedTake));
            CHECK(appliedTake.start == kProjectRate / 4 && appliedTake.frames == kProjectRate / 4);
            CHECK(std::string(appliedTake.name) == "AIFF take job");
            CHECK_REJ(platform.get(), daw_apply_import(platform.get(), takeJob, rev(platform.get()))); // one apply per job
            CHECK(trackById(platform.get(), aiffId).take_count == lanesBefore + 1);
            // A take queued for a lane that no longer exists must not resurrect it.
            const auto lanes = snapshotOf(platform.get()).track_count;
            uint64_t doomedTrack = 0;
            CHECK_OK(platform.get(), daw_duplicate_track(platform.get(), aiffId, &doomedTrack, rev(platform.get())));
            CHECK(snapshotOf(platform.get()).track_count == lanes + 1);
            auto* orphanJob = daw_begin_import_take_aiff(platform.get(), doomedTrack, takeAiff.string().c_str(), "Orphan",
                                                        0, rev(platform.get()));
            CHECK(orphanJob != nullptr);
            CHECK(waitImport(orphanJob, DAW_IMPORT_READY).status == DAW_IMPORT_READY);
            CHECK_OK(platform.get(), daw_remove_track(platform.get(), doomedTrack, rev(platform.get())));
            CHECK_REJ(platform.get(), daw_apply_import(platform.get(), orphanJob, rev(platform.get())));
            CHECK(snapshotOf(platform.get()).track_count == lanes); // the deleted lane stayed deleted
            daw_release_import(takeJob);
            daw_release_import(orphanJob);

            // A 44.1 kHz AIFF travels the shared converter: 4410 frames -> 4800.
            const auto wideAiff = root / "tone-441.aif";
            writeAiffFixture(wideAiff, constantInterleaved(4410, 1, 0.5f, 0.0f), 44100, 1, "pcm16");
            auto* aiffJob = daw_begin_import_aiff(platform.get(), wideAiff.string().c_str(), "AIFF 44.1", rev(platform.get()));
            CHECK(aiffJob != nullptr);
            const auto aiffReady = waitImport(aiffJob, DAW_IMPORT_READY);
            CHECK(aiffReady.source_sample_rate == 44100 && aiffReady.source_channels == 1);
            CHECK(aiffReady.source_frames == 4410 && aiffReady.output_frames == 4800);
            CHECK_OK(platform.get(), daw_apply_import(platform.get(), aiffJob, rev(platform.get())));
            const auto wideId = trackNamed(platform.get(), "AIFF 44.1");
            CHECK(trackById(platform.get(), wideId).audio_frames == 4800);
            std::vector<float> widePeaks(512, 0.0f);
            CHECK_OK(platform.get(), daw_get_waveform(platform.get(), wideId, widePeaks.data(), 512));
            // Interior bins only: Apple's converter may settle at both edges.
            // The window matches the aiff_import CTest's 0.05 converter gate.
            for (size_t bin = 128; bin < 384; ++bin) expectNear(widePeaks[bin], 0.5, 0.05, "resampled DC peak");
            daw_release_import(aiffJob);
        }
#else
        {
            // Non-Apple builds accept only 48 kHz PCM WAV, with a precise
            // reason, and they never fake it by keeping the wrong frame count.
            const auto before = snapshotOf(platform.get());
            CHECK_REJ(platform.get(), daw_import_wav(platform.get(), narrowPath.string().c_str(), "Converted", before.revision));
            CHECK(snapshotOf(platform.get()).revision == before.revision);
            CHECK(snapshotOf(platform.get()).track_count == before.track_count);
            auto* job44 = daw_begin_import_wav(platform.get(), narrowPath.string().c_str(), "Converted", before.revision);
            CHECK(job44 != nullptr);
            const auto refused = waitImport(job44, DAW_IMPORT_FAILED);
            CHECK(refused.status == DAW_IMPORT_FAILED);
            CHECK(refused.error[0] != '\0');
            CHECK(refused.output_frames == 0);
            CHECK_REJ(platform.get(), daw_apply_import(platform.get(), job44, rev(platform.get())));
            CHECK(snapshotOf(platform.get()).track_count == before.track_count);
            daw_release_import(job44);
            // The 48 kHz backbone still works everywhere.
            CHECK_OK(platform.get(), daw_import_wav(platform.get(), tonePath.string().c_str(), "Tone", rev(platform.get())));
            CHECK(trackById(platform.get(), trackNamed(platform.get(), "Tone")).audio_frames == kProjectRate);
        }
#endif

        // -------------------------------------------------------------------
        // 11. Durability: save the draft, reopen it in a brand-new session,
        //     compare the whole model, and re-render with the sources deleted.
        //
        // The two DC strips are muted first so the master bus carries nothing
        // but the imported tone; that makes the render a direct statement
        // about the imported samples instead of a sum of three sources.
        // -------------------------------------------------------------------
        const auto deskTrack = trackNamed(session, "Desk");
        const auto lastTrack = trackNamed(session, "Background");
        CHECK_OK(session, daw_set_mute(session, deskTrack, 1, rev(session)));
        CHECK_OK(session, daw_set_mute(session, lastTrack, 1, rev(session)));
        CHECK(trackById(session, deskTrack).muted == 1);
        CHECK(trackById(session, lastTrack).muted == 1);
        const auto built = dumpOf(session);
        CHECK(built.tracks.size() == 3);
        CHECK(built.tracks[0].takes.size() == 2); // base take + "Tone take"
        CHECK(built.tracks[0].clips.size() == 1);
        CHECK(built.tracks[1].muted == 1 && built.tracks[2].muted == 1);
        const auto reference = exportProject(session, root / "before-save.wav", 2);
        CHECK(reference.sampleRate == kProjectRate && reference.channels == 2);
        CHECK(reference.bits == 32 && reference.floating);
        CHECK(reference.frames() == kProjectRate); // duration = the longest region

        // A 48 kHz import is bit-transparent: after the track fader's
        // geometric ramp has settled, the bounced samples are the fixture's
        // own quantized values. Tolerance 1e-4 is the fader smoother, which
        // converges toward unity and stops once its increment drops below half
        // an ULP of the frame value - at most ~1.4e-5 relative (0.5 * 1.4e-5 is
        // 7e-6, so 1e-4 leaves room); it is not a codec or resampling slack.
        for (uint32_t frame = 4000; frame < kProjectRate; ++frame) {
            expectNear(reference.samples[static_cast<size_t>(frame) * 2],
                       toneFixture.samples[static_cast<size_t>(frame) * 2], 1e-4, "imported left sample");
            expectNear(reference.samples[static_cast<size_t>(frame) * 2 + 1],
                       toneFixture.samples[static_cast<size_t>(frame) * 2 + 1], 1e-4, "imported right sample");
        }

        const auto draft = root / "media.mydawdraft";
        saveDraftAndWait(session, draft);
        CHECK(fileNonEmpty(draft));

        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), draft.string().c_str()));
        const auto restored = dumpOf(reopened.get());
        if (!(restored == built))
            throw std::runtime_error("import round-trip changed the project:\nBEFORE:\n" + describe(built) +
                                     "\nAFTER:\n" + describe(restored));
        CHECK(restored.tracks[0].takes[1].start == kProjectRate / 2);
        CHECK(restored.tracks[0].takes[1].frames == kProjectRate / 4);
        CHECK(restored.tracks[0].takes[1].name == "Tone take");
        CHECK(restored.tracks[1].gainDb == -6.0);
        CHECK(restored.tracks[2].clips.size() == 1 && restored.tracks[2].clips[0].length == kProjectRate / 4);
        // The PCM itself survived SQLite: identical cached peaks, read back
        // through the same ABI the waveform view uses.
        std::vector<float> persistedPeaks(512, 0.0f);
        CHECK_OK(reopened.get(), daw_get_waveform(reopened.get(), restored.tracks[0].id, persistedPeaks.data(), 512));
        for (size_t bin = 0; bin < 512; ++bin) CHECK(persistedPeaks[bin] == tonePeaks[bin]);

        // Imported media is embedded, so the project no longer needs the files.
        std::filesystem::remove(tonePath);
        std::filesystem::remove(deskPath);
        std::filesystem::remove(backgroundPath);
        CHECK(!std::filesystem::exists(tonePath));

        const auto afterReopen = exportProject(reopened.get(), root / "after-reopen.wav", 2);
        CHECK(afterReopen.frames() == reference.frames());
        CHECK(afterReopen.channels == reference.channels);
        for (size_t index = 0; index < reference.samples.size(); ++index)
            if (reference.samples[index] != afterReopen.samples[index])
                throw std::runtime_error("reopened render differs at sample " + std::to_string(index));

        // And the 440 Hz of the imported file is really what comes back: the
        // spectral component of the bounced audio matches the component of the
        // source fixture measured over the same window. The 1e-3 slack is the
        // fader smoother described above plus Goertzel edge leakage from the
        // window starting off-grid; it is not room for a different tone.
        const double sourceMagnitude = toneMagnitude(toneFixture, 4800, 40000, 0, 440.0);
        const double mixedMagnitude = toneMagnitude(afterReopen, 4800, 40000, 0, 440.0);
        CHECK(sourceMagnitude > 0.3);
        expectNear(mixedMagnitude, sourceMagnitude, 1e-3, "440 Hz survives save + reopen");

        std::cout << "PASS: e2e_media_import — WAV/take import, background jobs (ready, cancel, failure, apply), peak caches, durable round trip\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}