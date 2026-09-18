#include "audio/effect.hpp"
#include "audio/export.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
const auto callerThread = std::this_thread::get_id();
std::atomic<unsigned> callerLive{0}, live{0}, workerStarts{0};
std::atomic<unsigned> overlap{0}, renderOverlap{0}, processed{0};
std::mutex mutex;
std::condition_variable entered;
bool holdProbeTeardown = false; // set only between completed jobs
unsigned failures = 0, checks = 0;
void expect(bool okay, const char *name) {
  ++checks;
  if (!okay) { ++failures; std::cerr << "FAIL " << name << '\n'; }
}
void reset(bool hold = false) {
  expect(live.load() == 0, "previous graph has been destroyed");
  callerLive = 0; workerStarts = 0; overlap = 0; renderOverlap = 0; processed = 0;
  holdProbeTeardown = hold;
}
// Link-time substitute for this executable only. Export, Renderer, WAV writer
// and job ownership are production code, not copies or runtime test switches.
class LifecycleEffect final : public daw::PreparedEffect {
  const bool caller = std::this_thread::get_id() == callerThread;
public:
  LifecycleEffect() {
    ++live;
    if (caller) ++callerLive;
    else {
      if (callerLive.load() != 0) ++overlap;
      std::lock_guard lock(mutex);
      ++workerStarts;
      entered.notify_all();
    }
  }
  ~LifecycleEffect() override {
    if (caller) {
      if (holdProbeTeardown) {
        // Widen the old race without adding a sleep/retry to production.
        // Old code launches the worker before entering this destructor; wait
        // for that constructor while the probe is alive. Correct code has no
        // worker yet and finishes teardown after the bounded rendezvous.
        std::unique_lock lock(mutex);
        entered.wait_for(lock, 200ms, [] { return workerStarts.load() != 0; });
      }
      --callerLive;
    }
    --live;
  }
  bool process(float *, float *, uint32_t, uint64_t,
               std::span<const daw::PreparedParameterEvent>,
               std::span<const daw::PreparedMidiEvent>) noexcept override {
    ++processed;
    if (live.load() != 1) ++renderOverlap;
    return true;
  }
  uint32_t latencyFrames() const noexcept override { return 0; }
};
void wait(const std::shared_ptr<daw::ExportResult> &job) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!job->status.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  if (!job->status.load(std::memory_order_acquire)) {
    // Do not destroy/reset fixture state while a worker might still use it.
    std::cerr << "Export lifecycle fixture timed out\n";
    std::_Exit(2);
  }
  expect(job->status == 1, "export succeeds");
  expect(live == 0, "terminal status follows graph destruction");
  expect(workerStarts == 1, "worker really constructs one processing graph");
  expect(overlap == 0, "worker starts only after ALL caller probes are destroyed");
  expect(renderOverlap == 0, "rendering does not retain a probe graph");
  expect(processed > 0, "effect is actually processed, not silently bypassed");
}
}
namespace daw {
std::vector<AudioUnitDescriptor> supportedAudioUnits() { return {}; }
bool audioUnitAvailable(const AudioUnitDescriptor &) noexcept { return true; }
AudioUnitSnapshot snapshotAudioUnit(const AudioUnitDescriptor &, uint32_t, uint32_t) {
  throw Error("Unexpected snapshot in export lifecycle fixture");
}
std::unique_ptr<PreparedEffect> prepareAudioUnit(const PluginInsert &, uint32_t, uint32_t) {
  return std::make_unique<LifecycleEffect>();
}
}
int main() {
  try {
    const auto root = std::filesystem::temp_directory_path() /
                      ("mydaw-export-lifecycle-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{root};
    auto clip = std::make_shared<const daw::Clip>(std::vector<float>(2000, 0.25f));
    daw::Session session;
    session.import("Lifecycle source", clip, 0);
    session.addMasterInsert({0, 0x61756678, 0x6761696e, 0x54657374,
                            "Lifecycle fixture", false, 0, {}, {},
                            daw::PluginHostingMode::InProcess}, 1);
    const auto revision = session.state().revision;
    // Both async entry points used by the public C ABI. Test full explicitly:
    // narrowing only startExportRange leaves the wrapper's probe racing.
    for (bool wholeProject : {false, true}) {
      reset(true);
      const auto file = (root / (wholeProject ? "full.wav" : "range.wav")).string();
      auto job = wholeProject
          ? daw::startExport(session.state(), file, daw::WavFormat::Float32)
          : daw::startExportRange(session.state(), file, daw::WavFormat::Float32, 200, 700);
      wait(job);
      const auto frames = wholeProject ? 1000U : 500U;
      expect(job->revision == revision && job->totalFrames == frames &&
             job->renderedFrames == frames, "revision and frame progress unchanged");
      const auto wav = daw::readWav(file);
      expect(wav->frames() == frames && wav->samples().back() > 0,
             "full/range WAV contains real rendered PCM");
    }
    reset();
    const auto sync = (root / "sync.wav").string();
    daw::writeWav(session.state(), sync, daw::WavFormat::Float32);
    expect(processed > 0 && renderOverlap == 0 && live == 0,
           "synchronous full export also destroys its duration probe first");
    expect(daw::readWav(sync)->frames() == 1000, "synchronous WAV keeps duration");
    reset();
    bool rejected = false;
    try { (void)daw::startExportRange(session.state(), sync,
                                    daw::WavFormat::Float32, 700, 200); }
    catch (const daw::Error &) { rejected = true; }
    expect(rejected && workerStarts == 0 && live == 0,
           "invalid range rejects before worker creation and cleans the probe");
    expect(daw::readWav(sync)->frames() == 1000, "rejection preserves existing file");
    reset();
    auto failed = daw::startExport(session.state(), (root / "missing" / "out.wav").string(),
                                   daw::WavFormat::Float32);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!failed->status.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    if (!failed->status.load(std::memory_order_acquire)) std::_Exit(2);
    expect(failed->status == 2 && live == 0 && overlap == 0,
           "filesystem failure tears down graph and reports failure");
    std::cout << "Export lifecycle: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
