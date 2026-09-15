#pragma once
#include "audio/clip.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace daw {
struct RecoveredTake {
    uint64_t startFrame=0;
    std::shared_ptr<const Clip> clip;
};

// One producer (audio callback), one consumer (writer thread). The producer
// allocates nothing, locks nothing and performs no file I/O.
class RecordingWriter {
    std::string path_;
    uint64_t startFrame_=0, capacityFrames_=0;
    std::vector<float> ring_;
    std::atomic<uint64_t> read_{0}, written_{0}, accepted_{0}, committed_{0};
    std::atomic<bool> stopping_{false}, overflow_{false}, failed_{false};
    std::thread worker_;
    int fd_=-1;
    void run() noexcept;
    void closeFile() noexcept;
public:
    RecordingWriter(std::string path,uint64_t startFrame,uint64_t capacityFrames,uint64_t ringFrames=48000*2);
    ~RecordingWriter();
    RecordingWriter(const RecordingWriter&)=delete;
    RecordingWriter& operator=(const RecordingWriter&)=delete;
    void writeMono(const float* input,uint32_t frames) noexcept;
    std::shared_ptr<const Clip> finish();
    void stopPreserving() noexcept;
    void discard() noexcept;
    uint64_t frames() const noexcept { return accepted_.load(std::memory_order_acquire); }
    uint64_t committedFrames() const noexcept { return committed_.load(std::memory_order_acquire); }
    bool overflowed() const noexcept { return overflow_.load(std::memory_order_acquire) || failed_.load(std::memory_order_acquire); }
    const std::string& path() const noexcept { return path_; }
};

RecoveredTake recoverTake(const std::string& path);
std::vector<std::shared_ptr<const Clip>> splitLoopPasses(const Clip& recording,uint64_t loopFrames);
}
