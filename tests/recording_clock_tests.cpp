#include "audio/recording_clock.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace daw;
int main() {
    size_t checks = 0;
    auto check = [&](bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); };
    auto stamp = [](double sample, uint64_t host = 0, bool validHost = false) {
        return CaptureTimestamp{sample, host, true, validHost};
    };
    try {
        for (double anchor : {-8192.25, -1.0, 0.0, 900000000000.5}) {
            CaptureClock clock;
            uint64_t offset = 0;
            for (uint32_t n = 0; n < 1000; ++n) {
                const uint32_t frames = std::array{1u, 64u, 257u, 4096u}[n % 4];
                check(clock.observe(stamp(anchor + double(offset), offset + 1, true), frames) == CaptureClockError::none,
                    "variable block sizes retain absolute sample anchor");
                offset += frames;
            }
        }
        for (double delta : {-4096.0, -64.0, -1.0, 1.0, 64.0, 4096.0}) {
            CaptureClock clock;
            check(clock.observe(stamp(700), 64) == CaptureClockError::none, "initial timestamp accepted");
            check(clock.observe(stamp(764 + delta), 64) == CaptureClockError::discontinuity, "gap/overlap rejected");
            check(clock.observe(stamp(764), 64) == CaptureClockError::discontinuity, "valid data cannot reset failed take");
        }
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                CaptureClock::maximumSampleTime, -CaptureClock::maximumSampleTime}) {
            CaptureClock clock;
            check(clock.observe(stamp(invalid), 1) == CaptureClockError::sampleTimeInvalid, "invalid numeric representation rejected");
        }
        {
            CaptureClock clock;
            auto missing = CaptureTimestamp{std::numeric_limits<double>::quiet_NaN(), 10, false, true};
            check(clock.observe(missing, 64) == CaptureClockError::sampleTimeUnavailable, "host time alone cannot prove sample continuity");
        }
        {
            CaptureClock clock;
            check(clock.observe(stamp(0), 64) == CaptureClockError::none, "sample-only driver supported");
            check(clock.observe({0, 0, false, false}, 64) == CaptureClockError::sampleTimeUnavailable, "sample validity loss rejected");
        }
        {
            CaptureClock clock;
            check(clock.observe({}, 0) == CaptureClockError::none, "empty callback cannot anchor/poison clock");
            check(clock.observe(stamp(-5.5, 100, true), 64) == CaptureClockError::none, "fractional negative anchor valid");
            check(clock.observe({std::numeric_limits<double>::quiet_NaN(), 0, false, true}, 0) == CaptureClockError::none,
                "empty callback cannot change host baseline");
            check(clock.observe(stamp(58.5, 101, true), 1) == CaptureClockError::none, "empty callback cannot advance expected frame");
        }
        for (uint64_t host : {0ULL, 99ULL, 100ULL}) {
            CaptureClock clock;
            check(clock.observe(stamp(0, 100, true), 64) == CaptureClockError::none, "first host timestamp accepted");
            check(clock.observe(stamp(64, host, true), 64) == CaptureClockError::hostTimeReversed, "host reversal/repetition rejected");
        }
        {
            CaptureClock clock;
            check(clock.observe(stamp(0, UINT64_MAX, false), 64) == CaptureClockError::none, "unflagged host bytes ignored");
            check(clock.observe(stamp(64, 100, true), 64) == CaptureClockError::none, "late valid host anchors");
            check(clock.observe(stamp(128, 0, false), 64) == CaptureClockError::none, "missing optional host uses sample time");
            check(clock.observe(stamp(192, 101, true), 64) == CaptureClockError::none, "host comparison resumes at last valid host");
        }
        {
            CaptureClock clock;
            check(clock.observe(stamp(0, 100, true), 64) == CaptureClockError::none, "host anchor");
            check(clock.observe(stamp(64, 0, false), 64) == CaptureClockError::none, "optional host absent");
            check(clock.observe(stamp(128, 99, true), 64) == CaptureClockError::hostTimeReversed, "missing host cannot erase prior baseline");
        }
        {
            CaptureClock clock;
            check(clock.observe(stamp(0), 64) == CaptureClockError::none, "rounding anchor");
            check(clock.observe(stamp(64.0004), 64) == CaptureClockError::none, "small rounding error tolerated");
            check(clock.observe(stamp(128.0008), 64) == CaptureClockError::none, "tolerance still relative to anchor");
            check(clock.observe(stamp(192.0012), 64) == CaptureClockError::discontinuity, "small errors cannot accumulate");
        }
        std::cout << "recording clock: " << checks << " checks passed (4000 block observations; not hardware tests)\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
