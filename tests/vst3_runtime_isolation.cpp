#include "platform/macos/vst3_runtime.hpp"

#include <array>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace daw;
using namespace daw::vst3runtime;

namespace {
[[noreturn]] void fail(const char *expression, int line) { std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression); std::abort(); }
#define CHECK(expression) do { if (!(expression)) fail(#expression, __LINE__); } while (false)
int helper(const char *name) {
  const int fd = shm_open(name, O_RDWR, 0); if (fd < 0) return 10;
  struct stat info{}; if (fstat(fd, &info) != 0) return 11;
  auto *mapping = static_cast<SharedMapping *>(mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (mapping == MAP_FAILED) return 11;
  const char *mode = std::getenv("MY_DAW_VST3_TEST_MODE");
  mapping->pluginLatencyFrames = 73; mapping->pluginTailFrames = 19;
  mapping->helperState.store(1, std::memory_order_release);
  std::fputs("READY\n", stdout); std::fflush(stdout);
  uint64_t last = 0;
  for (;;) {
    if (mapping->shutdown.load(std::memory_order_acquire) != 0) return 0;
    const auto sequence = mapping->requestSequence.load(std::memory_order_acquire);
    if (sequence == last) { std::this_thread::sleep_for(std::chrono::microseconds(100)); continue; }
    if (mode && std::strcmp(mode, "crash") == 0) _exit(99);
    if (mode && std::strcmp(mode, "no-reply") == 0) { last = sequence; continue; }
    const auto &input = mapping->request; auto &output = mapping->reply;
    if (input.sequence != sequence || input.frames > kMaximumFrames || input.eventCount > kMaximumParameterEvents) return 12;
    output.sequence = mode && std::strcmp(mode, "corrupt") == 0 ? sequence + 1 : sequence;
    output.sampleTime = input.sampleTime; output.frames = input.frames;
    for (uint32_t i = 0; i < input.frames; ++i) { output.left[i] = input.left[i] * 2; output.right[i] = input.right[i] * 2; }
    for (uint32_t i = 0; i < input.eventCount; ++i)
      if (input.events[i].parameterID == 77 && input.events[i].sampleOffset < input.frames)
        output.left[input.events[i].sampleOffset] += 10;
    mapping->replySequence.store(sequence, std::memory_order_release); last = sequence;
  }
}

PluginInsert isolated() { PluginInsert plugin{}; plugin.hostingMode = PluginHostingMode::OutOfProcess; return plugin; }
void block(std::array<float, kMaximumFrames> &left, std::array<float, kMaximumFrames> &right) { left.fill(0); right.fill(0); }
void assertReaped() { int status = 0; errno = 0; CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD); }
void expectNormal() {
  setenv("MY_DAW_VST3_TEST_MODE", "normal", 1);
  auto effect = prepareVst3OutOfProcessEffect(isolated());
  CHECK(effect->latencyFrames() == kMaximumFrames + 73); CHECK(effect->tailFrames() == 19);
  std::array<float, kMaximumFrames> left{}, right{}; block(left, right); left[0] = 1;
  const PreparedParameterEvent event{77, 9, 0.5f};
  CHECK(effect->process(left.data(), right.data(), kMaximumFrames, 0, {&event, 1}));
  for (float value : left) CHECK(value == 0); // fixed startup pipeline
  std::this_thread::sleep_for(std::chrono::milliseconds(10)); block(left, right);
  CHECK(effect->process(left.data(), right.data(), kMaximumFrames, kMaximumFrames));
  CHECK(std::fabs(left[0] - 2) < 0.0001f); CHECK(std::fabs(left[9] - 10) < 0.0001f);
  const auto status = effect->runtimeStatus();
  CHECK(status.state == PreparedEffectRuntimeState::ActiveIsolated && status.extraPipelineLatencyFrames == kMaximumFrames);
  effect.reset(); assertReaped();
}
void expectNoReply() {
  setenv("MY_DAW_VST3_TEST_MODE", "no-reply", 1);
  auto effect = prepareVst3OutOfProcessEffect(isolated());
  std::array<float, kMaximumFrames> left{}, right{};
  block(left, right); CHECK(effect->process(left.data(), right.data(), kMaximumFrames, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  block(left, right); CHECK(!effect->process(left.data(), right.data(), kMaximumFrames, kMaximumFrames));
  const auto began = std::chrono::steady_clock::now(); block(left, right);
  CHECK(effect->process(left.data(), right.data(), kMaximumFrames, kMaximumFrames * 2ULL));
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(20));
  const auto status = effect->runtimeStatus();
  CHECK(status.state == PreparedEffectRuntimeState::DryFallback && status.faultCode == 3);
  effect.reset(); assertReaped();
}
void expectCrash() {
  setenv("MY_DAW_VST3_TEST_MODE", "crash", 1);
  auto effect = prepareVst3OutOfProcessEffect(isolated());
  std::array<float, kMaximumFrames> left{}, right{};
  block(left, right); CHECK(effect->process(left.data(), right.data(), kMaximumFrames, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  block(left, right); CHECK(!effect->process(left.data(), right.data(), kMaximumFrames, kMaximumFrames));
  block(left, right); CHECK(effect->process(left.data(), right.data(), kMaximumFrames, kMaximumFrames * 2ULL));
  const auto status = effect->runtimeStatus();
  CHECK(status.state == PreparedEffectRuntimeState::DryFallback);
  CHECK(status.faultCode == 3 || status.faultCode == 5);
  effect.reset(); assertReaped();
}
void expectCorruptReply() {
  setenv("MY_DAW_VST3_TEST_MODE", "corrupt", 1);
  auto effect = prepareVst3OutOfProcessEffect(isolated());
  std::array<float, kMaximumFrames> left{}, right{};
  block(left, right); CHECK(effect->process(left.data(), right.data(), kMaximumFrames, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  block(left, right); CHECK(!effect->process(left.data(), right.data(), kMaximumFrames, kMaximumFrames));
  block(left, right); CHECK(effect->process(left.data(), right.data(), kMaximumFrames, kMaximumFrames * 2ULL));
  const auto status = effect->runtimeStatus();
  CHECK(status.state == PreparedEffectRuntimeState::DryFallback && status.faultCode == 4);
  effect.reset(); assertReaped();
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::strcmp(argv[1], "--shared-memory") == 0) return helper(argv[2]);
  CHECK(argc >= 1); setVst3RuntimeHelperPathForTesting(argv[0]);
  expectNormal(); expectNoReply(); expectCorruptReply(); expectCrash();
}
