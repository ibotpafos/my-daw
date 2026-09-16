// Integration with the unmodified Steinberg ADelay example, not a mock plugin.
#include "audio/effect.hpp"
#include "domain/session.hpp"
#include "plugins/plugin_descriptor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
const daw::Vst3Parameter &delayParameter(const std::vector<daw::Vst3Parameter> &parameters) {
    const auto found = std::find_if(parameters.begin(), parameters.end(),
        [](const auto &parameter) { return parameter.title == "Delay"; });
    if (found == parameters.end()) throw std::runtime_error("ADelay parameter missing");
    return *found;
}
void verifyImpulse(const daw::PluginInsert &plugin) {
    constexpr uint32_t blockFrames = 4096;
    constexpr uint64_t delayFrames = 12000; // ADelay: 0.25 seconds at 48 kHz.
    auto effect = daw::prepareVst3Effect(plugin, 48000, blockFrames);
    check(effect != nullptr, "No real VST3 processor");
    std::array<float, blockFrames> left{}, right{};
    for (uint64_t start = 0; start < 4 * blockFrames; start += blockFrames) {
        left.fill(0); right.fill(0);
        if (start == 0) { left[0] = 0.75f; right[7] = -0.5f; }
        check(effect->process(left.data(), right.data(), blockFrames, start), "ADelay process failed");
        for (uint32_t i = 0; i < blockFrames; ++i) {
            const float expectedLeft = start + i == delayFrames ? 0.75f : 0.0f;
            const float expectedRight = start + i == delayFrames + 7 ? -0.5f : 0.0f;
            check(std::isfinite(left[i]) && std::abs(left[i] - expectedLeft) < 1e-6f,
                  "Left impulse timing/gain mismatch (or dry fallback)");
            check(std::isfinite(right[i]) && std::abs(right[i] - expectedRight) < 1e-6f,
                  "Right impulse timing/gain mismatch (or channel crosstalk)");
        }
    }
    left.fill(0.125f); right.fill(-0.25f);
    const daw::PreparedParameterEvent bad{0, blockFrames, 0.5f};
    check(!effect->process(left.data(), right.data(), blockFrames, 0, {&bad, 1}),
          "Out-of-block automation was accepted");
    check(std::all_of(left.begin(), left.end(), [](float v) { return v == 0.125f; }) &&
          std::all_of(right.begin(), right.end(), [](float v) { return v == -0.25f; }),
          "Rejected block changed dry audio");
}
}

int main(int argc, char **argv) {
    try {
        check(argc == 4, "Expected ADelay bundle, scanned FUID and runtime helper");
        const auto fuid = daw::parseTextualVst3Fuid(argv[2]);
        check(fuid.has_value(), "Bad scanned class ID");
        daw::Vst3StateEnvelope envelope;
        envelope.descriptor.format = daw::PluginFormat::VST3;
        envelope.descriptor.vst3ClassFuid = *fuid;
        envelope.descriptor.modulePath = argv[1];
        daw::PluginInsert plugin;
        plugin.type = plugin.subtype = plugin.manufacturer = daw::kVst3PluginComponentSentinel;
        plugin.name = "ADelay SDK fixture";
        plugin.state = daw::encodeVst3StateEnvelope(envelope);
        const auto parameters = daw::vst3Parameters(plugin);
        const auto id = delayParameter(parameters).id;
        // ADelay inherits the SDK's no-controller-state implementation. A host
        // must still capture DSP state and synchronize its controller on restore.
        const auto changed = daw::setVst3Parameter(plugin, id, 0.25f);
        check(std::abs(delayParameter(changed.parameters).normalizedValue - 0.25f) < 1e-6f,
              "Parameter edit did not reach controller");
        const auto captured = daw::decodeVst3StateEnvelope(changed.state);
        check(captured && !captured->componentState.empty() && captured->controllerState.empty(),
              "Component state missing or optional controller state mishandled");
        plugin.state = changed.state;
        verifyImpulse(plugin);
        // Persist the real plugin state through the same SQLite storage used by
        // projects, then instantiate a fresh processor and prove the audio again.
        std::string pattern = (std::filesystem::temp_directory_path() / "mydaw-vst3-sdk-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back('\0');
        char *directory = mkdtemp(writable.data());
        check(directory != nullptr, "Could not create test directory");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
        } cleanup{directory};
        daw::Session session;
        session.addMasterInsert(plugin, session.state().revision);
        const auto path = (cleanup.path / "plugin.mydawdraft").string();
        daw::writeDraft(session.state(), path);
        const auto restored = daw::readDraft(path);
        check(restored.masterInserts.size() == 1 && restored.masterInserts[0].state == changed.state,
              "Project round-trip lost plugin state");
        verifyImpulse(restored.masterInserts[0]);
        // The real helper, unlike the existing protocol fake, loads ADelay and
        // exercises its optional controller state via remote control operations.
        check(setenv("MY_DAW_VST3_RUNTIME_HELPER", argv[3], 1) == 0, "Cannot set helper path");
        plugin.hostingMode = daw::PluginHostingMode::OutOfProcess;
        const auto remote = daw::setVst3Parameter(plugin, id, 0.5f);
        check(std::abs(delayParameter(remote.parameters).normalizedValue - 0.5f) < 1e-6f,
              "Real isolated helper lost parameter edit");
        plugin.state = remote.state;
        const auto remoteRead = daw::vst3Parameters(plugin);
        check(std::abs(delayParameter(remoteRead).normalizedValue - 0.5f) < 1e-6f,
              "Real isolated helper failed state restoration");
        std::cout << "PASS: real ADelay processing, exact stereo impulse timing, parameter/state, "
                     "project reopen and isolated control\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
