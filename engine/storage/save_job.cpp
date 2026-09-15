#include "storage/save_job.hpp"
#include "jobs/limiter.hpp"
#include <thread>
#include <cstdio>
namespace daw {
std::shared_ptr<SaveResult> startSave(State snapshot,std::string path) {
    auto permit=tryAcquireBackgroundJob();if(!permit)throw Error("Background job capacity reached");
    auto result=std::make_shared<SaveResult>(snapshot.revision);
    std::thread([snapshot=std::move(snapshot),path=std::move(path),result,permit=std::move(permit)] {
        (void)permit;
        try { writeDraft(snapshot,path); result->status.store(1,std::memory_order_release); }
        catch(const std::exception& e) { std::snprintf(result->error,sizeof(result->error),"%s",e.what()); result->status.store(2,std::memory_order_release); }
        catch(...) { std::snprintf(result->error,sizeof(result->error),"Unknown storage error"); result->status.store(2,std::memory_order_release); }
    }).detach();
    return result;
}
}
