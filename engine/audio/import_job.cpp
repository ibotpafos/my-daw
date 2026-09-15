#include "audio/import_job.hpp"
#include "domain/session.hpp"
#include "jobs/limiter.hpp"

#include <thread>

namespace daw {
namespace {
void setImportPhase(void* context, uint8_t phase) {
    static_cast<ImportJobResult*>(context)->phase.store(static_cast<ImportJobPhase>(phase),
                                                        std::memory_order_release);
}
void publishFailure(ImportJobResult& result, std::string error) {
    std::lock_guard lock(result.publication);
    result.error = std::move(error);
    ImportJobStatus expected = ImportJobStatus::Running;
    result.status.compare_exchange_strong(expected, ImportJobStatus::Failed,
                                          std::memory_order_release, std::memory_order_acquire);
}
}

std::shared_ptr<ImportJobResult> startWavImport(std::string path) {
    if (path.empty()) throw Error("Choose a WAV file to import");
    auto permit = tryAcquireBackgroundJob();
    if (!permit) throw Error("Background job capacity reached");
    auto result = std::make_shared<ImportJobResult>();
    std::thread([path = std::move(path), result, permit = std::move(permit)]() mutable {
        (void)permit;
        try {
            ImportControl control{&result->cancel, &result->progress, 0, 100, &setImportPhase, result.get()};
            uint32_t sourceRate = 0, sourceChannels = 0;
            uint64_t sourceFrames = 0;
            result->phase.store(ImportJobPhase::Reading, std::memory_order_release);
            auto clip = readWav(path, control, &sourceRate, &sourceChannels, &sourceFrames);
            checkImportCanceled(control);
            result->sourceSampleRate.store(sourceRate, std::memory_order_release);
            result->sourceChannels.store(sourceChannels, std::memory_order_release);
            result->sourceFrames.store(sourceFrames, std::memory_order_release);
            result->outputFrames.store(clip->frames(), std::memory_order_release);
            {
                std::lock_guard lock(result->publication);
                if (result->status.load(std::memory_order_acquire) != ImportJobStatus::Running ||
                    result->cancel.load(std::memory_order_acquire)) throw ImportCanceled{};
                result->clip = std::move(clip);
                result->phase.store(ImportJobPhase::Ready, std::memory_order_release);
                result->status.store(ImportJobStatus::Ready, std::memory_order_release);
            }
        } catch (const ImportCanceled&) {
            ImportJobStatus expected = ImportJobStatus::Running;
            result->status.compare_exchange_strong(expected, ImportJobStatus::Canceled,
                                                   std::memory_order_release, std::memory_order_acquire);
        } catch (const std::exception& error) {
            if (result->cancel.load(std::memory_order_acquire)) {
                ImportJobStatus expected = ImportJobStatus::Running;
                result->status.compare_exchange_strong(expected, ImportJobStatus::Canceled,
                                                       std::memory_order_release, std::memory_order_acquire);
            }
            else publishFailure(*result, error.what());
        } catch (...) {
            if (result->cancel.load(std::memory_order_acquire)) {
                ImportJobStatus expected = ImportJobStatus::Running;
                result->status.compare_exchange_strong(expected, ImportJobStatus::Canceled,
                                                       std::memory_order_release, std::memory_order_acquire);
            }
            else publishFailure(*result, "Unknown WAV import error");
        }
    }).detach();
    return result;
}

void cancelImport(ImportJobResult& result) noexcept {
    std::lock_guard lock(result.publication);
    const auto status = result.status.load(std::memory_order_acquire);
    if (status == ImportJobStatus::Running || status == ImportJobStatus::Ready) {
        result.cancel.store(true, std::memory_order_release);
        result.clip.reset();
        result.status.store(ImportJobStatus::Canceled, std::memory_order_release);
    }
}

std::shared_ptr<const Clip> importClip(const ImportJobResult& result) {
    if (result.status.load(std::memory_order_acquire) != ImportJobStatus::Ready) return {};
    std::lock_guard lock(result.publication);
    return result.clip;
}

std::string importError(const ImportJobResult& result) {
    std::lock_guard lock(result.publication);
    return result.error;
}

bool markImportApplied(ImportJobResult& result) noexcept {
    std::lock_guard lock(result.publication);
    if (result.status.load(std::memory_order_acquire) != ImportJobStatus::Ready) return false;
    result.status.store(ImportJobStatus::Applied, std::memory_order_release);
    result.clip.reset();
    return true;
}
}
