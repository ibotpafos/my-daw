// Public C ABI rejection/persistence tests. No hardware property is written;
// successful asynchronous transactions are tested with the injected backend in
// audio_hardware_tests.cpp. Physical HAL acknowledgement stays a manual gate.
#include "e2e.hpp"
#include <cstring>
#include <limits>

int main() {
    try {
        using namespace e2e;
        Bridge session;
        TempRoot root("hardware-format");
        const auto before = dumpOf(session.get());
        const auto revision = rev(session.get());
        auto caps = abi<daw_audio_device_capabilities>();
        caps.version = DAW_AUDIO_DEVICE_CAPABILITIES_VERSION;
        auto request = abi<daw_audio_device_change>();
        request.version = DAW_AUDIO_DEVICE_CHANGE_VERSION;
        request.sample_rate = 48000;
        request.buffer_frames = 256;
        std::strcpy(request.uid, "mydaw-ci-definitely-unavailable-uid");
        auto status = abi<daw_audio_device_change_status>();
        status.version = DAW_AUDIO_DEVICE_CHANGE_VERSION;
        CHECK_OK(session.get(), daw_poll_audio_device_change(session.get(), &status));
        CHECK(status.state == DAW_DEVICE_CHANGE_IDLE && !status.actual_known &&
              !status.may_have_changed);
        CHECK(daw_get_audio_device_capabilities(nullptr, request.uid, &caps) != 0);
        CHECK(daw_begin_audio_device_change(nullptr, &request) != 0);
        CHECK(daw_poll_audio_device_change(nullptr, &status) != 0);
        CHECK_REJ(session.get(), daw_get_audio_device_capabilities(session.get(), nullptr, &caps));
        CHECK_REJ(session.get(), daw_get_audio_device_capabilities(session.get(), "", &caps));
        CHECK_REJ(session.get(),
                  daw_get_audio_device_capabilities(session.get(), request.uid, nullptr));
        CHECK_REJ(session.get(),
                  daw_get_audio_device_capabilities(session.get(), request.uid, &caps));
        caps.version = 2;
        CHECK_REJ(session.get(),
                  daw_get_audio_device_capabilities(session.get(), request.uid, &caps));
        caps.version = DAW_AUDIO_DEVICE_CAPABILITIES_VERSION;
        --caps.struct_size;
        CHECK_REJ(session.get(),
                  daw_get_audio_device_capabilities(session.get(), request.uid, &caps));
        CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), nullptr));
        CHECK_REJ(session.get(), daw_poll_audio_device_change(session.get(), nullptr));
        auto badStatus = status;
        --badStatus.struct_size;
        CHECK_REJ(session.get(), daw_poll_audio_device_change(session.get(), &badStatus));
        badStatus = status;
        badStatus.version = 2;
        CHECK_REJ(session.get(), daw_poll_audio_device_change(session.get(), &badStatus));
        auto bad = request;
        --bad.struct_size;
        CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &bad));
        bad = request;
        bad.version = 2;
        CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &bad));
        bad = request;
        bad.uid[0] = 0;
        CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &bad));
        bad = request;
        std::memset(bad.uid, 'a', sizeof(bad.uid));
        CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &bad));
        for (double rate : {44100.0, 96000.0, 0.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN()}) {
            bad = request;
            bad.sample_rate = rate;
            CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &bad));
        }
        for (uint32_t frames : {0u, 4097u, UINT32_MAX}) {
            bad = request;
            bad.buffer_frames = frames;
            CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &bad));
        }
        CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &request));
        CHECK_OK(session.get(), daw_poll_audio_device_change(session.get(), &status));
        CHECK(status.state == DAW_DEVICE_CHANGE_IDLE);
        CHECK(rev(session.get()) == revision && dumpOf(session.get()) == before);
        auto config = abi<daw_audio_device_config>();
        config.version = DAW_AUDIO_DEVICE_CONFIG_VERSION;
        std::strcpy(config.input_uid, request.uid);
        std::strcpy(config.output_uid, request.uid);
        config.output_right = 1;
        CHECK_OK(session.get(), daw_set_audio_device_config(session.get(), &config));
        const auto path = root / "project.mydawdraft";
        saveDraftAndWait(session.get(), path);
        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), path.string().c_str()));
        CHECK_OK(reopened.get(), daw_poll_audio_device_change(reopened.get(), &status));
        CHECK(status.state ==
              DAW_DEVICE_CHANGE_IDLE); // Project opens never submit hardware changes.
        CHECK_OK(session.get(), daw_open_draft(session.get(), path.string().c_str()));
        auto actual = abi<daw_audio_device_config>();
        actual.version = DAW_AUDIO_DEVICE_CONFIG_VERSION;
        CHECK_OK(session.get(), daw_get_audio_device_config(session.get(), &actual));
        CHECK(std::strcmp(actual.output_uid, config.output_uid) == 0);
        // Preparation interlock rejects before the unavailable-UID HAL path; no
        // transport polling occurs, so no output unit can start in CI.
        const auto tone = root / "tone.wav";
        writeWavFixture(tone, sineInterleaved(4800, kProjectRate, 300.0, 0.2, 2), kProjectRate, 2,
                        "f32");
        CHECK_OK(session.get(),
                 daw_import_wav(session.get(), tone.string().c_str(), "Audio", rev(session.get())));
        CHECK_OK(session.get(), daw_play(session.get()));
        CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &request));
        char error[512]{};
        daw_error(session.get(), error, sizeof(error));
        CHECK(std::string(error).find("Stop playback") != std::string::npos);
        CHECK_OK(session.get(), daw_stop(session.get()));
        CHECK_REJ(session.get(), daw_begin_audio_device_change(session.get(), &request));
        daw_error(session.get(), error, sizeof(error));
        CHECK(std::string(error).find("Stop playback") == std::string::npos);
        std::cout
            << "PASS: hardware settings C ABI, validation, persistence and preparation interlock\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
