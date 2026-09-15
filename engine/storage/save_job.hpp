#pragma once
#include "domain/session.hpp"
#include <atomic>
#include <memory>
namespace daw {
struct SaveResult {
    const uint64_t revision;
    std::atomic<int> status{0}; // 0 running, 1 succeeded, 2 failed; release/acquire publication
    char error[512]{};
    explicit SaveResult(uint64_t rev): revision(rev) {}
};
// Copies only model metadata and shared immutable media on the owner thread.
// The detached worker owns the snapshot and result; it never accesses Session/Output.
std::shared_ptr<SaveResult> startSave(State snapshot, std::string path);
}
