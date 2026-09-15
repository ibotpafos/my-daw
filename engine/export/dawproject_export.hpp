#pragma once
#include "export/dawproject_model.hpp"
#include <atomic>
#include <memory>
#include <string>

namespace daw::dawproject {
struct ExportResult {
    const uint64_t revision;
    const uint32_t totalEntries;
    const uint32_t warningCount;
    const uint32_t infoCount;
    std::atomic<uint32_t> completedEntries{0};
    std::atomic<int> status{0}; // 0 running, 1 success, 2 failure, 3 canceled
    std::atomic<bool> cancel{false};
    char error[512]{};
    ExportResult(uint64_t revisionValue,uint32_t total,uint32_t warnings,uint32_t info)
        :revision(revisionValue),totalEntries(total),warningCount(warnings),infoCount(info){}
};
std::shared_ptr<ExportResult> startExport(State snapshot,std::string destination,
                                           ExportOptions options,std::string title,
                                           std::string appVersion);
}
