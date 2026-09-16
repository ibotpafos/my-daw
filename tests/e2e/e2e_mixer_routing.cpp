// e2e: MIX-01 / MIX-02 - the mixer strip and the routing graph, measured on
// the program the product actually prints.
//
// A user builds strips out of DC material whose every sample is a known
// number, then reads the exported WAV back with an independent reader. DC is
// chosen on purpose: peak, RMS and a single sample coincide, so gain, pan,
// mute, solo, master, bus and send law can be asserted as arithmetic instead
// of "louder/quieter". Every fixture is a 48 kHz float PCM WAV, so the whole
// scenario also runs on the Linux stub build.
//
// Tolerances. docs/30-mixer.md promises that the track L/R targets and the
// master target are smoothed "in about 5 ms", and docs/35 says track and bus
// gain/pan are smoothed too. The one-pole is
// "s += (target - s) * 0.004166667f" (renderer.cpp): a 240-sample time
// constant at 48 kHz. Every amplitude assertion here reads the SECOND HALF of
// the export, where the remaining ramp error is (1 - 1/240)^12000 = e^-50.
// What is left there is only float32: the one-pole stops once its increment
// falls under half an ULP of the running value, so the settled plateau sits
// about 240 half-ULPs below its target, a relative error under 6e-6. Values
// that never touch a smoother - the render of a zero-gain strip, and the
// master gain, which prepare() latches without ramping - are asserted
// bit-exactly. The first 512 samples are checked against the closed form of
// that same one-pole with tolerance 2e-5, which is the white-box audio_tests
// contract for the smoothing constant.
#include "e2e.hpp"

namespace {

using namespace e2e;

constexpr uint32_t kDcFrames = kProjectRate / 2;  // 0.5 s of DC per fixture

// Exactly -20*log10(2) dB: the halving an auditor means by "-6 dB", free of
// the 0.1187 % offset a rounded -6.0 carries.
constexpr double kHalfDb = -6.0205999132796239;
// 10^(-6/20): what a -6.0 dB fader actually multiplies by.
constexpr double kMinusSixLinear = 0.5011872336272722;
// Tolerance for a value that has passed through a fader/pan/gate smoother.
// In float32 the one-pole stops as soon as its increment falls below half an
// ULP of the running value, so the settled plateau sits up to
// halfUlp/0.004166667 ~= 240 * halfUlp below its target - a worst-case
// relative error of 6e-6, matching the 2e-5 the white-box audio_tests use for
// the same ramp. Anything bit-exact is asserted with checkPlateauExact().
constexpr double kSmoothedTol = 2e-5;

// Rejection plus revision stability: the pair every documented mutation
// contract promises (docs/35: stale-revision check, cycles and missing
// destinations "are rejected before the state is replaced").
#define CHECK_REJ_KEEPS_REV(session, call) do { \
    const uint64_t stable_ = e2e::rev(session); \
    CHECK_REJ(session, call); \
    if (e2e::rev(session) != stable_) \
        throw std::runtime_error(std::string("Rejected call still moved the revision: ") + #call); \
  } while (false)

double sampleAt(const Wav& wav, uint32_t frame, uint16_t channel) {
  return static_cast<double>(wav.samples[static_cast<size_t>(frame) * wav.channels + channel]);
}

// The second half of one channel must be one flat DC plateau at |expected|.
void checkPlateau(const Wav& wav, uint16_t channel, double expected, double tolerance, const std::string& what) {
  const uint32_t from = kDcFrames / 2;
  CHECK(wav.frames() > from);
  double lo = 1e300, hi = -1e300;
  for (uint32_t frame = from; frame < wav.frames(); ++frame) {
    const double value = std::abs(sampleAt(wav, frame, channel));
    lo = std::min(lo, value);
    hi = std::max(hi, value);
  }
  // A settled one-pole is a float fixed point: the plateau must be literally
  // flat, otherwise the export is not measuring a static mix.
  if (!(hi - lo <= 1e-9)) throw std::runtime_error("tail is not flat DC: " + what);
  e2e::expectNear(lo, expected, tolerance, "DC plateau " + what);
}

// Bit-exact variant, for paths with no smoother in them.
void checkPlateauExact(const Wav& wav, uint16_t channel, double expected, const std::string& what) {
  checkPlateau(wav, channel, expected, 0.0, what);
}

// Write a DC fixture and import it as a fresh strip; returns the new track id.
uint64_t importDc(daw_session* session, const TempRoot& root, const std::string& leaf, uint16_t channels,
                  float left, float right, const std::string& name) {
  const auto path = root / leaf;
  writeWavFixture(path, constantInterleaved(kDcFrames, channels, left, right), kProjectRate, channels, "f32");
  CHECK_OK(session, daw_import_wav(session, path.string().c_str(), name.c_str(), rev(session)));
  const auto snap = snapshotOf(session);
  auto track = abi<daw_track>();
  CHECK_OK(session, daw_get_track(session, snap.track_count - 1, &track));
  CHECK(track.clip_count == 1 && track.audio_frames == kDcFrames);
  return track.id;
}

// The exported program is exactly as long as the project, and stereo.
void expectProjectLength(const Wav& wav) { CHECK(wav.frames() == kDcFrames && wav.channels == 2); }

// Revision-free model snapshots, for undo/redo comparisons.
Dump stableDump(daw_session* session) {
  Dump dump = dumpOf(session);
  dump.revision = 0;
  return dump;
}

}  // namespace

int main() {
  try {
    using namespace e2e;
    TempRoot root("mixer_routing");

    // =========================================================================
    // 1. Track fader: -6 dB halves, bounds reject, and nothing moves silently.
    // =========================================================================
    Bridge gain;
    const auto strip = importDc(gain.get(), root, "gain-l.wav", 2, 0.4f, 0.2f, "Fader");
    CHECK(trackById(gain.get(), strip).gain_db == 0.0);

    // 1.1 Unity fader, center pan: the strip prints its input unchanged.
    const auto unity = exportProject(gain.get(), root / "gain-unity.wav", 2);
    expectProjectLength(unity);
    checkPlateau(unity, 0, 0.4, kSmoothedTol, "unity left");
    checkPlateau(unity, 1, 0.2, kSmoothedTol, "unity right");

    // 1.2 The documented ramp, against the closed form of the one-pole with
    // the smoothing constant 0.004166667 (1/240 per sample, ~5 ms per docs/30).
    for (uint32_t frame = 0; frame < 512; ++frame) {
      const double smoothed = 1.0 - std::pow(1.0 - 0.004166667, static_cast<double>(frame) + 1.0);
      expectNear(sampleAt(unity, frame, 0), 0.4 * smoothed, 2e-5, "fader ramp left at frame " + std::to_string(frame));
      expectNear(sampleAt(unity, frame, 1), 0.2 * smoothed, 2e-5, "fader ramp right at frame " + std::to_string(frame));
    }
    // The program starts from silence and rides the pole up: sample 0 is one
    // step (1/240) of the plateau, not the plateau itself.
    CHECK(sampleAt(unity, 0, 0) > 0.0 && sampleAt(unity, 0, 0) < 0.02 * 0.4);

    // 1.3 -6.0 dB multiplies by 10^(-6/20) = 0.5011872..., which is "half the
    // amplitude" to within the 0.12 % a rounded -6 dB always carries. The
    // tolerance below is the settled-smoother floor, not slack in the law.
    const uint64_t afterGain = rev(gain.get()) + 1;
    CHECK_OK(gain.get(), daw_set_gain(gain.get(), strip, -6.0, rev(gain.get())));
    CHECK(rev(gain.get()) == afterGain);
    const auto minusSix = exportProject(gain.get(), root / "gain-minus6.wav", 2);
    expectProjectLength(minusSix);
    checkPlateau(minusSix, 0, 0.4 * kMinusSixLinear, kSmoothedTol, "-6 dB left");
    checkPlateau(minusSix, 1, 0.2 * kMinusSixLinear, kSmoothedTol, "-6 dB right");
    CHECK(minusSix.samples.size() == unity.samples.size());
    expectNear(channelPeak(minusSix, kDcFrames / 2, kDcFrames, 0) / channelPeak(unity, kDcFrames / 2, kDcFrames, 0),
               kMinusSixLinear, 1e-5, "-6 dB follows the dB amplitude law (ratio)");

    // 1.4 The exact half: the dB value that halves amplitude, so the tail
    // ratio between the two renders is 0.5.
    CHECK_OK(gain.get(), daw_set_gain(gain.get(), strip, kHalfDb, rev(gain.get())));
    const auto halved = exportProject(gain.get(), root / "gain-half.wav", 2);
    expectProjectLength(halved);
    checkPlateau(halved, 0, 0.2, kSmoothedTol, "halved left");
    checkPlateau(halved, 1, 0.1, kSmoothedTol, "halved right");
    expectNear(channelPeak(halved, kDcFrames / 2, kDcFrames, 0) / channelPeak(unity, kDcFrames / 2, kDcFrames, 0),
               0.5, 1e-5, "tail amplitude ratio across one exact halving");

    // 1.5 Domain bounds. The engine's real fader range is -120...+24 dB
    // (docs/35-routing-buses-sends.md, enforced in the domain's validate();
    // the -60..12 message in Session::setTrackGain belongs to a domain-only
    // API that daw_set_gain does not route through). Both ends are inclusive.
    CHECK_OK(gain.get(), daw_set_gain(gain.get(), strip, 24.0, rev(gain.get())));
    CHECK(trackById(gain.get(), strip).gain_db == 24.0);
    CHECK_OK(gain.get(), daw_set_gain(gain.get(), strip, -120.0, rev(gain.get())));
    CHECK(trackById(gain.get(), strip).gain_db == -120.0);
    // gain() returns exactly 0.0f at or below -120 dB, so this strip is mute
    // by value: its smoother target is 0 from sample 0 and the export is pure
    // digital silence, bit-exact.
    const auto floor = exportProject(gain.get(), root / "gain-floor.wav", 2);
    expectProjectLength(floor);
    checkPlateauExact(floor, 0, 0.0, "silence floor left");
    checkPlateauExact(floor, 1, 0.0, "silence floor right");
    for (uint32_t frame = 0; frame < floor.frames(); ++frame) CHECK(sampleAt(floor, frame, 0) == 0.0);

    // 1.6 Out-of-domain and non-finite values reject and leave no trace: not a
    // revision, not a stored gain.
    const double illegal[] = {24.000001, 30.0, 100.0, -120.000001, -121.0, -999.0,
                              std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity()};
    for (const double value : illegal) {
      CHECK_REJ_KEEPS_REV(gain.get(), daw_set_gain(gain.get(), strip, value, rev(gain.get())));
    }
    CHECK(trackById(gain.get(), strip).gain_db == -120.0);
    CHECK_REJ_KEEPS_REV(gain.get(), daw_set_gain(gain.get(), 9999, 0.0, rev(gain.get())));

    // 1.7 An identical value is a silent no-op: rc 0 and no revision.
    const uint64_t beforeNoOp = rev(gain.get());
    CHECK_OK(gain.get(), daw_set_gain(gain.get(), strip, -120.0, beforeNoOp));
    CHECK(rev(gain.get()) == beforeNoOp);
    // 1.8 A stale expected_revision rejects the same way.
    CHECK_REJ_KEEPS_REV(gain.get(), daw_set_gain(gain.get(), strip, 0.0, beforeNoOp - 1));
    CHECK(trackById(gain.get(), strip).gain_db == -120.0);
    // 1.9 Gain is a track control: a bus id must not be accepted by it.
    const auto busProbe = addBus(gain.get(), "Gain guard");
    CHECK_REJ_KEEPS_REV(gain.get(), daw_set_gain(gain.get(), busProbe, -6.0, rev(gain.get())));
    CHECK(busById(gain.get(), busProbe).gain_db == 0.0);
    // 1.10 Master gain shares the same domain bounds.
    CHECK_OK(gain.get(), daw_set_master_gain(gain.get(), -6.0, rev(gain.get())));
    CHECK(snapshotOf(gain.get()).master_gain_db == -6.0);
    for (const double value : illegal) {
      CHECK_REJ_KEEPS_REV(gain.get(), daw_set_master_gain(gain.get(), value, rev(gain.get())));
    }
    const uint64_t afterMasterNoop = rev(gain.get());
    CHECK_OK(gain.get(), daw_set_master_gain(gain.get(), -6.0, afterMasterNoop));
    CHECK(rev(gain.get()) == afterMasterNoop);
    // 1.11 Pure ABI-shape gates: rc 1, no semantic error text promised.
    {
      auto tiny = abi<daw_snapshot>();
      tiny.struct_size = 4;
      CHECK(daw_get_snapshot(gain.get(), &tiny) == 1);
      CHECK(daw_get_snapshot(gain.get(), nullptr) == 1);
      CHECK(daw_get_track(gain.get(), 0, nullptr) == 1);
      auto badTrack = abi<daw_track>();
      CHECK(daw_get_track(gain.get(), 0, &badTrack) == 0);
      badTrack.struct_size = 4;
      CHECK(daw_get_track(gain.get(), 0, &badTrack) == 1);
      CHECK_REJ(gain.get(), daw_get_track(gain.get(), snapshotOf(gain.get()).track_count, &badTrack));
    }

    // =========================================================================
    // 2. Pan: linear stereo balance with unity center (docs/30-mixer.md). A
    //    mono import is duplicated into the stereo clip, so both channels
    //    start equal and the law shows up as "one side holds, the other
    //    fades" - never as a constant-power curve.
    // =========================================================================
    Bridge pan;
    const auto panned = importDc(pan.get(), root, "pan-mono.wav", 1, 0.5f, 0.0f, "Balance");
    CHECK(trackById(pan.get(), panned).pan == 0.0);
    CHECK(trackById(pan.get(), panned).audio_frames == kDcFrames);

    const struct {
      double value;
      double left;
      double right;
    } law[] = {
        {0.0, 0.5, 0.5},        // center equals the input on both channels
        {-1.0, 0.5, 0.0},       // hard left: all energy in L
        {1.0, 0.0, 0.5},        // hard right: all energy in R
        {0.5, 0.25, 0.5},       // balance keeps the far side at unity
        {-0.5, 0.5, 0.25},
        {0.25, 0.375, 0.5},
        {-0.75, 0.5, 0.125},
        {0.999999999, 0.0, 0.5},  // the bound is inclusive
    };
    for (const auto& step : law) {
      CHECK_OK(pan.get(), daw_set_pan(pan.get(), panned, step.value, rev(pan.get())));
      CHECK(trackById(pan.get(), panned).pan == step.value);
      const auto rendered = exportProject(pan.get(), root / ("pan-" + std::to_string(step.value) + ".wav"), 2);
      expectProjectLength(rendered);
      checkPlateau(rendered, 0, step.left, kSmoothedTol, "pan left " + std::to_string(step.value));
      checkPlateau(rendered, 1, step.right, kSmoothedTol, "pan right " + std::to_string(step.value));
    }
    // 2.1 Outside -1...+1 rejects, and so does a non-finite pan, with the
    // revision and the stored value untouched.
    const double badPans[] = {1.01, 2.0, -1.01, -4.0, std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity()};
    for (const double value : badPans) {
      CHECK_REJ_KEEPS_REV(pan.get(), daw_set_pan(pan.get(), panned, value, rev(pan.get())));
    }
    CHECK(trackById(pan.get(), panned).pan == 0.999999999);
    // 2.2 Pan is a track control too, and bus pan is its own command.
    const auto panBus = addBus(pan.get(), "Pan bus");
    CHECK_REJ_KEEPS_REV(pan.get(), daw_set_pan(pan.get(), panBus, 0.5, rev(pan.get())));
    CHECK_OK(pan.get(), daw_set_bus_pan(pan.get(), panBus, -1.0, rev(pan.get())));
    CHECK(busById(pan.get(), panBus).pan == -1.0);
    CHECK_REJ_KEEPS_REV(pan.get(), daw_set_bus_pan(pan.get(), panBus, 1.5, rev(pan.get())));
    CHECK_REJ_KEEPS_REV(pan.get(), daw_set_bus_pan(pan.get(), panned, 0.0, rev(pan.get())));
    CHECK_REJ_KEEPS_REV(pan.get(), daw_set_bus_pan(pan.get(), 9999, 0.0, rev(pan.get())));
    // 2.3 Bus gain is bounded the same way.
    CHECK_REJ_KEEPS_REV(pan.get(), daw_set_bus_gain(pan.get(), panBus, 30.0, rev(pan.get())));
    CHECK_REJ_KEEPS_REV(pan.get(), daw_set_bus_gain(pan.get(), panned, -6.0, rev(pan.get())));
    CHECK_OK(pan.get(), daw_set_bus_gain(pan.get(), panBus, 24.0, rev(pan.get())));
    CHECK(busById(pan.get(), panBus).gain_db == 24.0);

    // =========================================================================
    // 3. Mute, solo and the master fader on a two-strip mix.
    //    A = (0.4, 0.2), B = (0.1, 0.3): the full mix is 0.5 / 0.5 and every
    //    subset has a different, provable L/R pair.
    // =========================================================================
    Bridge mix;
    const auto lead = importDc(mix.get(), root, "mix-lead.wav", 2, 0.4f, 0.2f, "Lead");
    const auto doubled = importDc(mix.get(), root, "mix-double.wav", 2, 0.1f, 0.3f, "Double");
    CHECK(lead != doubled);

    const auto full = exportProject(mix.get(), root / "mix-full.wav", 2);
    expectProjectLength(full);
    checkPlateau(full, 0, 0.5, kSmoothedTol, "both strips left");
    checkPlateau(full, 1, 0.5, kSmoothedTol, "both strips right");

    // 3.1 Mute takes the strip out of the sum; the other one is untouched.
    CHECK_OK(mix.get(), daw_set_mute(mix.get(), lead, 1, rev(mix.get())));
    CHECK(trackById(mix.get(), lead).muted == 1);
    const auto muted = exportProject(mix.get(), root / "mix-muted.wav", 2);
    expectProjectLength(muted);
    checkPlateau(muted, 0, 0.1, kSmoothedTol, "muted lead leaves only the double");
    checkPlateau(muted, 1, 0.3, kSmoothedTol, "muted lead leaves only the double");
    // 3.2 Muting again is a silent no-op; a value other than 0/1 rejects.
    {
      const uint64_t stable = rev(mix.get());
      CHECK_OK(mix.get(), daw_set_mute(mix.get(), lead, 1, stable));
      CHECK(rev(mix.get()) == stable);
    }
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_mute(mix.get(), lead, 2, rev(mix.get())));
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_mute(mix.get(), 9999, 1, rev(mix.get())));
    CHECK_OK(mix.get(), daw_set_mute(mix.get(), lead, 0, rev(mix.get())));
    CHECK(trackById(mix.get(), lead).muted == 0);

    // 3.3 Solo: while any strip is soloed, only soloed-and-unmuted strips
    // sound (docs/30-mixer.md).
    CHECK_OK(mix.get(), daw_set_solo(mix.get(), lead, 1, rev(mix.get())));
    CHECK(trackById(mix.get(), lead).solo == 1);
    const auto soloed = exportProject(mix.get(), root / "mix-solo.wav", 2);
    expectProjectLength(soloed);
    checkPlateau(soloed, 0, 0.4, kSmoothedTol, "solo lead only");
    checkPlateau(soloed, 1, 0.2, kSmoothedTol, "solo lead only");
    // 3.4 Soloing both loud strips brings the full mix back.
    CHECK_OK(mix.get(), daw_set_solo(mix.get(), doubled, 1, rev(mix.get())));
    const auto bothSolo = exportProject(mix.get(), root / "mix-solo-both.wav", 2);
    expectProjectLength(bothSolo);
    checkPlateau(bothSolo, 0, 0.5, kSmoothedTol, "both strips soloed");
    checkPlateau(bothSolo, 1, 0.5, kSmoothedTol, "both strips soloed");
    // 3.5 Mute beats solo: a soloed strip that is also muted is silent, and
    // it does not unmute the rest of the room. Both channels are exactly 0.
    CHECK_OK(mix.get(), daw_set_solo(mix.get(), doubled, 0, rev(mix.get())));
    CHECK_OK(mix.get(), daw_set_mute(mix.get(), lead, 1, rev(mix.get())));
    const auto soloMuted = exportProject(mix.get(), root / "mix-solo-muted.wav", 2);
    expectProjectLength(soloMuted);
    checkPlateauExact(soloMuted, 0, 0.0, "soloed + muted is silence left");
    checkPlateauExact(soloMuted, 1, 0.0, "soloed + muted is silence right");
    for (uint32_t frame = 0; frame < soloMuted.frames(); ++frame)
      CHECK(sampleAt(soloMuted, frame, 0) == 0.0 && sampleAt(soloMuted, frame, 1) == 0.0);
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_solo(mix.get(), doubled, 3, rev(mix.get())));
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_solo(mix.get(), 9999, 1, rev(mix.get())));
    CHECK_OK(mix.get(), daw_set_mute(mix.get(), lead, 0, rev(mix.get())));
    CHECK_OK(mix.get(), daw_set_solo(mix.get(), lead, 0, rev(mix.get())));
    // 3.6 Solo is a track control: buses have gain/pan/mute only (docs/35
    // lists bus solo as not implemented).
    const auto mixBus = addBus(mix.get(), "Solo guard");
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_solo(mix.get(), mixBus, 1, rev(mix.get())));
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_mute(mix.get(), mixBus, 1, rev(mix.get())));
    CHECK_OK(mix.get(), daw_set_bus_mute(mix.get(), mixBus, 1, rev(mix.get())));
    CHECK(busById(mix.get(), mixBus).muted == 1);
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_bus_mute(mix.get(), mixBus, 2, rev(mix.get())));
    {
      const uint64_t stable = rev(mix.get());
      CHECK_OK(mix.get(), daw_set_bus_mute(mix.get(), mixBus, 1, stable));  // identical: no-op
      CHECK(rev(mix.get()) == stable);
    }
    CHECK_OK(mix.get(), daw_set_bus_mute(mix.get(), mixBus, 0, rev(mix.get())));

    // 3.7 Master gain is post everything: the same mix at unity master and at
    // one exact halving must print a tail ratio of exactly 0.5. prepare()
    // latches the master value with no ramp, so the ratio is clean.
    const auto masterUnity = exportProject(mix.get(), root / "mix-master-unity.wav", 2);
    expectProjectLength(masterUnity);
    checkPlateau(masterUnity, 0, 0.5, kSmoothedTol, "master unity left");
    checkPlateau(masterUnity, 1, 0.5, kSmoothedTol, "master unity right");
    CHECK_OK(mix.get(), daw_set_master_gain(mix.get(), kHalfDb, rev(mix.get())));
    const auto masterHalf = exportProject(mix.get(), root / "mix-master-half.wav", 2);
    expectProjectLength(masterHalf);
    checkPlateau(masterHalf, 0, 0.25, kSmoothedTol, "master halved left");
    checkPlateau(masterHalf, 1, 0.25, kSmoothedTol, "master halved right");
    expectNear(channelPeak(masterHalf, kDcFrames / 2, kDcFrames, 0) / channelPeak(masterUnity, kDcFrames / 2, kDcFrames, 0),
               0.5, 1e-5, "master tail ratio left");
    expectNear(channelPeak(masterHalf, kDcFrames / 2, kDcFrames, 1) / channelPeak(masterUnity, kDcFrames / 2, kDcFrames, 1),
               0.5, 1e-5, "master tail ratio right");
    // 3.8 Master is not a strip: no id names it in the per-track controls.
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_gain(mix.get(), 0, -6.0, rev(mix.get())));
    CHECK_REJ_KEEPS_REV(mix.get(), daw_set_mute(mix.get(), 0, 1, rev(mix.get())));
    CHECK_OK(mix.get(), daw_set_master_gain(mix.get(), 0.0, rev(mix.get())));

    // =========================================================================
    // 4. Buses: identity rules, audible routing, nesting, guards, deletion.
    // =========================================================================
    Bridge bus;
    const auto routed = importDc(bus.get(), root, "bus-a.wav", 2, 0.4f, 0.2f, "Routed");
    const auto unrouted = importDc(bus.get(), root, "bus-b.wav", 2, 0.1f, 0.3f, "Unrouted");

    // 4.1 Two creation doors: daw_add_bus mints no id, daw_create_bus reports
    // the fresh auto-increment id. Both take exactly one revision.
    const uint64_t revBeforeBus = rev(bus.get());
    CHECK_OK(bus.get(), daw_add_bus(bus.get(), "Plain bus", revBeforeBus));
    CHECK(rev(bus.get()) == revBeforeBus + 1);
    CHECK(snapshotOf(bus.get()).bus_count == 1);
    uint32_t counted = 0;
    CHECK_OK(bus.get(), daw_get_bus_count(bus.get(), &counted));
    CHECK(counted == 1);
    auto firstBus = abi<daw_bus>();
    CHECK_OK(bus.get(), daw_get_bus(bus.get(), 0, &firstBus));
    const auto plainId = firstBus.id;
    // A fresh bus is unity gain, centered, unmuted, and its output is Master,
    // which the ABI spells as id 0.
    CHECK(firstBus.gain_db == 0.0 && firstBus.pan == 0.0 && firstBus.muted == 0 && firstBus.output_bus_id == 0);
    const auto group = addBus(bus.get(), "Group bus");
    CHECK(group != plainId && group != 0);
    CHECK(busById(bus.get(), group).name == std::string("Group bus"));
    CHECK_OK(bus.get(), daw_get_bus_count(bus.get(), &counted));
    CHECK(counted == 2 && counted == snapshotOf(bus.get()).bus_count);
    // Empty and missing names are semantic rejections; a NULL out-param and a
    // bad struct size are ABI-shape rejections.
    {
      uint64_t scratch = 0;
      const uint64_t stable = rev(bus.get());
      CHECK_REJ_KEEPS_REV(bus.get(), daw_create_bus(bus.get(), "", &scratch, stable));
      CHECK_REJ_KEEPS_REV(bus.get(), daw_create_bus(bus.get(), nullptr, &scratch, stable));
      CHECK_REJ_KEEPS_REV(bus.get(), daw_add_bus(bus.get(), "", stable));
      CHECK_REJ_KEEPS_REV(bus.get(), daw_add_bus(bus.get(), nullptr, stable));
      CHECK(daw_get_bus_count(bus.get(), nullptr) == 1);
      auto tinyBus = abi<daw_bus>();
      tinyBus.struct_size = 4;
      CHECK(daw_get_bus(bus.get(), 0, &tinyBus) == 1);
      CHECK_REJ(bus.get(), daw_get_bus(bus.get(), 2, &tinyBus));
      // An out-id pointer is NOT an ABI guard on this call: the bridge only
      // reports the id when the caller supplies a pointer, so a NULL still
      // creates the bus and still costs one revision. docs/35 promises "NULL"
      // as a standard rejection for the NAME, which the second line above
      // covers; this asymmetry is asserted as it really behaves, and the
      // borrowed strip is handed straight back.
      tinyBus.struct_size = sizeof(daw_bus);
      const uint64_t beforeNullId = rev(bus.get());
      CHECK(daw_create_bus(bus.get(), "Nameless", nullptr, beforeNullId) == 0);
      CHECK(rev(bus.get()) == beforeNullId + 1);
      CHECK_OK(bus.get(), daw_get_bus(bus.get(), 2, &tinyBus));
      CHECK(std::string(tinyBus.name) == std::string("Nameless"));
      CHECK_OK(bus.get(), daw_delete_bus(bus.get(), tinyBus.id, rev(bus.get())));
      CHECK_OK(bus.get(), daw_get_bus_count(bus.get(), &counted));
      CHECK(counted == 2);
      scratch = 0;
      CHECK_OK(bus.get(), daw_create_bus(bus.get(), "Reported", &scratch, rev(bus.get())));
      CHECK(scratch != 0 && scratch != plainId && scratch != group);
      CHECK_OK(bus.get(), daw_delete_bus(bus.get(), scratch, rev(bus.get())));
      CHECK_OK(bus.get(), daw_get_bus_count(bus.get(), &counted));
      CHECK(counted == 2);
    }

    // 4.2 Track -> bus routing is audible: the bus fader moves the routed
    // strip and cannot be felt by the strip that still goes to Master.
    CHECK(trackById(bus.get(), routed).output_bus_id == 0);
    CHECK_OK(bus.get(), daw_set_track_output(bus.get(), routed, group, rev(bus.get())));
    CHECK(trackById(bus.get(), routed).output_bus_id == group);
    const auto throughUnityBus = exportProject(bus.get(), root / "bus-unity.wav", 2);
    expectProjectLength(throughUnityBus);
    checkPlateau(throughUnityBus, 0, 0.5, kSmoothedTol, "unity bus sums both strips");
    checkPlateau(throughUnityBus, 1, 0.5, kSmoothedTol, "unity bus sums both strips");
    CHECK_OK(bus.get(), daw_set_bus_gain(bus.get(), group, kHalfDb, rev(bus.get())));
    CHECK(busById(bus.get(), group).gain_db == kHalfDb);
    const auto halvedBus = exportProject(bus.get(), root / "bus-half.wav", 2);
    expectProjectLength(halvedBus);
    checkPlateau(halvedBus, 0, 0.4 * 0.5 + 0.1, kSmoothedTol, "routed strip halved, unrouted intact");
    checkPlateau(halvedBus, 1, 0.2 * 0.5 + 0.3, kSmoothedTol, "routed strip halved, unrouted intact");

    // 4.3 The bus has its own balance with the same law.
    CHECK_OK(bus.get(), daw_set_bus_pan(bus.get(), group, -1.0, rev(bus.get())));
    const auto busHardLeft = exportProject(bus.get(), root / "bus-pan-left.wav", 2);
    expectProjectLength(busHardLeft);
    checkPlateau(busHardLeft, 0, 0.4 * 0.5 + 0.1, kSmoothedTol, "bus pan keeps L");
    checkPlateau(busHardLeft, 1, 0.3, kSmoothedTol, "bus pan drops R entirely");
    CHECK_OK(bus.get(), daw_set_bus_pan(bus.get(), group, 0.0, rev(bus.get())));

    // 4.4 Muting the bus silences the strip routed into it, nothing else.
    CHECK_OK(bus.get(), daw_set_bus_mute(bus.get(), group, 1, rev(bus.get())));
    const auto busMuted = exportProject(bus.get(), root / "bus-muted.wav", 2);
    expectProjectLength(busMuted);
    checkPlateau(busMuted, 0, 0.1, kSmoothedTol, "bus muted leaves only the unrouted strip");
    checkPlateau(busMuted, 1, 0.3, kSmoothedTol, "bus muted leaves only the unrouted strip");
    CHECK_OK(bus.get(), daw_set_bus_mute(bus.get(), group, 0, rev(bus.get())));

    // 4.5 Bus -> bus nesting: a second bus now sits under the first, so the
    // routed strip is halved twice while the unrouted one is untouched.
    const auto outer = addBus(bus.get(), "Outer");
    CHECK_OK(bus.get(), daw_set_bus_output(bus.get(), group, outer, rev(bus.get())));
    CHECK(busById(bus.get(), group).output_bus_id == outer);
    CHECK(busById(bus.get(), outer).output_bus_id == 0);  // bus -> master by default
    CHECK_OK(bus.get(), daw_set_bus_gain(bus.get(), outer, kHalfDb, rev(bus.get())));
    const auto nested = exportProject(bus.get(), root / "bus-nested.wav", 2);
    expectProjectLength(nested);
    checkPlateau(nested, 0, 0.4 * 0.25 + 0.1, kSmoothedTol, "two-stage bus gain left");
    checkPlateau(nested, 1, 0.2 * 0.25 + 0.3, kSmoothedTol, "two-stage bus gain right");

    // 4.6 Routing guards, all atomic: self-route, unknown destination, a
    // track as a bus destination, and a real two-node cycle. None may move the
    // revision (docs/35: unknown target, self-route and any cycle are rejected
    // before the state is replaced).
    CHECK_REJ_KEEPS_REV(bus.get(), daw_set_bus_output(bus.get(), group, group, rev(bus.get())));
    CHECK_REJ_KEEPS_REV(bus.get(), daw_set_bus_output(bus.get(), group, 9999, rev(bus.get())));
    CHECK_REJ_KEEPS_REV(bus.get(), daw_set_bus_output(bus.get(), group, routed, rev(bus.get())));
    CHECK_REJ_KEEPS_REV(bus.get(), daw_set_bus_output(bus.get(), 9999, outer, rev(bus.get())));
    CHECK_REJ_KEEPS_REV(bus.get(), daw_set_bus_output(bus.get(), outer, group, rev(bus.get())));
    CHECK(busById(bus.get(), group).output_bus_id == outer);
    CHECK(busById(bus.get(), outer).output_bus_id == 0);
    CHECK(trackById(bus.get(), unrouted).output_bus_id == 0);
    // 4.7 A track output must name a real bus or Master (0), and re-setting
    // the value it already has is a silent no-op.
    CHECK_REJ_KEEPS_REV(bus.get(), daw_set_track_output(bus.get(), unrouted, 9999, rev(bus.get())));
    CHECK_REJ_KEEPS_REV(bus.get(), daw_set_track_output(bus.get(), 9999, group, rev(bus.get())));
    {
      const uint64_t stable = rev(bus.get());
      CHECK_OK(bus.get(), daw_set_track_output(bus.get(), unrouted, 0, stable));
      CHECK(rev(bus.get()) == stable);
    }

    // 4.8 delete_bus. Its documented consequences (docs/35: "Tracks routed to
    // this bus are redirected to master. Sends to this bus are deleted"; plus
    // the domain's deleteBus rerouting child buses and dropping its
    // automation with it) are asserted both structurally and audibly.
    CHECK_OK(bus.get(), daw_set_bus_output(bus.get(), group, 0, rev(bus.get())));  // group -> Master
    CHECK_OK(bus.get(), daw_set_bus_gain(bus.get(), outer, 0.0, rev(bus.get())));  // transparent
    CHECK_OK(bus.get(), daw_set_bus_output(bus.get(), outer, group, rev(bus.get())));  // outer -> group
    CHECK_OK(bus.get(), daw_upsert_send(bus.get(), unrouted, group, kHalfDb, 1, rev(bus.get())));
    // A 0 dB automation point overrides the strip's -6.02 dB static fader, so
    // the pre-delete sum below is the *automated* level, and the deleted bus
    // also carries its one lane point.
    CHECK_OK(bus.get(), daw_upsert_bus_gain_automation_point(bus.get(), group, 0, 0.0, rev(bus.get())));
    CHECK(busGainPoints(bus.get(), group).size() == 1);
    CHECK(trackById(bus.get(), unrouted).send_count == 1);
    CHECK(busById(bus.get(), outer).output_bus_id == group);
    const auto beforeDelete = stableDump(bus.get());
    const uint64_t revisionBeforeDelete = rev(bus.get());
    const auto preDeleteMix = exportProject(bus.get(), root / "bus-pre-delete.wav", 2);
    expectProjectLength(preDeleteMix);
    checkPlateau(preDeleteMix, 0, 0.4 + 0.1 + 0.05, kSmoothedTol, "routed + unrouted + pre-fader send");
    checkPlateau(preDeleteMix, 1, 0.2 + 0.3 + 0.15, kSmoothedTol, "routed + unrouted + pre-fader send");

    CHECK_OK(bus.get(), daw_delete_bus(bus.get(), group, revisionBeforeDelete));
    CHECK(rev(bus.get()) == revisionBeforeDelete + 1);
    CHECK_OK(bus.get(), daw_get_bus_count(bus.get(), &counted));
    CHECK(counted == 2);  // only plain and outer are left: group is gone
    CHECK(counted == snapshotOf(bus.get()).bus_count);
    {
      bool stillListed = false;
      for (uint32_t index = 0; index < counted; ++index) {
        auto probe = abi<daw_bus>();
        CHECK_OK(bus.get(), daw_get_bus(bus.get(), index, &probe));
        if (probe.id == group) stillListed = true;
      }
      CHECK(!stillListed);
      CHECK(trackById(bus.get(), routed).output_bus_id == 0);   // redirected to Master
      CHECK(trackById(bus.get(), unrouted).output_bus_id == 0);
      CHECK(busById(bus.get(), outer).output_bus_id == 0);      // child bus re-homed too
      auto host = trackById(bus.get(), unrouted);
      CHECK(host.send_count == 0);                              // its send died with the bus
      uint32_t orphan = 0;
      CHECK_REJ(bus.get(), daw_get_bus_gain_automation_count(bus.get(), group, &orphan));
      CHECK_REJ(bus.get(), daw_get_bus(bus.get(), 3, &firstBus));
      CHECK_REJ(bus.get(), daw_set_bus_gain(bus.get(), group, 0.0, rev(bus.get())));
    }
    // Audible aftermath: nothing routes through the deleted strip any more.
    const auto postDeleteMix = exportProject(bus.get(), root / "bus-post-delete.wav", 2);
    expectProjectLength(postDeleteMix);
    checkPlateau(postDeleteMix, 0, 0.5, kSmoothedTol, "after delete left");
    checkPlateau(postDeleteMix, 1, 0.5, kSmoothedTol, "after delete right");
    // Deleting it twice, deleting a phantom, and deleting Master (id 0, whose
    // button the mixer hides) all reject without moving the revision.
    CHECK_REJ_KEEPS_REV(bus.get(), daw_delete_bus(bus.get(), group, rev(bus.get())));
    CHECK_REJ_KEEPS_REV(bus.get(), daw_delete_bus(bus.get(), 9999, rev(bus.get())));
    CHECK_REJ_KEEPS_REV(bus.get(), daw_delete_bus(bus.get(), 0, rev(bus.get())));
    // 4.9 Undo restores the whole routing graph, send and automation lane
    // included, and the mix prints exactly what it printed before.
    CHECK_OK(bus.get(), daw_undo(bus.get(), rev(bus.get())));
    CHECK_OK(bus.get(), daw_get_bus_count(bus.get(), &counted));
    CHECK(counted == 3);  // Undo brought the strip back
    CHECK(busById(bus.get(), group).gain_db == kHalfDb);
    CHECK_OK(bus.get(), daw_get_bus(bus.get(), 1, &firstBus));
    CHECK(firstBus.id == group);
    {
      const auto undone = stableDump(bus.get());
      if (!(undone == beforeDelete))
        throw std::runtime_error("undo did not restore the routing graph: BEFORE " + describe(beforeDelete) +
                                 " AFTER " + describe(undone));
    }
    const auto undoMix = exportProject(bus.get(), root / "bus-undo.wav", 2);
    CHECK(undoMix.frames() == preDeleteMix.frames());
    for (size_t index = 0; index < preDeleteMix.samples.size(); ++index) {
      if (preDeleteMix.samples[index] != undoMix.samples[index])
        throw std::runtime_error("undo did not restore the render, differing at frame " +
                                 std::to_string(index / preDeleteMix.channels));
    }
    CHECK_OK(bus.get(), daw_redo(bus.get(), rev(bus.get())));
    CHECK_OK(bus.get(), daw_get_bus_count(bus.get(), &counted));
    CHECK(counted == 2);  // the redo really removed the strip again
    CHECK(trackById(bus.get(), routed).output_bus_id == 0);

    // =========================================================================
    // 5. Sends: the pre-fader tap versus the post-fader tap, the send gain
    //    law, and the upsert/remove rules.
    // =========================================================================
    Bridge sends;
    const auto dry = importDc(sends.get(), root, "send-dry.wav", 2, 0.4f, 0.2f, "Dry");
    const auto fx = addBus(sends.get(), "FX");
    const auto verb = addBus(sends.get(), "Verb");

    // 5.1 A unity post-fader send doubles the strip: main and send both carry
    // the fader's output on the way to the master sum.
    CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, fx, 0.0, 0, rev(sends.get())));
    {
      auto track = trackById(sends.get(), dry);
      CHECK(track.send_count == 1);
      auto send = abi<daw_send>();
      CHECK_OK(sends.get(), daw_get_send(sends.get(), dry, 0, &send));
      CHECK(send.bus_id == fx && send.gain_db == 0.0 && send.pre_fader == 0);
    }
    const auto unitySend = exportProject(sends.get(), root / "send-unity.wav", 2);
    expectProjectLength(unitySend);
    checkPlateau(unitySend, 0, 0.8, kSmoothedTol, "unity post-fader send left");
    checkPlateau(unitySend, 1, 0.4, kSmoothedTol, "unity post-fader send right");

    // 5.2 Send gain uses the same dB law as a fader, and upsert on the same
    // bus replaces in place instead of stacking a second route.
    CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, fx, kHalfDb, 0, rev(sends.get())));
    const auto halfSend = exportProject(sends.get(), root / "send-half-post.wav", 2);
    expectProjectLength(halfSend);
    checkPlateau(halfSend, 0, 0.4 + 0.2, kSmoothedTol, "post-fader send at half left");
    checkPlateau(halfSend, 1, 0.2 + 0.1, kSmoothedTol, "post-fader send at half right");
    {
      auto track = trackById(sends.get(), dry);
      CHECK(track.send_count == 1);
      auto send = abi<daw_send>();
      CHECK_OK(sends.get(), daw_get_send(sends.get(), dry, 0, &send));
      CHECK(send.gain_db == kHalfDb && send.bus_id == fx);
      const uint64_t stable = rev(sends.get());
      CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, fx, kHalfDb, 0, stable));  // identical: no-op
      CHECK(rev(sends.get()) == stable);
      // Flipping only the tap is a change, so it does cost a revision.
      CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, fx, kHalfDb, 1, stable));
      CHECK(rev(sends.get()) == stable + 1);
      auto flipped = abi<daw_send>();
      CHECK_OK(sends.get(), daw_get_send(sends.get(), dry, 0, &flipped));
      CHECK(flipped.pre_fader == 1);
      CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, fx, kHalfDb, 0, rev(sends.get())));
    }

    // 5.3 THE classic distinction. Drive the track fader down to its silence
    // floor: a post-fader send follows the fader to zero, while a pre-fader
    // send stays audible because its tap sits before gain and pan.
    CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, verb, kHalfDb, 1, rev(sends.get())));
    {
      auto post = abi<daw_send>();
      auto pre = abi<daw_send>();
      CHECK_OK(sends.get(), daw_get_send(sends.get(), dry, 0, &post));
      CHECK_OK(sends.get(), daw_get_send(sends.get(), dry, 1, &pre));
      CHECK(post.bus_id == fx && post.pre_fader == 0);
      CHECK(pre.bus_id == verb && pre.pre_fader == 1 && pre.gain_db == kHalfDb);
      CHECK(trackById(sends.get(), dry).send_count == 2);
    }
    CHECK_OK(sends.get(), daw_set_gain(sends.get(), dry, -120.0, rev(sends.get())));
    const auto atFloor = exportProject(sends.get(), root / "send-prefader-at-floor.wav", 2);
    expectProjectLength(atFloor);
    // main 0 + post-fader 0 + pre-fader 0.4*0.5, i.e. only the tap is left.
    checkPlateau(atFloor, 0, 0.2, kSmoothedTol, "pre-fader survives a silent fader");
    checkPlateau(atFloor, 1, 0.1, kSmoothedTol, "pre-fader survives a silent fader");
    // Switch the surviving send to post-fader: the program goes silent, and
    // silent to the bit - the gain() zero is exact, not a ramp.
    CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, verb, kHalfDb, 0, rev(sends.get())));
    const auto allPost = exportProject(sends.get(), root / "send-postfader-at-floor.wav", 2);
    expectProjectLength(allPost);
    checkPlateauExact(allPost, 0, 0.0, "post-fader follows the fader to silence");
    checkPlateauExact(allPost, 1, 0.0, "post-fader follows the fader to silence");
    for (uint32_t frame = 0; frame < allPost.frames(); ++frame)
      CHECK(sampleAt(allPost, frame, 0) == 0.0 && sampleAt(allPost, frame, 1) == 0.0);

    // 5.4 The pre-fader tap is not a mute bypass (docs/35: it "obeys the
    // common mute/solo gate"). Bring the fader back, then close the gate.
    CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, verb, kHalfDb, 1, rev(sends.get())));
    CHECK_OK(sends.get(), daw_set_gain(sends.get(), dry, 0.0, rev(sends.get())));
    const auto audibleAgain = exportProject(sends.get(), root / "send-prefader-unity.wav", 2);
    expectProjectLength(audibleAgain);
    checkPlateau(audibleAgain, 0, 0.4 + 0.2 + 0.2, kSmoothedTol, "main + post + pre left");
    checkPlateau(audibleAgain, 1, 0.2 + 0.1 + 0.1, kSmoothedTol, "main + post + pre right");
    CHECK_OK(sends.get(), daw_set_mute(sends.get(), dry, 1, rev(sends.get())));
    const auto gateClosed = exportProject(sends.get(), root / "send-muted.wav", 2);
    expectProjectLength(gateClosed);
    checkPlateauExact(gateClosed, 0, 0.0, "mute gates the pre-fader tap too");
    checkPlateauExact(gateClosed, 1, 0.0, "mute gates the pre-fader tap too");
    // Solo gating the same strip keeps the (soloed) send audible.
    CHECK_OK(sends.get(), daw_set_mute(sends.get(), dry, 0, rev(sends.get())));
    CHECK_OK(sends.get(), daw_set_solo(sends.get(), dry, 1, rev(sends.get())));
    const auto soloSend = exportProject(sends.get(), root / "send-solo.wav", 2);
    expectProjectLength(soloSend);
    checkPlateau(soloSend, 0, 0.8, kSmoothedTol, "soloed strip keeps its sends");
    checkPlateau(soloSend, 1, 0.4, kSmoothedTol, "soloed strip keeps its sends");
    CHECK_OK(sends.get(), daw_set_solo(sends.get(), dry, 0, rev(sends.get())));

    // 5.5 A pre-fader tap also ignores track pan: the tap is taken before both
    // gain and balance, while main and post-fader follow the pan.
    CHECK_OK(sends.get(), daw_set_pan(sends.get(), dry, 1.0, rev(sends.get())));
    const auto pannedSend = exportProject(sends.get(), root / "send-panned.wav", 2);
    expectProjectLength(pannedSend);
    checkPlateau(pannedSend, 0, 0.2, kSmoothedTol, "pre-fader tap ignores track pan");
    // Right: main 0.2 + post-fader 0.2*0.5 ride the pan, pre-fader adds its
    // own unpanned 0.2*0.5.
    checkPlateau(pannedSend, 1, 0.2 + 0.1 + 0.1, kSmoothedTol, "main + post follow the pan to the right");
    CHECK_OK(sends.get(), daw_set_pan(sends.get(), dry, 0.0, rev(sends.get())));

    // 5.6 Send guards: unknown bus, unknown track, bad tap flag, out-of-domain
    // gain, and removing what is not there - all atomic.
    CHECK_REJ_KEEPS_REV(sends.get(), daw_upsert_send(sends.get(), dry, 9999, 0.0, 0, rev(sends.get())));
    CHECK_REJ_KEEPS_REV(sends.get(), daw_upsert_send(sends.get(), 9999, fx, 0.0, 0, rev(sends.get())));
    CHECK_REJ_KEEPS_REV(sends.get(), daw_upsert_send(sends.get(), dry, fx, 0.0, 2, rev(sends.get())));
    const double badSend[] = {100.0, -121.0, std::numeric_limits<double>::quiet_NaN()};
    for (const double value : badSend) {
      CHECK_REJ_KEEPS_REV(sends.get(), daw_upsert_send(sends.get(), dry, fx, value, 0, rev(sends.get())));
    }
    CHECK(trackById(sends.get(), dry).send_count == 2);
    CHECK_REJ_KEEPS_REV(sends.get(), daw_remove_send(sends.get(), dry, 9999, rev(sends.get())));
    CHECK_OK(sends.get(), daw_remove_send(sends.get(), dry, verb, rev(sends.get())));
    CHECK(trackById(sends.get(), dry).send_count == 1);
    CHECK_REJ_KEEPS_REV(sends.get(), daw_remove_send(sends.get(), dry, verb, rev(sends.get())));
    {
      auto tinySend = abi<daw_send>();
      tinySend.struct_size = 4;
      CHECK(daw_get_send(sends.get(), dry, 0, &tinySend) == 1);
      CHECK(daw_get_send(sends.get(), dry, 0, nullptr) == 1);
      CHECK_REJ(sends.get(), daw_get_send(sends.get(), dry, 1, &tinySend));
      CHECK_REJ(sends.get(), daw_get_send(sends.get(), 9999, 0, &tinySend));
    }
    // 5.7 Eight sends per track is the documented ceiling (docs/35); the
    // ninth target is refused with the graph untouched, and removing one makes
    // room again.
    {
      CHECK_OK(sends.get(), daw_remove_send(sends.get(), dry, fx, rev(sends.get())));
      CHECK(trackById(sends.get(), dry).send_count == 0);
      std::vector<uint64_t> ids;
      for (uint32_t index = 0; index < 8; ++index) {
        const auto extra = addBus(sends.get(), "Send " + std::to_string(index));
        ids.push_back(extra);
        CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, extra, -1.0, 0, rev(sends.get())));
      }
      CHECK(trackById(sends.get(), dry).send_count == 8);
      const auto ninth = addBus(sends.get(), "Ninth");
      CHECK_REJ_KEEPS_REV(sends.get(), daw_upsert_send(sends.get(), dry, ninth, 0.0, 0, rev(sends.get())));
      CHECK(trackById(sends.get(), dry).send_count == 8);
      CHECK_OK(sends.get(), daw_remove_send(sends.get(), dry, ids.front(), rev(sends.get())));
      CHECK(trackById(sends.get(), dry).send_count == 7);
      CHECK_OK(sends.get(), daw_upsert_send(sends.get(), dry, ninth, 0.0, 1, rev(sends.get())));
      CHECK(trackById(sends.get(), dry).send_count == 8);
    }

    // =========================================================================
    // 6. One durable mix: names, colors, routing, sends, mute and solo survive
    //    a save + reopen in a brand-new session, and print the same bits.
    // =========================================================================
    Bridge durable;
    const auto vocal = importDc(durable.get(), root, "dur-vocal.wav", 2, 0.4f, 0.2f, "Old name");
    const auto spare = importDc(durable.get(), root, "dur-spare.wav", 2, 0.1f, 0.3f, "Spare");
    const auto drumBus = addBus(durable.get(), "Old bus");
    const auto returnBus = addBus(durable.get(), "Return");

    // 6.1 Names: rename is revision-checked, an identical rename is silent,
    // and empty/missing names reject without touching the model.
    CHECK_OK(durable.get(), daw_rename_track(durable.get(), vocal, "Lead", rev(durable.get())));
    CHECK(trackById(durable.get(), vocal).name == std::string("Lead"));
    {
      const uint64_t stable = rev(durable.get());
      CHECK_OK(durable.get(), daw_rename_track(durable.get(), vocal, "Lead", stable));
      CHECK(rev(durable.get()) == stable);
    }
    CHECK_REJ_KEEPS_REV(durable.get(), daw_rename_track(durable.get(), vocal, "", rev(durable.get())));
    CHECK_REJ_KEEPS_REV(durable.get(), daw_rename_track(durable.get(), 9999, "Ghost", rev(durable.get())));
    CHECK_OK(durable.get(), daw_rename_bus(durable.get(), drumBus, "Drum bus", rev(durable.get())));
    CHECK(busById(durable.get(), drumBus).name == std::string("Drum bus"));
    {
      const uint64_t stable = rev(durable.get());
      CHECK_OK(durable.get(), daw_rename_bus(durable.get(), drumBus, "Drum bus", stable));
      CHECK(rev(durable.get()) == stable);
    }
    CHECK_REJ_KEEPS_REV(durable.get(), daw_rename_bus(durable.get(), drumBus, "", rev(durable.get())));
    CHECK_REJ_KEEPS_REV(durable.get(), daw_rename_bus(durable.get(), vocal, "Not a bus", rev(durable.get())));

    // 6.2 Colors: 24-bit RGB on the track and on one clip region, 0 meaning
    // "no user color". Both are durable fields of the dump.
    CHECK(trackById(durable.get(), vocal).color == 0);
    CHECK_OK(durable.get(), daw_set_track_color(durable.get(), vocal, 0x11AA22, rev(durable.get())));
    CHECK(trackById(durable.get(), vocal).color == 0x11AA22);
    {
      const uint64_t stable = rev(durable.get());
      CHECK_OK(durable.get(), daw_set_track_color(durable.get(), vocal, 0x11AA22, stable));
      CHECK(rev(durable.get()) == stable);
    }
    auto region = abi<daw_clip>();
    CHECK_OK(durable.get(), daw_get_clip(durable.get(), vocal, 0, &region));
    CHECK(region.color == 0);
    CHECK_OK(durable.get(), daw_set_clip_color(durable.get(), vocal, 0, 0xBB00CC, rev(durable.get())));
    CHECK_OK(durable.get(), daw_get_clip(durable.get(), vocal, 0, &region));
    CHECK(region.color == 0xBB00CC);
    CHECK_REJ_KEEPS_REV(durable.get(), daw_set_clip_color(durable.get(), vocal, 7, 0x1, rev(durable.get())));
    CHECK_REJ_KEEPS_REV(durable.get(), daw_set_track_color(durable.get(), 9999, 0x1, rev(durable.get())));

    // 6.3 A full graph: track -> bus -> bus, two sends with different taps,
    // strip gain/pan, one soloed strip and a pulled master.
    CHECK_OK(durable.get(), daw_set_track_output(durable.get(), vocal, drumBus, rev(durable.get())));
    CHECK_OK(durable.get(), daw_set_bus_output(durable.get(), drumBus, returnBus, rev(durable.get())));
    CHECK_OK(durable.get(), daw_set_bus_gain(durable.get(), drumBus, -4.5, rev(durable.get())));
    CHECK_OK(durable.get(), daw_set_bus_pan(durable.get(), returnBus, 0.25, rev(durable.get())));
    CHECK_OK(durable.get(), daw_upsert_send(durable.get(), spare, drumBus, -9.0, 1, rev(durable.get())));
    CHECK_OK(durable.get(), daw_upsert_send(durable.get(), vocal, returnBus, -12.0, 0, rev(durable.get())));
    CHECK_OK(durable.get(), daw_set_gain(durable.get(), spare, -3.0, rev(durable.get())));
    CHECK_OK(durable.get(), daw_set_pan(durable.get(), spare, -0.5, rev(durable.get())));
    CHECK_OK(durable.get(), daw_set_solo(durable.get(), vocal, 1, rev(durable.get())));
    CHECK_OK(durable.get(), daw_set_master_gain(durable.get(), -1.5, rev(durable.get())));

    const auto built = dumpOf(durable.get());
    CHECK(built.tracks.size() == 2 && built.buses.size() == 2);
    CHECK(built.tracks[0].solo == 1 && built.tracks[0].color == 0x11AA22);
    CHECK(built.tracks[0].outputBusId == drumBus && built.tracks[0].sends.size() == 1);
    CHECK(built.tracks[0].sends[0].busId == returnBus && built.tracks[0].sends[0].gainDb == -12.0);
    CHECK(built.tracks[0].sends[0].preFader == 0);
    CHECK(built.tracks[1].pan == -0.5 && built.tracks[1].gainDb == -3.0 && built.tracks[1].solo == 0);
    CHECK(built.tracks[1].sends.size() == 1 && built.tracks[1].sends[0].preFader == 1);
    CHECK(built.buses[0].name == std::string("Drum bus") && built.buses[0].gainDb == -4.5);
    CHECK(built.buses[0].outputBusId == returnBus && built.buses[1].outputBusId == 0);
    CHECK(built.buses[1].pan == 0.25 && built.buses[1].muted == 0);
    CHECK(built.masterGain == -1.5);
    CHECK(built.tracks[0].clips.size() == 1 && built.tracks[0].clips[0].color == 0xBB00CC);

    // 6.4 Save the draft, reopen it in a fresh session: the whole model must
    // dump equal, and the render must come out bit-for-bit the same.
    const auto printedBefore = exportProject(durable.get(), root / "durable-before.wav", 2);
    expectProjectLength(printedBefore);
    CHECK(channelPeak(printedBefore, kDcFrames / 2, kDcFrames, 0) > 0.0);
    const auto draft = root / "mixer.mydawdraft";
    saveDraftAndWait(durable.get(), draft);
    CHECK(fileNonEmpty(draft));
    Bridge reopened;
    CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), draft.string().c_str()));
    const auto restored = dumpOf(reopened.get());
    if (!(restored == built))
      throw std::runtime_error("the mixer round-trip changed the project: BEFORE " + describe(built) + " AFTER " +
                               describe(restored));
    CHECK(restored.tracks[0].solo == 1);  // explicit: solo is a durable field
    CHECK(restored.tracks[0].muted == 0);
    const auto printedAfter = exportProject(reopened.get(), root / "durable-after.wav", 2);
    CHECK(printedAfter.frames() == printedBefore.frames());
    CHECK(printedAfter.channels == printedBefore.channels);
    CHECK(printedAfter.sampleRate == kProjectRate);
    for (size_t index = 0; index < printedBefore.samples.size(); ++index) {
      if (printedBefore.samples[index] != printedAfter.samples[index])
        throw std::runtime_error("the reopened mix differs at frame " +
                                 std::to_string(index / printedBefore.channels));
    }
    // 6.5 The reopened bus ids still own their routes, so they are live
    // targets for the same commands.
    CHECK(restored.tracks[0].outputBusId == restored.buses[0].id);
    CHECK_OK(reopened.get(), daw_set_bus_gain(reopened.get(), restored.buses[0].id, 0.0, rev(reopened.get())));
    CHECK(busById(reopened.get(), restored.buses[0].id).gain_db == 0.0);
    CHECK_OK(reopened.get(), daw_delete_bus(reopened.get(), restored.buses[1].id, rev(reopened.get())));
    CHECK_OK(reopened.get(), daw_get_bus_count(reopened.get(), &counted));
    CHECK(counted == 1);
    CHECK(trackById(reopened.get(), restored.tracks[1].id).send_count == 1);  // its send targeted bus 0

    std::cout << "PASS: e2e_mixer_routing - fader and pan law, bounds, mute/solo/master, bus routing, nesting, delete_bus, pre/post-fader sends, names, colors and the save/reopen round-trip" << '\n' << std::flush;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "E2E FAIL: " << error.what() << '\n' << std::flush;
    return 1;
  }
}
