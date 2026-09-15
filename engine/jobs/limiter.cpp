#include "jobs/limiter.hpp"
#include <atomic>

namespace daw {
namespace { std::atomic<uint32_t> inFlight{0}; }

std::shared_ptr<BackgroundJobPermit> tryAcquireBackgroundJob() {
    auto current=inFlight.load(std::memory_order_relaxed);
    while(current<backgroundJobLimit) {
        if(inFlight.compare_exchange_weak(current,current+1,std::memory_order_acq_rel,std::memory_order_relaxed)) {
            try { return std::shared_ptr<BackgroundJobPermit>(new BackgroundJobPermit); }
            catch(...) { inFlight.fetch_sub(1,std::memory_order_release); throw; }
        }
    }
    return {};
}
BackgroundJobPermit::~BackgroundJobPermit(){inFlight.fetch_sub(1,std::memory_order_release);}
uint32_t backgroundJobsInFlight() noexcept{return inFlight.load(std::memory_order_acquire);}
}
