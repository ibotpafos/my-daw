#include "audio/device.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <limits>

int main() {
    unsigned checks = 0;
    auto check = [&](bool ok) { ++checks; if (!ok) throw std::runtime_error("audio device invariant failed"); };
    auto rejects = [&](auto operation) {
        bool rejected = false;
        try { operation(); } catch (const std::exception&) { rejected = true; }
        check(rejected);
    };
    try {
        using namespace daw;
        std::vector<AudioDeviceInfo> devices{
            {7, "built-in", "Same name", 1, 2, 256, 48000, true, true},
            {9, "studio", "Same name", 8, 8, 128, 48000, false, false}};
        AudioDeviceConfiguration config;
        check(resolveAudioDevice(config, devices, AudioDeviceDirection::Input).id == 7);
        check(resolveAudioDevice(config, devices, AudioDeviceDirection::Output).id == 7);
        config.inputUID = config.outputUID = "studio";
        config.inputChannel = 7; config.outputLeft = 6; config.outputRight = 7;
        check(resolveAudioDevice(config, devices, AudioDeviceDirection::Input).id == 9);
        check(resolveAudioDevice(config, devices, AudioDeviceDirection::Output).id == 9);
        // A system default change cannot reroute an explicit UID or same-name device.
        devices[0].defaultInput = devices[0].defaultOutput = false;
        check(resolveAudioDevice(config, devices, AudioDeviceDirection::Output).id == 9);
        devices[1].id = 42;
        check(resolveAudioDevice(config, devices, AudioDeviceDirection::Input).id == 42);
        devices.pop_back();
        rejects([&] { resolveAudioDevice(config, devices, AudioDeviceDirection::Input); });
        rejects([&] { resolveAudioDevice(config, devices, AudioDeviceDirection::Output); });
        config.inputUID = config.outputUID = "built-in";
        rejects([&] { resolveAudioDevice(config, devices, AudioDeviceDirection::Input); });
        rejects([&] { resolveAudioDevice(config, devices, AudioDeviceDirection::Output); });
        config.inputChannel = config.outputLeft = 0; config.outputRight = 1;
        for (double rate : {44100.0, 96000.0, std::numeric_limits<double>::infinity(), std::nan("")}) {
            devices[0].sampleRate = rate;
            rejects([&] { resolveAudioDevice(config, devices, AudioDeviceDirection::Input); });
            rejects([&] { resolveAudioDevice(config, devices, AudioDeviceDirection::Output); });
        }
        devices[0].sampleRate = 48000;
        for (uint32_t frames : {0u, 4097u, UINT32_MAX}) {
            devices[0].bufferFrames = frames;
            rejects([&] { resolveAudioDevice(config, devices, AudioDeviceDirection::Output); });
        }
        devices[0].bufferFrames = 4096;
        check(resolveAudioDevice(config, devices, AudioDeviceDirection::Output).id == 7);
        auto bad = config; bad.inputUID = std::string(481, 'u');
        rejects([&] { validateAudioDeviceConfiguration(bad); });
        bad.inputUID = std::string("a\0b", 3);
        rejects([&] { validateAudioDeviceConfiguration(bad); });
        bad = config; bad.outputLeft = bad.outputRight;
        rejects([&] { validateAudioDeviceConfiguration(bad); });
        bad = config; bad.inputChannel = 128;
        rejects([&] { validateAudioDeviceConfiguration(bad); });
        bad = config; bad.inputChannels = 3;
        rejects([&] { validateAudioDeviceConfiguration(bad); });
        bad = config; bad.inputChannels = 2; bad.inputRight = bad.inputChannel;
        rejects([&] { validateAudioDeviceConfiguration(bad); });
        config.inputChannels = 2; config.inputChannel = 0; config.inputRight = 1;
        devices[0].inputChannels = 2;
        check(resolveAudioDevice(config, devices, AudioDeviceDirection::Input).id == 7);
        config.inputRight = 1; devices[0].inputChannels = 1;
        rejects([&] { resolveAudioDevice(config, devices, AudioDeviceDirection::Input); });
        devices[0].inputChannels = 2; config.inputChannels = 1;
        // Every legal stereo pair routes exactly once, with silence everywhere else.
        for (uint32_t channels : {2u, 4u, 8u, 128u}) {
            for (uint32_t left = 0; left < channels; ++left) {
                auto right = (left + 1) % channels;
                const auto map = audioOutputChannelMap(channels, left, right);
                check(map.size() == channels && map[left] == 0 && map[right] == 1);
                check(std::count(map.begin(), map.end(), -1) == channels - 2);
            }
        }
        rejects([&] { audioOutputChannelMap(129, 0, 1); });
        rejects([&] { audioOutputChannelMap(1, 0, 1); });
        rejects([&] { audioOutputChannelMap(2, 0, 2); });
        rejects([&] { audioOutputChannelMap(2, 1, 1); });
        std::cout << "PASS: audio device selection/channel maps: " << checks << " checks\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
