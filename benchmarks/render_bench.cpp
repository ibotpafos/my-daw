// Real-time budget benchmark for the offline renderer (goal: 'стабильно и быстро').
// Synthetic session (tracks x shared clip + volume automation + sends); measures
// sustained render wall-time and prints real-time factor (RTF):
//   RTF = audio_seconds / wall_seconds. Same graph shape the Core Audio callback
// runs per block, so RTF >> 1 is the RT-budget proxy. NOT a CTest (CI stays
// deterministic); run via scripts/bench-render.sh and record the table in the
// release notes of the slice that changed the render path.
#include "audio/clip.hpp"
#include "audio/renderer.hpp"
#include "domain/session.hpp"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static std::shared_ptr<const daw::Clip> makeClip(uint64_t frames) {
    std::vector<float> s(frames * 2);
    for (uint64_t f = 0; f < frames; ++f) {
        const auto v = 0.25f * std::sin(2.0f * 3.14159265f * 220.0f * (float)f / 48000.0f);
        s[f * 2] = v; s[f * 2 + 1] = v;
    }
    return std::make_shared<const daw::Clip>(std::move(s));
}

int main() {
    const uint64_t clipFrames = 48000 * 10;          // 10 s source
    auto clip = makeClip(clipFrames);
    struct Case { const char* label; uint32_t tracks; bool automation; bool sends; };
    // The domain prototype caps sessions at 8 audio tracks (validate in
    // engine/domain/session.cpp); raising that cap is itself a future arc —
    // until then the ladder probes the ceiling plus feature dimensions.
    const Case cases[] = {
        {"1 track", 1, false, false},
        {"4 tracks", 4, false, false},
        {"8 tracks", 8, false, false},
        {"8 tracks + automation", 8, true, false},
        {"8 tracks + automation + sends", 8, true, true},
    };
    for (const auto& c : cases) {
        daw::Session session;
        uint64_t rev = 0;
        uint64_t busId = 0;
        if (c.sends) { session.addBus("Bus 1", rev); busId = session.state().buses.back().id; ++rev; }
        std::vector<uint64_t> ids;
        for (uint32_t t = 0; t < c.tracks; ++t) { session.import("T" + std::to_string(t), clip, rev); ++rev; ids.push_back(session.state().tracks.back().id); }
        if (c.automation)
            for (const auto id : ids) {
                session.upsertTrackVolumeAutomation(id, 0, -6.0, rev); ++rev;
                session.upsertTrackVolumeAutomation(id, 48000 * 5, 0.0, rev); ++rev;
            }
        if (c.sends)
            for (const auto id : ids) { session.upsertSend(id, busId, -12.0, false, rev); ++rev; }
        daw::Renderer renderer;
        renderer.prepare(session.state());
        renderer.playing = true;
        std::vector<float> l(512), r(512);
        const uint64_t blocks = (48000 / 512) * 5;   // render ~5 s of media
        const auto begin = std::chrono::steady_clock::now();
        for (uint64_t b = 0; b < blocks; ++b) renderer.render(l.data(), r.data(), 512);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        const double audioSec = (double)(blocks * 512) / 48000.0;
        std::printf("%-34s blocks=%llu wall=%.1f ms  RTF=%.1fx  per-block avg=%.1f us (512-frame budget ~10666 us)\n",
                    c.label, (unsigned long long)blocks, us / 1000.0, audioSec / (us / 1e6),
                    (double)us / (double)blocks);
    }
    return 0;
}