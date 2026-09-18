// No real device is written: only invalid requests and an explicitly nonexistent
// UID/ID. Successful HAL transactions are covered by the deterministic driver
// tests; physical-device acceptance remains a separate gate.
#include "e2e.hpp"
#include "audio/hardware_settings.hpp"
#include <cstring>
#include <limits>

int main() {
    try {
        using namespace e2e;
        Bridge session;
        auto expected = abi<daw_audio_hardware_settings>();
        expected.version = DAW_AUDIO_HARDWARE_VERSION;
        expected.device_id = UINT32_MAX;
        std::strcpy(expected.uid, "mydaw-ci-nonexistent-hardware-f6f54052");
        expected.sample_rate = 44100; expected.buffer_frames = 256;
        expected.minimum_buffer = 32; expected.maximum_buffer = 4096;
        expected.supports_48k = expected.rate_writable = expected.buffer_writable = 1;
        const auto original = expected;
        const auto before = dumpOf(session.get()); const auto revision = rev(session.get());
        CHECK_REJ(session.get(), daw_get_audio_hardware_settings(session.get(), expected.uid, &expected));
        CHECK(expected.device_id == original.device_id && expected.buffer_frames == original.buffer_frames);
        CHECK_REJ(session.get(), daw_get_audio_hardware_settings(session.get(), "", &expected));
        CHECK_REJ(session.get(), daw_get_audio_hardware_settings(session.get(), nullptr, &expected));
        CHECK_REJ(session.get(), daw_get_audio_hardware_settings(session.get(), expected.uid, nullptr));
        CHECK(daw_get_audio_hardware_settings(nullptr, expected.uid, &expected) != 0);
        CHECK(daw_begin_audio_hardware_change(nullptr, &expected, 128) == nullptr);
        CHECK(daw_begin_audio_hardware_change(session.get(), nullptr, 128) == nullptr);
        for (uint32_t frames : {0u, 1u, 4097u, UINT32_MAX})
            CHECK(daw_begin_audio_hardware_change(session.get(), &expected, frames) == nullptr);
        auto bad = expected; bad.version = 9;
        CHECK(daw_begin_audio_hardware_change(session.get(), &bad, 128) == nullptr);
        bad = expected; --bad.struct_size;
        CHECK(daw_begin_audio_hardware_change(session.get(), &bad, 128) == nullptr);
        bad = expected; bad.sample_rate = std::numeric_limits<double>::quiet_NaN();
        CHECK(daw_begin_audio_hardware_change(session.get(), &bad, 128) == nullptr);
        bad = expected; std::memset(bad.uid, 'x', sizeof(bad.uid));
        CHECK(daw_begin_audio_hardware_change(session.get(), &bad, 128) == nullptr);
        bad = expected; bad.rate_writable = 2;
        CHECK(daw_begin_audio_hardware_change(session.get(), &bad, 128) == nullptr);
        {
            daw::AudioIOLease activeOtherSession;
            CHECK(daw_begin_audio_hardware_change(session.get(), &expected, 128) == nullptr);
        }
        {
            daw::AudioHardwareLease configuringOtherSession;
            auto config = abi<daw_audio_device_config>(); config.version = DAW_AUDIO_DEVICE_CONFIG_VERSION;
            CHECK_OK(session.get(), daw_get_audio_device_config(session.get(), &config));
            CHECK_REJ(session.get(), daw_set_audio_device_config(session.get(), &config));
            CHECK_REJ(session.get(), daw_midi_record_arm(session.get(), 0, 0));
            CHECK(daw_begin_audio_hardware_change(session.get(), &expected, 128) == nullptr);
        }
        auto job = daw_begin_audio_hardware_change(session.get(), &expected, 128);
        CHECK(job != nullptr);
        auto status = abi<daw_audio_hardware_status>(); status.version = DAW_AUDIO_HARDWARE_VERSION;
        CHECK(daw_poll_audio_hardware_change(nullptr, &status) != 0);
        CHECK(daw_poll_audio_hardware_change(job, nullptr) != 0);
        auto badStatus = status; badStatus.version = 99;
        CHECK(daw_poll_audio_hardware_change(job, &badStatus) != 0);
        for (int i = 0; i < 1000; ++i) {
            CHECK(daw_poll_audio_hardware_change(job, &status) == 0);
            if (status.status != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(status.status == 2 && status.actual_known == 0 && status.error[0] != 0);
        daw_release_audio_hardware_change(job);
        daw_release_audio_hardware_change(nullptr);
        CHECK(rev(session.get()) == revision && dumpOf(session.get()) == before);
        // Invalid and canceled jobs must not depend on a dead session pointer.
        auto owner = daw_create(); CHECK(owner != nullptr);
        job = daw_begin_audio_hardware_change(owner, &expected, 128); CHECK(job != nullptr);
        daw_destroy(owner);
        daw_release_audio_hardware_change(job);
        for (int i = 0; i < 1000 && daw::audioHardwareChangeActive(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(!daw::audioHardwareChangeActive());
        std::cout << "PASS: hardware format C ABI, negative requests, cross-session exclusion, independent job lifetime\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
