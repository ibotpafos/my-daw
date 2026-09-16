// e2e: AUT-01 - automation lanes and automation write gestures, proven on
// the samples the product prints.
//
// Four lane kinds are reachable through the public C ABI (track volume, track
// pan, bus gain, master gain), each with a generic getter/upsert/remove trio
// plus the buffered Touch/Latch gesture API. The scenario walks them the way a
// mixer user does: place points, watch the enumeration, then export and measure
// where the level actually changed. Every lane is also held to the limits its
// documentation names - the value domain, the ten-minute timeline and the
// 2048-point ceiling, the last one as a lane command and again inside an open
// take - and the audible half runs on two kinds of material: DC, whose plateau
// reads out a gain exactly, and a 1 kHz tone, whose magnitude, RMS and peak show
// that the automation moved the loudness of real program and nothing else.
//
// Why the audible assertions can be exact. docs/38-volume-automation-pdc.md
// promises "automation sample-accurate inside the render loop, segment shape
// linear-in-dB", and the renderer honours it by REPLACING the smoothed control
// value with gain(automationValue(...)) per sample (renderer.cpp: the fader
// path only uses smoothTrackFader/smoothLeft/smoothRight when the lane is
// empty). So an automated fader has no 5 ms ramp at all: a one-frame step in
// the lane is a one-sample step in the audio, and the plateau either side of
// it is bit-constant. What DOES still ramp on the automated path is the
// mute/solo gate (smoothPreFaderGate / smoothBusGate), the one-pole
// "s += (target - s) * 0.004166667f" of docs/30-mixer.md - a 240-sample time
// constant. That is why every measurement here starts at frame 6000 (125 ms),
// where those poles have reached their float fixed point, and why absolute
// tolerances are 2e-5: in float32 that pole stops once its increment falls
// below half an ULP of the running value, i.e. about 240 half-ULPs (6e-6
// relative) below its target. Halvings are asserted bit-exactly, because
// multiplying by 0.5 is exact in binary floating point.
#include "e2e.hpp"

namespace {

using namespace e2e;

constexpr uint32_t kDcFrames = kProjectRate / 2;    // 0.5 s of DC per fixture
constexpr uint32_t kStepFrame = 12000;              // 250 ms: the step lands here
constexpr double kHalfDb = -6.0205999132796239;     // exactly -20*log10(2)
constexpr double kHalfLinear = 0.5011872336272722;  // 10^(-6/20)
constexpr double kTol = 2e-5;                       // settled-gate floor, see above
constexpr uint32_t kMeasureFrom = 6000;             // past every smoother ramp

// Rejection that also names WHICH guard fired: "the owner is missing" and
// "the project is locked" are different contracts.
#define CHECK_REJ_SAYS(session, call, needle) do { \
    const int rc_ = (call); \
    if (rc_ == 0) throw std::runtime_error(std::string("Accepted, expected rejection: ") + #call); \
    const std::string reason_ = e2e::bridgeError(session); \
    if (reason_.find(needle) == std::string::npos) \
        throw std::runtime_error(std::string("Wrong reason for ") + #call + ": got \"" + reason_ + "\" want \"" + \
                                 (needle) + "\""); \
  } while (false)

// Rejection plus revision stability: the pair every documented mutation
// contract promises (expected_revision, atomic commit).
#define CHECK_REJ_KEEPS_REV(session, call) do { \
    const uint64_t stable_ = e2e::rev(session); \
    CHECK_REJ(session, call); \
    if (e2e::rev(session) != stable_) \
        throw std::runtime_error(std::string("Rejected call still moved the revision: ") + #call); \
  } while (false)

double sampleAt(const Wav& wav, uint32_t frame, uint16_t channel) {
  return static_cast<double>(wav.samples[static_cast<size_t>(frame) * wav.channels + channel]);
}

// Write a DC fixture (a mono source is duplicated into the stereo clip by the
// importer) and import it as a fresh strip; returns the new track id.
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

// Write a 1 kHz sine fixture - both channels in phase - and import it as a fresh
// strip; returns the new track id. At 48 kHz one period is exactly 48 samples,
// so a window of 48 samples (or any multiple of it) holds a whole number of
// periods, which is what makes the harness' Goertzel magnitude read out the
// amplitude itself instead of an approximation of it.
uint64_t importTone(daw_session* session, const TempRoot& root, const std::string& leaf, uint32_t frames,
                    double amplitude, const std::string& name) {
  const auto path = root / leaf;
  writeWavFixture(path, sineInterleaved(frames, kProjectRate, 1000.0, amplitude, 2), kProjectRate, 2, "f32");
  CHECK_OK(session, daw_import_wav(session, path.string().c_str(), name.c_str(), rev(session)));
  const auto snap = snapshotOf(session);
  auto track = abi<daw_track>();
  CHECK_OK(session, daw_get_track(session, snap.track_count - 1, &track));
  CHECK(track.clip_count == 1 && track.audio_frames == frames);
  return track.id;
}

// A ratio of two measurements taken from the same render. Whatever the frozen
// control smoothers leave behind is the same factor in both halves, so it cancels
// here and the tolerance can be float32-tight - the sharp form of a dB claim.
void expectRatio(double actual, double expected, const std::string& what) {
  e2e::expectNear(actual, expected, 1e-6 * std::abs(expected) + 1e-12, "ratio " + what);
}

// The exported program is exactly as long as the project, and stereo.
void expectProjectLength(const Wav& wav) { CHECK(wav.frames() == kDcFrames && wav.channels == 2); }

// The [from, to) window of one channel must be one bit-constant plateau, and
// that plateau must sit at |expected|.
void checkWindow(const Wav& wav, uint16_t channel, uint32_t from, uint32_t to, double expected, double tolerance,
                 const std::string& what) {
  CHECK(to > from);
  CHECK(static_cast<uint32_t>(wav.frames()) >= to);
  double lo = 1e300, hi = -1e300;
  for (uint32_t frame = from; frame < to; ++frame) {
    const double value = std::abs(sampleAt(wav, frame, channel));
    lo = std::min(lo, value);
    hi = std::max(hi, value);
  }
  // An automated fader is not smoothed at all, so a plateau is literally one
  // repeated float value: any spread here means a ramp leaked into the lane.
  if (!(hi - lo <= (tolerance == 0.0 ? 0.0 : 1e-9)))
    throw std::runtime_error("plateau is not flat: " + what + " [lo=" + std::to_string(lo) + " hi=" +
                             std::to_string(hi) + " spread=" + std::to_string(hi - lo) + "]");
  e2e::expectNear(lo, expected, tolerance, "plateau " + what);
}

// Convenience: the plateau runs to the end of the program.
void checkPlateau(const Wav& wav, uint16_t channel, uint32_t from, double expected, double tolerance,
                  const std::string& what) {
  checkWindow(wav, channel, from, static_cast<uint32_t>(wav.frames()), expected, tolerance, what);
}

// Transition searches: the first frame at or after "from" whose magnitude on
// one channel crosses the given bound.
uint32_t firstBelow(const Wav& wav, uint16_t channel, double threshold, uint32_t from) {
  for (uint32_t frame = from; frame < static_cast<uint32_t>(wav.frames()); ++frame) {
    if (std::abs(sampleAt(wav, frame, channel)) < threshold) return frame;
  }
  return static_cast<uint32_t>(wav.frames());
}

uint32_t firstAbove(const Wav& wav, uint16_t channel, double threshold, uint32_t from) {
  for (uint32_t frame = from; frame < static_cast<uint32_t>(wav.frames()); ++frame) {
    if (std::abs(sampleAt(wav, frame, channel)) > threshold) return frame;
  }
  return static_cast<uint32_t>(wav.frames());
}

std::string pointText(const Points& points) {
  std::string text;
  for (const auto& point : points)
    text += "(" + std::to_string(point.first) + "f " + std::to_string(point.second) + "dB) ";
  return text;
}

void expectPoints(const Points& actual, const Points& expected, const std::string& what) {
  if (!(actual == expected))
    throw std::runtime_error("lane " + what + " differs: got " + pointText(actual) + " want " + pointText(expected));
}

// True when two renders share every bit.
bool bitIdentical(const Wav& a, const Wav& b) {
  return a.frames() == b.frames() && a.channels == b.channels && a.samples == b.samples;
}

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
    TempRoot root("automation");

    // =========================================================================
    // 1. Lane bookkeeping for all four lane kinds: frame-ordered enumeration,
    //    replace-in-place, removal, silent no-ops and value ranges.
    // =========================================================================
    Bridge lanes;
    const auto vocal = importDc(lanes.get(), root, "lane-vocal.wav", 1, 0.5f, 0.0f, "Vocal");
    const auto drum = importDc(lanes.get(), root, "lane-drum.wav", 2, 0.4f, 0.2f, "Drum");
    const auto stems = addBus(lanes.get(), "Stems");

    // 1.1 Every lane starts empty, which is also the "static behavior" state:
    // no getter claims a phantom point.
    CHECK(volumePoints(lanes.get(), vocal).empty());
    CHECK(panPoints(lanes.get(), vocal).empty());
    CHECK(busGainPoints(lanes.get(), stems).empty());
    CHECK(masterGainPoints(lanes.get()).empty());
    {
      uint32_t count = 7;
      CHECK_OK(lanes.get(), daw_get_track_volume_automation_count(lanes.get(), vocal, &count));
      CHECK(count == 0);
      CHECK_OK(lanes.get(), daw_get_bus_gain_automation_count(lanes.get(), stems, &count));
      CHECK(count == 0);
      CHECK_OK(lanes.get(), daw_get_master_gain_automation_count(lanes.get(), &count));
      CHECK(count == 0);
      auto point = abi<daw_automation_point>();
      CHECK(daw_get_track_volume_automation_point(lanes.get(), vocal, 0, &point) == 1);
      CHECK(daw_get_track_volume_automation_point(lanes.get(), vocal, 0, nullptr) == 1);
      CHECK(daw_get_track_volume_automation_count(lanes.get(), vocal, nullptr) == 1);
    }

    // 1.2 Track volume: the header promises frame-ordered enumeration, so the
    // insertion order must not matter and an upsert at an occupied frame
    // replaces instead of duplicating.
    CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 14400, -12.0, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 4800, 0.0, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 9600, -6.0, rev(lanes.get())));
    expectPoints(volumePoints(lanes.get(), vocal), Points{{4800, 0.0}, {9600, -6.0}, {14400, -12.0}},
                 "track volume frame order");
    {
      const uint64_t before = rev(lanes.get());
      CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 9600, -3.0, before));
      CHECK(rev(lanes.get()) == before + 1);  // a real replacement is a change
      expectPoints(volumePoints(lanes.get(), vocal), Points{{4800, 0.0}, {9600, -3.0}, {14400, -12.0}},
                   "same-frame upsert replaces");
      const uint64_t settled = rev(lanes.get());
      CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 9600, -3.0, settled));
      CHECK(rev(lanes.get()) == settled);  // identical value: silent no-op
      CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 7200, -1.5, settled));
      CHECK(rev(lanes.get()) == settled + 1);
      expectPoints(volumePoints(lanes.get(), vocal),
                   Points{{4800, 0.0}, {7200, -1.5}, {9600, -3.0}, {14400, -12.0}}, "mid-insert keeps order");
    }
    // 1.3 The indexed getter and the dump agree, point for point.
    {
      const std::pair<uint64_t, double> expected[] = {{4800, 0.0}, {7200, -1.5}, {9600, -3.0}, {14400, -12.0}};
      uint32_t count = 0;
      CHECK_OK(lanes.get(), daw_get_track_volume_automation_count(lanes.get(), vocal, &count));
      CHECK(count == 4);
      for (uint32_t index = 0; index < count; ++index) {
        auto point = abi<daw_automation_point>();
        CHECK_OK(lanes.get(), daw_get_track_volume_automation_point(lanes.get(), vocal, index, &point));
        CHECK(point.frame == expected[index].first && point.gain_db == expected[index].second);
      }
      auto past = abi<daw_automation_point>();
      CHECK_REJ(lanes.get(), daw_get_track_volume_automation_point(lanes.get(), vocal, count, &past));
      past.struct_size = 4;
      CHECK(daw_get_track_volume_automation_point(lanes.get(), vocal, 0, &past) == 1);
    }
    // 1.4 Removal: the exact frame only, and removing from an empty lane is a
    // rejection, not a no-op.
    CHECK_OK(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, 7200, rev(lanes.get())));
    expectPoints(volumePoints(lanes.get(), vocal), Points{{4800, 0.0}, {9600, -3.0}, {14400, -12.0}},
                 "remove drops exactly one frame");
    CHECK_REJ_KEEPS_REV(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, 7200, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, 4800, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, 9600, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, 14400, rev(lanes.get())));
    CHECK(volumePoints(lanes.get(), vocal).empty());
    CHECK_REJ_KEEPS_REV(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, 14400, rev(lanes.get())));
    // Lanes belong to one strip: writing vocal never touched drum or the bus.
    CHECK(volumePoints(lanes.get(), drum).empty());
    CHECK(panPoints(lanes.get(), drum).empty());
    CHECK(busGainPoints(lanes.get(), stems).empty());

    // 1.5 Value and frame ranges. Volume lanes share the fader domain
    // -120...+24 dB (docs/39), both ends inclusive; the timeline ceiling is
    // 48000*600 frames, inclusive.
    for (const double value : {-120.0, 0.0, 24.0}) {
      CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 1000, value, rev(lanes.get())));
      CHECK(volumePoints(lanes.get(), vocal).size() == 1);
      CHECK(volumePoints(lanes.get(), vocal)[0].second == value);
    }
    const double illegalVolume[] = {-120.000001, -999.0, 24.000001, 60.0, 100.0,
                                    std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};
    for (const double value : illegalVolume) {
      CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 2000, value,
                                                                                rev(lanes.get())));
    }
    expectPoints(volumePoints(lanes.get(), vocal), Points{{1000, 24.0}}, "range rejections left nothing behind");
    CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 48000ULL * 600, 0.0,
                                                                   rev(lanes.get())));
    CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 48000ULL * 600 + 1,
                                                                             0.0, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, 1000, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, 48000ULL * 600,
                                                                   rev(lanes.get())));
    CHECK(volumePoints(lanes.get(), vocal).empty());
    // 1.6 A lane belongs to an existing strip, and revisions are enforced.
    CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), 9999, 0, 0.0,
                                                                             rev(lanes.get())));
    CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), stems, 0, 0.0,
                                                                             rev(lanes.get())));
    {
      const uint64_t stale = rev(lanes.get()) - 1;
      CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, 0, 0.0, stale));
    }
    // 1.7 A lane point does not stretch the program: the export stays the
    // length of the audio.
    {
      CHECK_OK(lanes.get(), daw_upsert_track_volume_automation_point(lanes.get(), vocal, kStepFrame, 0.0,
                                                                     rev(lanes.get())));
      const auto wav = exportProject(lanes.get(), root / "lane-length.wav", 2);
      expectProjectLength(wav);
    }

    // 1.8 Track pan: same mechanics, and its domain is the pan domain -1...+1.
    CHECK_OK(lanes.get(), daw_upsert_track_pan_automation_point(lanes.get(), vocal, 9600, 0.5, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_upsert_track_pan_automation_point(lanes.get(), vocal, 0, -1.0, rev(lanes.get())));
    expectPoints(panPoints(lanes.get(), vocal), Points{{0, -1.0}, {9600, 0.5}}, "track pan frame order");
    {
      const uint64_t before = rev(lanes.get());
      CHECK_OK(lanes.get(), daw_upsert_track_pan_automation_point(lanes.get(), vocal, 9600, 0.5, before));
      CHECK(rev(lanes.get()) == before);  // identical value: silent no-op
      CHECK_OK(lanes.get(), daw_upsert_track_pan_automation_point(lanes.get(), vocal, 9600, 1.0, before));
      CHECK(rev(lanes.get()) == before + 1);
    }
    {
      uint32_t count = 0;
      CHECK_OK(lanes.get(), daw_get_track_pan_automation_count(lanes.get(), vocal, &count));
      CHECK(count == 2);
      auto point = abi<daw_automation_point>();
      CHECK_OK(lanes.get(), daw_get_track_pan_automation_point(lanes.get(), vocal, 1, &point));
      CHECK(point.frame == 9600 && point.gain_db == 1.0);  // the value field is the pan
      const double illegalPan[] = {-1.000001, 1.000001, -2.0, 2.0, std::numeric_limits<double>::quiet_NaN()};
      for (const double value : illegalPan) {
        CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_track_pan_automation_point(lanes.get(), vocal, 12000, value,
                                                                              rev(lanes.get())));
      }
      CHECK_OK(lanes.get(), daw_upsert_track_pan_automation_point(lanes.get(), vocal, 12000, -1.0, rev(lanes.get())));
      CHECK_OK(lanes.get(), daw_remove_track_pan_automation_point(lanes.get(), vocal, 12000, rev(lanes.get())));
      expectPoints(panPoints(lanes.get(), vocal), Points{{0, -1.0}, {9600, 1.0}}, "pan remove is exact");
      CHECK_REJ_KEEPS_REV(lanes.get(), daw_remove_track_pan_automation_point(lanes.get(), vocal, 12000,
                                                                            rev(lanes.get())));
      CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_track_pan_automation_point(lanes.get(), 9999, 0, 0.0,
                                                                            rev(lanes.get())));
    }
    // A pan lane must not leak into the volume lane and vice versa.
    CHECK(volumePoints(lanes.get(), vocal).size() == 1);
    CHECK(panPoints(lanes.get(), vocal).size() == 2);
    CHECK_OK(lanes.get(), daw_remove_track_volume_automation_point(lanes.get(), vocal, kStepFrame, rev(lanes.get())));
    CHECK(volumePoints(lanes.get(), vocal).empty());
    CHECK(panPoints(lanes.get(), vocal).size() == 2);
    CHECK_OK(lanes.get(), daw_remove_track_pan_automation_point(lanes.get(), vocal, 0, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_remove_track_pan_automation_point(lanes.get(), vocal, 9600, rev(lanes.get())));
    CHECK(panPoints(lanes.get(), vocal).empty());

    // 1.9 Bus gain lane: same rules, owned by the bus, not by a track.
    CHECK_OK(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), stems, 19200, -18.0, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), stems, 2400, -6.0, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), stems, 9600, -12.0, rev(lanes.get())));
    expectPoints(busGainPoints(lanes.get(), stems), Points{{2400, -6.0}, {9600, -12.0}, {19200, -18.0}},
                 "bus gain frame order");
    {
      uint32_t count = 0;
      CHECK_OK(lanes.get(), daw_get_bus_gain_automation_count(lanes.get(), stems, &count));
      CHECK(count == 3);
      const uint64_t before = rev(lanes.get());
      CHECK_OK(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), stems, 9600, -12.0, before));
      CHECK(rev(lanes.get()) == before);
      auto point = abi<daw_automation_point>();
      CHECK_OK(lanes.get(), daw_get_bus_gain_automation_point(lanes.get(), stems, 2, &point));
      CHECK(point.frame == 19200 && point.gain_db == -18.0);
      const double illegalBus[] = {-121.0, 25.0, std::numeric_limits<double>::quiet_NaN()};
      for (const double value : illegalBus) {
        CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), stems, 4800, value,
                                                                             rev(lanes.get())));
      }
      CHECK_OK(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), stems, 4800, 24.0, rev(lanes.get())));
      CHECK_OK(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), stems, 2400, -120.0, rev(lanes.get())));
      CHECK(busGainPoints(lanes.get(), stems).size() == 4);
      CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), vocal, 0, 0.0,
                                                                           rev(lanes.get())));
      CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), 9999, 0, 0.0,
                                                                           rev(lanes.get())));
      CHECK_REJ_KEEPS_REV(lanes.get(), daw_remove_bus_gain_automation_point(lanes.get(), stems, 999999,
                                                                           rev(lanes.get())));
    }

    // 1.10 Master gain lane: one per project, so no id is involved at all.
    CHECK_OK(lanes.get(), daw_upsert_master_gain_automation_point(lanes.get(), 21600, -1.0, rev(lanes.get())));
    CHECK_OK(lanes.get(), daw_upsert_master_gain_automation_point(lanes.get(), 14400, -2.0, rev(lanes.get())));
    expectPoints(masterGainPoints(lanes.get()), Points{{14400, -2.0}, {21600, -1.0}}, "master gain frame order");
    {
      uint32_t count = 0;
      CHECK_OK(lanes.get(), daw_get_master_gain_automation_count(lanes.get(), &count));
      CHECK(count == 2);
      const uint64_t before = rev(lanes.get());
      CHECK_OK(lanes.get(), daw_upsert_master_gain_automation_point(lanes.get(), 14400, -2.0, before));
      CHECK(rev(lanes.get()) == before);
      auto point = abi<daw_automation_point>();
      CHECK_OK(lanes.get(), daw_get_master_gain_automation_point(lanes.get(), 0, &point));
      CHECK(point.frame == 14400 && point.gain_db == -2.0);
      CHECK_REJ(lanes.get(), daw_get_master_gain_automation_point(lanes.get(), 2, &point));
      const double illegalMaster[] = {-121.0, 100.0, std::numeric_limits<double>::quiet_NaN()};
      for (const double value : illegalMaster) {
        CHECK_REJ_KEEPS_REV(lanes.get(), daw_upsert_master_gain_automation_point(lanes.get(), 24000, value,
                                                                                rev(lanes.get())));
      }
      CHECK_OK(lanes.get(), daw_upsert_master_gain_automation_point(lanes.get(), 24000, 0.0, rev(lanes.get())));
      CHECK(masterGainPoints(lanes.get()).size() == 3);
      CHECK_OK(lanes.get(), daw_remove_master_gain_automation_point(lanes.get(), 24000, rev(lanes.get())));
      CHECK_OK(lanes.get(), daw_remove_master_gain_automation_point(lanes.get(), 14400, rev(lanes.get())));
      CHECK_OK(lanes.get(), daw_remove_master_gain_automation_point(lanes.get(), 21600, rev(lanes.get())));
      CHECK(masterGainPoints(lanes.get()).empty());
      CHECK_REJ_KEEPS_REV(lanes.get(), daw_remove_master_gain_automation_point(lanes.get(), 21600, rev(lanes.get())));
    }

    // 1.11 Undo/Redo restore a whole lane point set, not a partial edit.
    {
      CHECK_OK(lanes.get(), daw_upsert_bus_gain_automation_point(lanes.get(), stems, 1000, -3.0, rev(lanes.get())));
      const auto withPoint = stableDump(lanes.get());
      CHECK_OK(lanes.get(), daw_undo(lanes.get(), rev(lanes.get())));
      {
        const auto undone = stableDump(lanes.get());
        if (undone == withPoint) throw std::runtime_error("undo of an automation upsert did not change the model");
        if (!(busGainPoints(lanes.get(), stems).size() == 4))
          throw std::runtime_error("undo dropped the wrong number of bus points");
      }
      CHECK_OK(lanes.get(), daw_redo(lanes.get(), rev(lanes.get())));
      {
        const auto redone = stableDump(lanes.get());
        if (!(redone == withPoint))
          throw std::runtime_error("redo did not restore the lanes: BEFORE " + describe(withPoint) + " AFTER " +
                                   describe(redone));
      }
      // And the removal command itself: one frame, one revision, order kept.
      CHECK_OK(lanes.get(), daw_remove_bus_gain_automation_point(lanes.get(), stems, 1000, rev(lanes.get())));
      expectPoints(busGainPoints(lanes.get(), stems), Points{{2400, -120.0}, {4800, 24.0}, {9600, -12.0},
                                                            {19200, -18.0}}, "bus gain remove drops one frame");
      uint32_t remaining = 0;
      CHECK_OK(lanes.get(), daw_get_bus_gain_automation_count(lanes.get(), stems, &remaining));
      CHECK(remaining == 4);
      CHECK_OK(lanes.get(), daw_remove_bus_gain_automation_point(lanes.get(), stems, 2400, rev(lanes.get())));
      CHECK_OK(lanes.get(), daw_remove_bus_gain_automation_point(lanes.get(), stems, 4800, rev(lanes.get())));
      CHECK_OK(lanes.get(), daw_remove_bus_gain_automation_point(lanes.get(), stems, 9600, rev(lanes.get())));
      CHECK_OK(lanes.get(), daw_remove_bus_gain_automation_point(lanes.get(), stems, 19200, rev(lanes.get())));
      CHECK(busGainPoints(lanes.get(), stems).empty());
    }

    // 1.12 The point ceiling. docs/38, docs/39 and docs/44 all state the same
    // number - at most 2048 strictly ordered points per lane, on the ten-minute
    // timeline - and docs/44 says the write gestures are held to exactly the
    // limits of the stored lanes. So the ceiling is proved on the lane commands
    // and then again through an open take.
    {
      Bridge ceiling;
      const auto dense = addTrack(ceiling.get(), "Dense lane");
      const uint64_t empty = rev(ceiling.get());
      for (uint32_t index = 0; index < 2048; ++index)
        CHECK_OK(ceiling.get(), daw_upsert_track_volume_automation_point(ceiling.get(), dense, index, -1.0,
                                                                        rev(ceiling.get())));
      CHECK(rev(ceiling.get()) == empty + 2048);  // exactly one revision per committed point
      uint32_t denseCount = 0;
      CHECK_OK(ceiling.get(), daw_get_track_volume_automation_count(ceiling.get(), dense, &denseCount));
      CHECK(denseCount == 2048);
      CHECK_REJ_SAYS(ceiling.get(),
                     daw_upsert_track_volume_automation_point(ceiling.get(), dense, 2048, -2.0, rev(ceiling.get())),
                     "supports at most 2048 automation points");
      CHECK_REJ_KEEPS_REV(ceiling.get(),
                          daw_upsert_track_volume_automation_point(ceiling.get(), dense, 2048, -2.0,
                                                                   rev(ceiling.get())));
      // Replacing a frame the lane already holds is not an append, so it stays
      // legal at the ceiling.
      CHECK_OK(ceiling.get(), daw_upsert_track_volume_automation_point(ceiling.get(), dense, 1000, -4.5,
                                                                      rev(ceiling.get())));
      CHECK(volumePoints(ceiling.get(), dense)[1000].second == -4.5);
      CHECK_OK(ceiling.get(), daw_get_track_volume_automation_count(ceiling.get(), dense, &denseCount));
      CHECK(denseCount == 2048);
      // Free a slot and it fits again - the ceiling is a count, not a frame range.
      CHECK_OK(ceiling.get(), daw_remove_track_volume_automation_point(ceiling.get(), dense, 0, rev(ceiling.get())));
      CHECK_OK(ceiling.get(), daw_upsert_track_volume_automation_point(ceiling.get(), dense, 2048, -2.0,
                                                                      rev(ceiling.get())));
      const Points refilled = volumePoints(ceiling.get(), dense);
      CHECK(refilled.size() == 2048 && refilled.front().first == 1 && refilled.back().first == 2048);
      // The same ceiling inside an open take. And a take's ordering cursor only
      // moves with an accepted sample: the write that the ceiling refused did
      // not become the newest frame of the buffer, so an earlier frame is still
      // welcome - and only after it is written does ordering bind again.
      const uint64_t take = rev(ceiling.get());
      CHECK_OK(ceiling.get(), daw_begin_automation_gesture(ceiling.get(), DAW_AUTOMATION_TRACK_VOLUME, dense,
                                                          DAW_AUTOMATION_TOUCH, take));
      CHECK_REJ_SAYS(ceiling.get(), daw_write_automation_gesture(ceiling.get(), 2049, -3.0),
                     "supports at most 2048 automation points");
      CHECK_OK(ceiling.get(), daw_write_automation_gesture(ceiling.get(), 2046, -3.0));
      CHECK_REJ_SAYS(ceiling.get(), daw_write_automation_gesture(ceiling.get(), 2045, -3.0),
                     "Automation gesture frames must be ordered");
      CHECK_OK(ceiling.get(), daw_write_automation_gesture(ceiling.get(), 2046, -3.5));  // same frame: replaced
      daw_cancel_automation_gesture(ceiling.get());
      CHECK(rev(ceiling.get()) == take);  // a cancelled take costs nothing
      CHECK(volumePoints(ceiling.get(), dense) == refilled);
      // A take whose samples are all already in the lane still commits: the
      // silent no-op belongs to the lane upsert command, not to a recorded take,
      // which only has to have written at least one sample (docs/44).
      const uint64_t replay = rev(ceiling.get());
      CHECK_OK(ceiling.get(), daw_begin_automation_gesture(ceiling.get(), DAW_AUTOMATION_TRACK_VOLUME, dense,
                                                          DAW_AUTOMATION_TOUCH, replay));
      CHECK_OK(ceiling.get(), daw_write_automation_gesture(ceiling.get(), 1000, -4.5));
      CHECK_OK(ceiling.get(), daw_end_automation_gesture(ceiling.get(), 1000, replay));
      CHECK(rev(ceiling.get()) == replay + 1);
      CHECK(volumePoints(ceiling.get(), dense) == refilled);
      CHECK_OK(ceiling.get(), daw_set_gain(ceiling.get(), dense, -3.0, rev(ceiling.get())));  // live again
    }

    // =========================================================================
    // 2. Audible sample accuracy: a volume lane step is a ONE-SAMPLE step.
    //    The source is mono DC 0.5 (imported duplicated to L=R=0.5), the
    //    fader is at unity, so the automated lane is the only gain in play.
    // =========================================================================
    Bridge vol;
    const auto solo = importDc(vol.get(), root, "vol-solo.wav", 1, 0.5f, 0.0f, "Solo strip");

    // 2.1 Baseline: no lane at all, so the static smoothed fader.
    const auto staticMix = exportProject(vol.get(), root / "vol-static.wav", 2);
    expectProjectLength(staticMix);
    checkPlateau(staticMix, 0, kMeasureFrom, 0.5, kTol, "static unity lane-less program");
    checkPlateau(staticMix, 1, kMeasureFrom, 0.5, kTol, "static unity lane-less program");

    // 2.2 The step. Two adjacent points - 0 dB at kStepFrame-1, one exact
    // halving at kStepFrame - because a lane cannot hold two points at one
    // frame. docs/38 promises sample-accurate automation, so the halved
    // amplitude must start at kStepFrame, not "about 5 ms later".
    CHECK_OK(vol.get(), daw_upsert_track_volume_automation_point(vol.get(), solo, kStepFrame - 1, 0.0,
                                                                 rev(vol.get())));
    CHECK_OK(vol.get(), daw_upsert_track_volume_automation_point(vol.get(), solo, kStepFrame, kHalfDb, rev(vol.get())));
    CHECK(volumePoints(vol.get(), solo).size() == 2);
    const auto stepped = exportProject(vol.get(), root / "vol-step.wav", 2);
    expectProjectLength(stepped);
    checkWindow(stepped, 0, kMeasureFrom, kStepFrame - 1, 0.5, kTol, "pre-step plateau");
    checkWindow(stepped, 0, kStepFrame, kDcFrames, 0.25, kTol, "post-step plateau");
    // The transition search: the last full-scale frame is kStepFrame-1 and the
    // first halved frame is kStepFrame - the documented frames, to the sample.
    CHECK(firstBelow(stepped, 0, 0.375, kMeasureFrom) == kStepFrame);
    CHECK(firstAbove(stepped, 0, 0.375, kStepFrame) == static_cast<uint32_t>(stepped.frames()));
    // Strongest true statement: the halving is bit-exact. 0.5 and 1.0 are
    // powers of two, so both frames are the same float gate value scaled by a
    // power of two - any residual mute/solo ramp cancels in the ratio. The
    // static fader smoother is simply not in this path.
    CHECK(sampleAt(stepped, kStepFrame - 1, 0) == 2.0 * sampleAt(stepped, kStepFrame, 0));
    CHECK(sampleAt(stepped, kStepFrame - 1, 1) == 2.0 * sampleAt(stepped, kStepFrame, 1));
    CHECK(sampleAt(stepped, kStepFrame, 0) == sampleAt(stepped, kStepFrame + 1, 0));
    CHECK(sampleAt(stepped, kStepFrame - 2, 0) == sampleAt(stepped, kStepFrame - 1, 0));
    // Same numbers on the right channel: the lane drives the whole strip.
    CHECK(sampleAt(stepped, kStepFrame - 1, 1) == 2.0 * sampleAt(stepped, kStepFrame, 1));

    // 2.3 Segment shape: linear-in-dB, and the end points are held outside the
    // lane - the first point forever before it, the last point forever after.
    CHECK_OK(vol.get(), daw_remove_track_volume_automation_point(vol.get(), solo, kStepFrame - 1, rev(vol.get())));
    CHECK_OK(vol.get(), daw_remove_track_volume_automation_point(vol.get(), solo, kStepFrame, rev(vol.get())));
    CHECK(volumePoints(vol.get(), solo).empty());
    CHECK_OK(vol.get(), daw_upsert_track_volume_automation_point(vol.get(), solo, kStepFrame, kHalfDb, rev(vol.get())));
    CHECK_OK(vol.get(), daw_upsert_track_volume_automation_point(vol.get(), solo, kStepFrame + 6000, 0.0,
                                                                 rev(vol.get())));
    const auto ramped = exportProject(vol.get(), root / "vol-ramp.wav", 2);
    expectProjectLength(ramped);
    checkWindow(ramped, 0, kMeasureFrom, kStepFrame, 0.25, kTol, "first point holds before the lane");
    checkWindow(ramped, 0, kStepFrame + 6000, kDcFrames, 0.5, kTol, "last point holds after the lane");
    {
      const double mid = std::abs(sampleAt(ramped, kStepFrame + 3000, 0));
      e2e::expectNear(mid, 0.5 * std::pow(10.0, kHalfDb / 40.0), kTol,
                      "segment midpoint is linear in dB, not in amplitude");
      const double quarter = std::abs(sampleAt(ramped, kStepFrame + 1500, 0));
      e2e::expectNear(quarter, 0.5 * std::pow(10.0, kHalfDb * 0.75 / 20.0), kTol, "quarter point of the segment");
      CHECK(mid > quarter);  // the curve really moves monotonically through it
    }

    // 2.4 An emptied lane is static behavior again - and not merely similar:
    // the render is bit-identical to the program that never had a lane.
    CHECK_OK(vol.get(), daw_remove_track_volume_automation_point(vol.get(), solo, kStepFrame, rev(vol.get())));
    CHECK_OK(vol.get(), daw_remove_track_volume_automation_point(vol.get(), solo, kStepFrame + 6000, rev(vol.get())));
    CHECK(volumePoints(vol.get(), solo).empty());
    const auto backToStatic = exportProject(vol.get(), root / "vol-empty.wav", 2);
    CHECK(bitIdentical(staticMix, backToStatic));

    // 2.5 Deleting the whole strip with its lane and undoing it restores the
    // point set exactly, in the same frames and values.
    {
      CHECK_OK(vol.get(), daw_upsert_track_volume_automation_point(vol.get(), solo, 1000, -6.0, rev(vol.get())));
      CHECK_OK(vol.get(), daw_upsert_track_volume_automation_point(vol.get(), solo, 2000, -12.0, rev(vol.get())));
      const auto laneBeforeDelete = volumePoints(vol.get(), solo);
      const auto projectBeforeDelete = stableDump(vol.get());
      CHECK_OK(vol.get(), daw_remove_track(vol.get(), solo, rev(vol.get())));
      CHECK(dumpOf(vol.get()).tracks.empty());
      CHECK_OK(vol.get(), daw_undo(vol.get(), rev(vol.get())));
      const auto restoredTrack = dumpOf(vol.get()).tracks.at(0);
      if (!(restoredTrack.volumeAutomation == laneBeforeDelete))
        throw std::runtime_error("undo lost the volume lane: got " + pointText(restoredTrack.volumeAutomation) +
                                 " want " + pointText(laneBeforeDelete));
      const auto restoredProject = stableDump(vol.get());
      if (!(restoredProject == projectBeforeDelete))
        throw std::runtime_error("undo did not restore the automated project: BEFORE " +
                                 describe(projectBeforeDelete) + " AFTER " + describe(restoredProject));
    }

    // 2.6 The same contract on program material instead of DC, measured with the
    //     harness' own statistics helpers. A 1 kHz tone at 48 kHz repeats every
    //     48 samples, so every window below spans a whole number of periods and
    //     its Goertzel magnitude, its peak and its RMS are closed form: the
    //     amplitude, the amplitude, and the amplitude over root two. Three
    //     plateaus - unity, -6 dB, the -120 dB floor - and back to unity, plus
    //     the single-period windows either side of the first step, which is the
    //     frame-exact form of the claim on real audio.
    {
      Bridge tone;
      const auto oscillator = importTone(tone.get(), root, "tone-solo.wav", kDcFrames, 0.5, "Tone strip");
      const Points staircase{{0, 0.0},   {9024 - 1, 0.0},      {9024, -6.0},       {kStepFrame - 1, -6.0},
                             {kStepFrame, -120.0}, {14400 - 1, -120.0}, {14400, 0.0}};
      for (const auto& point : staircase)
        CHECK_OK(tone.get(), daw_upsert_track_volume_automation_point(tone.get(), oscillator, point.first,
                                                                     point.second, rev(tone.get())));
      expectPoints(volumePoints(tone.get(), oscillator), staircase, "tone staircase kept its frames");
      const auto stepped = exportProject(tone.get(), root / "tone-staircase.wav", 2);
      expectProjectLength(stepped);
      CHECK(stepped.sampleRate == kProjectRate);
      const double step6 = std::pow(10.0, -6.0 / 20.0);
      for (const uint16_t channel : {uint16_t{0}, uint16_t{1}}) {
        const std::string tag = "channel " + std::to_string(channel);
        // 2.6.1 Whole-window magnitudes: 63 periods at unity, 62 at -6 dB.
        const double unity = toneMagnitude(stepped, kMeasureFrom, 9024, channel, 1000.0);
        const double lowered = toneMagnitude(stepped, 9024, kStepFrame, channel, 1000.0);
        e2e::expectNear(unity, 0.5, kTol, "tone magnitude at unity, " + tag);
        e2e::expectNear(lowered, 0.5 * step6, kTol, "tone magnitude at -6 dB, " + tag);
        expectRatio(lowered / unity, step6, "magnitude across the step, " + tag);
        // 2.6.2 The same through RMS and peak, and the tone is still a tone: its
        // crest factor is root two, not the one of a plateau or of noise.
        e2e::expectNear(channelRms(stepped, kMeasureFrom, 9024, channel), 0.5 / std::sqrt(2.0), kTol,
                        "tone rms at unity, " + tag);
        expectRatio(channelRms(stepped, 9024, kStepFrame, channel) / channelRms(stepped, kMeasureFrom, 9024, channel),
                    step6, "rms across the step, " + tag);
        expectRatio(channelPeak(stepped, 9024, kStepFrame, channel) / channelPeak(stepped, kMeasureFrom, 9024, channel),
                    step6, "peak across the step, " + tag);
        expectRatio(channelPeak(stepped, kMeasureFrom, 9024, channel) / channelRms(stepped, kMeasureFrom, 9024, channel),
                    std::sqrt(2.0), "crest factor before the step, " + tag);
        // 2.6.3 Frame-exact on audio: one period ends on the frame before the
        // point and the next period starts on it, and those two periods already
        // differ by the whole 6 dB - a ramp of even one sample would move this
        // ratio by a percent, not by a millionth.
        expectRatio(toneMagnitude(stepped, 9024, 9072, channel, 1000.0) /
                        toneMagnitude(stepped, 8976, 9024, channel, 1000.0),
                    step6, "one period either side of frame 9024, " + tag);
        // 2.6.4 Nothing else is in the signal. The neighbour windows below are
        // 1200 samples = 25 periods of 1 kHz = 26 periods of 1040 Hz, and their
        // sum and difference both land on whole periods too, so the two bins are
        // exactly orthogonal over them: energy at 1040 Hz would be real leakage
        // caused by the automation, not windowing. The same window read at 1 kHz
        // is still the amplitude itself, which is what makes that read-out
        // meaningful.
        e2e::expectNear(toneMagnitude(stepped, kMeasureFrom, kMeasureFrom + 1200, channel, 1000.0), 0.5, kTol,
                        "whole-period sub-window at unity, " + tag);
        e2e::expectNear(toneMagnitude(stepped, kMeasureFrom, kMeasureFrom + 1200, channel, 1040.0), 0.0, 1e-6,
                        "adjacent bin at unity, " + tag);
        e2e::expectNear(toneMagnitude(stepped, 9024, 9024 + 1200, channel, 1040.0), 0.0, 1e-6,
                        "adjacent bin at -6 dB, " + tag);
        // 2.6.5 The -120 dB plateau is bit-exact silence, not a very quiet tone,
        // and at frame 14400 the lane hands the tone back exactly as it was.
        for (uint32_t frame = kStepFrame; frame < 14400; ++frame) CHECK(sampleAt(stepped, frame, channel) == 0.0f);
        CHECK(channelPeak(stepped, kStepFrame, 14400, channel) == 0.0);
        CHECK(channelRms(stepped, kStepFrame, 14400, channel) == 0.0);
        CHECK(toneMagnitude(stepped, kStepFrame, 14400, channel, 1000.0) == 0.0);
        expectRatio(toneMagnitude(stepped, 14400, kDcFrames, channel, 1000.0) / unity, 1.0,
                    "the tone returns to its first level, " + tag);
        expectRatio(channelPeak(stepped, 14400, kDcFrames, channel) / channelPeak(stepped, kMeasureFrom, 9024, channel),
                    1.0, "the returned peak, " + tag);
      }
      // The lane carried the whole shape; no static control moved on the way.
      CHECK(trackById(tone.get(), oscillator).gain_db == 0.0);
    }

    // =========================================================================
    // 3. Track pan automation: sample-accurate balance, linear in the pan
    //    value, with unity center.
    // =========================================================================
    Bridge pan;
    const auto panned = importDc(pan.get(), root, "pan-solo.wav", 1, 0.5f, 0.0f, "Panned");
    CHECK_OK(pan.get(), daw_upsert_track_pan_automation_point(pan.get(), panned, 6000, 0.0, rev(pan.get())));
    CHECK_OK(pan.get(), daw_upsert_track_pan_automation_point(pan.get(), panned, kStepFrame, -1.0, rev(pan.get())));
    const auto panMix = exportProject(pan.get(), root / "pan-automation.wav", 2);
    expectProjectLength(panMix);
    // Before the segment the center law holds: both channels at the input.
    checkWindow(panMix, 0, kMeasureFrom, 6001, 0.5, kTol, "pan automation keeps L");
    checkWindow(panMix, 1, kMeasureFrom, 6001, 0.5, kTol, "pan automation center");
    // Half way through the ramp the balance is exactly -0.5: L stays unity, R
    // is multiplied by 1 + pan = 0.5.
    e2e::expectNear(std::abs(sampleAt(panMix, 9000, 0)), 0.5, kTol, "pan ramp never touches the left channel");
    e2e::expectNear(std::abs(sampleAt(panMix, 9000, 1)), 0.25, kTol, "pan ramp midpoint halves the right channel");
    // Hard left: R is exactly zero from the frame the lane reaches -1, and the
    // last frame before it is still a nonzero (tiny) amount - so the arrival of
    // exact silence IS the sample-accuracy proof.
    checkWindow(panMix, 1, kStepFrame, kDcFrames, 0.0, 0.0, "hard-left pan zeroes R exactly");
    CHECK(std::abs(sampleAt(panMix, kStepFrame - 1, 1)) > 0.0);
    {
      uint32_t firstZero = kMeasureFrom;
      while (firstZero < static_cast<uint32_t>(panMix.frames()) && sampleAt(panMix, firstZero, 1) != 0.0) ++firstZero;
      CHECK(firstZero == kStepFrame);
    }
    checkWindow(panMix, 0, 6000, kDcFrames, 0.5, kTol, "left channel untouched across the whole pan move");

    // =========================================================================
    // 4. Bus gain and master gain lanes are just as sample-accurate, and they
    //    act exactly where the graph says they must. The two strips are
    //    separated by pan so that channel L carries only the routed signal and
    //    channel R only the direct one - one export then proves both halves of
    //    the claim.
    // =========================================================================
    Bridge graph;
    const auto fed = importDc(graph.get(), root, "graph-fed.wav", 1, 0.5f, 0.0f, "Routed");
    const auto direct = importDc(graph.get(), root, "graph-direct.wav", 1, 0.25f, 0.0f, "Direct");
    // Separate the two strips by pan so one export measures both: the routed
    // strip lives on L only (a negative pan never attenuates L), the direct one
    // on R only.
    CHECK_OK(graph.get(), daw_set_pan(graph.get(), fed, -1.0, rev(graph.get())));
    CHECK_OK(graph.get(), daw_set_pan(graph.get(), direct, 1.0, rev(graph.get())));
    const auto submix = addBus(graph.get(), "Submix");
    CHECK_OK(graph.get(), daw_set_track_output(graph.get(), fed, submix, rev(graph.get())));

    // 4.1 Bus automation halves the routed strip at one sample and cannot be
    // felt by the strip that never entered the bus.
    CHECK_OK(graph.get(), daw_upsert_bus_gain_automation_point(graph.get(), submix, kStepFrame - 1, 0.0,
                                                               rev(graph.get())));
    CHECK_OK(graph.get(), daw_upsert_bus_gain_automation_point(graph.get(), submix, kStepFrame, kHalfDb,
                                                               rev(graph.get())));
    CHECK(busGainPoints(graph.get(), submix).size() == 2);
    const auto busStep = exportProject(graph.get(), root / "bus-step.wav", 2);
    expectProjectLength(busStep);
    checkWindow(busStep, 0, kMeasureFrom, kStepFrame - 1, 0.5, kTol, "routed strip before the bus step");
    checkWindow(busStep, 0, kStepFrame, kDcFrames, 0.25, kTol, "routed strip after the bus step");
    CHECK(firstBelow(busStep, 0, 0.375, kMeasureFrom) == kStepFrame);
    CHECK(sampleAt(busStep, kStepFrame - 1, 0) == 2.0 * sampleAt(busStep, kStepFrame, 0));
    checkWindow(busStep, 1, kMeasureFrom, kDcFrames, 0.25, kTol, "the direct strip never sees bus automation");

    // 4.2 Muting the bus still works while its gain is automated - the gate is
    // a separate multiplier (docs/38), so the routed channel falls to silence
    // while the direct one keeps playing.
    CHECK_OK(graph.get(), daw_set_bus_mute(graph.get(), submix, 1, rev(graph.get())));
    const auto busMuted = exportProject(graph.get(), root / "bus-step-muted.wav", 2);
    expectProjectLength(busMuted);
    checkWindow(busMuted, 0, kStepFrame, kDcFrames, 0.0, 0.0, "muted bus is silence after the gate closes");
    CHECK(std::abs(sampleAt(busMuted, kDcFrames - 1, 0)) < kTol);
    checkWindow(busMuted, 1, kMeasureFrom, kDcFrames, 0.25, kTol, "direct strip survives a muted bus");
    CHECK_OK(graph.get(), daw_set_bus_mute(graph.get(), submix, 0, rev(graph.get())));

    // 4.3 The master lane replaces the smoothed master fader sample-for-sample
    // and acts post-routing: both channels fall together at the step.
    CHECK_OK(graph.get(), daw_remove_bus_gain_automation_point(graph.get(), submix, kStepFrame - 1, rev(graph.get())));
    CHECK_OK(graph.get(), daw_remove_bus_gain_automation_point(graph.get(), submix, kStepFrame, rev(graph.get())));
    CHECK(busGainPoints(graph.get(), submix).empty());
    CHECK_OK(graph.get(), daw_upsert_master_gain_automation_point(graph.get(), kStepFrame - 1, 0.0, rev(graph.get())));
    CHECK_OK(graph.get(), daw_upsert_master_gain_automation_point(graph.get(), kStepFrame, kHalfDb, rev(graph.get())));
    const auto masterStep = exportProject(graph.get(), root / "master-step.wav", 2);
    expectProjectLength(masterStep);
    checkWindow(masterStep, 0, kMeasureFrom, kStepFrame - 1, 0.5, kTol, "master lane pre-step L");
    checkWindow(masterStep, 0, kStepFrame, kDcFrames, 0.25, kTol, "master lane post-step L");
    checkWindow(masterStep, 1, kMeasureFrom, kStepFrame - 1, 0.25, kTol, "master lane pre-step R");
    checkWindow(masterStep, 1, kStepFrame, kDcFrames, 0.125, kTol, "master lane post-step R");
    CHECK(firstBelow(masterStep, 0, 0.375, kMeasureFrom) == kStepFrame);
    CHECK(firstBelow(masterStep, 1, 0.1875, kMeasureFrom) == kStepFrame);
    CHECK(sampleAt(masterStep, kStepFrame - 1, 0) == 2.0 * sampleAt(masterStep, kStepFrame, 0));
    CHECK(sampleAt(masterStep, kStepFrame - 1, 1) == 2.0 * sampleAt(masterStep, kStepFrame, 1));
    // 4.4 The static master fader is still the durable value the lane rides
    // around: the snapshot reports it, and the lane does not rewrite it.
    {
      const auto masterValue = snapshotOf(graph.get()).master_gain_db;
      CHECK(masterValue == 0.0);
      CHECK_OK(graph.get(), daw_set_master_gain(graph.get(), -6.0, rev(graph.get())));
      CHECK(masterGainPoints(graph.get()).size() == 2);
      CHECK(snapshotOf(graph.get()).master_gain_db == -6.0);
      CHECK_OK(graph.get(), daw_remove_master_gain_automation_point(graph.get(), kStepFrame - 1, rev(graph.get())));
      CHECK_OK(graph.get(), daw_remove_master_gain_automation_point(graph.get(), kStepFrame, rev(graph.get())));
      const auto nowStatic = exportProject(graph.get(), root / "master-static.wav", 2);
      expectProjectLength(nowStatic);
      checkPlateau(nowStatic, 0, kMeasureFrom, 0.5 * kHalfLinear, kTol, "static master -6 dB on L");
      checkPlateau(nowStatic, 1, kMeasureFrom, 0.25 * kHalfLinear, kTol, "static master -6 dB on R");
    }

    // =========================================================================
    // 5. Automation write gestures: the target matrix, the locked window, the
    //    exactly-one-revision commit, Touch versus Latch, and cancel.
    // =========================================================================
    Bridge gest;
    const auto singer = importDc(gest.get(), root, "gest-singer.wav", 1, 0.5f, 0.0f, "Singer");
    const auto drummer = importDc(gest.get(), root, "gest-drum.wav", 1, 0.25f, 0.0f, "Drummer");
    const auto room = addBus(gest.get(), "Room");

    // 5.1 The gesture state machine is reachable for every lane kind through
    // the generic target matrix, and opening one is not a mutation: it is
    // closed again with cancel leaving no lane point and no revision.
    {
      const uint64_t start = rev(gest.get());
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, singer,
                                                       DAW_AUTOMATION_TOUCH, start));
      CHECK(rev(gest.get()) == start);
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 1000, -6.0));
      daw_cancel_automation_gesture(gest.get());
      CHECK(rev(gest.get()) == start);
      CHECK(volumePoints(gest.get(), singer).empty());
      // Cancel twice is harmless: the session is unlocked again.
      daw_cancel_automation_gesture(gest.get());
      CHECK(rev(gest.get()) == start);
      CHECK_OK(gest.get(), daw_set_gain(gest.get(), singer, 0.0, start));  // identical value: no revision
      CHECK(rev(gest.get()) == start);

      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_PAN, singer,
                                                       DAW_AUTOMATION_LATCH, start));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 1000, 0.5));
      daw_cancel_automation_gesture(gest.get());
      CHECK(panPoints(gest.get(), singer).empty());
      CHECK(rev(gest.get()) == start);

      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_BUS_GAIN, room,
                                                       DAW_AUTOMATION_TOUCH, start));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 1000, -3.0));
      daw_cancel_automation_gesture(gest.get());
      CHECK(busGainPoints(gest.get(), room).empty());
      CHECK(rev(gest.get()) == start);

      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_MASTER_GAIN, 0,
                                                       DAW_AUTOMATION_LATCH, start));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 1000, -1.0));
      daw_cancel_automation_gesture(gest.get());
      CHECK(masterGainPoints(gest.get()).empty());
      CHECK(rev(gest.get()) == start);
    }
    // 5.2 Target and mode validation happens BEFORE the session is locked, so
    // a bad begin can never leave the project stuck in a half-open operation.
    {
      const uint64_t start = rev(gest.get());
      CHECK_REJ(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_MASTER_GAIN, singer,
                                                        DAW_AUTOMATION_TOUCH, start));
      CHECK_REJ(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, 9999,
                                                        DAW_AUTOMATION_TOUCH, start));
      CHECK_REJ(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_BUS_GAIN, singer,
                                                        DAW_AUTOMATION_TOUCH, start));
      CHECK_REJ(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_PAN, room,
                                                        DAW_AUTOMATION_LATCH, start));
      CHECK_REJ(gest.get(), daw_begin_automation_gesture(gest.get(), 99, singer, DAW_AUTOMATION_TOUCH, start));
      CHECK_REJ(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, singer, 99,
                                                        start));
      CHECK_REJ(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, singer,
                                                        DAW_AUTOMATION_TOUCH, start + 1));
      CHECK(rev(gest.get()) == start);
      // Still unlocked: an ordinary command goes through.
      CHECK_OK(gest.get(), daw_set_gain(gest.get(), drummer, -1.0, start));
      CHECK(rev(gest.get()) == start + 1);
      // The buffered calls need an open gesture to write into.
      const uint64_t open = rev(gest.get());
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), 0, 0.0));
      CHECK_REJ(gest.get(), daw_end_automation_gesture(gest.get(), 48000, open));
      CHECK(rev(gest.get()) == open);
    }

    // 5.3 While a gesture is open the project is locked: every revision-aware
    // mutation, every lane write and Undo/Redo are refused, and the revision
    // cannot move. The gesture's own samples cost nothing.
    {
      const uint64_t start = rev(gest.get());
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, singer,
                                                       DAW_AUTOMATION_TOUCH, start));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 2000, -3.0));
      CHECK(rev(gest.get()) == start);
      CHECK_REJ_KEEPS_REV(gest.get(), daw_set_gain(gest.get(), singer, -6.0, rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_set_mute(gest.get(), singer, 1, rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_add_track(gest.get(), "Too late", rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_add_bus(gest.get(), "Too late", rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_upsert_send(gest.get(), singer, room, 0.0, 0, rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_upsert_track_volume_automation_point(gest.get(), singer, 9000, -9.0,
                                                                              rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_set_master_gain(gest.get(), -3.0, rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_set_tempo(gest.get(), 0, 96.0, rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_add_marker(gest.get(), 0, "Nope", rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_undo(gest.get(), rev(gest.get())));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_redo(gest.get(), rev(gest.get())));
      // A second take cannot be opened on top of an open one - the lock covers
      // the gesture commands themselves, not only the lane commands.
      CHECK_REJ_SAYS(gest.get(),
                     daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_BUS_GAIN, room, DAW_AUTOMATION_LATCH,
                                                 start),
                     "Automation gesture is active");
      // And reading stays alive while the user drives the fader: the snapshot,
      // the transport and the lane readers all work, and they all still report
      // the buffered sample as private.
      CHECK(rev(gest.get()) == start);
      CHECK(snapshotOf(gest.get()).revision == start);
      CHECK_OK(gest.get(), daw_seek_frame(gest.get(), 1000));
      {
        uint32_t pending = 7;
        CHECK_OK(gest.get(), daw_get_track_volume_automation_count(gest.get(), singer, &pending));
        CHECK(pending == 0);
      }
      CHECK(rev(gest.get()) == start);
      daw_cancel_automation_gesture(gest.get());
      CHECK(rev(gest.get()) == start);
      CHECK_OK(gest.get(), daw_undo(gest.get(), start));  // unlocked again
      CHECK(rev(gest.get()) == start + 1);
      CHECK_OK(gest.get(), daw_redo(gest.get(), start + 1));
      CHECK(rev(gest.get()) == start + 2);
    }

    // 5.4 Touch: only the samples the user actually moved through are written.
    // The drummer is taken to the -120 dB floor first - gain(db) is EXACTLY
    // zero at the floor, so it contributes nothing at all - and the singer lane
    // is pre-seeded with a flat 0 dB segment between frames 6000 and 9000. That
    // segment is the one region a take at frame 12000 cannot reach, which makes
    // "nothing before the first supplied sample changed" a bit-for-bit
    // statement rather than a guess: a lane redraws LINEARLY-IN-dB between
    // neighbouring points, so a single lone pre-seed at frame 0 would have been
    // smeared across the whole lane.
    {
      CHECK_OK(gest.get(), daw_set_gain(gest.get(), drummer, -120.0, rev(gest.get())));
      CHECK_OK(gest.get(), daw_upsert_track_volume_automation_point(gest.get(), singer, 6000, 0.0, rev(gest.get())));
      CHECK_OK(gest.get(), daw_upsert_track_volume_automation_point(gest.get(), singer, 9000, 0.0, rev(gest.get())));
      expectPoints(volumePoints(gest.get(), singer), Points{{6000, 0.0}, {9000, 0.0}}, "touch pre-seed");
      const auto before = exportProject(gest.get(), root / "gesture-before.wav", 2);
      expectProjectLength(before);
      checkWindow(before, 0, 6000, 9000, 0.5, kTol, "pre-seeded flat segment carries the meter");
      const uint64_t start = rev(gest.get());
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, singer,
                                                       DAW_AUTOMATION_TOUCH, start));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), kStepFrame, -3.0));
      // The header demands ordered samples: a frame before the last accepted
      // one is refused, and refuses to move the buffered lane too.
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), kStepFrame - 1, -6.0));
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), 0, -6.0));
      // Same-frame writes are allowed (they replace), out-of-domain values are
      // not, and none of it costs a revision while the gesture is open.
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), kStepFrame, -18.0));
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), kStepFrame + 1, 500.0));
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), kStepFrame + 1, 24.000001));
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), kStepFrame + 1, -120.000001));
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), kStepFrame + 1,
                                                        std::numeric_limits<double>::quiet_NaN()));
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), 48000ULL * 600 + 1, 0.0));
      CHECK(rev(gest.get()) == start);
      // A wrong expected_revision is a conflict, and a conflicted end leaves
      // the gesture open - it does not silently drop the take.
      CHECK_REJ(gest.get(), daw_end_automation_gesture(gest.get(), kStepFrame + 8000, start + 1));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_set_gain(gest.get(), singer, -6.0, rev(gest.get())));
      // end_frame must not precede the final sample.
      CHECK_REJ(gest.get(), daw_end_automation_gesture(gest.get(), kStepFrame - 1, start));
      CHECK_REJ_KEEPS_REV(gest.get(), daw_set_gain(gest.get(), singer, -6.0, rev(gest.get())));
      // Ending at the final sample exactly is legal.
      CHECK_OK(gest.get(), daw_end_automation_gesture(gest.get(), kStepFrame, start));
      CHECK(rev(gest.get()) == start + 1);  // EXACTLY one revision for the take
      expectPoints(volumePoints(gest.get(), singer), Points{{6000, 0.0}, {9000, 0.0}, {kStepFrame, -18.0}},
                   "touch writes only the supplied samples");
      const auto touched = exportProject(gest.get(), root / "gesture-touch.wav", 2);
      expectProjectLength(touched);
      // The unreachable window is bit-identical, sample for sample.
      for (uint32_t frame = 6000; frame < 9000; ++frame)
        CHECK(sampleAt(touched, frame, 0) == sampleAt(before, frame, 0));
      // From the take's last sample the level holds to the end of the program:
      // touch added no point at end_frame, so nothing pulls it back up.
      checkWindow(touched, 0, kStepFrame, kDcFrames, 0.5 * std::pow(10.0, -18.0 / 20.0), kTol,
                  "touch level holds after the take");
      CHECK(std::abs(sampleAt(touched, kStepFrame, 0)) < std::abs(sampleAt(touched, kStepFrame - 1, 0)));
      CHECK(sampleAt(touched, kDcFrames - 1, 0) == sampleAt(touched, kStepFrame, 0));
      // Undo steps the whole take back as one unit - and the render with it.
      CHECK_OK(gest.get(), daw_undo(gest.get(), rev(gest.get())));
      expectPoints(volumePoints(gest.get(), singer), Points{{6000, 0.0}, {9000, 0.0}},
                   "undo rolls back the whole gesture");
      CHECK(bitIdentical(before, exportProject(gest.get(), root / "gesture-undo.wav", 2)));
      CHECK_OK(gest.get(), daw_redo(gest.get(), rev(gest.get())));
      expectPoints(volumePoints(gest.get(), singer), Points{{6000, 0.0}, {9000, 0.0}, {kStepFrame, -18.0}},
                   "redo replays the whole gesture");
      CHECK(bitIdentical(touched, exportProject(gest.get(), root / "gesture-redo.wav", 2)));

      // 5.5 Latch: the final value is written through end_frame, so the
      // trailing region holds instead of falling back to the static fader.
      const uint64_t latch = rev(gest.get());
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, singer,
                                                       DAW_AUTOMATION_LATCH, latch));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 18000, kHalfDb));
      CHECK_OK(gest.get(), daw_end_automation_gesture(gest.get(), 21000, latch));
      CHECK(rev(gest.get()) == latch + 1);
      expectPoints(volumePoints(gest.get(), singer),
                   Points{{6000, 0.0}, {9000, 0.0}, {kStepFrame, -18.0}, {18000, kHalfDb}, {21000, kHalfDb}},
                   "latch writes the trailing point");
      const auto latched = exportProject(gest.get(), root / "gesture-latch.wav", 2);
      expectProjectLength(latched);
      checkWindow(latched, 0, 21000, kDcFrames, 0.25, kTol, "latch holds to the end of the program");
      CHECK(sampleAt(latched, 21000, 0) == sampleAt(latched, 21001, 0));
      CHECK(sampleAt(latched, 21000, 0) == sampleAt(latched, kDcFrames - 1, 0));
      // The latch disturbed nothing before its own first sample: every frame
      // up to the earlier take is still bit-identical to the touch render.
      for (uint32_t frame = 6000; frame < kStepFrame; ++frame)
        CHECK(sampleAt(latched, frame, 0) == sampleAt(touched, frame, 0));
      // Latching with nothing left to hold (end_frame == final sample) adds no
      // duplicate point.
      const uint64_t next = rev(gest.get());
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, singer,
                                                       DAW_AUTOMATION_LATCH, next));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 22000, -6.0));
      CHECK_OK(gest.get(), daw_end_automation_gesture(gest.get(), 22000, next));
      CHECK(rev(gest.get()) == next + 1);
      expectPoints(volumePoints(gest.get(), singer),
                   Points{{6000, 0.0}, {9000, 0.0}, {kStepFrame, -18.0}, {18000, kHalfDb}, {21000, kHalfDb},
                         {22000, -6.0}}, "latch at its own last sample does not duplicate");
    }
    // 5.6 A gesture that never wrote a sample commits nothing: no lane, no
    // revision. docs/44 spells this out; the header's "one revision at end" is
    // about a take that actually carries samples.
    {
      const uint64_t empty = rev(gest.get());
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_PAN, singer,
                                                       DAW_AUTOMATION_TOUCH, empty));
      CHECK_OK(gest.get(), daw_end_automation_gesture(gest.get(), 24000, empty));
      CHECK(rev(gest.get()) == empty);
      CHECK(panPoints(gest.get(), singer).empty());
      CHECK_OK(gest.get(), daw_set_gain(gest.get(), singer, -1.0, empty));
      CHECK(rev(gest.get()) == empty + 1);
    }

    // =========================================================================
    // 6. The plug-in parameter automation surface, at contract level only. No
    //    Audio Unit or VST3 is loaded (and none is available on every platform
    //    this runs on), so every owner slot is legitimately empty. The ABI must
    //    answer with zero counts, name the missing insert on every attempt to
    //    touch its lanes, never move the revision, and leave the gesture state
    //    machine usable afterwards.
    // =========================================================================
    {
      uint32_t count = 7;
      CHECK_OK(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_MASTER, 0, &count));
      CHECK(count == 0);
      CHECK_OK(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_TRACK, singer, &count));
      CHECK(count == 0);
      CHECK_OK(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_BUS, room, &count));
      CHECK(count == 0);
      // The owner/ID matrix is the same on every insert call.
      const uint64_t start = rev(gest.get());
      CHECK_REJ_SAYS(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_MASTER, singer, &count),
                     "owner ID must be zero");
      CHECK_REJ_SAYS(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_TRACK, 0, &count), "require an ID");
      CHECK_REJ_SAYS(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_BUS, 0, &count), "require an ID");
      CHECK_REJ(gest.get(), daw_get_insert_count(gest.get(), 0, 0, &count));
      CHECK_REJ(gest.get(), daw_get_insert_count(gest.get(), 99, singer, &count));
      CHECK_REJ(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_TRACK, singer, nullptr));
      CHECK_REJ_SAYS(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_TRACK, 9999, &count),
                     "Track not found");
      CHECK_REJ_SAYS(gest.get(), daw_get_insert_count(gest.get(), DAW_INSERT_OWNER_BUS, 9999, &count), "Bus not found");
      auto plugin = abi<daw_plugin>();
      CHECK_REJ_SAYS(gest.get(), daw_get_insert(gest.get(), DAW_INSERT_OWNER_MASTER, 0, 0, &plugin),
                     "Insert index out of range");
      // A phantom insert id is a semantic rejection on all four lane calls.
      const uint64_t phantom = 4242;
      CHECK_REJ_SAYS(gest.get(), daw_get_insert_parameter_automation_count(gest.get(), DAW_INSERT_OWNER_TRACK, singer,
                                                                          phantom, 0, &count), "Insert not found");
      auto ppoint = abi<daw_plugin_parameter_automation_point>();
      CHECK_REJ(gest.get(), daw_get_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_TRACK, singer,
                                                                     phantom, 0, 0, &ppoint));
      ppoint.struct_size = 4;
      CHECK(daw_get_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_TRACK, singer, phantom, 0, 0,
                                                     &ppoint) == 1);
      CHECK_REJ_SAYS(gest.get(), daw_upsert_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_TRACK,
          singer, phantom, 7, "Cutoff", 1000, 0.5, start), "Plug-in insert not found");
      CHECK_REJ_SAYS(gest.get(), daw_remove_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_TRACK,
          singer, phantom, 7, 1000, start), "Plug-in insert not found");
      CHECK_REJ_SAYS(gest.get(), daw_upsert_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_BUS, room,
          phantom, 7, "Mix", 1000, 1.0, start), "Plug-in insert not found");
      // Normalized means 0...1, and that guard is ABI-cheap: it fires before
      // the owner lookup, so it is the reason you get even for a phantom id.
      CHECK_REJ_SAYS(gest.get(), daw_upsert_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_MASTER, 0,
          phantom, 0, "Mix", 1000, -0.001, start), "normalized");
      CHECK_REJ_SAYS(gest.get(), daw_upsert_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_MASTER, 0,
          phantom, 0, "Mix", 1000, 1.001, start), "normalized");
      CHECK_REJ(gest.get(), daw_upsert_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_MASTER, 0,
          phantom, 0, "Mix", 1000, std::numeric_limits<double>::quiet_NaN(), start));
      CHECK_REJ_SAYS(gest.get(), daw_upsert_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_MASTER,
          singer, phantom, 0, "Mix", 1000, 2.0, start), "owner ID must be zero");
      CHECK(rev(gest.get()) == start);
      // The parameter gesture surface is closed without a live insert - and
      // the bridge resolves the insert first, so it names the insert.
      CHECK_REJ_SAYS(gest.get(), daw_begin_insert_parameter_automation_gesture(gest.get(), DAW_INSERT_OWNER_TRACK,
          singer, phantom, 0, "Mix", DAW_AUTOMATION_TOUCH, start), "Insert not found");
      CHECK_REJ_SAYS(gest.get(), daw_write_insert_parameter_automation_gesture(gest.get(), 1000, 0.5),
                     "No plug-in parameter automation gesture is active");
      CHECK_REJ(gest.get(), daw_end_insert_parameter_automation_gesture(gest.get(), 2000, start));
      daw_cancel_insert_parameter_automation_gesture(gest.get());  // void: harmless with nothing open
      CHECK(rev(gest.get()) == start);
      // One session-wide gesture machine: with a lane gesture open, the domain
      // lock is what stops a parameter lane write.
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_VOLUME, singer,
                                                       DAW_AUTOMATION_TOUCH, start));
      CHECK_REJ_SAYS(gest.get(), daw_upsert_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_MASTER, 0,
          phantom, 0, "Mix", 1000, 0.5, start), "Automation gesture is active");
      CHECK_REJ_SAYS(gest.get(), daw_remove_insert_parameter_automation_point(gest.get(), DAW_INSERT_OWNER_MASTER, 0,
          phantom, 0, 1000, start), "Automation gesture is active");
      daw_cancel_automation_gesture(gest.get());
      CHECK(rev(gest.get()) == start);
      // Still usable afterwards: a real take commits exactly one revision.
      const uint64_t after = rev(gest.get());
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_BUS_GAIN, room,
                                                       DAW_AUTOMATION_LATCH, after));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 4000, -6.0));
      CHECK_OK(gest.get(), daw_end_automation_gesture(gest.get(), 8000, after));
      CHECK(rev(gest.get()) == after + 1);
      expectPoints(busGainPoints(gest.get(), room), Points{{4000, -6.0}, {8000, -6.0}},
                   "bus gain latch through the generic API");
      // A pan take through the generic API, on the same target rules.
      const uint64_t panned2 = rev(gest.get());
      CHECK_OK(gest.get(), daw_begin_automation_gesture(gest.get(), DAW_AUTOMATION_TRACK_PAN, drummer,
                                                       DAW_AUTOMATION_TOUCH, panned2));
      CHECK_OK(gest.get(), daw_write_automation_gesture(gest.get(), 1000, 1.0));
      CHECK_REJ(gest.get(), daw_write_automation_gesture(gest.get(), 2000, 1.0001));
      CHECK_OK(gest.get(), daw_end_automation_gesture(gest.get(), 2000, panned2));
      expectPoints(panPoints(gest.get(), drummer), Points{{1000, 1.0}}, "pan touch through the generic API");
      CHECK(rev(gest.get()) == panned2 + 1);
    }

    // =========================================================================
    // 7. Durability: all four lane kinds go to the draft and come back point
    //    for point, the reopened project renders bit-identically, and its lanes
    //    are still editable and undoable in the new session.
    // =========================================================================
    Bridge store;
    const auto vox = importDc(store.get(), root, "store-vox.wav", 2, 0.4f, 0.2f, "Vox");
    const auto bed = importDc(store.get(), root, "store-bed.wav", 1, 0.5f, 0.0f, "Bed");
    const auto print = addBus(store.get(), "Print");
    CHECK_OK(store.get(), daw_set_track_output(store.get(), bed, print, rev(store.get())));
    CHECK_OK(store.get(), daw_upsert_track_volume_automation_point(store.get(), vox, 0, 0.0, rev(store.get())));
    CHECK_OK(store.get(), daw_upsert_track_volume_automation_point(store.get(), vox, kStepFrame, kHalfDb,
                                                                   rev(store.get())));
    CHECK_OK(store.get(), daw_upsert_track_pan_automation_point(store.get(), vox, 3000, -0.5, rev(store.get())));
    CHECK_OK(store.get(), daw_upsert_bus_gain_automation_point(store.get(), print, 6000, -3.0, rev(store.get())));
    CHECK_OK(store.get(), daw_upsert_master_gain_automation_point(store.get(), 9000, -1.5, rev(store.get())));
    {
      const uint64_t take = rev(store.get());
      CHECK_OK(store.get(), daw_begin_automation_gesture(store.get(), DAW_AUTOMATION_TRACK_VOLUME, vox,
                                                        DAW_AUTOMATION_LATCH, take));
      CHECK_OK(store.get(), daw_write_automation_gesture(store.get(), 18000, -12.0));
      CHECK_OK(store.get(), daw_end_automation_gesture(store.get(), 20000, take));
      CHECK(rev(store.get()) == take + 1);
    }
    const auto built = dumpOf(store.get());
    CHECK(built.tracks.size() == 2 && built.buses.size() == snapshotOf(store.get()).bus_count);
    expectPoints(built.tracks.at(0).volumeAutomation, Points{{0, 0.0}, {kStepFrame, kHalfDb}, {18000, -12.0},
                                                            {20000, -12.0}}, "dump carries the lane");
    const auto mixBefore = exportProject(store.get(), root / "store-before.wav", 2);
    expectProjectLength(mixBefore);
    const auto draft = root / "automation.mydawdraft";
    saveDraftAndWait(store.get(), draft);
    CHECK(fileNonEmpty(draft));
    Bridge reopened;
    CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), draft.string().c_str()));
    const auto restored = dumpOf(reopened.get());
    if (!(restored == built))
        throw std::runtime_error("save/reopen changed the automation model: BEFORE " + describe(built) +
                                 " AFTER " + describe(restored));
    // Not only the dump: the per-lane getters on the new session agree, frame
    // for frame and value for value.
    expectPoints(volumePoints(reopened.get(), vox), Points{{0, 0.0}, {kStepFrame, kHalfDb}, {18000, -12.0},
                                                          {20000, -12.0}}, "reopened volume lane");
    expectPoints(panPoints(reopened.get(), vox), Points{{3000, -0.5}}, "reopened pan lane");
    expectPoints(busGainPoints(reopened.get(), print), Points{{6000, -3.0}}, "reopened bus gain lane");
    expectPoints(masterGainPoints(reopened.get()), Points{{9000, -1.5}}, "reopened master gain lane");
    const auto mixAfter = exportProject(reopened.get(), root / "store-after.wav", 2);
    CHECK(bitIdentical(mixBefore, mixAfter));
    // The reopened lanes are live: every edit is a revision, and Undo steps
    // them back one edit at a time - the whole point set, not a partial draw.
    const auto reopenedModel = stableDump(reopened.get());
    CHECK_OK(reopened.get(), daw_upsert_track_volume_automation_point(reopened.get(), vox, 21000, 0.0,
                                                                      rev(reopened.get())));
    expectPoints(volumePoints(reopened.get(), vox),
                 Points{{0, 0.0}, {kStepFrame, kHalfDb}, {18000, -12.0}, {20000, -12.0}, {21000, 0.0}},
                 "reopened lane takes a new point");
    CHECK_OK(reopened.get(), daw_remove_track_volume_automation_point(reopened.get(), vox, 0, rev(reopened.get())));
    expectPoints(volumePoints(reopened.get(), vox),
                 Points{{kStepFrame, kHalfDb}, {18000, -12.0}, {20000, -12.0}, {21000, 0.0}},
                 "reopened lane drops a point");
    CHECK_OK(reopened.get(), daw_undo(reopened.get(), rev(reopened.get())));
    expectPoints(volumePoints(reopened.get(), vox),
                 Points{{0, 0.0}, {kStepFrame, kHalfDb}, {18000, -12.0}, {20000, -12.0}, {21000, 0.0}},
                 "one undo steps back exactly one point edit");
    CHECK_OK(reopened.get(), daw_undo(reopened.get(), rev(reopened.get())));
    {
      const auto back = stableDump(reopened.get());
      if (!(back == reopenedModel))
          throw std::runtime_error("undo on the reopened project did not restore the lanes: AFTER " +
                                   describe(back));
    }
    expectPoints(volumePoints(reopened.get(), vox), Points{{0, 0.0}, {kStepFrame, kHalfDb}, {18000, -12.0},
                                                          {20000, -12.0}}, "undo restored the lane point set");
    // A gesture opened against the reopened session commits the same way.
    {
      const uint64_t take = rev(reopened.get());
      CHECK_OK(reopened.get(), daw_begin_automation_gesture(reopened.get(), DAW_AUTOMATION_MASTER_GAIN, 0,
                                                           DAW_AUTOMATION_TOUCH, take));
      CHECK_OK(reopened.get(), daw_write_automation_gesture(reopened.get(), 12000, -6.0));
      CHECK_OK(reopened.get(), daw_end_automation_gesture(reopened.get(), 13000, take));
      CHECK(rev(reopened.get()) == take + 1);
      expectPoints(masterGainPoints(reopened.get()), Points{{9000, -1.5}, {12000, -6.0}},
                   "reopened master lane takes a touch write");
    }

    std::cout << "PASS: e2e_automation - four lane kinds ordered, replaced and removed; silent no-ops and value "
                 "ranges; the ten-minute timeline and 2048-point ceilings, on lane commands and inside an open take; "
                 "sample-accurate audible steps for track volume, pan, bus gain and master gain, linear-in-dB "
                 "segments, the -120 dB floor as bit-exact silence and a 1 kHz tone verified by magnitude, RMS and "
                 "peak; Touch versus Latch with one-revision commits and a locked mutation window; plug-in parameter "
                 "surface contract checks; save/reopen and undo fidelity" << '\n' << std::flush;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "E2E FAIL: " << e.what() << '\n';
    return 1;
  }
}
