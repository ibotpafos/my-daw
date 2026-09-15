#pragma once
#include <cstdint>
#include <memory>

namespace daw {
constexpr uint32_t backgroundJobLimit=4;
class BackgroundJobPermit {
public:
    BackgroundJobPermit(const BackgroundJobPermit&)=delete;
    BackgroundJobPermit& operator=(const BackgroundJobPermit&)=delete;
    ~BackgroundJobPermit();
private:
    BackgroundJobPermit()=default;
    friend std::shared_ptr<BackgroundJobPermit> tryAcquireBackgroundJob();
};
std::shared_ptr<BackgroundJobPermit> tryAcquireBackgroundJob();
uint32_t backgroundJobsInFlight() noexcept;
}
