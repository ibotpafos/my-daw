#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <thread>

namespace te = tracktion::engine;
using namespace tracktion::literals;

namespace {

class HeadlessEngineBehaviour final : public te::EngineBehaviour {
public:
  bool autoInitialiseDeviceManager() override { return false; }
};

class MemoryPropertyStorage final : public te::PropertyStorage {
public:
  MemoryPropertyStorage() : PropertyStorage("My DAW Tracktion Proof") {}
  void removeProperty(te::SettingID) override {}
  juce::var getProperty(te::SettingID, const juce::var& fallback) override { return fallback; }
  void setProperty(te::SettingID, const juce::var&) override {}
  std::unique_ptr<juce::XmlElement> getXmlProperty(te::SettingID) override { return {}; }
  void setXmlProperty(te::SettingID, const juce::XmlElement&) override {}
  void removePropertyItem(te::SettingID, juce::StringRef) override {}
  juce::var getPropertyItem(te::SettingID, juce::StringRef, const juce::var& fallback) override { return fallback; }
  void setPropertyItem(te::SettingID, juce::StringRef, const juce::var&) override {}
  std::unique_ptr<juce::XmlElement> getXmlPropertyItem(te::SettingID, juce::StringRef) override { return {}; }
  void setXmlPropertyItem(te::SettingID, juce::StringRef, const juce::XmlElement&) override {}
};

bool writeSineWave(const juce::File& file) {
  constexpr double sampleRate = 48000.0;
  constexpr int samples = 48000;
  juce::AudioBuffer<float> buffer(1, samples);
  for (int i = 0; i < samples; ++i) {
    buffer.setSample(0, i, 0.25f * std::sin(juce::MathConstants<double>::twoPi * 440.0 * i / sampleRate));
  }

  std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
  if (stream == nullptr)
    return false;
  juce::WavAudioFormat wav;
  auto options = juce::AudioFormatWriterOptions()
                     .withSampleRate(sampleRate)
                     .withNumChannels(1)
                     .withBitsPerSample(24);
  auto writer = wav.createWriterFor(stream, options);
  return writer != nullptr && writer->writeFromAudioSampleBuffer(buffer, 0, samples);
}

constexpr float dbToGain(float db) {
  return std::pow(10.0f, db / 20.0f);
}

// This deliberately small routing graph is shared conceptually with B-011:
//
//   source track (-3 dB) --------------------------> master
//          | post-fader send (-6 dB)
//          v
//     aux bus / return track -----------------------> master
//
// Tracktion calls the bus endpoints AuxSendPlugin and AuxReturnPlugin; the
// master endpoint is the Edit's implicit master track.  The expected peak is
// therefore sourcePeak * trackGain * (1 + sendGain).  Keeping the fixture
// gain-only makes it deterministic across render block sizes and avoids
// pretending that Tracktion's routing representation is My DAW's model.
constexpr float kFixtureTrackGainDb = -3.0f;
constexpr float kFixtureSendGainDb = -6.0f;
constexpr float kFixtureSourcePeak = 0.25f;

float fixtureExpectedPeak() {
  return kFixtureSourcePeak * dbToGain(kFixtureTrackGainDb)
       * (1.0f + dbToGain(kFixtureSendGainDb));
}

int fail(const juce::String& message) {
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

} // namespace

int main() {
  juce::ScopedJuceInitialiser_GUI juce;
  std::cerr << "stage=engine\n";
  te::Engine engine(std::make_unique<MemoryPropertyStorage>(), nullptr,
                    std::make_unique<HeadlessEngineBehaviour>());

  const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getNonexistentChildFile("mydaw-tracktion-proof", {}, false);
  if (!root.createDirectory())
    return fail("cannot create temporary proof directory");

  const auto source = root.getChildFile("source.wav");
  const auto editFile = root.getChildFile("proof.tracktionedit");
  const auto renderFile = root.getChildFile("render.wav");
  if (!writeSineWave(source))
    return fail("cannot create 48 kHz source WAV");

  std::cerr << "stage=edit\n";
  auto edit = te::createEmptyEdit(engine, editFile);
  edit->ensureNumberOfAudioTracks(2);
  auto tracks = te::getAudioTracks(*edit);
  if (tracks.size() != 2)
    return fail("two-track edit was not created");

  te::AudioFile audioFile(engine, source);
  if (!audioFile.isValid())
    return fail("generated WAV was not accepted");

  auto clip = tracks[0]->insertWaveClip(
      "Proof tone", source,
      {{0_tp, tracktion::TimeDuration::fromSeconds(audioFile.getLength())}, {}}, false);
  if (clip == nullptr)
    return fail("audio clip insertion failed");

  // The proof lives in a temporary directory, so make the persisted fixture
  // explicit instead of relying on project-relative path heuristics.
  clip->getSourceFileReference().setToFile(
      source, te::SourceFileReference::PathStyle::alwaysAbsolute, false);
  clip->setGainDB(kFixtureTrackGainDb);

  // Use Tracktion's native aux pair as the portable equivalent of the B-011
  // post-fader send to a bus.  The second audio track is the return endpoint.
  auto sendPlugin = edit->getPluginCache().createNewPlugin(te::AuxSendPlugin::xmlTypeName, {});
  auto returnPlugin = edit->getPluginCache().createNewPlugin(te::AuxReturnPlugin::xmlTypeName, {});
  if (sendPlugin == nullptr || returnPlugin == nullptr)
    return fail("cannot create native aux routing plugins");
  tracks[0]->pluginList.insertPlugin(sendPlugin, 0, nullptr);
  tracks[1]->pluginList.insertPlugin(returnPlugin, 0, nullptr);
  auto* send = dynamic_cast<te::AuxSendPlugin*>(sendPlugin.get());
  auto* auxReturn = dynamic_cast<te::AuxReturnPlugin*>(returnPlugin.get());
  if (send == nullptr || auxReturn == nullptr)
    return fail("native aux routing plugin types are unavailable");
  send->busNumber = 1;
  send->setGainDb(kFixtureSendGainDb);
  auxReturn->busNumber = 1;
  auto& transport = edit->getTransport();
  transport.setLoopRange(clip->getEditTimeRange());
  transport.looping = true;

  std::cerr << "stage=save\n";
  bool saveFinished = false;
  bool saveOK = false;
  te::EditFileOperations(*edit).save(false, true, false, [&](bool ok) {
    saveOK = ok;
    saveFinished = true;
  });
  while (!saveFinished)
    juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
  if (!saveOK || !editFile.existsAsFile())
    return fail("edit persistence failed");

  std::cerr << "stage=render\n";
  edit.reset();
  edit = te::loadEditFromFile(engine, editFile, te::Edit::forRendering);
  if (edit == nullptr || te::getAudioTracks(*edit).size() != 2)
    return fail("saved edit cannot be loaded for rendering");
  std::cerr << "edit_seconds=" << edit->getLength().inSeconds() << '\n';
  te::Renderer::Parameters render(*edit);
  render.destFile = renderFile;
  render.audioFormat = engine.getAudioFileFormatManager().getWavFormat();
  render.bitDepth = 24;
  render.sampleRateForAudio = 48000.0;
  render.blockSizeForAudio = 512;
  render.time = {0_tp, edit->getLength()};
  render.tracksToDo = te::toBitSet(te::getAllTracks(*edit));
  {
    te::Renderer::RenderTask task("My DAW Tracktion proof", render, nullptr, nullptr);
    std::atomic<bool> renderFinished = false;
    std::thread renderThread([&] {
      while (task.runJob() == juce::ThreadPoolJob::jobNeedsRunningAgain) {}
      renderFinished = true;
    });
    while (!renderFinished)
      juce::MessageManager::getInstance()->runDispatchLoopUntil(5);
    renderThread.join();
    if (task.errorMessage.isNotEmpty() || !renderFile.existsAsFile())
      return fail("offline render failed: " + task.errorMessage);
  }

  std::unique_ptr<juce::AudioFormatReader> reader(
      engine.getAudioFileFormatManager().readFormatManager.createReaderFor(renderFile));
  if (reader == nullptr || reader->sampleRate != 48000.0 || reader->lengthInSamples < 47000)
    return fail("rendered WAV has invalid format or duration");

  juce::AudioBuffer<float> rendered(static_cast<int>(reader->numChannels),
                                    static_cast<int>(reader->lengthInSamples));
  if (!reader->read(&rendered, 0, rendered.getNumSamples(), 0, true, true))
    return fail("rendered WAV cannot be read");
  const auto renderPeak = rendered.getMagnitude(0, rendered.getNumSamples());
  if (renderPeak < 0.15f)
    return fail("offline render is unexpectedly silent");

  const auto expectedPeak = fixtureExpectedPeak();
  // The source's sampled sine peak is fractionally below 0.25, so compare
  // the routing result with a tight but waveform-safe relative tolerance.
  if (std::abs(renderPeak - expectedPeak) > expectedPeak * 0.015f)
    return fail("B-011 aux routing peak differs from the deterministic fixture");

  std::cerr << "stage=verify\n";
  std::cout << "PASS tracktion=" << te::Engine::getVersion()
            << " tracks=" << tracks.size()
            << " source_hz=" << audioFile.getSampleRate()
            << " render_hz=" << reader->sampleRate
            << " render_frames=" << reader->lengthInSamples
            << " render_peak=" << renderPeak
            << " fixture=post_fader_aux_to_master"
            << " expected_peak=" << expectedPeak
            << '\n';

  reader.reset();
  edit.reset();
  root.deleteRecursively(false);
  return 0;
}
