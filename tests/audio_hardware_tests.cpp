#include "audio/device_settings.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool value) {
    ++checks;
    if (!value)
        throw std::runtime_error("Hardware format assertion " + std::to_string(checks));
}
template <class F> void rejects(F fn) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected);
}
struct Fixture {
    daw::AudioDeviceCapabilities caps;
    int reads = 0, rates = 0, buffers = 0;
    bool failRead = false, failRate = false, failBuffer = false;
    bool immediateRate = true, immediateBuffer = true;
    bool rateNotified = true, bufferNotified = true;
    Fixture() {
        caps.device = {17, "fixture-uid", "Studio interface", 2, 2, 128, 44100, true, true};
        caps.sampleRates = {{44100, 44100}, {48000, 96000}};
        caps.bufferRange = {32, 2048};
        caps.rateWritable = caps.bufferWritable = true;
    }
};
class Control final : public daw::AudioDeviceControl {
    Fixture &f;

  public:
    explicit Control(Fixture &value) : f(value) {}
    daw::AudioDeviceCapabilities inspect() override {
        ++f.reads;
        if (f.failRead)
            throw std::runtime_error("device disconnected");
        return f.caps;
    }
    bool sampleRateAcknowledged() const override {
        return f.rateNotified;
    }
    bool bufferAcknowledged() const override {
        return f.bufferNotified;
    }
    void setSampleRate(double rate) override {
        ++f.rates;
        if (f.failRate)
            throw std::runtime_error("rate rejected");
        if (f.immediateRate)
            f.caps.device.sampleRate = rate;
    }
    void setBufferFrames(uint32_t frames) override {
        ++f.buffers;
        if (f.failBuffer)
            throw std::runtime_error("buffer rejected");
        if (f.immediateBuffer)
            f.caps.device.bufferFrames = frames;
    }
};
using Change = daw::AudioDeviceChange;
using State = daw::AudioDeviceChangeState;
const auto start = Change::Clock::time_point{};
Change request(Fixture &f, uint32_t frames = 256, double rate = 48000) {
    return Change(std::make_unique<Control>(f), rate, frames, start);
}
void failed(const Change &c) {
    check(c.state == State::Failed && !c.error.empty());
}
}
int main() {
    try {
        {
            Fixture f;
            auto c = request(f);
            check(c.pending() && f.rates == 0 && f.buffers == 0);
            c.poll(start);
            check(c.pending() && f.rates == 1 && f.buffers == 0 && !c.actualKnown);
            c.poll(start);
            check(c.pending() && f.buffers == 1);
            c.poll(start);
            check(c.state == State::Applied && c.actual.sampleRate == 48000 &&
                  c.actual.bufferFrames == 256 && c.actualKnown);
            const auto reads = f.reads;
            c.poll(start + std::chrono::seconds(5));
            check(c.state == State::Applied && f.reads == reads);
        }
        {
            Fixture f;
            f.immediateRate = f.immediateBuffer = false;
            auto c = request(f);
            c.poll(start);
            for (int i = 1; i < 10; ++i) {
                c.poll(start + std::chrono::milliseconds(10 * i));
                check(c.pending() && f.buffers == 0 && f.rates == 1);
            }
            f.caps.device.sampleRate = 48000;
            c.poll(start + std::chrono::milliseconds(100));
            check(f.buffers == 1 && c.pending());
            c.poll(start + std::chrono::milliseconds(200));
            check(f.buffers == 1 && c.pending());
            f.caps.device.bufferFrames = 256;
            c.poll(start + std::chrono::milliseconds(300));
            check(c.state == State::Applied);
        }
        {
            Fixture f;
            f.caps.device.sampleRate = 48000;
            f.caps.rateWritable = f.caps.bufferWritable = false;
            f.caps.sampleRates.clear();
            auto c = request(f, 128);
            check(c.state == State::Applied && f.rates == 0 && f.buffers == 0);
            rejects([&] {
                request(f, 256);
            });
        }
        for (double rate :
             {0.0, 44100.0, 96000.0, std::numeric_limits<double>::infinity(), std::nan("")}) {
            Fixture f;
            rejects([&] {
                request(f, 256, rate);
            });
            check(f.rates == 0 && f.buffers == 0);
        }
        for (uint32_t frames : {0u, 31u, 2049u, 4097u, UINT32_MAX}) {
            Fixture f;
            rejects([&] {
                request(f, frames);
            });
            check(f.rates == 0 && f.buffers == 0);
        }
        for (const auto &uid : {std::string(), std::string(481, 'x'), std::string("a\0b", 3)})
            rejects([&] {
                daw::validateAudioDeviceUID(uid);
            });
        for (int bad = 0; bad < 11; ++bad) {
            Fixture f;
            switch (bad) {
            case 0:
                f.caps.device.id = 0;
                break;
            case 1:
                f.caps.device.uid.clear();
                break;
            case 2:
                f.caps.device.sampleRate = std::nan("");
                break;
            case 3:
                f.caps.device.bufferFrames = 0;
                break;
            case 4:
                f.caps.bufferRange = {128, 32};
                break;
            case 5:
                f.caps.bufferRange = {0, 1024};
                break;
            case 6:
                f.caps.sampleRates = {{48000, std::nan("")}};
                break;
            case 7:
                f.caps.sampleRates.resize(65, {48000, 48000});
                break;
            case 8:
                f.caps.rateWritable = false;
                break;
            case 9:
                f.caps.sampleRates = {{44100, 44100}};
                break;
            case 10:
                f.caps.running = true;
                break;
            }
            rejects([&] {
                request(f);
            });
            check(f.rates == 0 && f.buffers == 0);
        }
        {
            Fixture f;
            auto c = request(f);
            c.poll(start + Change::timeout);
            failed(c);
            check(!c.mayHaveChanged && f.rates == 0 && f.buffers == 0);
        }
        {
            Fixture f;
            f.immediateRate = false;
            auto c = request(f);
            c.poll(start);
            c.poll(start + Change::timeout);
            failed(c);
            check(c.mayHaveChanged && f.buffers == 0 && c.actualKnown);
            f.caps.device.sampleRate = 48000;
            c.poll(start + Change::timeout);
            failed(c);
            check(f.rates == 1 && f.buffers == 0); // late acknowledgement cannot reverse a timeout
        }
        {
            Fixture f;
            f.immediateBuffer = false;
            auto c = request(f);
            c.poll(start);
            c.poll(start);
            c.poll(start + Change::timeout);
            failed(c);
            check(f.buffers == 1 && c.actual.bufferFrames == 128);
        }
        for (int phase = 0; phase < 3; ++phase) {
            Fixture f;
            auto c = request(f);
            for (int i = 0; i < phase; ++i)
                c.poll(start);
            f.failRead = true;
            c.poll(start);
            failed(c);
            check(!c.actualKnown);
        }
        for (int kind = 0; kind < 4; ++kind) {
            Fixture f;
            auto c = request(f);
            if (kind == 0)
                f.caps.device.id = 18;
            if (kind == 1)
                f.caps.device.uid = "replacement";
            if (kind == 2)
                f.caps.running = true;
            if (kind == 3)
                f.caps.bufferRange = {512, 2048};
            c.poll(start);
            failed(c);
            check(f.rates == 0 && f.buffers == 0);
        }
        {
            Fixture f;
            f.failRate = true;
            auto c = request(f);
            c.poll(start);
            failed(c);
            check(c.mayHaveChanged && !c.actualKnown && f.buffers == 0);
        }
        {
            Fixture f;
            f.failBuffer = true;
            auto c = request(f);
            c.poll(start);
            c.poll(start);
            failed(c);
            check(c.mayHaveChanged && !c.actualKnown && f.rates == 1 && f.buffers == 1);
        }
        {
            Fixture f;
            auto c = request(f);
            c.poll(start);
            f.caps.bufferRange = {512, 2048};
            c.poll(start);
            failed(c);
            check(f.buffers == 0); // rate-dependent range must be re-read
        }
        {
            Fixture f;
            auto c = request(f);
            c.poll(start);
            c.poll(start);
            f.caps.device.sampleRate = 44100;
            c.poll(start);
            failed(c);
            check(f.rates == 1 && f.buffers == 1);
        }
        {
            Fixture f;
            f.rateNotified = f.bufferNotified = false;
            auto c = request(f);
            c.poll(start);
            c.poll(start);
            check(c.pending() && f.caps.device.sampleRate == 48000 && f.buffers == 0);
            f.rateNotified = true;
            c.poll(start);
            c.poll(start);
            check(c.pending() && f.caps.device.bufferFrames == 256 && f.buffers == 1);
            f.bufferNotified = true;
            c.poll(start);
            check(c.state == State::Applied);
        }
        {
            Fixture f;
            f.rateNotified = false;
            auto c = request(f);
            c.poll(start);
            c.poll(start + Change::timeout);
            failed(c);
            check(f.buffers == 0); // readback alone is not HAL acknowledgement
        }
        {
            Fixture f;
            f.bufferNotified = false;
            auto c = request(f);
            c.poll(start);
            c.poll(start);
            c.poll(start + Change::timeout);
            failed(c);
            check(c.actual.bufferFrames == 256); // target visible but no notification
        }
        // Exhaust supported frame boundaries, not only common powers of two.
        for (uint32_t frames : {32u, 33u, 127u, 255u, 1023u, 2048u}) {
            Fixture f;
            f.caps.device.sampleRate = 48000;
            auto c = request(f, frames);
            c.poll(start);
            c.poll(start);
            check(c.state == State::Applied && c.actual.bufferFrames == frames && f.rates == 0);
        }
        std::cout << "PASS: audio hardware acknowledgement: " << checks << " checks\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
