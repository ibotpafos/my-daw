// No microphone/output unit is opened. Enumeration reads HAL metadata only;
// any prepared playback is canceled before the owner-thread start boundary.
#include "e2e.hpp"
#include <cstring>

int main() {
    try {
        using namespace e2e;
        Bridge session;
        TempRoot root("audio-settings");
        auto config = abi<daw_audio_device_config>();
        config.version = DAW_AUDIO_DEVICE_CONFIG_VERSION;
        CHECK_OK(session.get(), daw_get_audio_device_config(session.get(), &config));
        CHECK(config.input_uid[0] == 0 && config.output_uid[0] == 0);
        CHECK(config.input_channel == 0 && config.output_left == 0 && config.output_right == 1);
        auto recordingInput = abi<daw_record_input_config>();
        recordingInput.version = DAW_RECORD_INPUT_CONFIG_VERSION;
        CHECK_OK(session.get(), daw_get_record_input_config(session.get(), &recordingInput));
        CHECK(recordingInput.channels == 1 && recordingInput.left == 0 && recordingInput.right == 1);
        const auto before = dumpOf(session.get());
        const auto revision = rev(session.get());
        std::strcpy(config.input_uid, "unavailable-studio-input");
        std::strcpy(config.output_uid, "unavailable-studio-output");
        config.input_channel = 3; config.output_left = 2; config.output_right = 5;
        recordingInput.channels = 2; recordingInput.left = 3; recordingInput.right = 4;
        CHECK_OK(session.get(), daw_set_record_input_config(session.get(), &recordingInput));
        CHECK_OK(session.get(), daw_set_audio_device_config(session.get(), &config));
        CHECK(rev(session.get()) == revision && dumpOf(session.get()) == before);
        auto actual = abi<daw_audio_device_config>(); actual.version = DAW_AUDIO_DEVICE_CONFIG_VERSION;
        CHECK_OK(session.get(), daw_get_audio_device_config(session.get(), &actual));
        CHECK(std::strcmp(actual.input_uid, config.input_uid) == 0 &&
              actual.input_channel == 3 && actual.output_right == 5);
        auto actualInput = abi<daw_record_input_config>(); actualInput.version = DAW_RECORD_INPUT_CONFIG_VERSION;
        CHECK_OK(session.get(), daw_get_record_input_config(session.get(), &actualInput));
        CHECK(actualInput.channels == 2 && actualInput.left == 3 && actualInput.right == 4);
        auto bad = config; bad.output_right = bad.output_left;
        CHECK_REJ(session.get(), daw_set_audio_device_config(session.get(), &bad));
        bad = config; bad.input_channel = UINT32_MAX;
        CHECK_REJ(session.get(), daw_set_audio_device_config(session.get(), &bad));
        auto badInput = recordingInput; badInput.right = badInput.left;
        CHECK_REJ(session.get(), daw_set_record_input_config(session.get(), &badInput));
        badInput = recordingInput; badInput.channels = 3;
        CHECK_REJ(session.get(), daw_set_record_input_config(session.get(), &badInput));
        badInput = recordingInput; badInput.version = 99;
        CHECK_REJ(session.get(), daw_set_record_input_config(session.get(), &badInput));
        badInput = recordingInput; --badInput.struct_size;
        CHECK_REJ(session.get(), daw_set_record_input_config(session.get(), &badInput));
        CHECK_REJ(session.get(), daw_set_record_input_config(session.get(), nullptr));
        CHECK_REJ(session.get(), daw_get_record_input_config(session.get(), nullptr));
        bad = config; bad.version = 99;
        CHECK_REJ(session.get(), daw_set_audio_device_config(session.get(), &bad));
        bad = config; --bad.struct_size;
        CHECK_REJ(session.get(), daw_set_audio_device_config(session.get(), &bad));
        bad = config; std::memset(bad.input_uid, 'x', sizeof(bad.input_uid));
        CHECK_REJ(session.get(), daw_set_audio_device_config(session.get(), &bad));
        CHECK_REJ(session.get(), daw_set_audio_device_config(session.get(), nullptr));
        CHECK_REJ(session.get(), daw_get_audio_device_config(session.get(), nullptr));
        CHECK(daw_get_audio_device_config(nullptr, &actual) != 0);
        CHECK(daw_set_audio_device_config(nullptr, &config) != 0);
        CHECK_OK(session.get(), daw_get_audio_device_config(session.get(), &actual));
        CHECK(std::strcmp(actual.input_uid, config.input_uid) == 0 && actual.output_right == 5);
        uint32_t count = 0;
        CHECK_REJ(session.get(), daw_refresh_audio_devices(session.get(), nullptr));
        CHECK(daw_refresh_audio_devices(nullptr, &count) != 0);
        CHECK_OK(session.get(), daw_refresh_audio_devices(session.get(), &count));
        CHECK(count <= 256);
        for (uint32_t i = 0; i < count; ++i) {
            auto device = abi<daw_audio_device>(); device.version = DAW_AUDIO_DEVICE_VERSION;
            CHECK_OK(session.get(), daw_get_audio_device(session.get(), i, &device));
            CHECK(device.device_id != 0 && device.uid[0] != 0);
            CHECK(std::memchr(device.uid, 0, sizeof(device.uid)) != nullptr);
            CHECK(device.input_channels <= 128 && device.output_channels <= 128);
        }
        auto device = abi<daw_audio_device>(); device.version = DAW_AUDIO_DEVICE_VERSION;
        CHECK_REJ(session.get(), daw_get_audio_device(session.get(), count, &device));
        CHECK_REJ(session.get(), daw_get_audio_device(session.get(), 0, nullptr));
        CHECK(daw_get_audio_device(nullptr, 0, &device) != 0);
        device.version = 7;
        CHECK_REJ(session.get(), daw_get_audio_device(session.get(), 0, &device));
        const auto path = root / "project.mydawdraft";
        saveDraftAndWait(session.get(), path);
        Bridge second;
        CHECK_OK(second.get(), daw_open_draft(second.get(), path.string().c_str()));
        CHECK_OK(second.get(), daw_get_audio_device_config(second.get(), &actual));
        CHECK(actual.input_uid[0] == 0); // importing a project cannot select hardware
        CHECK_OK(session.get(), daw_open_draft(session.get(), path.string().c_str()));
        CHECK_OK(session.get(), daw_get_audio_device_config(session.get(), &actual));
        CHECK(std::strcmp(actual.input_uid, config.input_uid) == 0); // own machine preference survives open
        // Freeze routing during prepare, even if the worker has already finished.
        // No transport polling: therefore no hardware start can occur on macOS.
        const auto tone = root / "tone.wav";
        writeWavFixture(tone, sineInterleaved(4800, kProjectRate, 300.0, 0.2, 2), kProjectRate, 2, "f32");
        CHECK_OK(session.get(), daw_import_wav(session.get(), tone.string().c_str(), "Audio", rev(session.get())));
        CHECK_OK(session.get(), daw_play(session.get()));
        CHECK_REJ(session.get(), daw_set_audio_device_config(session.get(), &config));
        CHECK_OK(session.get(), daw_stop(session.get()));
        CHECK_OK(session.get(), daw_set_audio_device_config(session.get(), &config));
        std::cout << "PASS: audio settings C ABI, atomic validation, offline UID, persistence boundary and prepare interlock\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
