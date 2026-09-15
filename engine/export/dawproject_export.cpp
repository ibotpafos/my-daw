#include "export/dawproject_export.hpp"
#include "export/dawproject_xml.hpp"
#include "jobs/limiter.hpp"
#include "storage/zip_package.hpp"
#include <bit>
#include <cstdio>
#include <cstring>
#include <thread>

namespace daw::dawproject {
namespace {
struct Canceled final {};
void put16(std::vector<uint8_t>& out,size_t offset,uint16_t value){out[offset]=static_cast<uint8_t>(value);out[offset+1]=static_cast<uint8_t>(value>>8);}
void put32(std::vector<uint8_t>& out,size_t offset,uint32_t value){for(unsigned i=0;i<4;++i)out[offset+i]=static_cast<uint8_t>(value>>(i*8));}
std::vector<uint8_t> floatWav(const MediaReference& media,const std::atomic<bool>& cancel) {
    if(!media.audio||media.channels!=2||media.sampleRate!=48000||media.audio->frames()!=media.frames)
        throw Error("DAWproject media snapshot is inconsistent");
    const auto& samples=media.audio->samples();
    const uint64_t dataBytes=samples.size()*sizeof(float);
    if(dataBytes>0xffffffffULL-36)throw Error("DAWproject WAV media exceeds RIFF32 limits");
    std::vector<uint8_t> bytes(44+static_cast<size_t>(dataBytes));
    std::memcpy(bytes.data(),"RIFF",4);put32(bytes,4,static_cast<uint32_t>(36+dataBytes));
    std::memcpy(bytes.data()+8,"WAVEfmt ",8);put32(bytes,16,16);put16(bytes,20,3);put16(bytes,22,2);
    put32(bytes,24,48000);put32(bytes,28,48000*2*4);put16(bytes,32,8);put16(bytes,34,32);
    std::memcpy(bytes.data()+36,"data",4);put32(bytes,40,static_cast<uint32_t>(dataBytes));
    for(size_t index=0;index<samples.size();++index){if((index&0x3fffU)==0&&cancel.load(std::memory_order_acquire))throw Canceled{};put32(bytes,44+index*4,std::bit_cast<uint32_t>(samples[index]));}
    return bytes;
}
std::vector<uint8_t> bytes(std::string value){return {value.begin(),value.end()};}
uint32_t countSeverity(const LossReport& report,LossSeverity severity){uint32_t count=0;for(const auto& item:report.items)if(item.severity==severity)++count;return count;}
uint32_t entryCount(const ExportModel& model){uint64_t count=3;for(const auto& track:model.tracks()){count+=track.media.size();for(const auto& plugin:track.plugins)if(!plugin.statePath.empty())++count;}for(const auto& bus:model.buses())for(const auto& plugin:bus.plugins)if(!plugin.statePath.empty())++count;for(const auto& plugin:model.master().plugins)if(!plugin.statePath.empty())++count;if(count>0xffffffffULL)throw Error("DAWproject has too many package entries");return static_cast<uint32_t>(count);}
}

std::shared_ptr<ExportResult> startExport(State snapshot,std::string destination,
                                           ExportOptions options,std::string title,
                                           std::string appVersion) {
    if(destination.empty())throw Error("Choose a DAWproject export path");
    auto model=makeExportModel(snapshot,options);
    if(model.losses().hasErrors())throw Error("DAWproject mapping contains blocking losses");
    auto permit=tryAcquireBackgroundJob();
    if(!permit)throw Error("Background job capacity reached");
    auto result=std::make_shared<ExportResult>(snapshot.revision,entryCount(model)+1,
        countSeverity(model.losses(),LossSeverity::Warning),
        countSeverity(model.losses(),LossSeverity::Info));
    std::thread([model=std::move(model),destination=std::move(destination),title=std::move(title),
                 appVersion=std::move(appVersion),result,permit=std::move(permit)]() mutable {
        (void)permit;
        try {
            std::vector<ZipPackageEntry> entries;
            entries.reserve(result->totalEntries);
            auto add=[&](std::string name,std::vector<uint8_t> content){
                if(result->cancel.load(std::memory_order_acquire))throw Canceled{};
                entries.push_back({std::move(name),std::move(content)});
                result->completedEntries.fetch_add(1,std::memory_order_release);
            };
            add("project.xml",bytes(projectXml(model,appVersion)));
            add("metadata.xml",bytes(metadataXml(title)));
            add("loss-report.json",bytes(lossReportJson(model.losses())));
            for(const auto& track:model.tracks())for(const auto& media:track.media)
                add("audio/"+media.id+".wav",floatWav(media,result->cancel));
            for(const auto& plugin:model.master().plugins)if(!plugin.statePath.empty())
                add(plugin.statePath,plugin.state);
            for(const auto& track:model.tracks())for(const auto& plugin:track.plugins)if(!plugin.statePath.empty())
                add(plugin.statePath,plugin.state);
            for(const auto& bus:model.buses())for(const auto& plugin:bus.plugins)if(!plugin.statePath.empty())
                add(plugin.statePath,plugin.state);
            if(result->cancel.load(std::memory_order_acquire))throw Canceled{};
            std::string error;
            if(!writeZipPackage(entries,destination,error,&result->cancel)){
                if(result->cancel.load(std::memory_order_acquire))throw Canceled{};
                throw Error(error);
            }
            result->completedEntries.fetch_add(1,std::memory_order_release);
            result->status.store(1,std::memory_order_release);
        } catch(const Canceled&) { result->status.store(3,std::memory_order_release); }
        catch(const std::exception& error) {
            std::snprintf(result->error,sizeof(result->error),"%s",error.what());
            result->status.store(2,std::memory_order_release);
        } catch(...) {
            std::snprintf(result->error,sizeof(result->error),"Unknown DAWproject export error");
            result->status.store(2,std::memory_order_release);
        }
    }).detach();
    return result;
}
}
