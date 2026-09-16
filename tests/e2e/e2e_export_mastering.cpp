// e2e: EXP-01 / EXP-02 - the mastering surface: what the product prints, what
// the finished file actually contains, and what the meters say about it.
//
// Everything is driven through the SAME public C ABI the macOS app uses and
// checked with the harness's INDEPENDENT WAV reader: headers byte by byte,
// samples against the closed-form arithmetic of DC fixtures that cannot lie
// (peak == RMS == one sample), stems re-read from disk, and the finished file
// measured a second time by daw_measure_wav. Material is deliberately tiny
// (12000 frames == 0.25 s of program), so the journey stays fast and every
// expected number below is hand-computable.
//
// Fixture arithmetic (the same measured law as e2e_mixer_routing): a track
// fader starts at zero and rides the documented one-pole
// "s += (target - s) * 0.004166667" - a 240-sample time constant at 48 kHz -
// while the master gain is latched by prepare(). The pole stops as soon as its
// increment falls under half an ULP of the running value (frame ~2700 here), so
// a plateau is only asserted from frame 3000 of a window on, and the first 512
// frames of a render are checked against the closed form of that same pole.
//
// Two rows of docs/78 are asserted the way the ABI actually promises them, with
// the gap written down where a headless black-box run cannot reach the original
// expectation: tail-policy durations (section 5) and live meters after an
// export (section 9). Both are recorded, not bent.
#include "e2e.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using e2e::Wav;

// Project backbone: 8000 frames of lead DC, then 8000 frames of bed DC placed
// at frame 4000, so the last region ends at 12000 - the duration every export
// below has to reproduce exactly.
constexpr uint32_t kRegion = 8000;
constexpr uint64_t kBedStart = 4000;
constexpr uint64_t kDuration = kBedStart + kRegion;

// Exactly -20*log10(2) dB, i.e. a linear half: master and bus both halve, so
// each expected amplitude stays a hand-computable number.
constexpr double kHalfDb = -6.0205999132796239;
constexpr double kHalfLinear = 0.5;
// Lead DC: L 0.4 / R 0.2 (straight to master). Bed DC: L -0.25 / R 0.0 via a bus.
constexpr double kLeadLeft = 0.4, kLeadRight = 0.2;
constexpr double kBedLeft = -0.25;
// Budget for a value that has passed through a gain one-pole.
constexpr double kSmoothedTol = 2e-5;
// One LSB of 24-bit PCM, the unit the documented TPDF budget is counted in.
constexpr double kLsb24 = 1.0 / 8388608.0;
// Frames after which the documented one-pole has stopped moving at all.
constexpr uint64_t kSettled = 3000;

// A job API rejects by returning NULL and leaving text behind (CHECK_REJ covers
// the int-returning calls); either way nothing may move the revision.
template <typename Call>
void expectJobRejection(daw_session* session, const std::string& what, Call&& call) {
    const uint64_t stable = e2e::rev(session);
    if (call() != nullptr) throw std::runtime_error("Accepted, expected rejection: " + what);
    if (e2e::bridgeError(session).empty()) throw std::runtime_error("Rejected without error text: " + what);
    if (e2e::rev(session) != stable) throw std::runtime_error("Rejected call moved the revision: " + what);
}

template <typename Call>
void expectIntRejection(daw_session* session, const std::string& what, Call&& call) {
    const uint64_t stable = e2e::rev(session);
    if (call() == 0) throw std::runtime_error("Accepted, expected rejection: " + what);
    if (e2e::bridgeError(session).empty()) throw std::runtime_error("Rejected without error text: " + what);
    if (e2e::rev(session) != stable) throw std::runtime_error("Rejected call moved the revision: " + what);
}

std::vector<unsigned char> headBytes(const std::filesystem::path& path, size_t count) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) throw std::runtime_error("cannot open " + path.string());
    std::vector<unsigned char> bytes(count);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
    if (static_cast<size_t>(stream.gcount()) != count) throw std::runtime_error("short read of " + path.string());
    return bytes;
}

uint32_t rd32(const std::vector<unsigned char>& bytes, size_t at) {
    return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) | (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

uint16_t rd16(const std::vector<unsigned char>& bytes, size_t at) {
    return static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

uint64_t fileSize(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("no file " + path.string());
    return static_cast<uint64_t>(size);
}

double sampleAt(const Wav& wav, uint64_t frame, uint16_t channel) {
    return static_cast<double>(wav.samples[static_cast<size_t>(frame) * wav.channels + channel]);
}

double dbfs(double amplitude) { return 20.0 * std::log10(amplitude); }

// Versioned options, built the only legal way: real struct_size, real version,
// one of the three documented tail modes. A zeroed daw_export_options is NOT a
// valid value (struct_size 0 and version 0 are both refused), so this is the
// scenario's only options factory.
daw_export_options validOptions(uint32_t tailMode, uint32_t manualTailFrames) {
    auto options = e2e::abi<daw_export_options>();
    options.version = DAW_EXPORT_OPTIONS_VERSION;
    options.tail_mode = tailMode;
    options.manual_tail_frames = manualTailFrames;
    return options;
}

bool samplesEqual(const Wav& left, const Wav& right) {
    return left.channels == right.channels && left.frames() == right.frames() && left.samples == right.samples;
}

std::vector<std::string> listFiles(const std::filesystem::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

// Commands mint ids we do not own, so a strip is found by its user-visible name
// (an index is not an id: buses share the same counter).
uint64_t trackIdNamed(daw_session* session, const std::string& name) {
    const auto snap = e2e::snapshotOf(session);
    for (uint32_t index = 0; index < snap.track_count; ++index) {
        auto track = e2e::abi<daw_track>();
        CHECK_OK(session, daw_get_track(session, index, &track));
        if (name == track.name) return track.id;
    }
    throw std::runtime_error("no track named " + name);
}

// Import a DC fixture as a fresh strip; its region always starts at frame 0.
uint64_t importDc(daw_session* session, const std::filesystem::path& path, const std::string& name) {
    CHECK_OK(session, daw_import_wav(session, path.string().c_str(), name.c_str(), e2e::rev(session)));
    const auto id = trackIdNamed(session, name);
    const auto track = e2e::trackById(session, id);
    CHECK(track.clip_count == 1 && track.audio_frames == kRegion);
    return id;
}

// One settled DC window of one channel: literally flat, and at that value.
void expectDcWindow(const Wav& wav, uint64_t from, uint64_t to, uint16_t channel, double expected,
                    const std::string& what) {
    CHECK(to > from && to <= wav.frames());
    double lo = 1e300, hi = -1e300;
    for (uint64_t frame = from; frame < to; ++frame) {
        const double value = sampleAt(wav, frame, channel);
        lo = std::min(lo, value);
        hi = std::max(hi, value);
    }
    if (!(hi - lo <= 1e-9)) throw std::runtime_error("window is not a flat plateau: " + what);
    e2e::expectNear(lo, expected, kSmoothedTol, "DC plateau " + what);
}

// The same window on a dithered PCM24 render: no flatness claim (TPDF moves
// every sample), but every frame stays inside the smoother plus dither budget.
void expectPcm24Window(const Wav& wav, uint64_t from, uint64_t to, uint16_t channel, double expected,
                       const std::string& what) {
    CHECK(to > from && to <= wav.frames());
    const double tolerance = kSmoothedTol + 2.0 * kLsb24;
    for (uint64_t frame = from; frame < to; frame += 31)
        e2e::expectNear(sampleAt(wav, frame, channel), expected, tolerance, "PCM24 window " + what);
}

// The documented fader ramp, silence to plateau, over the first 512 frames.
void expectFaderRamp(const Wav& wav, uint16_t channel, double plateau, const std::string& what) {
    for (uint32_t frame = 0; frame < 512; ++frame) {
        const double smoothed = plateau * (1.0 - std::pow(1.0 - 0.004166667, static_cast<double>(frame) + 1.0));
        e2e::expectNear(sampleAt(wav, frame, channel), smoothed, kSmoothedTol,
                        "fader ramp " + what + " at " + std::to_string(frame));
    }
}

// A refused export has to say why: either synchronously (NULL job plus bridge
// text) or in its own terminal status. What the destination must look like
// afterwards is asserted at the call site, because an unusable destination can
// legitimately exist already - a plain file where a stems directory belongs.
void expectJobFailure(daw_session* session, daw_export_job* job) {
    if (job == nullptr) {
        CHECK(!e2e::bridgeError(session).empty());
        return;
    }
    const auto status = e2e::waitExport(job, 2);
    daw_release_export(job);
    CHECK(status.status == 2);
    CHECK(std::string(status.error).size() > 0);
}

}  // namespace

int main() {
    try {
        using e2e::abi;
        using e2e::Bridge;
        using e2e::TempRoot;

        TempRoot root("export-mastering");

        // =========================================================================
        // 1. The mastering source of truth: two DC strips (one through a
        //    half-gain bus), a user-muted strip, a strip whose only clip is
        //    muted, an empty strip, and a master at exactly one half.
        // =========================================================================
        const auto leadPath = root / "lead.wav";
        const auto bedPath = root / "bed.wav";
        e2e::writeWavFixture(leadPath, e2e::constantInterleaved(kRegion, 2, static_cast<float>(kLeadLeft),
                                                                static_cast<float>(kLeadRight)),
                             e2e::kProjectRate, 2, "f32");
        e2e::writeWavFixture(bedPath, e2e::constantInterleaved(kRegion, 2, static_cast<float>(kBedLeft), 0.0f),
                             e2e::kProjectRate, 2, "f32");
        // One tone fixture for the loudness section, where K-weighting has a
        // fixed answer that DC material cannot show.
        const auto tonePath = root / "tone.wav";
        e2e::writeWavFixture(tonePath, e2e::sineInterleaved(e2e::kProjectRate, e2e::kProjectRate, 997.0, 0.5, 2),
                             e2e::kProjectRate, 2, "f32");

        Bridge bridge;
        daw_session* session = bridge.get();
        const auto lead = importDc(session, leadPath, "Lead/Vox");  // names needing stem sanitising
        const auto bed = importDc(session, bedPath, "Bed:Sub");
        CHECK_OK(session, daw_edit_clip(session, bed, 0, kBedStart, 0, kRegion, e2e::rev(session)));
        const auto room = e2e::addBus(session, "Room");
        CHECK_OK(session, daw_set_bus_gain(session, room, kHalfDb, e2e::rev(session)));
        CHECK_OK(session, daw_set_track_output(session, bed, room, e2e::rev(session)));
        const auto muted = importDc(session, leadPath, "Fly Muted");
        CHECK_OK(session, daw_set_mute(session, muted, 1, e2e::rev(session)));
        const auto clipMuted = importDc(session, bedPath, "Clip Muted");
        CHECK_OK(session, daw_set_clip_muted(session, clipMuted, 0, 1, e2e::rev(session)));
        const auto empty = e2e::addTrack(session, "Empty Four");
        CHECK_OK(session, daw_set_master_gain(session, kHalfDb, e2e::rev(session)));
        const uint64_t baseRevision = e2e::rev(session);

        // The mix by hand: the master halves everything, the bus halves the bed.
        const double leadL = kLeadLeft * kHalfLinear, leadR = kLeadRight * kHalfLinear;
        const double bedL = kBedLeft * kHalfLinear * kHalfLinear;

        // =========================================================================
        // 2. Float32 (format 2): header bytes off the disk, length against the
        //    last region end, samples against the fixture arithmetic.
        // =========================================================================
        const auto mixPath = root / "mix-f32.wav";
        {
            auto* job = daw_begin_export(session, mixPath.string().c_str(), 2);
            CHECK(job != nullptr);
            const auto status = e2e::waitExport(job);
            CHECK(status.status == 1);
            CHECK(status.total_frames == kDuration && status.rendered_frames == kDuration);
            CHECK(status.revision == baseRevision);  // the frozen snapshot it rendered
            daw_release_export(job);
        }
        const auto header = headBytes(mixPath, 58);
        CHECK(std::memcmp(header.data(), "RIFF", 4) == 0 && std::memcmp(header.data() + 8, "WAVE", 4) == 0);
        CHECK(std::memcmp(header.data() + 12, "fmt ", 4) == 0 && rd32(header, 16) == 18);
        CHECK(rd16(header, 20) == 3);  // IEEE float
        CHECK(rd16(header, 22) == 2);  // stereo
        CHECK(rd32(header, 24) == e2e::kProjectRate);  // the project rate, never the fixture's
        CHECK(rd16(header, 32) == 8 && rd32(header, 28) == e2e::kProjectRate * 8);
        CHECK(rd16(header, 34) == 32);
        CHECK(std::memcmp(header.data() + 38, "fact", 4) == 0 && rd32(header, 42) == 4);  // docs/25
        CHECK(rd32(header, 46) == kDuration);  // fact sample count
        CHECK(std::memcmp(header.data() + 50, "data", 4) == 0 && rd32(header, 54) == kDuration * 8);
        CHECK(rd32(header, 4) == 58 + kDuration * 8 - 8);  // RIFF size
        CHECK(fileSize(mixPath) == 58 + kDuration * 8);    // no padding, no trailer

        const Wav mix = e2e::readWavFixture(mixPath);
        CHECK(mix.floating && mix.bits == 32 && mix.channels == 2 && mix.sampleRate == e2e::kProjectRate);
        CHECK(mix.frames() == kDuration);
        expectFaderRamp(mix, 0, leadL, "lead left");
        expectDcWindow(mix, kSettled, kBedStart, 0, leadL, "lead only left");
        expectDcWindow(mix, kSettled, kBedStart, 1, leadR, "lead only right");
        expectDcWindow(mix, kBedStart + kSettled, kRegion, 0, leadL + bedL, "overlap left");
        expectDcWindow(mix, kBedStart + kSettled, kRegion, 1, leadR, "overlap right");
        expectDcWindow(mix, kRegion + kSettled, kDuration, 0, bedL, "bed only left");
        expectDcWindow(mix, kRegion + kSettled, kDuration, 1, 0.0, "bed only right");
        // A muted strip, a clip-muted strip and an empty strip add nothing: the
        // loudest frame of the program is still the settled lead plateau.
        CHECK(e2e::channelPeak(mix, 0, static_cast<uint32_t>(kDuration), 0) <= leadL + kSmoothedTol);
        CHECK(sampleAt(mix, kDuration - 1, 1) == 0.0);

        // Determinism twice over: one snapshot renders byte-identical again.
        const Wav rerender = e2e::exportProject(session, root / "mix-f32-b.wav", 2);
        if (!samplesEqual(mix, rerender)) throw std::runtime_error("float32 render is not deterministic");
        CHECK(headBytes(mixPath, 58) == headBytes(root / "mix-f32-b.wav", 58));

        // =========================================================================
        // 3. PCM24 (format 1): code 1 / 24 bit / 44-byte header, the same
        //    arithmetic, and samples inside the documented one-pass TPDF budget.
        // =========================================================================
        const auto pcmPath = root / "mix-pcm24.wav";
        const Wav pcm = e2e::exportProject(session, pcmPath, 1);
        const auto pcmHeader = headBytes(pcmPath, 44);
        CHECK(std::memcmp(pcmHeader.data() + 12, "fmt ", 4) == 0 && rd32(pcmHeader, 16) == 16);
        CHECK(rd16(pcmHeader, 20) == 1 && rd16(pcmHeader, 34) == 24);  // integer PCM, 24-bit
        CHECK(rd16(pcmHeader, 22) == 2 && rd32(pcmHeader, 24) == e2e::kProjectRate);
        CHECK(rd16(pcmHeader, 32) == 6 && rd32(pcmHeader, 28) == e2e::kProjectRate * 6);
        CHECK(std::memcmp(pcmHeader.data() + 36, "data", 4) == 0 && rd32(pcmHeader, 40) == kDuration * 6);
        CHECK(rd32(pcmHeader, 4) == 44 + kDuration * 6 - 8);
        CHECK(fileSize(pcmPath) == 44 + kDuration * 6);  // PCM24 carries no fact chunk
        CHECK(!pcm.floating && pcm.bits == 24 && pcm.channels == 2 && pcm.frames() == kDuration);
        expectPcm24Window(pcm, kSettled, kBedStart, 0, leadL, "pcm24 lead left");
        expectPcm24Window(pcm, kSettled, kBedStart, 1, leadR, "pcm24 lead right");
        expectPcm24Window(pcm, kBedStart + kSettled, kRegion, 0, leadL + bedL, "pcm24 overlap");
        expectPcm24Window(pcm, kRegion + kSettled, kDuration, 1, 0.0, "pcm24 silent right");
        // docs/25: exactly one TPDF draw before the final quantisation, so the
        // distance to the float32 render is at most one LSB of dither plus half
        // an LSB of quantisation noise - on EVERY sample, not merely on average.
        CHECK(pcm.samples.size() == mix.samples.size());
        double worstDither = 0.0;
        for (uint64_t frame = 0; frame < kDuration; ++frame)
            for (uint16_t channel = 0; channel < 2; ++channel)
                worstDither = std::max(worstDither, std::abs(sampleAt(pcm, frame, channel) - sampleAt(mix, frame, channel)));
        CHECK(worstDither <= 2.0 * kLsb24);
        // The documented fixed PRNG state: a second PCM24 render is byte-identical.
        CHECK(samplesEqual(pcm, e2e::exportProject(session, root / "mix-pcm24-b.wav", 1)));
        CHECK(headBytes(pcmPath, 44) == headBytes(root / "mix-pcm24-b.wav", 44));

        // =========================================================================
        // 4. Ranges (docs/25 1.1): half-open [start, end) arithmetic, content
        //    that belongs to the range, and rejections that never reach the disk.
        // =========================================================================
        {
            const auto rangePath = root / "range.wav";
            auto* job = daw_begin_export_range(session, rangePath.string().c_str(), 2, kBedStart, kDuration);
            CHECK(job != nullptr);
            const auto status = e2e::waitExport(job);
            daw_release_export(job);
            CHECK(status.total_frames == kDuration - kBedStart && status.rendered_frames == kDuration - kBedStart);
            const Wav range = e2e::readWavFixture(rangePath);
            CHECK(range.frames() == kDuration - kBedStart && range.channels == 2 && range.bits == 32);
            // A range render restarts the gain smoothing at its own boundary
            // instead of slicing the whole-program render, so it re-derives every
            // plateau: the first frame is far below the settled value, the run-up
            // only ever climbs towards it, and once the poles have stopped the
            // range is BIT-exact with the matching slice of the whole render.
            CHECK(sampleAt(range, 0, 0) < 0.02 * (leadL + bedL));
            CHECK(sampleAt(range, 0, 0) != sampleAt(mix, kBedStart, 0));
            for (uint16_t channel = 0; channel < 2; ++channel) {
                CHECK(sampleAt(range, 10, channel) < sampleAt(range, 100, channel));
                CHECK(sampleAt(range, 100, channel) < sampleAt(range, 1000, channel));
                // Every frame of the run-up stays under the plateau that the
                // whole-program render has long since reached.
                for (uint64_t frame = 0; frame < kSettled; ++frame)
                    CHECK(sampleAt(range, frame, channel) < (channel == 0 ? leadL + bedL : leadR));
            }
            for (uint64_t frame = kSettled; frame < range.frames(); ++frame)
                for (uint16_t channel = 0; channel < 2; ++channel)
                    CHECK(sampleAt(range, frame, channel) == sampleAt(mix, frame + kBedStart, channel));
            expectDcWindow(range, kSettled, kRegion - kBedStart, 0, leadL + bedL, "range overlap");
            expectDcWindow(range, kRegion - kBedStart + kSettled, range.frames(), 0, bedL, "range into the bed alone");
            // [0, duration) is the whole program again, bit for bit.
            CHECK(samplesEqual(e2e::exportProject(session, root / "range-whole.wav", 2), mix));
            // PCM24 keeps the same half-open length and header rules.
            const auto pcmRangePath = root / "range-pcm24.wav";
            auto* pcmJob = daw_begin_export_range(session, pcmRangePath.string().c_str(), 1, kBedStart, kDuration);
            CHECK(pcmJob != nullptr);
            const auto pcmStatus = e2e::waitExport(pcmJob);
            daw_release_export(pcmJob);
            CHECK(pcmStatus.total_frames == kDuration - kBedStart);
            CHECK(fileSize(pcmRangePath) == 44 + (kDuration - kBedStart) * 6);

            // The transport loop is ephemeral: no revision, and it does not bias a
            // whole-program render. "Export the loop" is exactly this read-back of
            // the loop bounds into the range API.
            const uint64_t beforeLoop = e2e::rev(session);
            CHECK_OK(session, daw_set_loop(session, 1, 1000, 5000));
            CHECK(e2e::rev(session) == beforeLoop);
            auto transport = abi<daw_transport>();
            CHECK_OK(session, daw_get_transport(session, &transport));
            CHECK(transport.loop_enabled == 1 && transport.loop_start == 1000 && transport.loop_end == 5000);
            CHECK(transport.duration == kDuration);  // the loop is not the export range
            CHECK(samplesEqual(e2e::exportProject(session, root / "mix-looped.wav", 2), mix));
            const auto loopPath = root / "loop-range.wav";
            auto* loopJob = daw_begin_export_range(session, loopPath.string().c_str(), 2, transport.loop_start,
                                                   transport.loop_end);
            CHECK(loopJob != nullptr);
            const auto loopStatus = e2e::waitExport(loopJob);
            daw_release_export(loopJob);
            CHECK(loopStatus.total_frames == transport.loop_end - transport.loop_start);
            CHECK(e2e::readWavFixture(loopPath).frames() == transport.loop_end - transport.loop_start);
            CHECK_OK(session, daw_set_loop(session, 0, 0, 0));
            CHECK(e2e::rev(session) == beforeLoop);

            // Half-open nonsense rejects synchronously: no job, no file, no revision.
            const std::string badPath = (root / "bad-range.wav").string();
            expectJobRejection(session, "end before start", [&] {
                return daw_begin_export_range(session, badPath.c_str(), 2, kRegion, kBedStart);
            });
            expectJobRejection(session, "end past the program", [&] {
                return daw_begin_export_range(session, badPath.c_str(), 2, 0, kDuration + 1);
            });
            expectJobRejection(session, "empty range", [&] {
                return daw_begin_export_range(session, badPath.c_str(), 2, 5000, 5000);
            });
            expectJobRejection(session, "zero range", [&] {
                return daw_begin_export_range(session, badPath.c_str(), 2, 0, 0);
            });
            CHECK(!std::filesystem::exists(root / "bad-range.wav"));
        }

        // =========================================================================
        // 5. Versioned export options and the tail policy (docs/60).
        //
        // The three policies differ ONLY in how an infinite VST3 declaration is
        // bounded: automatic appends the 30 s safety limit, none drops that
        // component, manual limits it to the caller's frame count - while a finite
        // decay is retained by all three (resolveExportTailImpl takes
        // max(finite, bounded infinite)). Through the public ABI a tail can only
        // come from a hosted plug-in, and the AU host reports tail 0
        // (engine/audio/effect.hpp), so on this plug-in-free graph every policy
        // must select zero frames and render the very same length. That is what is
        // asserted here, together with the whole validation matrix; a duration
        // spread would need a VST3 carrying the infinite sentinel, i.e. vendor
        // hosting, which docs/78 keeps outside this set. Recorded as a divergence
        // instead of being faked with an unverifiable plug-in.
        // =========================================================================
        {
            const uint64_t stable = e2e::rev(session);
            const uint32_t modes[3] = {DAW_EXPORT_TAIL_AUTOMATIC, DAW_EXPORT_TAIL_NONE, DAW_EXPORT_TAIL_MANUAL_LIMIT};
            std::vector<Wav> policyRenders;
            for (uint32_t index = 0; index < 3; ++index) {
                const auto options = validOptions(modes[index], index == 2 ? 48000u : 0u);
                auto summary = abi<daw_export_tail_summary>();
                CHECK_OK(session, daw_get_export_tail_summary(session, &options, &summary));
                CHECK(summary.version == DAW_EXPORT_TAIL_SUMMARY_VERSION);
                CHECK(summary.infinite_tail_detected == 0);
                CHECK(summary.selected_tail_frames == summary.finite_tail_frames);
                const auto path = root / ("tail-" + std::to_string(index) + ".wav");
                auto* job = daw_begin_export_with_options(session, path.string().c_str(), 2, &options);
                CHECK(job != nullptr);
                const auto status = e2e::waitExport(job);
                daw_release_export(job);
                // The non-mutating preview is honest: the render is exactly the
                // program plus the tail frames that summary promised for this job.
                CHECK(status.total_frames == kDuration + summary.selected_tail_frames);
                const Wav rendered = e2e::readWavFixture(path);
                CHECK(rendered.frames() == kDuration + summary.selected_tail_frames);
                policyRenders.push_back(rendered);
            }
            // Nothing declared a tail, so all three agree with each other and with
            // the option-less render, byte for byte.
            CHECK(samplesEqual(policyRenders[0], mix));
            CHECK(samplesEqual(policyRenders[1], mix));
            CHECK(samplesEqual(policyRenders[2], mix));
            CHECK(e2e::rev(session) == stable);  // policy is per job, never durable

            // A range keeps its own half-open length under every policy.
            for (const uint32_t mode : modes) {
                const auto options = validOptions(mode, 4800u);
                const auto path = root / ("range-tail-" + std::to_string(mode) + ".wav");
                auto* job = daw_begin_export_range_with_options(session, path.string().c_str(), 2, kBedStart, kDuration,
                                                                &options);
                CHECK(job != nullptr);
                const auto status = e2e::waitExport(job);
                daw_release_export(job);
                CHECK(status.total_frames == kDuration - kBedStart);
                CHECK(e2e::readWavFixture(path).frames() == kDuration - kBedStart);
            }

            // Incompatible option shapes are refused before anything is written.
            const std::string optPath = (root / "opt.wav").string();
            expectJobRejection(session, "NULL options",
                               [&] { return daw_begin_export_with_options(session, optPath.c_str(), 2, nullptr); });
            daw_export_options zeroed{};  // the classic trap: struct_size 0, version 0, tail_mode 0
            expectJobRejection(session, "zeroed options",
                               [&] { return daw_begin_export_with_options(session, optPath.c_str(), 2, &zeroed); });
            auto smallSize = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            smallSize.struct_size = sizeof(daw_export_options) - 4;
            expectJobRejection(session, "short options struct",
                               [&] { return daw_begin_export_with_options(session, optPath.c_str(), 2, &smallSize); });
            auto bigSize = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            bigSize.struct_size = sizeof(daw_export_options) + 4;
            expectJobRejection(session, "oversized options struct",
                               [&] { return daw_begin_export_with_options(session, optPath.c_str(), 2, &bigSize); });
            auto badVersion = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            badVersion.version = DAW_EXPORT_OPTIONS_VERSION + 1;
            expectJobRejection(session, "unknown options version",
                               [&] { return daw_begin_export_with_options(session, optPath.c_str(), 2, &badVersion); });
            auto noMode = validOptions(0, 0);
            expectJobRejection(session, "tail mode 0",
                               [&] { return daw_begin_export_with_options(session, optPath.c_str(), 2, &noMode); });
            auto bogusMode = validOptions(99, 0);
            expectJobRejection(session, "unknown tail mode on a range", [&] {
                return daw_begin_export_range_with_options(session, optPath.c_str(), 2, 0, kDuration, &bogusMode);
            });
            // docs/60: a manual limit is capped at 30 seconds of frames.
            auto overCap = validOptions(DAW_EXPORT_TAIL_MANUAL_LIMIT, 48000u * 30u + 1u);
            expectJobRejection(session, "manual limit past 30 s",
                               [&] { return daw_begin_export_with_options(session, optPath.c_str(), 2, &overCap); });
            auto maxU32 = validOptions(DAW_EXPORT_TAIL_MANUAL_LIMIT, 4294967295u);
            expectJobRejection(session, "manual limit at UINT32_MAX",
                               [&] { return daw_begin_export_with_options(session, optPath.c_str(), 2, &maxU32); });
            // The summary call gates its own output buffer too.
            const auto good = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            auto tinySummary = abi<daw_export_tail_summary>();
            tinySummary.struct_size = sizeof(daw_export_tail_summary) - 4;
            expectIntRejection(session, "short summary struct",
                               [&] { return daw_get_export_tail_summary(session, &good, &tinySummary); });
            expectIntRejection(session, "NULL summary",
                               [&] { return daw_get_export_tail_summary(session, &good, nullptr); });
            expectIntRejection(session, "zeroed options into a valid summary",
                               [&] { return daw_get_export_tail_summary(session, &zeroed, &tinySummary); });
            // The 30 s edge itself is legal, and still adds nothing on this graph.
            const auto atCap = validOptions(DAW_EXPORT_TAIL_MANUAL_LIMIT, 48000u * 30u);
            auto* boundary = daw_begin_export_with_options(session, (root / "edge.wav").string().c_str(), 2, &atCap);
            CHECK(boundary != nullptr);
            const auto edgeStatus = e2e::waitExport(boundary);
            daw_release_export(boundary);
            CHECK(edgeStatus.total_frames == kDuration);
            CHECK(e2e::readWavFixture(root / "edge.wav").frames() == kDuration);
            CHECK(!std::filesystem::exists(root / "opt.wav"));
        }

        // =========================================================================
        // 6. Stems (docs/25 1.53): one pre-master WAV per audible track, silent
        //    tracks honestly skipped, sanitised "NN - name.wav" names, id filter.
        // =========================================================================
        {
            const auto dir = root / "stems";
            std::filesystem::create_directories(dir);
            const auto options = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            auto* job = daw_begin_stem_export(session, dir.string().c_str(), 2, &options);
            CHECK(job != nullptr);
            const auto status = e2e::waitExport(job);
            daw_release_export(job);
            // Progress aggregates across stems, and only audible strips count.
            CHECK(status.total_frames == 2 * kDuration && status.rendered_frames == 2 * kDuration);
            const std::vector<std::string> expected{"01 - Lead_Vox.wav", "02 - Bed_Sub.wav"};
            CHECK(listFiles(dir) == expected);  // '/' and ':' sanitised
            CHECK(!std::filesystem::exists(dir / "03 - Fly Muted.wav"));   // user-muted strip
            CHECK(!std::filesystem::exists(dir / "04 - Clip Muted.wav"));  // only muted clips
            CHECK(!std::filesystem::exists(dir / "05 - Empty Four.wav"));  // no material at all

            const Wav stemLead = e2e::readWavFixture(dir / "01 - Lead_Vox.wav");
            const Wav stemBed = e2e::readWavFixture(dir / "02 - Bed_Sub.wav");
            CHECK(stemLead.frames() == kDuration && stemBed.frames() == kDuration);  // whole program each
            CHECK(stemLead.floating && stemLead.channels == 2 && stemLead.bits == 32);
            // Pre-master by convention: the master half is NOT applied, the
            // strip's own bus stays in the path, and only this strip is audible.
            expectDcWindow(stemLead, kSettled, kBedStart, 0, kLeadLeft, "stem lead left");
            expectDcWindow(stemLead, kSettled, kBedStart, 1, kLeadRight, "stem lead right");
            expectDcWindow(stemLead, kRegion + kSettled, kDuration, 0, 0.0, "stem lead past its region");
            expectDcWindow(stemBed, kSettled, kBedStart, 0, 0.0, "stem bed before its region");
            expectDcWindow(stemBed, kBedStart + kSettled, kRegion, 0, kBedLeft * kHalfLinear, "stem bed through the bus");
            expectDcWindow(stemBed, kRegion + kSettled, kDuration, 0, kBedLeft * kHalfLinear, "stem bed alone");
            // Summing the stems and restoring the master gain once reproduces the mix.
            double worstSum = 0.0;
            for (uint64_t frame = kBedStart + kSettled; frame < kRegion; ++frame)
                for (uint16_t channel = 0; channel < 2; ++channel) {
                    const double sum =
                        (sampleAt(stemLead, frame, channel) + sampleAt(stemBed, frame, channel)) * kHalfLinear;
                    worstSum = std::max(worstSum, std::abs(sum - sampleAt(mix, frame, channel)));
                }
            CHECK(worstSum <= 1e-6);

            // PCM24 stems go through the same writer and the same header rules.
            const auto pcmDir = root / "stems-pcm24";
            std::filesystem::create_directories(pcmDir);
            auto* pcmJob = daw_begin_stem_export(session, pcmDir.string().c_str(), 1, &options);
            CHECK(pcmJob != nullptr);
            CHECK(e2e::waitExport(pcmJob).total_frames == 2 * kDuration);
            daw_release_export(pcmJob);
            CHECK(listFiles(pcmDir) == expected);
            for (const auto& name : expected) {
                const auto path = pcmDir / name;
                CHECK(rd16(headBytes(path, 44), 34) == 24);
                CHECK(fileSize(path) == 44 + kDuration * 6);
            }
        }
        {
            const auto options = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            // Filter: only the requested strips ship, and the "NN" keeps the slot.
            const auto dir = root / "stems-filtered";
            std::filesystem::create_directories(dir);
            const uint64_t picked[1] = {bed};
            auto* job = daw_begin_stem_export_tracks(session, dir.string().c_str(), 2, &options, picked, 1);
            CHECK(job != nullptr);
            const auto status = e2e::waitExport(job);
            daw_release_export(job);
            CHECK(status.total_frames == kDuration && status.rendered_frames == kDuration);  // filter-aware
            CHECK(listFiles(dir) == std::vector<std::string>({"02 - Bed_Sub.wav"}));
            CHECK(e2e::readWavFixture(dir / "02 - Bed_Sub.wav").frames() == kDuration);
            // Both audible strips in one filtered run reproduce the unfiltered pair.
            const auto both = root / "stems-both";
            std::filesystem::create_directories(both);
            const uint64_t pair[2] = {lead, bed};
            auto* pairJob = daw_begin_stem_export_tracks(session, both.string().c_str(), 2, &options, pair, 2);
            CHECK(pairJob != nullptr);
            e2e::waitExport(pairJob);
            daw_release_export(pairJob);
            CHECK(listFiles(both) == listFiles(root / "stems"));
            // NULL ids with count 0 is the documented "everything audible".
            const auto broad = root / "stems-broad";
            std::filesystem::create_directories(broad);
            auto* broadJob = daw_begin_stem_export_tracks(session, broad.string().c_str(), 2, &options, nullptr, 0);
            CHECK(broadJob != nullptr);
            e2e::waitExport(broadJob);
            daw_release_export(broadJob);
            CHECK(listFiles(broad) == listFiles(root / "stems"));

            // Filter negatives: ids resolve against the current revision BEFORE
            // the worker starts, so a rejected filter cannot write anything.
            const auto untouched = root / "stems-none";
            std::filesystem::create_directories(untouched);
            const std::string dirName = untouched.string();
            const uint64_t ghost[1] = {987654};
            expectJobRejection(session, "unknown stem id", [&] {
                return daw_begin_stem_export_tracks(session, dirName.c_str(), 2, &options, ghost, 1);
            });
            const uint64_t duplicate[2] = {lead, lead};
            expectJobRejection(session, "duplicate stem id", [&] {
                return daw_begin_stem_export_tracks(session, dirName.c_str(), 2, &options, duplicate, 2);
            });
            expectJobRejection(session, "ids without a list", [&] {
                return daw_begin_stem_export_tracks(session, dirName.c_str(), 2, &options, nullptr, 1);
            });
            const uint64_t silent[1] = {muted};
            expectJobRejection(session, "only a user-muted strip", [&] {
                return daw_begin_stem_export_tracks(session, dirName.c_str(), 2, &options, silent, 1);
            });
            const uint64_t clipOnly[1] = {clipMuted};
            expectJobRejection(session, "only clip-muted material", [&] {
                return daw_begin_stem_export_tracks(session, dirName.c_str(), 2, &options, clipOnly, 1);
            });
            const uint64_t emptyOnly[1] = {empty};
            expectJobRejection(session, "only an empty strip", [&] {
                return daw_begin_stem_export_tracks(session, dirName.c_str(), 2, &options, emptyOnly, 1);
            });
            auto badOptions = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            badOptions.version = 0;
            expectJobRejection(session, "zeroed options into stems", [&] {
                return daw_begin_stem_export_tracks(session, dirName.c_str(), 2, &badOptions, pair, 2);
            });
            CHECK(listFiles(untouched).empty());

            // Destination problems are reported, never swallowed: an unusable
            // stems directory fails and publishes no partial stem.
            expectJobFailure(session,
                             daw_begin_stem_export(session, (root / "no-such-dir").string().c_str(), 2, &options));
            CHECK(!std::filesystem::exists(root / "no-such-dir"));  // a refused stems run creates nothing
            const auto notADirectory = root / "a-file.wav";
            e2e::writeWavFixture(notADirectory, e2e::constantInterleaved(48, 2, 0.0f, 0.0f), e2e::kProjectRate, 2, "f32");
            const uint64_t fixtureSize = fileSize(notADirectory);
            expectJobFailure(session, daw_begin_stem_export(session, notADirectory.string().c_str(), 2, &options));
            CHECK(fileSize(notADirectory) == fixtureSize);  // still our own 48-frame fixture
            CHECK(e2e::readWavFixture(notADirectory).frames() == 48);
            expectJobRejection(session, "empty stems directory",
                               [&] { return daw_begin_stem_export(session, "", 2, &options); });
            expectJobRejection(session, "NULL stems directory",
                               [&] { return daw_begin_stem_export(session, nullptr, 2, &options); });
        }

        // =========================================================================
        // 7. Cancellation and atomic publish (docs/25, docs/27): the destination
        //    keeps the bytes it already had, and nothing partial is left behind.
        // =========================================================================
        {
            Bridge longBridge;
            daw_session* longProject = longBridge.get();
            constexpr uint32_t kLongFrames = e2e::kProjectRate * 30;  // 30 s: cancellation has a wide window
            const auto longFixture = root / "long-source.wav";
            e2e::writeWavFixture(longFixture, e2e::constantInterleaved(kLongFrames, 2, 0.25f, -0.25f),
                                 e2e::kProjectRate, 2, "f32");
            CHECK_OK(longProject, daw_import_wav(longProject, longFixture.string().c_str(), "Long", e2e::rev(longProject)));
            const auto target = root / "keep-me.wav";
            const std::vector<unsigned char> sentinel{'P', 'R', 'E', '-', 'E', 'X', 'I', 'S', 'T', 'I', 'N', 'G'};
            {
                std::ofstream stream(target, std::ios::binary);
                stream.write(reinterpret_cast<const char*>(sentinel.data()),
                             static_cast<std::streamsize>(sentinel.size()));
                CHECK(stream.good());
            }
            auto* job = daw_begin_export(longProject, target.string().c_str(), 2);
            CHECK(job != nullptr);
            daw_cancel_export(job);
            const auto status = e2e::waitExport(job, 3);
            CHECK(status.status == 3);
            CHECK(status.rendered_frames < status.total_frames);
            CHECK(fileSize(target) == sentinel.size());
            CHECK(headBytes(target, sentinel.size()) == sentinel);
            // A canceled job leaves no temporary in the destination directory.
            for (const auto& entry : std::filesystem::directory_iterator(root.path))
                CHECK(entry.path().filename().string().rfind(".mydaw-export-", 0) != 0);
            // Cancel is idempotent, and retrying the same destination still stops.
            daw_cancel_export(job);
            CHECK(e2e::waitExport(job, 3).status == 3);
            daw_release_export(job);
            auto* second = daw_begin_export(longProject, target.string().c_str(), 2);
            CHECK(second != nullptr);
            daw_cancel_export(second);
            e2e::waitExport(second, 3);
            daw_release_export(second);
            CHECK(headBytes(target, sentinel.size()) == sentinel);
            // The same path stays usable for a completed render afterwards: the
            // cancels left no lock, no stub and no half file.
            const Wav finished = e2e::exportProject(longProject, target, 1);
            CHECK(finished.frames() == kLongFrames);
            CHECK(fileSize(target) == 44 + static_cast<uint64_t>(kLongFrames) * 6);
            CHECK(e2e::channelPeak(finished, 0, kLongFrames, 0) > 0.1);
            CHECK(std::abs(e2e::channelPeak(finished, 0, kLongFrames, 1)) > 0.1);

            // The same rule per file: stems are published one whole WAV at a
            // time, so a cancellation may stop the run between files but must
            // never leave a truncated one behind.
            CHECK_OK(longProject, daw_import_wav(longProject, longFixture.string().c_str(), "Second",
                                                 e2e::rev(longProject)));
            const auto stemDir = root / "stems-cancel";
            std::filesystem::create_directories(stemDir);
            const auto stemOptions = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            auto* stems = daw_begin_stem_export(longProject, stemDir.string().c_str(), 2, &stemOptions);
            CHECK(stems != nullptr);
            daw_cancel_export(stems);
            const auto stemStatus = e2e::waitExport(stems, 3);
            daw_release_export(stems);
            CHECK(stemStatus.status == 3);
            CHECK(stemStatus.total_frames == 2 * static_cast<uint64_t>(kLongFrames));
            // docs/25: cancellation lands BETWEEN files, never inside one, so the
            // first stem is published whole and the second never appears. Progress
            // stops at exactly one stem, and what is on disk is a complete file.
            const auto published = listFiles(stemDir);
            CHECK(published.size() == 1);
            CHECK(published[0] == "01 - Long.wav");
            CHECK(stemStatus.rendered_frames == kLongFrames);
            CHECK(fileSize(stemDir / published[0]) == 58 + static_cast<uint64_t>(kLongFrames) * 8);
            const Wav halfDone = e2e::readWavFixture(stemDir / published[0]);
            CHECK(halfDone.frames() == kLongFrames && halfDone.channels == 2 && halfDone.bits == 32);
            CHECK(std::abs(e2e::channelPeak(halfDone, kSettled, kLongFrames, 0) - 0.25) <= kSmoothedTol);
        }

        // =========================================================================
        // 8. Loudness of the finished file (docs/25 1.56): the meter is
        //    cross-checked against our OWN reader's peak and RMS, on both
        //    encodings and on the documented BS.1770-4 anchors.
        // =========================================================================
        {
            Bridge toneBridge;
            daw_session* tone = toneBridge.get();
            CHECK_OK(tone, daw_import_wav(tone, tonePath.string().c_str(), "Tone", e2e::rev(tone)));
            const auto toneMixPath = root / "tone-mix.wav";
            const Wav toneMix = e2e::exportProject(tone, toneMixPath, 2);
            CHECK(toneMix.frames() == e2e::kProjectRate);
            auto report = abi<daw_loudness_report>();
            CHECK_OK(tone, daw_measure_wav(tone, toneMixPath.string().c_str(), &report));
            CHECK(report.gated_silence == 0);
            // Same bytes, independent math: a smooth 997 Hz tone has essentially
            // no inter-sample overshoot, so dBTP sits on the sample peak found by
            // our own reader and the K-weighted integrated value tracks it too.
            // Steady state only: the documented fader ramp at the head of the
            // render is real, and it would drag a whole-file RMS below A/sqrt(2).
            const uint32_t toneFrames = static_cast<uint32_t>(toneMix.frames());
            const double peak = e2e::channelPeak(toneMix, static_cast<uint32_t>(kSettled), toneFrames, 0);
            e2e::expectNear(peak, 0.5, kSmoothedTol, "rendered half-scale tone peak");
            e2e::expectNear(report.true_peak_db, dbfs(peak), 0.05, "dBTP vs independent sample peak");
            e2e::expectNear(report.integrated_lufs, dbfs(peak), 0.1, "integrated LUFS vs independent peak at 997 Hz");
            const double rms = e2e::channelRms(toneMix, static_cast<uint32_t>(kSettled), toneFrames, 0);
            e2e::expectNear(rms, peak / std::sqrt(2.0), 1e-3, "steady-state reader RMS of a sine against its peak");
            CHECK(report.integrated_lufs > dbfs(rms));  // K-weighting lifts a 1 kHz tone
            // And the WHOLE file is pinned too, not waved away. Its RMS sits 0.37%
            // under A/sqrt(2) for exactly one reason: the documented fader one-pole
            // at the head of the render. Predicting the file RMS from that closed
            // form (silence climbing to the plateau with a 240-sample time
            // constant) lands on the measured value to under 1e-5, so no loss of
            // its own is hiding in the WAV writer, the reader, or the export path -
            // a drift of that kind would be more than ten times this tolerance.
            double envelopeEnergy = 0.0;
            for (uint32_t frame = 0; frame < toneFrames; ++frame) {
                const double g = 1.0 - std::pow(1.0 - 0.004166667, static_cast<double>(frame) + 1.0);
                envelopeEnergy += g * g;
            }
            const double predictedRms = std::sqrt(0.5 * 0.5 * 0.5 * envelopeEnergy / toneFrames);
            e2e::expectNear(e2e::channelRms(toneMix, 0, toneFrames, 0), predictedRms, 1e-4,
                            "whole-file RMS against the documented head ramp");

            // The documented anchors: a full-scale 997 Hz sine sits at about 0
            // LUFS, and amplitude scales 1:1 in LUFS. PCM16 and float32 agree.
            const auto fullPath = root / "tone-full.wav";
            e2e::writeWavFixture(fullPath, e2e::sineInterleaved(e2e::kProjectRate, e2e::kProjectRate, 997.0, 1.0, 2),
                                 e2e::kProjectRate, 2, "f32");
            auto full = abi<daw_loudness_report>();
            CHECK_OK(tone, daw_measure_wav(tone, fullPath.string().c_str(), &full));
            e2e::expectNear(full.integrated_lufs, 0.0, 0.1, "full-scale 997 Hz anchor");
            CHECK(full.true_peak_db <= 0.2 && full.true_peak_db >= -0.1);  // 4x oversampling may peek over
            const auto quietPath = root / "tone-quiet.wav";
            e2e::writeWavFixture(quietPath, e2e::sineInterleaved(e2e::kProjectRate, e2e::kProjectRate, 997.0, 0.1, 2),
                                 e2e::kProjectRate, 2, "pcm16");
            auto quiet = abi<daw_loudness_report>();
            CHECK_OK(tone, daw_measure_wav(tone, quietPath.string().c_str(), &quiet));
            e2e::expectNear(full.integrated_lufs - quiet.integrated_lufs, 20.0, 0.1, "20 dB of amplitude is 20 LU");
            CHECK(quiet.gated_silence == 0);
            const auto monoPath = root / "tone-mono.wav";
            e2e::writeWavFixture(monoPath, e2e::sineInterleaved(e2e::kProjectRate / 4, e2e::kProjectRate, 997.0, 0.5, 1),
                                 e2e::kProjectRate, 1, "f32");
            auto mono = abi<daw_loudness_report>();
            CHECK_OK(tone, daw_measure_wav(tone, monoPath.string().c_str(), &mono));
            e2e::expectNear(mono.integrated_lufs, dbfs(0.5), 0.2, "a mono file measures like the same mono content");

            // Half the master: the file's own peak, the meter's integrated value
            // and its true peak must all move by that half and nothing else.
            CHECK_OK(tone, daw_set_master_gain(tone, kHalfDb, e2e::rev(tone)));
            const auto lowPath = root / "tone-half.wav";
            const Wav toneLow = e2e::exportProject(tone, lowPath, 2);
            auto low = abi<daw_loudness_report>();
            CHECK_OK(tone, daw_measure_wav(tone, lowPath.string().c_str(), &low));
            e2e::expectNear(report.integrated_lufs - low.integrated_lufs, 6.0206, 0.05, "half-master loudness step");
            e2e::expectNear(report.true_peak_db - low.true_peak_db, 6.0206, 0.05, "half-master true-peak step");
            const uint32_t lowFrames = static_cast<uint32_t>(toneLow.frames());
            const double lowPeak = e2e::channelPeak(toneLow, static_cast<uint32_t>(kSettled), lowFrames, 0);
            e2e::expectNear(lowPeak / peak, kHalfLinear, 1e-4, "half-master independent peak ratio");
            e2e::expectNear(e2e::channelRms(toneLow, static_cast<uint32_t>(kSettled), lowFrames, 0) / rms, kHalfLinear,
                            1e-3, "half-master independent RMS ratio");
            // Quantising to PCM24 changes what the meter says by less than the
            // documented dither, and stays inside it sample by sample.
            const auto tone24Path = root / "tone-half-pcm24.wav";
            const Wav tone24 = e2e::exportProject(tone, tone24Path, 1);
            auto quantised = abi<daw_loudness_report>();
            CHECK_OK(tone, daw_measure_wav(tone, tone24Path.string().c_str(), &quantised));
            e2e::expectNear(quantised.integrated_lufs, low.integrated_lufs, 0.05, "PCM24 vs float32 integrated");
            e2e::expectNear(quantised.true_peak_db, low.true_peak_db, 0.05, "PCM24 vs float32 true peak");
            CHECK(quantised.gated_silence == 0);
            double worst24 = 0.0;
            for (uint64_t frame = 0; frame < tone24.frames(); ++frame)
                for (uint16_t channel = 0; channel < 2; ++channel)
                    worst24 = std::max(worst24, std::abs(sampleAt(tone24, frame, channel) - sampleAt(toneLow, frame, channel)));
            CHECK(worst24 <= 2.0 * kLsb24);

            // The DC program mix: hard region edges DO overshoot under the 4x
            // oversampled true peak, so the honest relation here is "never below
            // the sample peak our reader found, and bounded above by it".
            auto mixReport = abi<daw_loudness_report>();
            CHECK_OK(session, daw_measure_wav(session, mixPath.string().c_str(), &mixReport));
            const double mixPeak = std::max(e2e::channelPeak(mix, 0, static_cast<uint32_t>(kDuration), 0),
                                            e2e::channelPeak(mix, 0, static_cast<uint32_t>(kDuration), 1));
            CHECK(mixReport.true_peak_db >= dbfs(mixPeak) - 0.05);
            CHECK(mixReport.true_peak_db <= dbfs(mixPeak) + 1.5);
            CHECK(mixReport.gated_silence == 0);
            CHECK(mixReport.integrated_lufs < report.integrated_lufs);  // the DC mix really is quieter

            // Digital silence is honestly reported as gated silence with the
            // documented sentinels, never as a loudness number.
            Bridge quietBridge;
            const auto silentStrip = importDc(quietBridge.get(), leadPath, "Only Muted");
            CHECK_OK(quietBridge.get(), daw_set_mute(quietBridge.get(), silentStrip, 1, e2e::rev(quietBridge.get())));
            const auto silencePath = root / "silent-mix.wav";
            const Wav silentMix = e2e::exportProject(quietBridge.get(), silencePath, 2);
            auto silent = abi<daw_loudness_report>();
            CHECK_OK(quietBridge.get(), daw_measure_wav(quietBridge.get(), silencePath.string().c_str(), &silent));
            CHECK(silent.gated_silence == 1);
            CHECK(silent.integrated_lufs == -100.0 && silent.true_peak_db == -400.0);
            CHECK(e2e::channelPeak(silentMix, 0, static_cast<uint32_t>(silentMix.frames()), 0) == 0.0);
            // The reader gates of the measure API.
            expectIntRejection(tone, "missing file", [&] {
                return daw_measure_wav(tone, (root / "no-such-file.wav").string().c_str(), &report);
            });
            const auto junkPath = root / "junk.wav";
            {
                std::ofstream stream(junkPath);
                stream << "not a RIFF/WAVE file at all";
                CHECK(stream.good());
            }
            expectIntRejection(tone, "junk where a WAV should be",
                               [&] { return daw_measure_wav(tone, junkPath.string().c_str(), &report); });
            expectIntRejection(tone, "NULL path", [&] { return daw_measure_wav(tone, nullptr, &report); });
            expectIntRejection(tone, "NULL report",
                               [&] { return daw_measure_wav(tone, toneMixPath.string().c_str(), nullptr); });
            auto tinyReport = abi<daw_loudness_report>();
            tinyReport.struct_size = sizeof(daw_loudness_report) - 4;
            expectIntRejection(tone, "short loudness report",
                               [&] { return daw_measure_wav(tone, toneMixPath.string().c_str(), &tinyReport); });
        }

        // =========================================================================
        // 9. Live meter ABI around a finished render (docs/49, docs/25 1.57).
        //
        // daw_get_channel_meter and daw_get_master_loudness read the REALTIME
        // renderer only: daw.h promises zero peaks when no renderer is playing
        // and docs/49 repeats it for the stopped state, with -200 LUFS sentinels
        // and live = 0. An offline export renders on its own frozen renderer and
        // never feeds session telemetry, and opening an output device is out of
        // the e2e bounds (docs/78 keeps the physical arc in e2e_transport). So
        // this section proves what the ABI really offers headless: the documented
        // silent contract holds before and after an export - a render cannot fake
        // live numbers - the strip is still validated while no renderer exists,
        // and the ABI gates work. The file-level cross-check of peak and RMS is
        // section 8, the only meter that CAN see an export.
        // =========================================================================
        {
            const uint64_t stable = e2e::rev(session);
            auto meter = abi<daw_channel_meter>();
            CHECK_OK(session, daw_get_channel_meter(session, DAW_INSERT_OWNER_TRACK, lead, &meter));
            CHECK(meter.version == DAW_CHANNEL_METER_VERSION);
            CHECK(meter.left_peak == 0.0f && meter.right_peak == 0.0f);
            CHECK_OK(session, daw_get_channel_meter(session, DAW_INSERT_OWNER_BUS, room, &meter));
            CHECK(meter.left_peak == 0.0f && meter.right_peak == 0.0f);
            CHECK_OK(session, daw_get_channel_meter(session, DAW_INSERT_OWNER_MASTER, 0, &meter));
            CHECK(meter.left_peak == 0.0f && meter.right_peak == 0.0f);
            auto loudness = abi<daw_master_loudness>();
            CHECK_OK(session, daw_get_master_loudness(session, &loudness));
            CHECK(loudness.version == DAW_MASTER_LOUDNESS_VERSION);
            CHECK(loudness.live == 0 && loudness.momentary_lufs == -200.0f && loudness.short_term_lufs == -200.0f);
            // Rendering both encodings again changes nothing about telemetry.
            CHECK(samplesEqual(e2e::exportProject(session, root / "mix-f32-c.wav", 2), mix));
            CHECK(samplesEqual(e2e::exportProject(session, root / "mix-pcm24-c.wav", 1), pcm));
            CHECK_OK(session, daw_get_channel_meter(session, DAW_INSERT_OWNER_MASTER, 0, &meter));
            CHECK(meter.left_peak == 0.0f && meter.right_peak == 0.0f);
            CHECK_OK(session, daw_get_master_loudness(session, &loudness));
            CHECK(loudness.live == 0 && loudness.momentary_lufs == -200.0f && loudness.short_term_lufs == -200.0f);
            // A stale strip id cannot masquerade as a legitimately stopped meter.
            expectIntRejection(session, "unknown track meter",
                               [&] { return daw_get_channel_meter(session, DAW_INSERT_OWNER_TRACK, 987654, &meter); });
            expectIntRejection(session, "unknown bus meter",
                               [&] { return daw_get_channel_meter(session, DAW_INSERT_OWNER_BUS, 987654, &meter); });
            expectIntRejection(session, "unknown owner kind",
                               [&] { return daw_get_channel_meter(session, 99, 0, &meter); });
            expectIntRejection(session, "master owner with a nonzero id",
                               [&] { return daw_get_channel_meter(session, DAW_INSERT_OWNER_MASTER, 7, &meter); });
            auto smallMeter = abi<daw_channel_meter>();
            smallMeter.struct_size = sizeof(daw_channel_meter) - 4;
            expectIntRejection(session, "short channel meter",
                               [&] { return daw_get_channel_meter(session, DAW_INSERT_OWNER_MASTER, 0, &smallMeter); });
            expectIntRejection(session, "NULL channel meter",
                               [&] { return daw_get_channel_meter(session, DAW_INSERT_OWNER_MASTER, 0, nullptr); });
            auto smallLoudness = abi<daw_master_loudness>();
            smallLoudness.struct_size = sizeof(daw_master_loudness) - 4;
            expectIntRejection(session, "short master loudness",
                               [&] { return daw_get_master_loudness(session, &smallLoudness); });
            expectIntRejection(session, "NULL master loudness",
                               [&] { return daw_get_master_loudness(session, nullptr); });
            CHECK(e2e::rev(session) == stable);
        }

        // =========================================================================
        // 10. Last negatives: formats, paths, and a project with nothing audible.
        // =========================================================================
        {
            const uint64_t stable = e2e::rev(session);
            const std::string neverPath = (root / "never.wav").string();
            for (const int32_t bogus : {0, 3, -1, 99}) {
                expectJobRejection(session, "unsupported export format",
                                   [&] { return daw_begin_export(session, neverPath.c_str(), bogus); });
                expectJobRejection(session, "unsupported range format",
                                   [&] { return daw_begin_export_range(session, neverPath.c_str(), bogus, 0, 1000); });
            }
            expectJobRejection(session, "NULL export path", [&] { return daw_begin_export(session, nullptr, 2); });
            expectJobRejection(session, "NULL range path",
                               [&] { return daw_begin_export_range(session, nullptr, 2, 0, 1000); });
            CHECK(!std::filesystem::exists(root / "never.wav"));
            // A destination whose parent is missing is reported by the job
            // itself; nothing is published and the tree stays as it was.
            const auto missingParent = root / "no-such-parent" / "child.wav";
            expectJobFailure(session, daw_begin_export(session, missingParent.string().c_str(), 2));
            CHECK(!std::filesystem::exists(missingParent));
            CHECK(!std::filesystem::exists(root / "no-such-parent"));
            // Polling is gated by the caller's buffer, not by the session.
            auto poll = abi<daw_export_status>();
            poll.struct_size = sizeof(daw_export_status) - 4;
            auto* pollJob = daw_begin_export(session, (root / "poll.wav").string().c_str(), 2);
            CHECK(pollJob != nullptr);
            CHECK(daw_poll_export(pollJob, &poll) == 1);
            CHECK(daw_poll_export(pollJob, nullptr) == 1);
            e2e::waitExport(pollJob);
            daw_release_export(pollJob);
            // An empty project cannot be printed, nor stemmed.
            Bridge bare;
            expectJobRejection(bare.get(), "nothing to export",
                               [&] { return daw_begin_export(bare.get(), (root / "bare.wav").string().c_str(), 2); });
            const auto options = validOptions(DAW_EXPORT_TAIL_AUTOMATIC, 0);
            expectJobRejection(bare.get(), "nothing to stem",
                               [&] { return daw_begin_stem_export(bare.get(), root.path.string().c_str(), 2, &options); });
            CHECK(!std::filesystem::exists(root / "bare.wav"));
            // Nothing in this section moved the project or the published mix.
            CHECK(samplesEqual(e2e::exportProject(session, root / "final.wav", 2), mix));
            CHECK(e2e::rev(session) == stable);
        }

        std::cout << "PASS: e2e_export_mastering - float32/PCM24 header and sample proofs against fixture "
                     "arithmetic, deterministic re-renders, half-open ranges and loop bounds, versioned tail "
                     "policy with its validation matrix, stems with pre-master convention and track filter, "
                     "cancellation that leaves the old file alone, loudness cross-checked against independent "
                     "peak and RMS, live-meter and path/format rejections with a frozen revision" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << std::endl;
        return 1;
    }
}
