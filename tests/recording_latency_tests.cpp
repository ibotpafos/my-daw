#include "audio/duplex_capture.hpp"
#include "audio/recording_channels.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <unistd.h>

using namespace daw;
namespace {
size_t checks = 0;
void check(bool value, const char* message) { ++checks; if (!value) throw Error(message); }
template<class F> void reject(F f) { bool rejected = false; try { f(); } catch (const Error&) { rejected = true; } check(rejected, "expected rejection"); }
RecordingLatency profile(uint32_t in = 17, uint32_t out = 43) {
    RecordingLatency p;
    p.enabled = true; p.deviceID = 1; p.bufferFrames = 256;
    p.inputDevice = in; p.outputDevice = out;
    p.inputStream = 3; p.outputStream = 5;
    p.inputSafety = 13; p.outputSafety = 29;
    p.inputStreamID = 3; p.outputLeftStreamID = p.outputRightStreamID = 4;
    p.hostTicksPerSecond = 48000;
    return p;
}
class ExactDelay final : public PreparedEffect {
    std::array<float, 97> left_{}, right_{};
    size_t offset_ = 0;
public:
    uint32_t latencyFrames() const noexcept override { return 97; }
    bool process(float* left, float* right, uint32_t frames, uint64_t,
                 std::span<const PreparedParameterEvent>, std::span<const PreparedMidiEvent>) noexcept override {
        for (uint32_t f = 0; f < frames; ++f) {
            std::swap(left[f], left_[offset_]); std::swap(right[f], right_[offset_]);
            offset_ = (offset_ + 1) % left_.size();
        }
        return true;
    }
};
float signal(int64_t frame) {
    return frame >= 0 && (frame % 997 == 0 || frame % 997 == 400) ? 0.375f : 0.0f;
}
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("daw-recording-latency-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        const auto p = profile(); p.validate();
        check(p.presentationFrames() == 68, "separate device/stream terms, no buffer/safety double count");
        for (int kind = 0; kind < 5; ++kind) {
            auto bad = p;
            if (kind == 0) bad.bufferFrames = 0;
            if (kind == 1) bad.hostTicksPerSecond = std::numeric_limits<double>::quiet_NaN();
            if (kind == 2) bad.inputDevice = UINT32_MAX;
            if (kind == 3) bad.outputLeftStreamID = 0;
            if (kind == 4) bad.inputSafety = 48001;
            reject([&] { bad.validate(); });
        }
        RecordingChannel channel;
        const uint32_t layout[]{2, 1, 4};
        check(locateRecordingChannel(layout, 5, channel) && channel == RecordingChannel{2, 2, 4}, "non-first interleaved channel routing");
        check(locateRecordingChannel(layout, 2, channel) && channel == RecordingChannel{1, 0, 1}, "planar channel routing");
        check(!locateRecordingChannel(layout, 7, channel), "missing channel rejected");
        const uint32_t invalid[]{2, 0};
        check(!locateRecordingChannel(invalid, 0, channel), "entire invalid layout rejected");
        uint32_t frames = 0;
        check(recordingBufferFrames(4 * 7 * 257, 7, frames) && frames == 257, "interleaved frame count");
        check(!recordingBufferFrames(5, 2, frames) && !recordingBufferFrames(4 * 4097, 1, frames), "misaligned/oversized buffer rejected");
        for (uint32_t block : {1u, 64u, 257u, 4096u})
        for (uint64_t start : {0ULL, 700ULL})
        for (uint64_t preroll : {0ULL, 333ULL, 900ULL})
        for (bool loop : {false, true}) {
            if (loop && !start) continue;
            constexpr uint64_t capacity = 2301, separation = 256;
            const auto lead = std::min(start, preroll), delay = separation + p.presentationFrames();
            Renderer renderer; State empty;
            const auto path = (root / "exact.mydawtake").string();
            DuplexCapture capture(renderer, empty, capacity, path, start, loop ? start : 0,
                loop ? start + 997 : 0, preroll, false, p);
            renderer.playing = true;
            check(!capture.timing().ready && !capture.timing().canFinish, "no fabricated startup latency success");
            uint64_t elapsed = 0;
            while (!capture.progress().complete) {
                std::vector<float> input(block), l(block), r(block);
                for (uint32_t f = 0; f < block; ++f)
                    input[f] = signal(int64_t(elapsed + f) - int64_t(lead + delay));
                capture.process(input.data(), l.data(), r.data(), block,
                    {double(elapsed) + separation - 999.25, elapsed + 100000 + separation, true, true},
                    {double(elapsed) - 999.25, elapsed + 100000, true, true});
                elapsed += block;
                check(capture.clockError() == CaptureClockError::none, "valid paired stamps accepted");
                check(elapsed <= lead + capacity + delay + block, "bounded drain");
            }
            const auto t = capture.timing();
            check(t.compensationFrames == delay && t.timestampSeparation == separation, "measured timing not fixed buffer heuristic");
            check(t.discardedLeadingFrames == lead + delay && t.canFinish, "leading interval and tail captured");
            check(capture.frames() == capacity && !capture.overflowed(), "exact requested length including tail");
            const auto clip = capture.finish();
            for (uint64_t f = 0; f < capacity; ++f)
                check(clip->samples()[f * 2] == signal(static_cast<int64_t>(f)), "impulses placed at requested frames independently of callback partition");
            check(capture.progress().timelineFrame == (loop ? start + capacity % 997 : start + capacity), "loop position uses compensated time");
            const auto recovery = recoverTake(path);
            check(recovery.startFrame == start && recovery.clip->samples() == clip->samples(), "recovery has same placement/PCM");
            capture.discard();
        }
        for (uint64_t lead : {0ULL, 100ULL, 700ULL}) {
            Renderer renderer; State empty;
            const auto path = (root / "manual.mydawtake").string();
            DuplexCapture capture(renderer, empty, 4000, path, 700, 0, 0, lead, true, p);
            renderer.playing = true;
            constexpr uint64_t separation = 256, delay = 324, cutoff = 384;
            uint64_t elapsed = 0;
            auto pump = [&] {
                float input[128], l[128], r[128];
                for (uint32_t f = 0; f < 128; ++f) input[f] = signal(int64_t(elapsed + f) - int64_t(lead + delay));
                capture.process(input, l, r, 128,
                    {double(elapsed + separation), elapsed + separation + 100000, true, true},
                    {double(elapsed), elapsed + 100000, true, true});
                elapsed += 128;
                if (elapsed > cutoff) check(std::all_of(l,l+128,[](float v){return v==0;}), "Stop silences backing and MON during drain");
            };
            while (elapsed < cutoff) pump();
            capture.requestStop(); capture.requestStop();
            check(!capture.timing().canFinish, "request does not prematurely finalize");
            reject([&] { (void)capture.finish(); }); // must not stop the still-needed writer
            while (!capture.timing().canFinish) { pump(); check(elapsed < cutoff + delay + 256, "manual drain bounded"); }
            const auto clip = capture.finish();
            if (lead >= cutoff) check(!clip && capture.frames() == 0, "Stop during preroll creates no clip");
            else {
                check(clip && clip->frames() == cutoff - lead, "manual Stop keeps audible interval not delayed cursor");
                for (uint64_t f = 0; f < clip->frames(); ++f) check(clip->samples()[2*f] == signal(static_cast<int64_t>(f)), "manual tail impulse preserved");
            }
            capture.discard();
        }
        // Independent wire-loop simulation: input is the ACTUAL earlier
        // renderer output, not synthesized from the placement algorithm.
        for (bool manual : {false, true}) for (bool click : {false, true}) {
            constexpr uint64_t hardwareDelay = 700, separation = 632, graphDelay = 97, block = 64;
            constexpr uint64_t capacity = 1501, cutoff = 1024;
            Session model;
            std::vector<float> source(4000, 0);
            for (uint64_t f = 0; f < 2000; ++f) source[2*f] = source[2*f+1] = signal(static_cast<int64_t>(f));
            model.import("Wire impulse", std::make_shared<Clip>(source), 0);
            model.addMasterInsert({0, 0x61756678, 0x64656c79, 0x6170706c, "Test delay", false, 97, {}, {}}, model.state().revision);
            Renderer renderer;
            renderer.insertFactoryForTest = [](const PluginInsert&) { return std::make_unique<ExactDelay>(); };
            renderer.setMetronome(click);
            const auto path = (root / "wire.mydawtake").string();
            DuplexCapture capture(renderer, model.state(), capacity, path, 0, 0, 0, 0, false, p);
            renderer.playing = true;
            std::vector<float> history;
            uint64_t elapsed = 0;
            while (!(manual ? capture.timing().canFinish : capture.progress().complete)) {
                if (manual && elapsed == cutoff) capture.requestStop();
                float input[block]{}, l[block]{}, r[block]{};
                for (uint64_t f = 0; f < block; ++f)
                    if (elapsed + f >= hardwareDelay) input[f] = history.at(elapsed + f - hardwareDelay);
                capture.process(input,l,r,block,
                    {double(elapsed + separation),elapsed + separation + 100000,true,true},
                    {double(elapsed),elapsed + 100000,true,true});
                history.insert(history.end(),l,l+block); elapsed += block;
                check(capture.clockError() == CaptureClockError::none && elapsed < 4096, "wire loop stable/bounded");
            }
            const auto clip = capture.finish();
            check(capture.timing().graphFrames == graphDelay && capture.timing().compensationFrames == hardwareDelay + graphDelay, "graph delay separate from hardware");
            const auto expected = manual ? cutoff - graphDelay : capacity;
            check(clip->frames() == expected, "wire loop retains precisely audible duration");
            for (uint64_t f = 0; f < expected; ++f)
                check(clip->samples()[2*f] == history.at(f + graphDelay), "wire loop PCM returns to graph input timeline");
            check(std::all_of(history.begin(),history.begin()+graphDelay,[](float v){return v==0;}), "click also waits for graph startup latency");
            check(std::all_of(history.begin()+(manual ? cutoff : capacity + graphDelay),history.end(),[](float v){return v==0;}), "no output after audible end");
            capture.discard();
        }
        // Click itself is partition-invariant and delayed only once by G.
        {
            State empty;
            Session delayModel; delayModel.addMasterInsert({0,0x61756678,0x64656c79,0x6170706c,"Delay",false,97,{},{}},0);
            Renderer reference, delayed;
            reference.setMetronome(true); delayed.setMetronome(true);
            delayed.insertFactoryForTest = [](const PluginInsert&) { return std::make_unique<ExactDelay>(); };
            reference.prepare(empty,0,0,0,333); reference.playing = true;
            float expected[333]{}, other[333]{}; reference.render(expected,other,333);
            const auto path=(root/"click.mydawtake").string();
            DuplexCapture capture(delayed,delayModel.state(),333,path,0,0,0,0,false,p);
            delayed.playing=true;
            std::vector<float> actual;
            for (uint64_t f=0; !capture.progress().complete; f+=64) {
                float input[64]{},l[64]{},r[64]{};
                capture.process(input,l,r,64,{double(f+256),100000+f+256,true,true},{double(f),100000+f,true,true});
                actual.insert(actual.end(),l,l+64);
            }
            for (size_t i=0;i<333;++i) check(actual.at(97+i)==expected[i],"click matches zero-latency musical grid including tail");
            (void)capture.finish(); capture.discard();
        }
        // Never continue after a missing input flag or changed timestamp pair.
        for (int fault = 0; fault < 3; ++fault) {
            Renderer renderer; State empty; const auto path = (root / "fault.mydawtake").string();
            DuplexCapture capture(renderer, empty, 4000, path, 0, 0, 0, 0, false, p);
            renderer.playing = true;
            float input[512], l[512], r[512]; std::fill_n(input,512,0.25f);
            capture.process(input,l,r,512,{256,100256,true,true},{0,100000,true,true});
            auto stamp = CaptureTimestamp{512,100512,true,true};
            if (fault == 0) stamp.sampleTimeValid = false;
            if (fault == 1) stamp.sampleTime += 1;
            if (fault == 2) stamp.hostTime += 10;
            capture.process(input,l,r,512,{768,100768,true,true},stamp);
            check(capture.clockError() != CaptureClockError::none && !renderer.playing, "invalid actual input timing latches failure");
            check(capture.frames() == 188, "fault does not append unaligned PCM");
            reject([&] { (void)capture.finish(); });
            check(recoverTake(path).clip->frames() == 188, "failure retains confirmed aligned prefix");
            capture.discard();
        }
        std::filesystem::remove_all(root);
        std::cout << "recording latency: " << checks << " assertions passed (synthetic paired clocks; real writer/renderer)\n";
    } catch (const std::exception& e) { std::filesystem::remove_all(root); std::cerr << e.what() << '\n'; return 1; }
}
