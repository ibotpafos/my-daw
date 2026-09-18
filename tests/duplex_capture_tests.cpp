#include "audio/duplex_capture.hpp"
#include "audio/hardware_settings.hpp"
#include "audio/duplex.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <unistd.h>

using namespace daw;
int main() {
    size_t checks = 0;
    auto check = [&](bool value, const char* message) {
        ++checks; if (!value) throw std::runtime_error(message);
    };
    auto reject = [&](auto fn) {
        bool rejected = false; try { fn(); } catch (const Error&) { rejected = true; }
        check(rejected, "expected invalid range rejection");
    };
    auto root = std::filesystem::temp_directory_path() / ("daw-duplex-core-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    try {
        State empty;
        Renderer ordinary;
        reject([&] { ordinary.prepare(empty); }); // normal playback/export contract unchanged
        reject([&] { ordinary.prepare(empty, 0, 0, 0, 48000ULL * 600 + 1); });
        const std::vector<float> backing(4096 * 2, 0.125f);
        Session model;
        model.importAt("Backing", std::make_shared<Clip>(backing), 0, model.state().revision);
        // Same production callback body across awkward block edges. Check raw
        // recording exactly, including when the click and monitored mix sound.
        std::vector<float> referenceOutput, referenceRaw;
        for (bool loop : {false, true}) for (bool withBacking : {false, true}) {
            referenceOutput.clear(); referenceRaw.clear();
            for (uint32_t block : {64u, 257u, 4096u}) {
                constexpr uint64_t start = 137, lead = 137, capacity = 4099;
                const auto path = (root / "partition.mydawtake").string();
                Renderer render;
                render.setMetronome(true);
                DuplexCapture capture(render, withBacking ? model.state() : empty, capacity, path,
                    start, loop ? start : 0, loop ? start + 777 : 0, lead, true);
                render.playing = true;
                std::vector<float> input(lead + capacity);
                for (size_t f = 0; f < input.size(); ++f) input[f] = float(int(f % 31) - 15) / 64.0f;
                std::vector<float> output;
                for (uint32_t offset = 0; offset < input.size();) {
                    const auto count = std::min<uint32_t>(block, static_cast<uint32_t>(input.size()) - offset);
                    std::vector<float> l(count), r(count);
                    capture.process(input.data() + offset, l.data(), r.data(), count, {double(offset) - 2048.25, offset + 1, true, true});
                    check(l == r, "mono mix channels equal");
                    check(std::all_of(l.begin(), l.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 1; }), "bounded finite monitor output");
                    output.insert(output.end(), l.begin(), l.end());
                    offset += count;
                    const auto p = capture.progress();
                    check(p.prerollRemaining == (offset < lead ? lead - offset : 0), "sample-exact pre-roll progress");
                }
                check(capture.frames() == capacity && !capture.overflowed(), "exact cap without overflow");
                check(capture.progress().complete && !render.playing, "cap stops backing");
                check(capture.progress().timelineFrame == (loop ? start + capacity % 777 : start + capacity), "capture clock");
                std::vector<float> zeroL(17, 99), zeroR(17, 99);
                capture.process(input.data(), zeroL.data(), zeroR.data(), 17, {});
                check(zeroL == std::vector<float>(17, 0) && zeroR == zeroL && capture.frames() == capacity, "late callback after cap is silent");
                auto raw = capture.finish();
                check(raw && raw->frames() == capacity, "writer completed exact frames");
                for (size_t f = 0; f < capacity; ++f)
                    check(raw->samples()[f * 2] == input[f + lead] && raw->samples()[f * 2 + 1] == input[f + lead], "dry PCM has no backing, click or MON");
                if (referenceOutput.empty()) { referenceOutput = output; referenceRaw = raw->samples(); }
                else { check(referenceOutput == output, "callback partition invariant output"); check(referenceRaw == raw->samples(), "callback partition invariant raw audio"); }
                capture.discard();
                check(!std::filesystem::exists(path), "recovery discarded only after success");
            }
        }
        {
            Renderer render;
            const auto path = (root / "safety.mydawtake").string();
            DuplexCapture capture(render, empty, 1024, path, 0, 0, 0, 0, false);
            render.playing = true;
            std::vector<float> input(512, 0.25f), l(512), r(512);
            capture.process(input.data(), l.data(), r.data(), 256, {0, 1, true, true});
            check(std::all_of(l.begin(), l.begin() + 256, [](float v) { return v == 0; }), "MON off suppresses input only");
            capture.setMonitor(true);
            capture.process(input.data(), l.data(), r.data(), 256, {256, 2, true, true});
            check(l[0] > 0 && l[0] < 0.002 && l[255] == 0.25f, "MON ramps to unity");
            input[0] = std::numeric_limits<float>::quiet_NaN();
            input[1] = std::numeric_limits<float>::infinity(); input[2] = -32; input[3] = 32;
            capture.process(input.data(), l.data(), r.data(), 4, {512, 3, true, true});
            check(l[0] == 0 && l[1] == 0 && l[2] == -1 && l[3] == 1 && r == l, "invalid/extreme monitor input sanitized");
            check(render.clipped.load() == 2 && render.peak.load() >= 16, "direct monitor clipping reported");
            capture.setMonitor(false);
            std::fill(input.begin(), input.end(), 0.25f);
            capture.process(input.data(), l.data(), r.data(), 256, {516, 4, true, true});
            check(l[0] < 0.25f && l[0] > 0.24f && l[255] == 0, "MON ramps out while capturing");
            auto raw = capture.finish();
            check(raw->frames() == 772, "live MON never skips recording");
            check(raw->samples()[512 * 2] == 0 && raw->samples()[513 * 2] == 0 &&
                  raw->samples()[514 * 2] == -16 && raw->samples()[515 * 2] == 16, "writer preserves existing finite sample policy");
            capture.discard();
        }
        {
            Renderer render;
            const auto path = (root / "preroll-only.mydawtake").string();
            DuplexCapture capture(render, empty, 100, path, 100, 0, 0, 1000, false);
            check(capture.progress().prerollRemaining == 100, "pre-roll clamps at project zero");
            render.playing = true;
            float in[32]{}, l[32]{}, r[32]{};
            capture.process(in, l, r, 32, {-32, 0, true, false});
            check(!capture.finish(), "pre-roll-only finish yields no audio");
            capture.discard();
            check(!std::filesystem::exists(path), "no empty recovery after pre-roll stop");
        }
        // Fault BEFORE disk acceptance, frozen cursor, latched silence and
        // recovery prefix. The same policy also applies across pre-roll/loops.
        for (uint64_t lead : {0ULL, 32ULL, 1000ULL}) for (bool loop : {false, true}) {
            Renderer render;
            const auto path = (root / "clock-failure.mydawtake").string();
            DuplexCapture capture(render, model.state(), 4096, path, 1000,
                loop ? 1000 : 0, loop ? 1128 : 0, lead, true);
            render.playing = true;
            float input[64], left[64], right[64]; std::fill_n(input, 64, 0.25f);
            capture.process(input, left, right, 64, {-100.25, 10, true, true});
            const auto before = capture.progress(); const auto accepted = capture.frames();
            capture.process(input, left, right, 64, {-35.25, 11, true, true}); // one missing sample
            check(capture.clockError() == CaptureClockError::discontinuity, "sample gap latched");
            check(!render.playing && capture.frames() == accepted, "fault accepts no audio");
            check(capture.progress().timelineFrame == before.timelineFrame && !capture.progress().complete, "no fabricated cursor advance/cap");
            check(std::all_of(left, left+64, [](float v){return v==0;}) && std::equal(left,left+64,right), "fault silences backing and MON");
            capture.process(input, left, right, 64, {-36.25, 12, true, true});
            check(capture.frames() == accepted && std::all_of(left,left+64,[](float v){return v==0;}), "later callbacks cannot resume failed take");
            reject([&]{ (void)capture.finish(); }); // never auto-commit a failed take
            if (accepted) {
                const auto recovered = recoverTake(path);
                check(recovered.startFrame == 1000 && recovered.clip->frames() == accepted, "recovery retains only contiguous prefix");
                check(std::all_of(recovered.clip->samples().begin(), recovered.clip->samples().end(), [](float v){return v==0.25f;}), "dry recovery PCM unchanged");
            } else reject([&]{ (void)recoverTake(path); });
            capture.discard();
        }
        // A partial final capture must still validate the WHOLE hardware block.
        // Otherwise HAL may reject its timestamp while capture accepts stale
        // input scratch data for the shorter remaining prefix.
        for (const double anchor : {CaptureClock::maximumSampleTime - 128,
                                    -CaptureClock::maximumSampleTime + 64}) {
            Renderer render;
            const auto path = (root / "clock-partial-cap.mydawtake").string();
            DuplexCapture capture(render, empty, 65, path, 0, 0, 0, 0, true);
            render.playing = true;
            float input[256], left[256], right[256]; std::fill_n(input, 256, 0.25f);
            capture.process(input, left, right, 64, {anchor, 1, true, true});
            check(capture.frames() == 64, "valid near-limit prefix recorded");
            capture.process(input, left, right, 256, {anchor + 64, 2, true, true});
            check(capture.clockError() == CaptureClockError::sampleTimeInvalid, "full callback bounds apply to partial final cap");
            check(capture.frames() == 64 && !capture.progress().complete, "bad partial final callback is not accepted as success");
            check(std::all_of(left, left+256, [](float v){return v==0;}) && std::equal(left,left+256,right), "partial-cap fault silences entire output");
            reject([&]{ (void)capture.finish(); });
            check(recoverTake(path).clip->frames() == 64, "partial-cap fault preserves valid prefix");
            capture.discard();
        }
        {
            Renderer render;
            const auto path = (root / "clock-valid-partial-cap.mydawtake").string();
            DuplexCapture capture(render, empty, 65, path, 0, 0, 0, 0, false);
            render.playing = true;
            float input[256]{}, left[256]{}, right[256]{};
            capture.process(input, left, right, 64, {-64, 1, true, true});
            capture.process(input, left, right, 256, {0, 2, true, true});
            check(capture.frames() == 65 && capture.progress().complete, "valid oversized final block records only remaining prefix");
            check(capture.clockError() == CaptureClockError::none, "valid partial cap has no clock error");
            check(capture.finish()->frames() == 65, "valid partial cap is committable");
            capture.discard();
        }
        {
            const auto path = (root / "bad.mydawtake").string(); Renderer render;
            for (uint64_t capacity : {uint64_t(0), uint64_t(48000 * 60 + 1), UINT64_MAX})
                reject([&] { DuplexCapture bad(render, empty, capacity, path, 0, 0, 0, 0, false); });
            reject([&] { DuplexCapture bad(render, empty, 1, path, 48000 * 600, 0, 0, 0, false); });
            reject([&] { DuplexCapture bad(render, empty, 2, path, 48000 * 600 - 1, 0, 0, 0, false); });
            reject([&] { DuplexCapture bad(render, empty, 1, path, 0, 0, 0, 48000 * 30 + 1, false); });
            reject([&] { DuplexCapture bad(render, empty, 1, path, 0, 1, 8, 0, false); });
            check(!std::filesystem::exists(path), "invalid preparation does not create recovery file");
        }
        std::filesystem::remove_all(root);
        std::cout << "duplex capture: " << checks << " checks passed (real renderer and disk writer; no physical hardware)\n";
    } catch (const std::exception& e) {
        std::filesystem::remove_all(root); std::cerr << e.what() << '\n'; return 1;
    }
}
