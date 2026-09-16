// e2e: CUT-01 — cutting the timeline, checked twice: in the model and in the
// rendered bounce.
//
// Two sessions, two jobs.
//
//   "exact"  a DC clip, where the renderer's output can be compared bit for
//            bit: splitting is non-destructive, two linear crossfades over
//            continuous material sum to exactly unity, clip gain scales by
//            10^(dB/20), a muted clip is exactly silent.
//   "cut"    a staircase clip - one constant, different level per block - so a
//            rendered level reads back as a source position. Move, trim, fade,
//            loop, group edits, comping and track surgery are all audible.
//
// Expectations follow docs/19, 21, 22, 28, 31 and 76; nothing here is invented.
#include "e2e.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// A rejected mutation must not consume a revision: the domain checks
// expected_revision before touching anything (docs/19, docs/76), so conflict
// and boundary errors leave the timeline exactly as found.
#define CHECK_REJ_KEEPS_REV(session, call) do { const uint64_t stable_ = e2e::rev(session); CHECK_REJ(session, call); if (e2e::rev(session) != stable_) throw std::runtime_error(std::string("rejected call still consumed a revision: ") + #call); } while (false)

namespace {

// A staircase: 'blocks' constant blocks of 'blockFrames' frames, the b-th block
// holding +/- (b+1)/scale. Every value is a binary fraction, so a PCM16
// container round-trips it exactly and a rendered level identifies a position.
std::vector<float> staircase(uint32_t blocks, uint32_t blockFrames, uint32_t scale) {
    std::vector<float> out;
    out.reserve(static_cast<size_t>(blocks) * blockFrames * 2);
    for (uint32_t block = 0; block < blocks; ++block)
        for (uint32_t frame = 0; frame < blockFrames; ++frame) {
            out.push_back(static_cast<float>(block + 1) / static_cast<float>(scale));
            out.push_back(-static_cast<float>(block + 1) / static_cast<float>(scale));
        }
    return out;
}

double level(const e2e::Wav& render, uint32_t frame, uint16_t channel) {
    return render.samples[static_cast<size_t>(frame) * render.channels + channel];
}

daw_clip clipAt(daw_session* session, uint64_t trackId, uint32_t index) {
    auto out = e2e::abi<daw_clip>();
    CHECK_OK(session, daw_get_clip(session, trackId, index, &out));
    return out;
}

std::vector<daw_clip> regions(daw_session* session, uint64_t trackId) {
    std::vector<daw_clip> out;
    const auto track = e2e::trackById(session, trackId);
    for (uint32_t index = 0; index < track.clip_count; ++index) out.push_back(clipAt(session, trackId, index));
    return out;
}

uint64_t materialEnd(daw_session* session, uint64_t trackId) {
    uint64_t end = 0;
    for (const auto& region : regions(session, trackId)) end = std::max(end, region.start + region.length);
    return end;
}

uint64_t trackNamed(daw_session* session, const std::string& name) {
    const auto snap = e2e::snapshotOf(session);
    for (uint32_t index = 0; index < snap.track_count; ++index) {
        auto track = e2e::abi<daw_track>();
        CHECK_OK(session, daw_get_track(session, index, &track));
        if (name == track.name) return track.id;
    }
    throw std::runtime_error("no track named " + name);
}

// Bit-for-bit render comparison: a float32 export of PCM clips with no plug-ins
// is deterministic, so "the user hears the same thing" can be literal.
void expectIdentical(const e2e::Wav& left, const e2e::Wav& right, const char* what) {
    if (left.frames() != right.frames())
        throw std::runtime_error(std::string(what) + ": render lengths differ " + std::to_string(left.frames()) + " vs " +
                                 std::to_string(right.frames()));
    CHECK(left.channels == right.channels);
    for (size_t index = 0; index < left.samples.size(); ++index)
        if (left.samples[index] != right.samples[index])
            throw std::runtime_error(std::string(what) + ": renders differ at sample " + std::to_string(index) +
                                     " (" + std::to_string(left.samples[index]) + " vs " + std::to_string(right.samples[index]) + ")");
}

// Same, over one window of frames - used when the timeline itself grew.
void expectRangeIdentical(const e2e::Wav& left, const e2e::Wav& right, uint32_t from, uint32_t to, const char* what) {
    for (uint32_t frame = from; frame < to; ++frame)
        for (uint16_t channel = 0; channel < 2; ++channel)
            if (level(left, frame, channel) != level(right, frame, channel))
                throw std::runtime_error(std::string(what) + ": renders differ at frame " + std::to_string(frame) +
                                         " channel " + std::to_string(channel));
}

// A rendered frame inside a settled region equals its clip sample scaled by the
// track fader smoother, which converges geometrically toward unity and stops
// once its increment falls below half an ULP - a bounded ~7.1e-6 relative
// offset (measured). Tolerance 1e-5 absolute therefore leaves no room for a
// wrong source window (that is off by 1/16 = 0.0625) or a wrong gain.
void expectLevel(double actual, double expected, const char* what = "rendered level") { e2e::expectNear(actual, expected, 1e-5, what); }

} // namespace

int main() {
    try {
        using namespace e2e;
        TempRoot root("timeline-editing");

        const auto dcPath = root / "dc.wav";
        writeWavFixture(dcPath, constantInterleaved(2 * kProjectRate, 2, 0.25f, -0.25f), kProjectRate, 2, "pcm16");
        const auto stairPath = root / "stair-a.wav";
        writeWavFixture(stairPath, staircase(8, kProjectRate / 4, 16), kProjectRate, 2, "pcm16");
        const auto shortPath = root / "stair-b.wav";
        writeWavFixture(shortPath, staircase(4, kProjectRate / 4, 8), kProjectRate, 2, "pcm16");
        const Wav dcFixture = readWavFixture(dcPath);
        CHECK(dcFixture.frames() == 2 * kProjectRate && level(dcFixture, 0, 0) == 0.25 && level(dcFixture, 0, 1) == -0.25);
        const Wav stairFixture = readWavFixture(stairPath);
        CHECK(stairFixture.frames() == 2 * kProjectRate);
        // One binary fraction per block: block b reads +/- (b+1)/16, and PCM16
        // round-trips every one of those values exactly.
        CHECK(level(stairFixture, 0, 0) == 1.0 / 16.0 && level(stairFixture, kProjectRate / 4 - 1, 0) == 1.0 / 16.0);
        CHECK(level(stairFixture, kProjectRate / 4, 0) == 2.0 / 16.0 && level(stairFixture, kProjectRate / 2, 0) == 3.0 / 16.0);
        CHECK(level(stairFixture, 2 * kProjectRate - 1, 0) == 0.5 && level(stairFixture, 2 * kProjectRate - 1, 1) == -0.5);
        const Wav shortFixture = readWavFixture(shortPath);
        CHECK(shortFixture.frames() == kProjectRate && level(shortFixture, 0, 1) == -0.125);

        // ===================================================================
        // A. "exact": DC material, so equality means bit equality.
        // ===================================================================
        Bridge exactBridge;
        daw_session* exact = exactBridge.get();

        // A1. One clip holding the whole 2-second source.
        CHECK_OK(exact, daw_import_wav(exact, dcPath.string().c_str(), "DC", rev(exact)));
        const auto dcTrack = trackById(exact, 1);
        CHECK(dcTrack.clip_count == 1 && dcTrack.audio_frames == 2 * kProjectRate);
        const auto baseRender = exportProject(exact, root / "dc-imported.wav", 2);
        CHECK(baseRender.frames() == 2 * kProjectRate); // project duration = the region end
        CHECK(baseRender.sampleRate == kProjectRate && baseRender.channels == 2 && baseRender.bits == 32);
        // The fader opens from silence and settles onto the source value.
        CHECK(level(baseRender, 0, 0) > 0.0 && level(baseRender, 0, 0) < 0.01);
        expectLevel(level(baseRender, 95999, 0), 0.25);
        expectLevel(level(baseRender, 95999, 1), -0.25);

        // A2. Split is non-destructive: the cut adds a region, never a sample.
        const auto splitBase = rev(exact);
        CHECK_OK(exact, daw_split_clip(exact, 1, 0, kProjectRate, splitBase));
        CHECK(rev(exact) == splitBase + 1); // one command, one revision
        CHECK(trackById(exact, 1).audio_frames == 2 * kProjectRate); // the source is untouched
        const auto halves = regions(exact, 1);
        CHECK(halves.size() == 2);
        CHECK(halves[0].start == 0 && halves[0].source_offset == 0 && halves[0].length == kProjectRate);
        CHECK(halves[1].start == kProjectRate && halves[1].source_offset == kProjectRate);
        CHECK(halves[1].length == kProjectRate);
        CHECK(halves[0].length + halves[1].length == dcTrack.audio_frames); // tiles the original window
        CHECK(halves[0].start + halves[0].length == halves[1].start); // ... with no gap and no overlap
        CHECK(halves[1].start + halves[1].length == 2 * kProjectRate); // ... and the same end
        CHECK(halves[0].take_index == 0 && halves[1].take_index == 0); // both still read the same take
        const auto afterSplit = exportProject(exact, root / "dc-split.wav", 2);
        expectIdentical(baseRender, afterSplit, "split changed the audible timeline");
        CHECK_REJ_KEEPS_REV(exact, daw_split_clip(exact, 1, 0, 0, rev(exact))); // outside the clip
        CHECK_REJ_KEEPS_REV(exact, daw_split_clip(exact, 1, 0, 2 * kProjectRate, rev(exact))); // on its end
        CHECK(trackById(exact, 1).clip_count == 2); // the rejections changed nothing

        // A3. Clip gain is exactly 10^(dB/20) against the reference render.
        const double minusSixDb = std::pow(10.0, -6.0 / 20.0);
        CHECK_OK(exact, daw_set_clip_gain(exact, 1, 0, -6.0, rev(exact)));
        CHECK(clipAt(exact, 1, 0).gain_db == -6.0);
        const auto gained = exportProject(exact, root / "dc-gain.wav", 2);
        CHECK(gained.frames() == baseRender.frames());
        for (uint32_t frame = 4000; frame < kProjectRate; frame += 37)
            expectNear(level(gained, frame, 0) / level(baseRender, frame, 0), minusSixDb, 1e-6, "clip gain ratio");
        for (uint32_t frame = kProjectRate; frame < 2 * kProjectRate; ++frame)
            CHECK(level(gained, frame, 0) == level(baseRender, frame, 0)); // the other clip is untouched
        CHECK_OK(exact, daw_set_clip_gain(exact, 1, 0, 0.0, rev(exact))); // back to unity
        expectIdentical(baseRender, exportProject(exact, root / "dc-unity.wav", 2), "gain reset did not restore the render");
        CHECK_REJ_KEEPS_REV(exact, daw_set_clip_gain(exact, 1, 0, 60.0, rev(exact))); // documented -60..12 dB
        CHECK(clipAt(exact, 1, 0).gain_db == 0.0);

        // A4. Clip pan moves the balance; a hard pan silences one channel.
        CHECK_OK(exact, daw_set_clip_pan(exact, 1, 1, 0.5, rev(exact)));
        CHECK(clipAt(exact, 1, 1).pan == 0.5);
        const auto panned = exportProject(exact, root / "dc-pan.wav", 2);
        // pan > 0 keeps the right leg at unity and scales the left by 1 - pan,
        // so the balance moves without changing the program level.
        for (uint32_t frame = kProjectRate + 100; frame < 2 * kProjectRate; frame += 37) {
            expectNear(level(panned, frame, 0) / level(baseRender, frame, 0), 0.5, 1e-6, "panned left leg");
            CHECK(level(panned, frame, 1) == level(baseRender, frame, 1)); // right leg at unity
        }
        CHECK_OK(exact, daw_set_clip_pan(exact, 1, 1, -1.0, rev(exact)));
        const auto hard = exportProject(exact, root / "dc-pan-hard.wav", 2);
        for (uint32_t frame = kProjectRate; frame < 2 * kProjectRate; ++frame) CHECK(level(hard, frame, 1) == 0.0);
        for (uint32_t frame = kProjectRate; frame < 2 * kProjectRate; frame += 61)
            CHECK(level(hard, frame, 0) == level(baseRender, frame, 0)); // left leg unchanged
        CHECK_REJ_KEEPS_REV(exact, daw_set_clip_pan(exact, 1, 1, 1.5, rev(exact))); // documented -1..1
        CHECK(clipAt(exact, 1, 1).pan == -1.0);
        CHECK_OK(exact, daw_set_clip_pan(exact, 1, 1, 0.0, rev(exact)));

        // A5. A muted clip is exactly silent - the voice is never built, so the
        // window is zero rather than merely quiet - and only in its own window.
        // Muting the first half keeps a live region behind it, which is what
        // fixes the render length: muted regions do not extend it.
        CHECK_OK(exact, daw_set_clip_muted(exact, 1, 0, 1, rev(exact)));
        CHECK(clipAt(exact, 1, 0).muted == 1);
        const auto muted = exportProject(exact, root / "dc-muted.wav", 2);
        CHECK(muted.frames() == 2 * kProjectRate);
        CHECK(channelPeak(muted, 0, kProjectRate, 0) == 0.0);
        CHECK(channelPeak(muted, 0, kProjectRate, 1) == 0.0);
        for (uint32_t frame = 0; frame < kProjectRate; frame += 97) {
            CHECK(level(muted, frame, 0) == 0.0);
            CHECK(level(muted, frame, 1) == 0.0);
        }
        for (uint32_t frame = kProjectRate; frame < 2 * kProjectRate; ++frame)
            CHECK(level(muted, frame, 0) == level(baseRender, frame, 0)); // the live clip is unaffected
        CHECK_OK(exact, daw_set_clip_muted(exact, 1, 0, 0, rev(exact)));
        expectIdentical(baseRender, exportProject(exact, root / "dc-unmuted.wav", 2), "mute/unmute is not reversible");
        // And the truncation the other way, as measured behaviour worth naming:
        // silence the last region and the bounce ends where the audible audio
        // does - the project has no other material to reach.
        CHECK_OK(exact, daw_set_clip_muted(exact, 1, 1, 1, rev(exact)));
        const auto tailMuted = exportProject(exact, root / "dc-tail-muted.wav", 2);
        CHECK(tailMuted.frames() == kProjectRate);
        CHECK(channelPeak(tailMuted, 0, kProjectRate, 0) > 0.2); // the live half is intact
        CHECK_OK(exact, daw_set_clip_muted(exact, 1, 1, 0, rev(exact)));
        expectIdentical(baseRender, exportProject(exact, root / "dc-restored.wav", 2), "the second mute/unmute round trip");

        // A6. Crossfades: linear fades over continuous material sum to exactly 1.
        const uint64_t overlap = 480;
        const auto crossfadeBase = rev(exact);
        CHECK_OK(exact, daw_set_crossfade(exact, 1, 0, overlap, crossfadeBase));
        CHECK(rev(exact) == crossfadeBase + 1);
        const auto crossed = regions(exact, 1);
        CHECK(crossed.size() == 2);
        CHECK(crossed[0].start == 0 && crossed[0].length == kProjectRate);
        CHECK(crossed[0].fade_out == overlap); // the boundary fades are the overlap
        CHECK(crossed[1].fade_in == overlap);
        CHECK(crossed[1].start == kProjectRate - overlap); // the right clip took the space
        CHECK(crossed[1].source_offset == kProjectRate - overlap); // ... and its read with it
        CHECK(crossed[1].length == kProjectRate + overlap); // total material unchanged
        CHECK(crossed[1].start + crossed[1].length == 2 * kProjectRate);
        const auto crossRender = exportProject(exact, root / "dc-crossfade.wav", 2);
        expectIdentical(baseRender, crossRender, "crossfade changed continuous audio");
        // Smooth by construction: not one sample-to-sample step inside the
        // overlap, and a flat 120-frame energy scan across it.
        double jump = 0.0;
        for (uint32_t frame = kProjectRate - overlap + 1; frame < kProjectRate + overlap; ++frame)
            jump = std::max(jump, std::abs(level(crossRender, frame, 0) - level(crossRender, frame - 1, 0)));
        CHECK(jump == 0.0); // docs/28: no notch, no click
        for (uint32_t from = kProjectRate - 600; from + 120 <= kProjectRate + 600; from += 120)
            expectNear(channelRms(crossRender, from, from + 120, 0), channelRms(baseRender, from, from + 120, 0),
                       1e-9, "crossfade energy stays flat");
        CHECK_REJ_KEEPS_REV(exact, daw_set_crossfade(exact, 1, 0, overlap * 100, rev(exact))); // beyond min(len)/2
        CHECK(regions(exact, 1)[0].fade_out == overlap);

        // ===================================================================
        // B. "cut": staircase material, so a level is a position.
        // ===================================================================
        Bridge cutBridge;
        daw_session* cut = cutBridge.get();

        // B1. Import one clip carrying the staircase.
        CHECK_OK(cut, daw_import_wav(cut, stairPath.string().c_str(), "Stair", rev(cut)));
        CHECK(trackById(cut, 1).clip_count == 1);
        CHECK(trackById(cut, 1).audio_frames == 2 * kProjectRate);

        // B2. Move + trim through daw_edit_clip: geometry, and where the
        //     material lands. Source frame 24 000 is the third block: 3/16.
        const auto editBase = rev(cut);
        CHECK_OK(cut, daw_edit_clip(cut, 1, 0, kProjectRate, kProjectRate / 2, kProjectRate, editBase));
        CHECK(rev(cut) == editBase + 1);
        const auto moved = clipAt(cut, 1, 0);
        CHECK(moved.start == kProjectRate && moved.source_offset == kProjectRate / 2 && moved.length == kProjectRate);
        CHECK(trackById(cut, 1).clip_count == 1);
        const auto movedRender = exportProject(cut, root / "stair-moved.wav", 2);
        CHECK(movedRender.frames() == 2 * kProjectRate); // the duration followed the region
        CHECK(channelPeak(movedRender, 0, kProjectRate, 0) == 0.0); // everything before it is silence
        expectLevel(level(movedRender, kProjectRate, 0), 3.0 / 16.0); // first frame of the trimmed source
        expectLevel(level(movedRender, kProjectRate + kProjectRate / 4, 0), 4.0 / 16.0); // one block later
        expectLevel(level(movedRender, kProjectRate + kProjectRate / 4, 1), -4.0 / 16.0); // mirrored right
        // Geometry that would read outside the source, or off the timeline, is
        // refused without touching the revision or the model.
        const auto boundsBase = rev(cut);
        CHECK_REJ(cut, daw_edit_clip(cut, 1, 0, kProjectRate, kProjectRate, kProjectRate + 1, boundsBase));
        CHECK(rev(cut) == boundsBase);
        CHECK_REJ(cut, daw_edit_clip(cut, 1, 0, kProjectRate, 0, 0, boundsBase)); // zero length
        CHECK(rev(cut) == boundsBase);
        CHECK_REJ(cut, daw_edit_clip(cut, 1, 0, 48000ULL * 600ULL, 0, kProjectRate, boundsBase)); // past 10 min
        CHECK(rev(cut) == boundsBase);
        CHECK_REJ(cut, daw_edit_clip(cut, 1, 7, 0, 0, kProjectRate, boundsBase)); // no such clip
        CHECK(rev(cut) == boundsBase);
        const auto afterRejections = regions(cut, 1);
        CHECK(afterRejections.size() == 1 && afterRejections[0].start == kProjectRate &&
              afterRejections[0].source_offset == kProjectRate / 2 && afterRejections[0].length == kProjectRate);
        CHECK_OK(cut, daw_edit_clip(cut, 1, 0, kProjectRate, kProjectRate / 2, kProjectRate, boundsBase)); // no-op
        CHECK(rev(cut) == boundsBase); // docs/76: a no-op does not create history

        // B3. daw_edit_clip_full writes geometry and fades in one command.
        CHECK_OK(cut, daw_edit_clip_full(cut, 1, 0, 12000, kProjectRate / 2, 48000, 120, 240, rev(cut)));
        const auto faded = clipAt(cut, 1, 0);
        CHECK(faded.start == 12000 && faded.source_offset == kProjectRate / 2 && faded.length == 48000);
        CHECK(faded.fade_in == 120 && faded.fade_out == 240);
        const auto fadedRender = exportProject(cut, root / "stair-fades.wav", 2);
        CHECK(fadedRender.frames() == 60000); // 12 000 + 48 000
        // docs/22: the first sample of the fade-in and the last sample of the
        // fade-out are zero - exactly, not approximately.
        CHECK(level(fadedRender, 12000, 0) == 0.0);
        CHECK(level(fadedRender, 59999, 0) == 0.0);
        CHECK(level(fadedRender, 59998, 0) > 0.0); // the ramp is only zero at its very end
        expectLevel(level(fadedRender, 12119, 0), 3.0 / 16.0); // ramp complete at fadeIn-1
        for (uint32_t frame = 12001; frame < 12119; ++frame)
            CHECK(level(fadedRender, frame, 0) >= level(fadedRender, frame - 1, 0)); // never dips
        // Linear ramp: a quarter of the way through it, the level is under half.
        CHECK(level(fadedRender, 12030, 0) < 0.5 * level(fadedRender, 12119, 0));
        for (uint32_t frame = 59770; frame < 59999; ++frame)
            CHECK(level(fadedRender, frame, 0) >= level(fadedRender, frame + 1, 0)); // down, not out
        expectLevel(level(fadedRender, 59760, 0), 6.0 / 16.0); // full level where the fade-out starts
        CHECK(channelRms(fadedRender, 12000, 12120, 0) < 0.5 * channelRms(fadedRender, 30000, 30120, 0));
        CHECK_REJ_KEEPS_REV(cut, daw_set_clip_fades(cut, 1, 0, 40000, 40000, rev(cut))); // sum exceeds length
        CHECK(clipAt(cut, 1, 0).fade_in == 120);

        // B4. daw_set_clip_fades on its own: audible ramps at both ends.
        CHECK_OK(cut, daw_set_clip_fades(cut, 1, 0, 480, 480, rev(cut)));
        const auto ramps = clipAt(cut, 1, 0);
        CHECK(ramps.fade_in == 480 && ramps.fade_out == 480);
        const auto rampRender = exportProject(cut, root / "stair-ramps.wav", 2);
        CHECK(level(rampRender, 12000, 0) == 0.0); // first frame of the fade-in
        CHECK(level(rampRender, 59999, 0) == 0.0); // last frame of the fade-out
        for (uint32_t frame = 12001; frame < 12479; ++frame)
            CHECK(level(rampRender, frame, 0) >= level(rampRender, frame - 1, 0));
        expectLevel(level(rampRender, 12479, 0), level(rampRender, 12480, 0)); // complete by fadeIn-1
        expectLevel(level(rampRender, 12480, 0), 3.0 / 16.0); // steady body
        CHECK(level(rampRender, 12240, 0) < 0.6 * level(rampRender, 12480, 0)); // half-way up
        CHECK(level(rampRender, 12240, 0) > level(rampRender, 12120, 0));
        expectLevel(level(rampRender, 59520, 0), 6.0 / 16.0); // the fade-out starts here
        CHECK(level(rampRender, 59900, 0) < level(rampRender, 59700, 0));
        // Back to a hard cut: the very first and very last samples of the region
        // now carry full material instead of the ramp's zeros.
        CHECK_OK(cut, daw_set_clip_fades(cut, 1, 0, 0, 0, rev(cut)));
        const auto hardCut = exportProject(cut, root / "stair-hard.wav", 2);
        CHECK(hardCut.frames() == 60000);
        expectLevel(level(hardCut, 12000, 0), 3.0 / 16.0); // region head, no fade
        expectLevel(level(hardCut, 59999, 0), 6.0 / 16.0); // region tail, no fade
        CHECK(clipAt(cut, 1, 0).fade_in == 0 && clipAt(cut, 1, 0).fade_out == 0);

        // B5. Looping: a region longer than its slice wraps the read.
        const auto loopBase = rev(cut);
        // A plain region may not read past the end of its slice.
        CHECK_REJ(cut, daw_edit_clip(cut, 1, 0, kProjectRate, 0, 3 * kProjectRate, loopBase));
        CHECK(rev(cut) == loopBase);
        CHECK(clipAt(cut, 1, 0).length == kProjectRate);
        // Looping is what licenses it: the read wraps instead of running out.
        CHECK_OK(cut, daw_set_clip_looped(cut, 1, 0, 1, loopBase));
        CHECK(clipAt(cut, 1, 0).looped == 1);
        CHECK_OK(cut, daw_edit_clip(cut, 1, 0, kProjectRate, 0, 3 * kProjectRate, rev(cut)));
        const auto longRegion = clipAt(cut, 1, 0);
        CHECK(longRegion.length == 3 * kProjectRate && longRegion.start == kProjectRate);
        const auto loopRender = exportProject(cut, root / "stair-looped.wav", 2);
        CHECK(loopRender.frames() == 4 * kProjectRate); // 48 000 + 3 x 48 000
        // The loop spans the whole slice - 2 seconds of source, offset 0 - so
        // the wrapped pass repeats samples one slice later, bit for bit.
        const uint32_t slice = 2 * kProjectRate;
        CHECK(loopRender.frames() == kProjectRate + 3 * kProjectRate);
        for (uint32_t frame = 50000; frame < 90000; ++frame)
            CHECK(level(loopRender, frame, 0) == level(loopRender, frame + slice, 0));
        expectLevel(level(loopRender, kProjectRate, 0), 1.0 / 16.0); // each pass restarts at the slice
        expectLevel(level(loopRender, 3 * kProjectRate, 0), 1.0 / 16.0); // and here the second pass starts
        expectLevel(level(loopRender, 3 * kProjectRate - 1, 0), 0.5); // the frame before it is the slice end
        // Un-looping a region that no longer fits its slice must not be allowed.
        CHECK_REJ(cut, daw_set_clip_looped(cut, 1, 0, 0, rev(cut)));
        CHECK(clipAt(cut, 1, 0).looped == 1);
        CHECK(rev(cut) == loopBase + 2); // only the two accepted commands spent one
        // Shorten it back inside the slice, then un-loop cleanly: a loop that
        // never wraps is inaudible by definition.
        CHECK_OK(cut, daw_edit_clip(cut, 1, 0, kProjectRate, 0, kProjectRate, rev(cut)));
        CHECK_OK(cut, daw_set_clip_looped(cut, 1, 0, 0, rev(cut)));
        const auto unlooped = exportProject(cut, root / "stair-unlooped.wav", 2);
        CHECK(unlooped.frames() == 2 * kProjectRate);
        expectRangeIdentical(loopRender, unlooped, 0, 2 * kProjectRate, "looping inside the slice changed the sound");

        // B6. Group editing: duplicate, nudge and delete each cost one revision.
        const auto groupBase = rev(cut);
        CHECK_OK(cut, daw_duplicate_clip(cut, 1, 0, groupBase));
        CHECK(rev(cut) == groupBase + 1);
        CHECK_OK(cut, daw_duplicate_clip(cut, 1, 0, groupBase + 1));
        CHECK(trackById(cut, 1).clip_count == 3);
        const auto triple = regions(cut, 1);
        CHECK(triple[0].start == kProjectRate && triple[1].start == 2 * kProjectRate);
        CHECK(triple[2].start == 3 * kProjectRate); // copies land after the material end
        for (const auto& region : triple) CHECK(region.length == kProjectRate && region.source_offset == 0);
        const uint32_t pair[2] = {1, 2};
        const auto nudgeBase = rev(cut);
        CHECK_OK(cut, daw_nudge_clips(cut, 1, pair, 2, 12000, nudgeBase));
        CHECK(rev(cut) == nudgeBase + 1); // the group moved as one command
        const auto nudged = regions(cut, 1);
        CHECK(nudged[1].start == 2 * kProjectRate + 12000 && nudged[2].start == 3 * kProjectRate + 12000);
        CHECK(nudged[0].start == kProjectRate); // the un-listed clip stayed put
        const uint32_t badIndex[2] = {0, 99};
        CHECK_REJ(cut, daw_nudge_clips(cut, 1, badIndex, 2, 12000, rev(cut))); // one bad index
        CHECK(rev(cut) == nudgeBase + 1);
        const int64_t offTimeline = -static_cast<int64_t>(kProjectRate) * 100;
        CHECK_REJ(cut, daw_nudge_clips(cut, 1, pair, 2, offTimeline, rev(cut))); // negative space
        CHECK(rev(cut) == nudgeBase + 1);
        const auto stillThere = regions(cut, 1);
        CHECK(stillThere.size() == nudged.size());
        for (size_t index = 0; index < stillThere.size(); ++index) CHECK(stillThere[index].start == nudged[index].start);
        // Deleting the whole group costs one revision, and the last clip of a
        // track may never go away: its audio would be orphaned (docs/76).
        const uint32_t doomed[2] = {1, 2};
        const uint32_t allThree[3] = {0, 1, 2};
        const auto deleteBase = rev(cut);
        CHECK_REJ(cut, daw_delete_clips(cut, 1, allThree, 3, deleteBase)); // would empty the track
        CHECK(rev(cut) == deleteBase);
        CHECK(trackById(cut, 1).clip_count == 3);
        CHECK_OK(cut, daw_delete_clips(cut, 1, doomed, 2, deleteBase));
        CHECK(rev(cut) == deleteBase + 1);
        const auto survivor = regions(cut, 1);
        CHECK(survivor.size() == 1 && survivor[0].start == kProjectRate);
        CHECK_REJ(cut, daw_delete_clip(cut, 1, 0, rev(cut))); // the last clip stays
        CHECK(trackById(cut, 1).clip_count == 1);
        CHECK_REJ(cut, daw_delete_clips(cut, 1, badIndex, 2, rev(cut))); // atomic rejection
        CHECK(trackById(cut, 1).clip_count == 1 && rev(cut) == deleteBase + 1);

        // B7. Take lanes and comping over two takes.
        CHECK_OK(cut, daw_import_take_wav(cut, 1, shortPath.string().c_str(), "Take B", kProjectRate, rev(cut)));
        CHECK(trackById(cut, 1).take_count == 2);
        auto takeB = abi<daw_take>();
        CHECK_OK(cut, daw_get_take(cut, 1, 1, &takeB));
        CHECK(takeB.start == kProjectRate && takeB.frames == kProjectRate && std::string(takeB.name) == "Take B");
        CHECK(trackById(cut, 1).clip_count == 1); // an import take never mints a region by itself
        const auto compBase = rev(cut);
        CHECK_OK(cut, daw_comp_range(cut, 1, 1, kProjectRate + 6000, kProjectRate + 18000, compBase));
        CHECK(rev(cut) == compBase + 1);
        const auto comped = regions(cut, 1);
        CHECK(comped.size() == 3);
        CHECK(comped[0].start == kProjectRate && comped[0].length == 6000 && comped[0].take_index == 0);
        CHECK(comped[1].start == kProjectRate + 6000 && comped[1].length == 12000);
        CHECK(comped[1].take_index == 1); // the comp references the second take
        CHECK(comped[1].source_offset == 6000); // ... its own slice inside that take
        CHECK(comped[2].start == kProjectRate + 18000 && comped[2].take_index == 0);
        CHECK(comped[2].source_offset == 18000); // the base take's offsets still line up
        CHECK(comped[0].start + comped[0].length == comped[1].start); // left / comp / right tile
        CHECK(comped[1].start + comped[1].length == comped[2].start); // ... the original window
        CHECK(comped[2].start + comped[2].length == 2 * kProjectRate);
        const auto compRender = exportProject(cut, root / "stair-comp.wav", 2);
        // Inside the comp we hear take B (1/8 per block), outside take A
        // (1/16 per block): the same timeline frame tells the two apart.
        expectLevel(level(compRender, kProjectRate + 3000, 0), 1.0 / 16.0); // before the comp
        expectLevel(level(compRender, kProjectRate + 9000, 0), 1.0 / 8.0); // inside the comp
        expectLevel(level(compRender, kProjectRate + 15000, 0), 2.0 / 8.0); // second block of B
        expectLevel(level(compRender, kProjectRate + 24000, 0), 3.0 / 16.0); // after the comp
        // The losing take is still data: comping selects, it never deletes.
        CHECK(trackById(cut, 1).take_count == 2);
        auto takeA = abi<daw_take>();
        CHECK_OK(cut, daw_get_take(cut, 1, 0, &takeA));
        CHECK(takeA.frames == 2 * kProjectRate && std::string(takeA.name) == "Stair");
        CHECK_REJ(cut, daw_comp_range(cut, 1, 9, kProjectRate + 6000, kProjectRate + 7000, rev(cut))); // no such take
        CHECK_REJ(cut, daw_comp_range(cut, 1, 1, kProjectRate + 7000, kProjectRate + 7000, rev(cut))); // empty
        CHECK_REJ(cut, daw_comp_range(cut, 1, 1, 0, 1000, rev(cut))); // outside the take
        CHECK(trackById(cut, 1).clip_count == 3);
        CHECK(rev(cut) == compBase + 1);

        // B8. Cross-track clipboard: only clips sharing a take may travel.
        uint64_t copiedTrack = 0;
        const auto duplicateTrackBase = rev(cut);
        CHECK_OK(cut, daw_duplicate_track(cut, 1, &copiedTrack, duplicateTrackBase));
        CHECK(rev(cut) == duplicateTrackBase + 1); // the whole subtree, one revision
        CHECK(copiedTrack != 0 && copiedTrack != 1); // ... under a fresh id
        const auto sourceRegions = regions(cut, 1);
        const auto copiedRegions = regions(cut, copiedTrack);
        CHECK(copiedRegions.size() == sourceRegions.size());
        for (size_t index = 0; index < copiedRegions.size(); ++index) {
            CHECK(copiedRegions[index].start == sourceRegions[index].start);
            CHECK(copiedRegions[index].source_offset == sourceRegions[index].source_offset);
            CHECK(copiedRegions[index].length == sourceRegions[index].length);
            CHECK(copiedRegions[index].take_index == sourceRegions[index].take_index);
        }
        CHECK(trackById(cut, copiedTrack).take_count == 2); // and the take lane travelled too
        CHECK(trackById(cut, copiedTrack).audio_frames == trackById(cut, 1).audio_frames);
        const auto copyBase = rev(cut);
        CHECK_OK(cut, daw_copy_clip_to_track(cut, 1, 0, copiedTrack, materialEnd(cut, copiedTrack), copyBase));
        CHECK(rev(cut) == copyBase + 1);
        CHECK(trackById(cut, copiedTrack).clip_count == 4);
        CHECK(trackById(cut, 1).clip_count == 3); // a copy leaves the original alone
        const auto landed = clipAt(cut, copiedTrack, 3);
        CHECK(landed.start == 2 * kProjectRate && landed.take_index == 0);
        CHECK(landed.source_offset == 0 && landed.length == 6000); // same slice, new place
        CHECK_OK(cut, daw_import_wav(cut, shortPath.string().c_str(), "Foreign", rev(cut)));
        const auto foreign = trackNamed(cut, "Foreign");
        CHECK_REJ_KEEPS_REV(cut, daw_copy_clip_to_track(cut, 1, 0, foreign, 3 * kProjectRate, rev(cut))); // different take
        CHECK(trackById(cut, foreign).clip_count == 1);
        CHECK_REJ(cut, daw_copy_clip_to_track(cut, 1, 0, 9999, 0, rev(cut))); // no such track
        CHECK(rev(cut) == copyBase + 2); // import + nothing else
        const auto moveBase = rev(cut);
        CHECK_OK(cut, daw_move_clip_to_track(cut, 1, 2, copiedTrack, 5 * kProjectRate, moveBase));
        CHECK(rev(cut) == moveBase + 1);
        CHECK(trackById(cut, 1).clip_count == 2); // a move erases the source region
        CHECK(trackById(cut, copiedTrack).clip_count == 5);
        const auto arrived = clipAt(cut, copiedTrack, 4);
        CHECK(arrived.start == 5 * kProjectRate && arrived.source_offset == 18000 && arrived.length == 30000);
        CHECK(arrived.take_index == 0); // it kept its take reference
        CHECK_REJ(cut, daw_move_clip_to_track(cut, foreign, 0, copiedTrack, 6 * kProjectRate, rev(cut)));
        CHECK(trackById(cut, foreign).clip_count == 1); // the lone region stays on its track
        CHECK(rev(cut) == moveBase + 1);

        // B9. Track order, and undo of a whole-track removal.
        const auto ordered = dumpOf(cut);
        CHECK(ordered.tracks.size() == 3);
        CHECK(ordered.tracks[0].id == 1 && ordered.tracks[1].id == copiedTrack);
        CHECK(ordered.tracks[2].name == "Foreign");
        const auto moveTrackBase = rev(cut);
        CHECK_OK(cut, daw_move_track(cut, copiedTrack, 0, moveTrackBase));
        CHECK(rev(cut) == moveTrackBase + 1);
        auto firstSlot = abi<daw_track>();
        CHECK_OK(cut, daw_get_track(cut, 0, &firstSlot));
        CHECK(firstSlot.id == copiedTrack); // ids are stable, order is not
        auto secondSlot = abi<daw_track>();
        CHECK_OK(cut, daw_get_track(cut, 1, &secondSlot));
        CHECK(secondSlot.id == 1);
        CHECK(trackById(cut, copiedTrack).clip_count == 5); // the subtree came with it
        const auto samePlace = rev(cut);
        CHECK_OK(cut, daw_move_track(cut, copiedTrack, 0, samePlace)); // documented successful no-op
        CHECK(rev(cut) == samePlace);
        CHECK_REJ(cut, daw_move_track(cut, copiedTrack, 99, rev(cut))); // out of range
        CHECK(rev(cut) == samePlace);
        const auto beforeRemoval = dumpOf(cut);
        const auto removeBase = rev(cut);
        CHECK_OK(cut, daw_remove_track(cut, copiedTrack, removeBase));
        CHECK(rev(cut) == removeBase + 1);
        CHECK(snapshotOf(cut).track_count == 2);
        CHECK(dumpOf(cut).tracks.size() == 2);
        CHECK_OK(cut, daw_undo(cut, rev(cut)));
        const auto afterUndo = dumpOf(cut);
        CHECK(snapshotOf(cut).track_count == 3);
        auto restored = afterUndo;
        auto expected = beforeRemoval;
        restored.revision = expected.revision; // undo mints a new revision by design
        if (!(restored == expected))
            throw std::runtime_error("undo did not restore the removed track:\nBEFORE:\n" + describe(expected) +
                                     "\nAFTER:\n" + describe(restored));
        CHECK(afterUndo.tracks[0].id == copiedTrack && afterUndo.tracks[0].clips.size() == 5);
        CHECK_OK(cut, daw_redo(cut, rev(cut))); // and redo takes it away again
        CHECK(snapshotOf(cut).track_count == 2);
        CHECK_OK(cut, daw_undo(cut, rev(cut)));
        CHECK(snapshotOf(cut).track_count == 3);

        // B10. The stale-revision conflict path for every edit family: each one
        // rejects with a readable error and burns nothing.
        {
            const uint64_t now = rev(cut);
            const uint64_t stale = now - 1;
            const uint32_t one[1] = {0};
            uint64_t freshId = 0;
            CHECK_REJ_KEEPS_REV(cut, daw_edit_clip(cut, 1, 0, kProjectRate, 0, kProjectRate, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_edit_clip_full(cut, 1, 0, kProjectRate, 0, kProjectRate, 0, 0, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_split_clip(cut, 1, 0, kProjectRate + 100, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_duplicate_clip(cut, 1, 0, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_delete_clip(cut, 1, 0, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_delete_clips(cut, 1, one, 1, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_nudge_clips(cut, 1, one, 1, 100, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_set_clip_fades(cut, 1, 0, 0, 0, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_set_crossfade(cut, 1, 0, 2, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_set_clip_gain(cut, 1, 0, 0.0, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_set_clip_pan(cut, 1, 0, 0.0, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_set_clip_muted(cut, 1, 0, 0, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_set_clip_looped(cut, 1, 0, 0, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_import_take_wav(cut, 1, stairPath.string().c_str(), "Stale", kProjectRate, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_comp_range(cut, 1, 1, kProjectRate + 6000, kProjectRate + 18000, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_copy_clip_to_track(cut, 1, 0, copiedTrack, 6 * kProjectRate, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_move_clip_to_track(cut, 1, 0, copiedTrack, 6 * kProjectRate, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_duplicate_track(cut, 1, &freshId, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_move_track(cut, 1, 1, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_remove_track(cut, copiedTrack, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_undo(cut, stale));
            CHECK_REJ_KEEPS_REV(cut, daw_redo(cut, stale));
            CHECK(rev(cut) == now); // the whole sweep cost nothing
            CHECK(snapshotOf(cut).track_count == 3); // ... and the model never blinked
            CHECK(trackById(cut, 1).clip_count == 2);
            CHECK(trackById(cut, copiedTrack).clip_count == 5);
        }

        // B11. All of that cutting is durable: save, reopen, compare, re-render.
        const auto built = dumpOf(cut);
        CHECK(built.tracks.size() == 3);
        CHECK(built.tracks[0].id == copiedTrack && built.tracks[0].clips.size() == 5);
        CHECK(built.tracks[1].clips.size() == 2);
        CHECK(built.tracks[1].takes.size() == 2); // comp references stay resolvable
        CHECK(built.tracks[1].clips[1].takeIndex == 1); // the surviving comp slice
        const auto current = exportProject(cut, root / "cutting-current.wav", 2);
        // The bounce ends at the furthest region end: the 30 000-frame region
        // that moved onto the copy track sits at 5 x 48 000.
        CHECK(current.frames() == 5 * kProjectRate + 30000);

        const auto draft = root / "cutting.mydawdraft";
        saveDraftAndWait(cut, draft);
        CHECK(fileNonEmpty(draft));

        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), draft.string().c_str()));
        const auto reopenedProject = dumpOf(reopened.get());
        if (!(reopenedProject == built))
            throw std::runtime_error("clip editing did not survive the draft round trip:\nBEFORE:\n" + describe(built) +
                                     "\nAFTER:\n" + describe(reopenedProject));
        CHECK(reopenedProject.tracks[0].clips.size() == 5);
        CHECK(reopenedProject.tracks[1].clips[1].takeIndex == 1);
        CHECK(reopenedProject.tracks[1].takes[1].name == "Take B");
        // With the sources deleted the reopened project still renders the cut,
        // bit for bit: every region kept its own embedded copy of the PCM.
        std::filesystem::remove(stairPath);
        std::filesystem::remove(shortPath);
        CHECK(!std::filesystem::exists(stairPath));
        const auto reread = exportProject(reopened.get(), root / "cutting-reopened.wav", 2);
        expectIdentical(current, reread, "the reopened project renders a different cut");
        // Only the foreign take covers the first second, so its fourth block
        // (4/8) is what comes back - proof the reopened mix reads the same lanes.
        expectLevel(level(reread, 40000, 0), 4.0 / 8.0);
        expectLevel(level(reread, 40000, 1), -4.0 / 8.0);

        std::cout << "PASS: e2e_timeline_editing — split, edit, fade, gain, pan, mute, loop, group, comp and track cuts verified in model and render\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}