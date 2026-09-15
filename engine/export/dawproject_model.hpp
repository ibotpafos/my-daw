#pragma once
#include "domain/session.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace daw::dawproject {
enum class LossSeverity : uint8_t { Info, Warning, Error };
struct LossItem { LossSeverity severity=LossSeverity::Info; std::string code; std::string objectID; std::string detail; };
struct LossReport { std::vector<LossItem> items; bool hasErrors() const; };
struct ExportOptions { double tempoBpm=120; uint32_t timeSignatureNumerator=4; uint32_t timeSignatureDenominator=4; };
struct TempoMap { double bpm=120; uint32_t numerator=4; uint32_t denominator=4; };
struct MediaReference { std::string id; uint64_t trackID=0; uint32_t takeIndex=0; uint64_t frames=0; uint32_t sampleRate=48000; uint32_t channels=2; std::shared_ptr<const Clip> audio; };
struct ExportRegion { uint32_t index=0; uint32_t takeIndex=0; uint64_t startFrame=0; uint64_t sourceOffsetFrames=0; uint64_t lengthFrames=0; uint64_t fadeInFrames=0; uint64_t fadeOutFrames=0; std::string mediaID; };
struct ExportSend { std::string busID; double gainDb=0; bool preFader=false; };
struct ExportPluginParameterAutomationLane { uint32_t parameterID=0; std::string name; std::vector<PluginParameterAutomationPoint> points; };
struct ExportMasterPlugin { std::string id; uint32_t type=0; uint32_t subtype=0; uint32_t manufacturer=0; std::string name; bool bypassed=false; uint32_t latencyFrames=0; std::string statePath; std::vector<uint8_t> state; bool vst3=false; std::array<uint8_t,16> vst3ClassFuid{}; std::string vendor; std::vector<ExportPluginParameterAutomationLane> parameterAutomation; };
struct ExportTrack { std::string id; std::string name; double gainDb=0; double pan=0; bool muted=false; bool solo=false; std::string outputBusID; std::vector<AutomationPoint> volumeAutomation; std::vector<AutomationPoint> panAutomation; std::vector<MediaReference> media; std::vector<ExportRegion> regions; std::vector<ExportSend> sends; std::vector<ExportMasterPlugin> plugins; };
struct ExportBus { std::string id; std::string name; double gainDb=0; double pan=0; bool muted=false; std::string outputBusID; std::vector<AutomationPoint> gainAutomation; std::vector<ExportMasterPlugin> plugins; };
struct ExportMaster { double gainDb=0; std::vector<AutomationPoint> gainAutomation; std::vector<ExportMasterPlugin> plugins; };
class ExportModel final { public: const TempoMap& tempo() const{return tempo_;} const std::vector<ExportTrack>& tracks() const{return tracks_;} const std::vector<ExportBus>& buses() const{return buses_;} const ExportMaster& master() const{return master_;} const LossReport& losses() const{return losses_;} private: friend ExportModel makeExportModel(const State&,const ExportOptions&); ExportModel(TempoMap tempo,std::vector<ExportTrack> tracks,std::vector<ExportBus> buses,ExportMaster master,LossReport losses):tempo_(std::move(tempo)),tracks_(std::move(tracks)),buses_(std::move(buses)),master_(std::move(master)),losses_(std::move(losses)){} const TempoMap tempo_; const std::vector<ExportTrack> tracks_; const std::vector<ExportBus> buses_; const ExportMaster master_; const LossReport losses_; };
ExportModel makeExportModel(const State& state,const ExportOptions& options = {});
}
