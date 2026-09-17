#include "audio/effect.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
  try {
    auto catalog = daw::supportedAudioUnits();
    if (catalog.size() != 3)
      throw daw::Error("Expected the 3 approved Apple Audio Units");

    for (const auto &component : catalog) {
      auto snapshot = daw::snapshotAudioUnit(component);
      if (snapshot.state.empty())
        throw daw::Error("Audio Unit returned empty state");
      daw::PluginInsert plugin{1,
                               component.type,
                               component.subtype,
                               component.manufacturer,
                               snapshot.name,
                               false,
                               snapshot.latencyFrames,
                               snapshot.state};
      auto effect = daw::prepareAudioUnit(plugin);
      std::vector<float> left(512), right(512);
      left[0] = right[0] = 0.25f;
      bool ok = true;
      float peak = 0;
      // Track chains receive the track MIDI span at every insert. A normal AU
      // effect must ignore that lane, not fail the block merely because an
      // instrument earlier in the same serial chain consumed the notes.
      const std::array<daw::PreparedMidiEvent, 1> midi{{{0, 0, 60, 100, false}}};
      for (uint64_t block = 0; block < 4; ++block) {
        ok = effect->process(left.data(), right.data(), 512, block * 512, {}, midi) && ok;
        for (size_t i = 0; i < left.size(); ++i) {
          if (!std::isfinite(left[i]) || !std::isfinite(right[i]))
            throw daw::Error("Audio Unit returned non-finite audio");
          peak = std::max({peak, std::abs(left[i]), std::abs(right[i])});
        }
        std::fill(left.begin(), left.end(), 0);
        std::fill(right.begin(), right.end(), 0);
      }
      if (!ok)
        throw daw::Error("Audio Unit render failed");
      std::cout << "PASS component=\"" << snapshot.name
                << "\" state_bytes=" << snapshot.state.size()
                << " latency_frames=" << snapshot.latencyFrames
                << " peak=" << peak << '\n';
    }

    // Hardware-free real MusicDevice proof. macOS normally ships DLS Synth;
    // keep the test portable across stripped runners by treating absence as a
    // named skip, but when the component exists it must accept frame-offset
    // MIDI and produce finite, non-silent audio through the same PreparedEffect
    // contract used by the renderer.
    const daw::AudioUnitDescriptor dls{
        kAudioUnitType_MusicDevice,
        kAudioUnitSubType_DLSSynth,
        kAudioUnitManufacturer_Apple,
        "Apple DLS Synth"};
    if (daw::audioUnitAvailable(dls)) {
      daw::PluginInsert synth{7,
                              dls.type,
                              dls.subtype,
                              dls.manufacturer,
                              dls.name,
                              false,
                              0,
                              {}};
      auto instrument = daw::prepareAudioUnit(synth);
      std::vector<float> left(512, 0.0f), right(512, 0.0f);
      float peak = 0.0f;
      bool ok = true;
      constexpr uint64_t blocks = 96;
      for (uint64_t block = 0; block < blocks; ++block) {
        std::array<daw::PreparedMidiEvent, 1> event{};
        std::span<const daw::PreparedMidiEvent> midi;
        if (block == 0) {
          event[0] = {0, 0, 60, 100, false};
          midi = event;
        } else if (block == 48) {
          event[0] = {0, 0, 60, 0, true};
          midi = event;
        }
        ok = instrument->process(left.data(), right.data(), 512, block * 512, {}, midi) && ok;
        for (size_t frame = 0; frame < left.size(); ++frame) {
          if (!std::isfinite(left[frame]) || !std::isfinite(right[frame]))
            throw daw::Error("DLS MusicDevice returned non-finite audio");
          peak = std::max({peak, std::abs(left[frame]), std::abs(right[frame])});
        }
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
      }
      if (!ok)
        throw daw::Error("DLS MusicDevice render failed");
      if (peak <= 1.0e-6f)
        throw daw::Error("DLS MusicDevice rendered silence after Note On");
      std::cout << "PASS MusicDevice=\"Apple DLS Synth\" latency_frames="
                << instrument->latencyFrames() << " peak=" << peak << '\n';
    } else {
      std::cout << "SKIP MusicDevice=\"Apple DLS Synth\" unavailable\n";
    }

    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
