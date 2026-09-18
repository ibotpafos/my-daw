#include "audio/capture_clock.hpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace daw;
int main() {
    unsigned checks = 0;
    auto check = [&](bool value, const char* message) {
        ++checks; if (!value) throw std::runtime_error(message);
    };
    try {
        for (double origin : {-4096.0, 0.0, 0.125, 0x1p40}) {
            CaptureClock clock;
            check(clock.report().initialFlags == 0, "initial clock is unobserved");
            uint64_t elapsed = 0, host = 0;
            for (uint32_t count : {1u, 64u, 257u, 4096u, 3u}) {
                check(clock.accept({origin + static_cast<double>(elapsed), host, 3}, count), "variable slices stay continuous");
                elapsed += count; host += 100;
            }
            auto report = clock.report();
            check(report.firstSampleTime == origin && report.firstHostTime == 0 && report.initialFlags == 3,
                  "original anchors do not move; a valid host zero is not missing");
            check(report.validatedFrames == elapsed && report.fault == CaptureClockFault::none, "device frame count");
        }
        for (double unexpected : {0.0, 63.0, 65.0, 128.0, -64.0}) {
            CaptureClock clock;
            check(clock.accept({0, 100, 3}, 64), "first timestamp");
            check(!clock.accept({unexpected, 200, 3}, 64), "gaps/repeats fail");
            auto r = clock.report();
            check(r.fault == CaptureClockFault::sampleDiscontinuity && r.expectedSampleTime == 64 &&
                  r.observedSampleTime == unexpected && r.validatedFrames == 64, "failure captures first disagreement");
            check(!clock.accept({64, 201, 3}, 64) && clock.report().observedSampleTime == unexpected,
                  "failure is sticky, never silently resynchronizes");
        }
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), 0x1p51}) {
            CaptureClock clock;
            check(!clock.accept({invalid, 1, 3}, 64), "invalid sample time rejected before anchor");
            const auto r = clock.report();
            check(r.initialFlags == 0 && r.validatedFrames == 0 && r.fault == CaptureClockFault::invalidTimestamp &&
                  std::isfinite(r.observedSampleTime), "invalid diagnostic remains bounded/finite");
        }
        for (uint32_t flags : {0u, 2u, 7u, UINT32_MAX}) {
            CaptureClock clock; check(!clock.accept({0, 1, flags}, 64), "requires sample flag and known portable flags");
        }
        for (uint32_t frames : {0u, 4097u, UINT32_MAX}) {
            CaptureClock clock; check(!clock.accept({0, 1, 3}, frames), "invalid slice size rejected");
        }
        for (uint64_t host : {0u, 99u, 100u}) {
            CaptureClock clock; check(clock.accept({0, 100, 3}, 64), "host anchor");
            check(!clock.accept({64, host, 3}, 64) && clock.fault() == CaptureClockFault::hostDiscontinuity,
                  "host repeat/reversal stops capture");
        }
        {
            CaptureClock clock;
            check(clock.accept({0, 99, 1}, 64), "host is optional; ignore unflagged garbage");
            check(clock.accept({64, 1, 3}, 64), "host may become available");
            check(clock.accept({128, 0, 1}, 64), "sample clock survives temporarily absent host flag");
            check(clock.accept({192, 2, 3}, 64), "compare to last valid host");
            check(clock.report().initialFlags == 1 && clock.report().firstHostTime == 0, "no fabricated initial host anchor");
            check(!clock.accept({256, 0, 0}, 64), "sample validity must remain present");
        }
        {
            CaptureClock clock;
            check(clock.accept({0, 1, 3}, 64), "drift start");
            check(clock.accept({64.125, 2, 3}, 64), "allow subframe roundoff");
            check(!clock.accept({128.375, 3, 3}, 64), "tolerance does not accumulate across buffers");
        }
        // Exercise publication under contention. No assertion counter is shared.
        // This is an actual concurrent read test; ASan is not a TSan substitute.
        {
            CaptureClock clock;
            std::atomic<bool> done{false}, good{true};
            std::thread reader([&] {
                uint64_t previous = 0;
                while (!done.load(std::memory_order_acquire)) {
                    auto r = clock.report();
                    if (r.initialFlags && (r.firstSampleTime != -128 || r.firstHostTime != 10 || r.initialFlags != 3 ||
                        r.validatedFrames < previous)) good.store(false, std::memory_order_relaxed);
                    previous = r.validatedFrames;
                }
            });
            for (uint64_t i = 0; i < 10000; ++i) {
                if (!clock.accept({-128.0 + static_cast<double>(i * 64), 10 + i, 3}, 64)) good = false;
            }
            done.store(true, std::memory_order_release); reader.join();
            check(good.load() && clock.report().validatedFrames == 640000, "concurrent report publication");
        }
        std::cout << "capture clock: " << checks << " checks passed; includes concurrent snapshot stress\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
