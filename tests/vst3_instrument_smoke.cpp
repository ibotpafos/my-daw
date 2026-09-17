// Real, unmodified mda DX10 from the pinned Steinberg SDK. No test DSP/factory.
#include "audio/clip.hpp"
#include "audio/effect.hpp"
#include "audio/renderer.hpp"
#include "bridge/daw.h"
#include "domain/session.hpp"
#include "platform/macos/vst3_runtime.hpp"
#include "plugins/plugin_descriptor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {
constexpr uint32_t sampleRate = 48000;
constexpr uint32_t totalFrames = 48000;
constexpr uint32_t noteStart = 137; // Neither a typical block boundary nor a beat.
constexpr uint32_t noteEnd = 12031;
constexpr uint8_t pitch = 60;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
const daw::Vst3Parameter &parameter(const std::vector<daw::Vst3Parameter> &values, uint32_t id) {
    const auto it = std::find_if(values.begin(), values.end(),
                                 [id](const auto &value) { return value.id == id; });
    if (it == values.end()) throw std::runtime_error("DX10 envelope parameter missing");
    return *it;
}
struct Audio {
    std::vector<float> left, right;
    explicit Audio(size_t frames) : left(frames), right(frames) {}
};
struct Measurement {
    size_t onset = totalFrames;
    double peak = 0;
    double energy = 0;
};
Measurement measure(const Audio &audio, size_t expectedStart = noteStart) {
    Measurement result;
    check(audio.left.size() == totalFrames && audio.right.size() == totalFrames,
          "Unexpected instrument render duration");
    for (size_t i = 0; i < totalFrames; ++i) {
        const double left = audio.left[i], right = audio.right[i];
        check(std::isfinite(left) && std::isfinite(right), "Non-finite instrument output");
        // This specific SDK instrument has identical left/right output.
        check(std::abs(left - right) < 1e-6, "DX10 stereo channels diverged");
        if (i < expectedStart) check(left == 0 && right == 0, "MIDI note played too early / dry leak");
        if (std::abs(left) > 1e-7 && result.onset == totalFrames) result.onset = i;
        result.peak = std::max(result.peak, std::abs(left));
        result.energy += left * left;
        // Amp decay is held indefinitely; this only becomes silent if Note Off
        // reached the real processor (release is set to its shortest setting).
        if (i >= totalFrames - 4096) check(left == 0 && right == 0, "Note Off did not release voice");
    }
    check(result.onset >= expectedStart && result.onset <= expectedStart + 2,
          "Note On lost its sample offset");
    check(result.peak > 1e-4 && result.energy > 1e-4, "Silent instrument / dry fallback passed");
    check(result.peak < 10, "Unbounded instrument output");
    return result;
}

Audio renderDirect(const daw::PluginInsert &plugin, uint32_t block, uint32_t start = noteStart) {
    auto effect = daw::prepareVst3Effect(plugin, sampleRate, block);
    check(effect != nullptr && effect->latencyFrames() == 0, "DX10 preparation/latency failed");
    Audio audio(totalFrames);
    std::vector<float> left(block), right(block);
    for (uint32_t frame = 0; frame < totalFrames;) {
        const auto count = std::min(block, totalFrames - frame);
        // Buffers are in-place in the DAW, but MUST NOT become audio input to a
        // generator. The first no-note blocks also detect stale output leakage.
        std::fill(left.begin(), left.end(), 0.125f);
        std::fill(right.begin(), right.end(), -0.25f);
        std::array<daw::PreparedMidiEvent, 2> events{};
        size_t used = 0;
        if (start >= frame && start < frame + count)
            events[used++] = {start - frame, 0, pitch, 100, false};
        if (noteEnd >= frame && noteEnd < frame + count)
            events[used++] = {noteEnd - frame, 0, pitch, 0, true};
        check(effect->process(left.data(), right.data(), count, frame, {}, {events.data(), used}),
              "Real instrument rejected process block");
        std::copy_n(left.data(), count, audio.left.data() + frame);
        std::copy_n(right.data(), count, audio.right.data() + frame);
        frame += count;
    }
    return audio;
}

void verifyRejectedEvents(const daw::PluginInsert &plugin) {
    constexpr uint32_t block = 64;
    auto effect = daw::prepareVst3Effect(plugin, sampleRate, block);
    std::array<float, block> left{}, right{};
    const std::array<daw::PreparedMidiEvent, 4> malformed{{
        {block, 0, pitch, 100, false}, {0, 16, pitch, 100, false},
        {0, 0, 128, 100, false}, {0, 0, pitch, 128, false}}};
    for (const auto &event : malformed) {
        left.fill(0.125f); right.fill(-0.25f);
        check(!effect->process(left.data(), right.data(), block, 0, {}, {&event, 1}),
              "Invalid note event accepted");
        check(std::all_of(left.begin(), left.end(), [](float v) { return v == 0.125f; }) &&
              std::all_of(right.begin(), right.end(), [](float v) { return v == -0.25f; }),
              "Rejected MIDI block changed caller's buffers");
    }
    std::array<daw::PreparedMidiEvent, 513> tooMany{};
    check(!effect->process(left.data(), right.data(), block, 0, {}, tooMany),
          "Oversized note list accepted");
    // Rejected notes must not have entered the synth and remain stuck there.
    check(effect->process(left.data(), right.data(), block, 0), "Recovery after bad MIDI failed");
    check(std::all_of(left.begin(), left.end(), [](float v) { return v == 0; }),
          "Rejected event left an active voice");
}

Audio renderProject(const daw::State &state) {
    daw::Renderer renderer;
    renderer.prepare(state);
    check(renderer.duration() == totalFrames, "MIDI-only duration missing");
    renderer.playing.store(true);
    Audio audio(totalFrames);
    constexpr uint32_t block = 257;
    for (uint32_t frame = 0; frame < totalFrames;) {
        const auto count = std::min(block, totalFrames - frame);
        renderer.renderExport(audio.left.data() + frame, audio.right.data() + frame, count);
        frame += count;
    }
    check(renderer.pluginErrors.load() == 0, "MIDI-only graph used plugin fallback");
    return audio;
}

Audio exportThroughBridge(const std::filesystem::path &draft) {
    std::unique_ptr<daw_session, decltype(&daw_destroy)> session(daw_create(), daw_destroy);
    check(session != nullptr, "Create bridge session failed");
    check(daw_open_draft(session.get(), draft.c_str()) == 0, "Open MIDI-only project through ABI failed");
    daw_transport transport{}; transport.struct_size = sizeof(transport);
    check(daw_get_transport(session.get(), &transport) == 0 && transport.duration == totalFrames,
          "Bridge did not expose MIDI duration");
    const auto path = std::filesystem::current_path() / "vst3-instrument.wav";
    std::unique_ptr<daw_export_job, decltype(&daw_release_export)> job(
        daw_begin_export(session.get(), path.c_str(), 2), daw_release_export);
    check(job != nullptr, "MIDI-only WAV export failed to start");
    daw_export_status status{}; status.struct_size = sizeof(status);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
        check(daw_poll_export(job.get(), &status) == 0, "Poll export failed");
        if (status.status != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    if (status.status != 1) {
        daw_cancel_export(job.get());
        throw std::runtime_error(std::string("MIDI-only export failed/timed out: ") + status.error);
    }
    check(status.rendered_frames == totalFrames && status.total_frames == totalFrames,
          "MIDI-only export frame count changed");
    auto clip = daw::readWav(path.string());
    check(clip->frames() == totalFrames, "Exported WAV length is wrong");
    Audio audio(totalFrames);
    for (size_t i = 0; i < totalFrames; ++i) {
        audio.left[i] = clip->samples()[2 * i];
        audio.right[i] = clip->samples()[2 * i + 1];
    }
    return audio;
}

void verifyIsolated(daw::PluginInsert plugin, const char *helper) {
    daw::setVst3RuntimeHelperPathForTesting(helper);
    plugin.hostingMode = daw::PluginHostingMode::OutOfProcess;
    // This goes through the real helper's controller + zero-audio-input flush.
    const auto changed = daw::setVst3Parameter(plugin, 2, 0.0f);
    plugin.state = changed.state;
    check(parameter(daw::vst3Parameters(plugin), 1).normalizedValue == 1.0f &&
          parameter(daw::vst3Parameters(plugin), 2).normalizedValue == 0.0f,
          "Isolated instrument parameter/state was lost");
    constexpr uint32_t block = 4096;
    auto effect = daw::prepareVst3Effect(plugin, sampleRate, block);
    check(effect->latencyFrames() == block, "Isolated instrument pipeline latency changed");
    std::array<float, block> left{}, right{};
    const daw::PreparedMidiEvent note{noteStart, 0, pitch, 100, false};
    check(effect->process(left.data(), right.data(), block, 0, {}, {&note, 1}),
          "Isolated instrument did not accept first block");
    check(std::all_of(left.begin(), left.end(), [](float v) { return v == 0; }),
          "Isolated pipeline did not start with silence");
    // Control-thread pacing only: this proves real IPC audio, NOT low-latency
    // realtime scheduling on a loaded device. No sleep is added to production.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    left.fill(0); right.fill(0);
    check(effect->process(left.data(), right.data(), block, block), "Isolated instrument missed reply");
    const auto state = effect->runtimeStatus();
    check(state.state == daw::PreparedEffectRuntimeState::ActiveIsolated &&
          state.extraPipelineLatencyFrames == block && state.faultCode == 0,
          "Isolated instrument fell back to dry audio");
    check(std::all_of(left.begin(), left.begin() + noteStart, [](float v) { return v == 0; }),
          "Isolated MIDI offset changed");
    check(std::any_of(left.begin() + noteStart, left.begin() + noteStart + 3,
                     [](float v) { return std::abs(v) > 1e-7f; }),
          "Real isolated instrument produced no note at the pipeline-correct offset");
    plugin.hostingMode = daw::PluginHostingMode::InProcess;
    const auto reference = renderDirect(plugin, block);
    for (size_t i = 0; i < block; ++i)
        check(std::abs(left[i] - reference.left[i]) < 1e-6f &&
              std::abs(right[i] - reference.right[i]) < 1e-6f,
              "Real IPC audio differs from in-process instrument");
}
} // namespace

int main(int argc, char **argv) {
    try {
        check(argc == 5, "Expected DX10 module, scanned FUID, real helper and fingerprint");
        const auto fuid = daw::parseTextualVst3Fuid(argv[2]);
        check(fuid.has_value(), "Invalid scanned instrument class ID");
        daw::Vst3StateEnvelope envelope;
        envelope.descriptor.format = daw::PluginFormat::VST3;
        envelope.descriptor.vst3ClassFuid = *fuid;
        envelope.descriptor.modulePath = argv[1];
        envelope.descriptor.fingerprint = argv[4];
        daw::PluginInsert plugin;
        plugin.name = "mda DX10 SDK fixture";
        plugin.state = daw::encodeVst3StateEnvelope(envelope);
        // Pinned DX10 IDs 1/2 are amplitude decay/release. Decay=1 sustains
        // indefinitely, so silence at the end specifically proves Note Off.
        plugin.state = daw::setVst3Parameter(plugin, 1, 1.0f).state;
        plugin.state = daw::setVst3Parameter(plugin, 2, 0.0f).state;
        const auto params = daw::vst3Parameters(plugin);
        check(parameter(params, 1).normalizedValue == 1 && parameter(params, 2).normalizedValue == 0,
              "Instrument envelope parameters did not survive state capture");
        for (const auto block : {64U, 257U, 4096U}) {
            const auto audio = renderDirect(plugin, block);
            const auto value = measure(audio);
            const auto zero = renderDirect(plugin, block, 0);
            for (size_t i = 0; i < 256; ++i)
                check(std::abs(audio.left[noteStart + i] - zero.left[i]) < 1e-6f,
                      "Non-aligned note is not the same waveform shifted by 137 samples");
            std::cout << "PASS DX10 block=" << block << " onset=" << value.onset
                      << " peak=" << value.peak << " energy=" << value.energy << '\n';
        }
        verifyRejectedEvents(plugin);
        daw::Session session;
        session.add("DX10 MIDI only", session.state().revision);
        const auto track = session.state().tracks[0].id;
        daw::MidiClip midi;
        midi.length = totalFrames; midi.track = 0;
        midi.notes.push_back({noteStart, noteEnd - noteStart, pitch, 0, 100});
        session.addMidiClip(track, midi, session.state().revision);
        session.addTrackInsert(track, plugin, session.state().revision);
        session.undo(session.state().revision);
        check(session.state().tracks[0].inserts.empty(), "Instrument add Undo failed");
        session.redo(session.state().revision);
        check(session.state().tracks[0].audio == nullptr && session.state().tracks[0].inserts.size() == 1,
              "MIDI-only project unexpectedly contains audio or lost its instrument");
        const auto rendered = renderProject(session.state());
        const auto projectMeasurement = measure(rendered);
        std::string pattern = (std::filesystem::temp_directory_path() / "mydaw-vst3-instrument-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back('\0');
        const char *directory = mkdtemp(writable.data());
        check(directory != nullptr, "Create temporary project directory failed");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
        } cleanup{directory};
        const auto path = cleanup.path / "instrument.mydawdraft";
        daw::writeDraft(session.state(), path.string());
        const auto restored = daw::readDraft(path.string());
        check(restored.tracks[0].midiClips == session.state().tracks[0].midiClips &&
              restored.tracks[0].inserts == session.state().tracks[0].inserts,
              "Project round trip lost notes or instrument state");
        const auto reopened = renderProject(restored);
        check(reopened.left == rendered.left && reopened.right == rendered.right,
              "Reopened instrument project renders different audio");
        const auto exported = measure(exportThroughBridge(path));
        verifyIsolated(plugin, argv[3]);
        std::ofstream proof("vst3-instrument-proof.json");
        proof << "{\n  \"fixture\": \"mda DX10\",\n  \"sample_rate\": 48000,\n"
              << "  \"frames\": " << totalFrames << ",\n  \"note_on_frame\": " << noteStart
              << ",\n  \"note_off_frame\": " << noteEnd << ",\n  \"block_sizes\": [64, 257, 4096],\n"
              << "  \"project_onset\": " << projectMeasurement.onset
              << ",\n  \"export_peak\": " << exported.peak
              << ",\n  \"isolated_pipeline_frames\": 4096,\n  \"physical_audio_tested\": false\n}\n";
        proof.close(); check(proof.good(), "Write instrument evidence failed");
        std::cout << "PASS: real VST3 instrument, MIDI-only graph, sample offsets, note release, "
                     "Undo/Redo, save/open, public ABI WAV export and paced isolated audio\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
