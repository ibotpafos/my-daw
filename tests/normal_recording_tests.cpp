// The real public C ABI, Renderer, SPSC RecordingWriter and durable commands.
// Only this executable supplies a deterministic device factory: there is no
// production environment switch/test backdoor and CI never changes real HAL.
#include "audio/duplex.hpp"
#include "audio/duplex_render.hpp"
#include "e2e/e2e.hpp"
#include <limits>

namespace {
using namespace e2e;
struct Invocation {
    uint64_t start = 0, loopStart = 0, loopEnd = 0, preroll = 0, capacity = 0;
    bool monitor = false;
    daw::AudioDeviceConfiguration config;
} invocation;
bool failStart = false, failDevice = false;
unsigned factories = 0, destroyed = 0;
std::shared_ptr<const daw::Clip> finishedCapture;
class CaptureDevice;
CaptureDevice *live = nullptr;
class CaptureDevice final : public daw::Duplex {
    daw::State state;
    std::string path;
    std::unique_ptr<daw::RecordingWriter> writer;
    uint64_t callbackCount = 0;
    bool active = false;

  public:
    CaptureDevice(const daw::State &model, const std::string &recovery)
        : state(model), path(recovery) {
        setMonitor(invocation.monitor);
        live = this;
    }
    ~CaptureDevice() override {
        cancel();
        if (live == this)
            live = nullptr;
        ++destroyed;
    }
    void start() override {
        if (failStart)
            throw daw::Error("Injected device-open failure");
        writer =
            daw::prepareDuplexCapture(renderer, state, invocation.capacity, path, invocation.start,
                                      invocation.loopStart, invocation.loopEnd, invocation.preroll);
        renderer.playing.store(true);
        active = true;
    }
    std::pair<std::vector<float>, std::vector<float>> pump(const std::vector<float> &input) {
        CHECK(active && input.size() <= 4096);
        std::pair<std::vector<float>, std::vector<float>> output;
        output.first.resize(input.size());
        output.second.resize(input.size());
        // Same production callback composition as MacDuplex. Input/output storage
        // is prepared above; this call contains no fixture allocation itself.
        daw::renderDuplexBlock(renderer, *writer, monitoring(), input.data(), output.first.data(),
                               output.second.data(), static_cast<uint32_t>(input.size()));
        ++callbackCount;
        return output;
    }
    std::shared_ptr<const daw::Clip> stop() override {
        CHECK(active);
        active = false;
        renderer.playing.store(false);
        if (!writer->frames()) {
            writer->discard();
            return {};
        }
        finishedCapture = writer->finish();
        return finishedCapture;
    }
    void cancel() noexcept override {
        active = false;
        renderer.playing.store(false);
        if (writer)
            writer->stopPreserving();
    }
    void markStalled() noexcept override {
        cancel();
    }
    void checkDevices() override {
        if (failDevice) {
            cancel();
            throw daw::Error("Injected device disconnect");
        }
    }
    uint64_t frames() const noexcept override {
        return writer ? writer->frames() : 0;
    }
    uint64_t callbacks() const noexcept override {
        return callbackCount;
    }
    bool overflowed() const noexcept override {
        return writer && writer->overflowed();
    }
    void discardRecovery() noexcept override {
        if (writer)
            writer->discard();
    }
    daw::OutputTelemetry telemetry() const noexcept override {
        return {active ? daw::OutputState::running : daw::OutputState::stopped, 123, 1,
                callbackCount, 0};
    }
};
void equal(const std::vector<float> &values, float expected) {
    CHECK(!values.empty());
    for (size_t index = 0; index < values.size(); ++index) {
        if (!std::isfinite(values[index]) || std::abs(values[index] - expected) >= 1e-6f)
            throw std::runtime_error("PCM mismatch at " + std::to_string(index) + ": " +
                                     std::to_string(values[index]) + " vs " +
                                     std::to_string(expected));
    }
}
// Existing mixer startup smoothing is intentional. Use an independent scalar
// oracle, not the production Renderer, when checking backing/export PCM.
float startupGain(size_t elapsedFrames) {
    float gain = 0;
    for (size_t f = 0; f < elapsedFrames; ++f)
        gain += (1.0f - gain) * 0.004166667f;
    return gain;
}
void backingPCM(const std::vector<float> &samples, size_t channels, size_t elapsedFrames = 0,
                float monitor = 0) {
    CHECK(!samples.empty() && samples.size() % channels == 0);
    for (size_t index = 0; index < samples.size(); ++index) {
        const auto expected = 0.25f * startupGain(elapsedFrames + index / channels + 1) + monitor;
        CHECK(std::isfinite(samples[index]) && std::abs(samples[index] - expected) < 1e-6f);
    }
}
daw_recording recording(daw_session *session) {
    auto info = abi<daw_recording>();
    CHECK_OK(session, daw_get_recording(session, &info));
    return info;
}
daw_transport transport(daw_session *session) {
    auto value = abi<daw_transport>();
    CHECK_OK(session, daw_get_transport(session, &value));
    return value;
}
void start(daw_session *session, uint64_t frame, const std::filesystem::path &path) {
    CHECK_OK(session, daw_record_start(session, frame, path.c_str()));
    CHECK(live);
}
void stop(daw_session *session, const char *name) {
    CHECK_OK(session, daw_record_stop(session, name, rev(session)));
    CHECK(!live && recording(session).recording == 0);
}
}
namespace daw {
// Deliberate test-executable link seam: the application still links MacDuplex.
// This strong factory means the static library's platform factory object is not
// extracted here; no conditional production compilation or runtime injection.
std::unique_ptr<Duplex> makeDuplex(const State &state, uint64_t capacity, const std::string &path,
                                   uint64_t start, uint64_t loopStart, uint64_t loopEnd,
                                   uint64_t preroll, bool monitor,
                                   const AudioDeviceConfiguration &configuration) {
    ++factories;
    invocation = {start, loopStart, loopEnd, preroll, capacity, monitor, configuration};
    return std::make_unique<CaptureDevice>(state, path);
}
}
int main() {
    try {
        using namespace e2e;
        TempRoot root("normal-duplex");
        Bridge owner;
        auto *s = owner.get();
        const auto originalRevision = rev(s);
        auto config = abi<daw_audio_device_config>();
        config.version = DAW_AUDIO_DEVICE_CONFIG_VERSION;
        std::strcpy(config.input_uid, "selected-interface");
        std::strcpy(config.output_uid, "selected-interface");
        config.input_channel = 3;
        config.output_left = 4;
        config.output_right = 5;
        CHECK_OK(s, daw_set_audio_device_config(s, &config));
        CHECK_OK(s, daw_set_record_preroll(s, 128));
        const auto first = root / "first.mydawtake";
        start(s, 0, first); // Empty project: no fake track is inserted to render.
        CHECK(invocation.start == 0 && invocation.loopStart == 0 && invocation.loopEnd == 0);
        CHECK(invocation.config.inputUID == "selected-interface" &&
              invocation.config.inputChannel == 3 && invocation.config.outputLeft == 4 &&
              invocation.config.outputRight == 5);
        CHECK(rev(s) == originalRevision && snapshotOf(s).track_count == 0);
        bool leaseRejected = false;
        try {
            daw::AudioHardwareLease exclusive;
        } catch (const daw::Error &) {
            leaseRejected = true;
        }
        CHECK(leaseRejected); // P0-02 exclusion survives the normal-record path.
        auto output = live->pump(std::vector<float>(256, 0.25f));
        equal(output.first, 0);
        equal(output.second, 0); // MON off does not mute capture.
        auto info = recording(s);
        CHECK(info.recording && !info.loop_recording && info.pass_count == 0 && info.frames == 256);
        CHECK(info.callbacks == 1 && info.target_track_id == 0);
        auto position = transport(s);
        CHECK(position.playing && position.frame == 256 && position.callbacks == 1);
        CHECK_REJ(s, daw_record_start(s, 0, (root / "duplicate").c_str()));
        CHECK_REJ(s, daw_seek_frame(s, 0));
        CHECK_REJ(s, daw_set_loop(s, 1, 0, 100));
        CHECK_REJ(s, daw_set_record_preroll(s, 0));
        CHECK_REJ(s, daw_play(s));
        CHECK_REJ(s, daw_stop(s));
        CHECK_REJ(s, daw_set_record_monitor(s, 2));
        CHECK_REJ(s, daw_record_stop(s, "", rev(s)));
        CHECK_REJ(s, daw_record_stop(s, "Stale", rev(s) + 1));
        CHECK(recording(s).recording && live && rev(s) == originalRevision);
        stop(s, "First");
        CHECK(!std::filesystem::exists(first));
        { daw::AudioHardwareLease releasedAfterStop; }
        CHECK(finishedCapture && finishedCapture->frames() == 256);
        equal(finishedCapture->samples(), 0.25f);
        CHECK(rev(s) == originalRevision + 1 && snapshotOf(s).track_count == 1);
        const auto backing = trackById(s, 1);
        CHECK(backing.audio_frames == 256 && backing.take_count == 1);
        auto region = abi<daw_clip>();
        CHECK_OK(s, daw_get_clip(s, backing.id, 0, &region));
        CHECK(region.start == 0 && region.length == 256);
        const auto exported = exportProject(s, root / "first.wav");
        CHECK(exported.frames() == 256);
        backingPCM(exported.samples, 2);
        const auto saved = root / "recorded.mydawdraft";
        saveDraftAndWait(s, saved);
        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), saved.c_str()));
        CHECK(dumpOf(s) == dumpOf(reopened.get()));
        CHECK(exportProject(reopened.get(), root / "reopened.wav").samples == exported.samples);
        CHECK_OK(s, daw_undo(s, rev(s)));
        CHECK(snapshotOf(s).track_count == 0);
        CHECK_OK(s, daw_redo(s, rev(s)));
        CHECK(snapshotOf(s).track_count == 1);

        // Backing during preroll. A block crosses the exact capture boundary.
        CHECK_OK(s, daw_set_record_preroll(s, 64));
        const auto over = root / "overdub.mydawtake";
        start(s, 96, over);
        CHECK(transport(s).frame == 32);
        std::vector<float> input(96, 0.5f);
        std::fill_n(input.begin(), 64, 0.1f);
        output = live->pump(input);
        backingPCM(output.first, 1);
        backingPCM(output.second, 1);
        CHECK(recording(s).frames == 32 && transport(s).frame == 128);
        CHECK_OK(s, daw_set_record_monitor(s, 1));
        CHECK(live->monitoring());
        const auto beforeMonitor = rev(s);
        output = live->pump(std::vector<float>(64, 0.75f));
        backingPCM(output.first, 1, 96, 0.75f);
        backingPCM(output.second, 1, 96, 0.75f);
        CHECK(rev(s) == beforeMonitor && recording(s).frames == 96);
        CHECK_OK(s, daw_set_record_monitor(s, 0));
        CHECK(!live->monitoring());
        stop(s, "Overdub");
        CHECK(finishedCapture && finishedCapture->frames() == 96);
        for (size_t f = 0; f < 96; ++f) {
            CHECK(finishedCapture->samples()[f * 2] == (f < 32 ? 0.5f : 0.75f));
            CHECK(finishedCapture->samples()[f * 2 + 1] == finishedCapture->samples()[f * 2]);
        }
        CHECK(snapshotOf(s).track_count == 2);
        auto newTrack = abi<daw_track>();
        CHECK_OK(s, daw_get_track(s, 1, &newTrack));
        CHECK(newTrack.take_count == 1 && newTrack.audio_frames == 96);
        CHECK_OK(s, daw_get_clip(s, newTrack.id, 0, &region));
        CHECK(region.start == 96 && region.length == 96);
        CHECK_OK(s, daw_set_mute(s, backing.id, 1, rev(s)));
        const auto dry = exportProject(s, root / "dry.wav");
        for (size_t f = 0; f < 96; ++f) {
            const auto expected = (f < 32 ? 0.5f : 0.75f) * startupGain(96 + f + 1);
            CHECK(std::abs(dry.samples[(96 + f) * 2] - expected) < 1e-6f);
            CHECK(dry.samples[(96 + f) * 2] == dry.samples[(96 + f) * 2 + 1]);
        }
        CHECK_OK(s, daw_set_mute(s, backing.id, 0, rev(s)));

        // A transport cycle does not turn a NEW linear track into loop passes;
        // capture beyond arrangement end remains silent and advances its clock.
        CHECK_OK(s, daw_set_loop(s, 1, 0, 256));
        CHECK_OK(s, daw_set_record_preroll(s, 10));
        start(s, 500, root / "beyond.mydawtake");
        CHECK(invocation.loopStart == 0 && invocation.loopEnd == 0);
        CHECK(transport(s).frame == 490);
        output = live->pump(std::vector<float>(32, 0.2f));
        equal(output.first, 0);
        CHECK(recording(s).frames == 22 && recording(s).loop_recording == 0 &&
              transport(s).frame == 522);
        stop(s, "Beyond backing");
        CHECK(snapshotOf(s).track_count == 3);
        CHECK_OK(s, daw_get_track(s, 2, &newTrack));
        CHECK_OK(s, daw_get_clip(s, newTrack.id, 0, &region));
        CHECK(region.start == 500 && region.length == 22);
        CHECK_OK(s, daw_set_loop(s, 0, 0, 0));

        // Normal armed take is ONE take, not a split with a zero loop span.
        CHECK_OK(s, daw_set_record_preroll(s, 0));
        CHECK_OK(s, daw_record_start_take(s, backing.id, 20, (root / "take.mydawtake").c_str()));
        live->pump(std::vector<float>(24, 0.15f));
        CHECK(!recording(s).loop_recording && recording(s).target_track_id == backing.id);
        stop(s, "Normal take");
        CHECK(trackById(s, backing.id).take_count == 2 && snapshotOf(s).track_count == 3);
        auto take = abi<daw_take>();
        CHECK_OK(s, daw_get_take(s, backing.id, 1, &take));
        CHECK(take.start == 20 && take.frames == 24 && std::string(take.name) == "Normal take");

        // Existing loop mode still splits exact passes, including final partial.
        CHECK_OK(s, daw_set_loop(s, 1, 32, 96));
        CHECK_OK(s, daw_set_record_preroll(s, 16));
        CHECK_OK(s, daw_record_start_take(s, backing.id, 32, (root / "loop.mydawtake").c_str()));
        CHECK(invocation.loopStart == 32 && invocation.loopEnd == 96);
        live->pump(std::vector<float>(112, 0.3f));
        info = recording(s);
        CHECK(info.loop_recording && info.frames == 96 && info.pass_count == 2);
        stop(s, "Cycle");
        CHECK(trackById(s, backing.id).take_count == 4);
        CHECK_OK(s, daw_get_take(s, backing.id, 2, &take));
        CHECK(take.start == 32 && take.frames == 64);
        CHECK_OK(s, daw_get_take(s, backing.id, 3, &take));
        CHECK(take.start == 32 && take.frames == 32);
        CHECK_OK(s, daw_set_loop(s, 0, 0, 0));

        // Stop in preroll: no invalid empty file, phantom track, revision or undo.
        CHECK_OK(s, daw_set_record_preroll(s, 100));
        const auto zeroRevision = rev(s);
        const auto zeroTracks = snapshotOf(s).track_count;
        const auto empty = root / "empty.mydawtake";
        start(s, 100, empty);
        live->pump(std::vector<float>(30, 0.5f));
        CHECK(recording(s).frames == 0);
        stop(s, "Empty");
        CHECK(rev(s) == zeroRevision && snapshotOf(s).track_count == zeroTracks &&
              !std::filesystem::exists(empty));
        CHECK_REJ(s, daw_record_stop(s, "Nothing", rev(s)));

        // Open failure is not converted into input-only success or a model edit.
        failStart = true;
        CHECK_REJ(s, daw_record_start(s, 0, (root / "failed.mydawtake").c_str()));
        failStart = false;
        CHECK(!live && !recording(s).recording && rev(s) == zeroRevision);
        CHECK(!std::filesystem::exists(root / "failed.mydawtake"));
        const auto factoryBefore = factories;
        CHECK_REJ(s, daw_record_start(s, 48000 * 600, (root / "limit").c_str()));
        CHECK_REJ(s, daw_record_start_take(s, 999999, 0, (root / "missing").c_str()));
        CHECK(factories == factoryBefore);

        // Disconnect/cancel preserve the confirmed raw prefix for the real
        // recovery command. Destruction has no active callback/writer left.
        CHECK_OK(s, daw_set_record_preroll(s, 0));
        const auto recovery = root / "recovery.mydawtake";
        start(s, 30, recovery);
        live->pump(std::vector<float>(64, 0.125f));
        failDevice = true;
        auto bad = abi<daw_recording>();
        CHECK_REJ(s, daw_get_recording(s, &bad));
        failDevice = false;
        CHECK_OK(s, daw_record_cancel(s));
        CHECK(!live && !recording(s).recording);
        CHECK(rev(s) == zeroRevision && std::filesystem::exists(recovery));
        auto confirmed = daw::recoverTake(recovery);
        CHECK(confirmed.startFrame == 30 && confirmed.clip->frames() == 64);
        equal(confirmed.clip->samples(), 0.125f);
        CHECK_OK(s, daw_recover_take(s, recovery.c_str(), "Recovered duplex", rev(s)));
        CHECK(!std::filesystem::exists(recovery));

        // Monitor hygiene: unsafe hardware samples never escape to output;
        // dry capture applies the identical existing sanitization policy.
        CHECK_OK(s, daw_set_record_monitor(s, 1));
        start(s, 600, root / "finite.mydawtake");
        output = live->pump({std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(), 20.0f, -20.0f});
        CHECK(output.first == std::vector<float>({0, 0, 16, -16}) && output.first == output.second);
        CHECK_OK(s, daw_record_cancel(s));
        CHECK(destroyed == factories && !live);

        // Metronome/backing are audible during empty-project preroll but never
        // copied to raw capture. New session also exercises the shared IO lease.
        {
            Bridge clickOwner;
            auto *clickSession = clickOwner.get();
            CHECK_OK(clickSession, daw_set_record_preroll(clickSession, 64));
            CHECK_OK(clickSession, daw_set_metronome(clickSession, 1));
            start(clickSession, 64, root / "click.mydawtake");
            output = live->pump(std::vector<float>(128, 0.0f));
            CHECK(std::any_of(output.first.begin(), output.first.end(), [](float x) {
                return x != 0;
            }));
            CHECK(recording(clickSession).frames == 64);
            stop(clickSession, "Dry with click");
            equal(finishedCapture->samples(), 0);
        }
        CHECK(destroyed == factories && !live);

        // Playback must still reject empty projects; only recording opts into
        // a silent finite horizon. Invalid horizons never enable an RT graph.
        daw::Renderer renderer;
        bool rejected = false;
        try {
            renderer.prepare(daw::State{});
        } catch (const daw::Error &) {
            rejected = true;
        }
        CHECK(rejected);
        for (const auto horizon : {uint64_t(10), uint64_t(48000 * 600 + 1)}) {
            rejected = false;
            try {
                renderer.prepare(daw::State{}, 10, 0, 0, horizon);
            } catch (const daw::Error &) {
                rejected = true;
            }
            CHECK(rejected);
        }
        std::cout
            << "PASS: normal full-duplex recording: real bridge/render/writer; empty/backing/preroll/MON,\n"
               "linear vs loop, exact captures, guards, Undo/Redo, Save/Open/WAV, recovery, and lifetime\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
