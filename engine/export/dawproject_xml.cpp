#include "export/dawproject_xml.hpp"
#include "plugins/plugin_descriptor.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace daw::dawproject {
namespace {
std::string xml(std::string_view value) {
    std::string result;result.reserve(value.size());
    for(const unsigned char c:value){switch(c){case '&':result+="&amp;";break;case '<':result+="&lt;";break;case '>':result+="&gt;";break;case '\"':result+="&quot;";break;case '\'':result+="&apos;";break;case '\t':result+="&#9;";break;case '\n':result+="&#10;";break;case '\r':result+="&#13;";break;default:if(c>=0x20)result+=static_cast<char>(c);}}
    return result;
}
std::string json(std::string_view value) {
    std::string result;result.reserve(value.size()+8);
    for(const unsigned char c:value){switch(c){case '\"':result+="\\\"";break;case '\\':result+="\\\\";break;case '\b':result+="\\b";break;case '\f':result+="\\f";break;case '\n':result+="\\n";break;case '\r':result+="\\r";break;case '\t':result+="\\t";break;default:if(c<0x20){char escaped[7];std::snprintf(escaped,sizeof(escaped),"\\u%04x",c);result+=escaped;}else result+=static_cast<char>(c);}}
    return result;
}
std::string number(double value) {
    if(!std::isfinite(value))throw Error("DAWproject cannot serialize a non-finite number");
    char bytes[64];const auto [end,error]=std::to_chars(bytes,bytes+sizeof(bytes),value,std::chars_format::general);if(error!=std::errc{})throw Error("Cannot serialize DAWproject number");return {bytes,end};
}
std::string frameTime(uint64_t frame){return number(double(frame)/48000.0);}
std::string id(std::string_view prefix,std::string_view value){return std::string(prefix)+std::string(value);}
std::string channelID(std::string_view value){return id("channel-",value);}
std::string volumeID(std::string_view value){return id("volume-",value);}
std::string panID(std::string_view value){return id("pan-",value);}
std::string boolText(bool value){return value?"true":"false";}
std::string normalizedPan(double pan){return number((pan+1.0)*0.5);}
std::string componentID(const ExportMasterPlugin& plugin){
    if(plugin.vst3)return textualVst3Fuid(plugin.vst3ClassFuid);
    char value[32];std::snprintf(value,sizeof(value),"au-%08x-%08x-%08x",plugin.type,plugin.subtype,plugin.manufacturer);return value;
}
void parameter(std::string& out,std::string_view tag,std::string_view parameterID,std::string_view unit,std::string value) {
    out+="<";out+=tag;out+=" id=\"";out+=parameterID;out+="\" name=\"";out+=tag;out+="\" unit=\"";out+=unit;out+="\" value=\"";out+=value;out+="\"/>";
}
void automation(std::string& out,std::string_view laneID,std::string_view trackID,std::string_view target,std::string_view unit,const std::vector<AutomationPoint>& points,bool pan) {
    if(points.empty())return;
    out+="<Points id=\"";out+=laneID;out+="\"";if(!trackID.empty()){out+=" track=\"";out+=trackID;out+="\"";}out+=" timeUnit=\"seconds\" unit=\"";out+=unit;out+="\"><Target parameter=\"";out+=target;out+="\"/>";
    for(const auto& point:points){out+="<RealPoint time=\"";out+=frameTime(point.frame);out+="\" value=\"";out+=pan?normalizedPan(point.gainDb):number(point.gainDb);out+="\" interpolation=\"linear\"/>";}
    out+="</Points>";
}
std::string pluginParameterID(const ExportMasterPlugin& plugin,uint32_t parameterID){return "parameter-plugin-"+plugin.id+"-"+std::to_string(parameterID);}
std::string pluginVendorParameterID(uint32_t parameterID){return std::to_string(static_cast<int32_t>(parameterID));}
void pluginAutomation(std::string& out,const ExportMasterPlugin& plugin,std::string_view trackID){
    for(const auto& lane:plugin.parameterAutomation){if(lane.points.empty())continue;const auto parameterID=pluginParameterID(plugin,lane.parameterID);out+="<Points id=\"automation-plugin-"+plugin.id+"-"+std::to_string(lane.parameterID)+"\" track=\""+std::string(trackID)+"\" timeUnit=\"seconds\" unit=\"normalized\"><Target parameter=\""+parameterID+"\"/>";for(const auto& point:lane.points){out+="<RealPoint time=\""+frameTime(point.frame)+"\" value=\""+number(point.normalizedValue)+"\" interpolation=\"linear\"/>";}out+="</Points>";}
}
void channel(std::string& out,std::string_view objectID,std::string_view name,double gainDb,double pan,bool muted,bool solo,std::string_view destination,std::string_view role,const std::vector<ExportSend>* sends=nullptr,const std::vector<ExportMasterPlugin>* plugins=nullptr) {
    const auto channel=channelID(objectID);out+="<Channel id=\"";out+=channel;out+="\" name=\"";out+=xml(name);out+="\" audioChannels=\"2\" role=\"";out+=role;out+="\" solo=\"";out+=boolText(solo);out+="\"";if(!destination.empty()){out+=" destination=\"";out+=channelID(destination);out+="\"";}out+=">";
    if(plugins&&!plugins->empty()){out+="<Devices>";for(const auto& plugin:*plugins){out+="<";out+=plugin.vst3?"Vst3Plugin":"AuPlugin";out+=" id=\"plugin-";out+=plugin.id;out+="\" name=\"";out+=xml(plugin.name);out+="\" deviceName=\"";out+=xml(plugin.name);out+="\" deviceID=\"";out+=componentID(plugin);out+="\" deviceVendor=\"";out+=xml(plugin.vst3?(plugin.vendor.empty()?"Unknown":plugin.vendor):"Apple");out+="\" deviceRole=\"audioFX\" loaded=\"true\"><Parameters>";for(const auto& lane:plugin.parameterAutomation){out+="<RealParameter id=\""+pluginParameterID(plugin,lane.parameterID)+"\" parameterID=\""+pluginVendorParameterID(lane.parameterID)+"\" name=\""+xml(lane.name.empty()?std::string("Parameter ")+std::to_string(lane.parameterID):lane.name)+"\" unit=\"normalized\" value=\"0\"/>";}out+="</Parameters><Enabled id=\"enabled-plugin-";out+=plugin.id;out+="\" name=\"On/Off\" value=\"";out+=boolText(!plugin.bypassed);out+="\"/>";if(!plugin.statePath.empty()){out+="<State path=\"";out+=xml(plugin.statePath);out+="\" external=\"false\"/>";}out+="</";out+=plugin.vst3?"Vst3Plugin":"AuPlugin";out+=">";}out+="</Devices>";}
    out+="<Mute id=\"mute-";out+=objectID;out+="\" name=\"Mute\" value=\"";out+=boolText(muted);out+="\"/>";
    parameter(out,"Pan",panID(objectID),"normalized",normalizedPan(pan));
    if(sends&&!sends->empty()){out+="<Sends>";for(size_t index=0;index<sends->size();++index){const auto& send=(*sends)[index];out+="<Send id=\"send-";out+=objectID;out+="-";out+=std::to_string(index);out+="\" destination=\"";out+=channelID(send.busID);out+="\" type=\"";out+=send.preFader?"pre":"post";out+="><Volume id=\"send-volume-";out+=objectID;out+="-";out+=std::to_string(index);out+="\" name=\"Volume\" unit=\"decibel\" value=\"";out+=number(send.gainDb);out+="\"/></Send>";}out+="</Sends>";}
    parameter(out,"Volume",volumeID(objectID),"decibel",number(gainDb));out+="</Channel>";
}
void audioClip(std::string& out,const ExportRegion& region,const ExportTrack& track) {
    const auto media=std::find_if(track.media.begin(),track.media.end(),[&](const auto& item){return item.id==region.mediaID;});if(media==track.media.end())throw Error("DAWproject region references missing media");
    out+="<Clip time=\"";out+=frameTime(region.startFrame);out+="\" duration=\"";out+=frameTime(region.lengthFrames);out+="\" contentTimeUnit=\"seconds\" playStart=\"";out+=frameTime(region.sourceOffsetFrames);out+="\" fadeTimeUnit=\"seconds\" fadeInTime=\"";out+=frameTime(region.fadeInFrames);out+="\" fadeOutTime=\"";out+=frameTime(region.fadeOutFrames);out+="><Audio id=\"audio-";out+=track.id;out+="-";out+=std::to_string(region.index);out+="\" duration=\"";out+=frameTime(media->frames);out+="\" channels=\"";out+=std::to_string(media->channels);out+="\" sampleRate=\"";out+=std::to_string(media->sampleRate);out+="><File path=\"audio/";out+=media->id;out+=".wav\" external=\"false\"/></Audio></Clip>";
}
const char* severity(LossSeverity value){switch(value){case LossSeverity::Info:return "info";case LossSeverity::Warning:return "warning";case LossSeverity::Error:return "error";}return "error";}
}

std::string projectXml(const ExportModel& model,std::string_view appVersion) {
    if(appVersion.empty())throw Error("DAWproject application version is required");
    std::string out="<?xml version=\"1.0\" encoding=\"UTF-8\"?><Project version=\"1.0\"><Application name=\"My DAW\" version=\""+xml(appVersion)+"\"/><Transport><Tempo id=\"tempo\" name=\"Tempo\" min=\"20\" max=\"400\" unit=\"bpm\" value=\""+number(model.tempo().bpm)+"\"/><TimeSignature id=\"time-signature\" numerator=\""+std::to_string(model.tempo().numerator)+"\" denominator=\""+std::to_string(model.tempo().denominator)+"\"/></Transport><Structure>";
    for(const auto& track:model.tracks()){out+="<Track id=\"track-"+track.id+"\" name=\""+xml(track.name)+"\" contentType=\"audio\" loaded=\"true\">";channel(out,track.id,track.name,track.gainDb,track.pan,track.muted,track.solo,track.outputBusID.empty()?"master":track.outputBusID,"regular",&track.sends,&track.plugins);out+="</Track>";}
    for(const auto& bus:model.buses()){bool effect=false;for(const auto& track:model.tracks())for(const auto& send:track.sends)if(send.busID==bus.id)effect=true;out+="<Track id=\"bus-"+bus.id+"\" name=\""+xml(bus.name)+"\" contentType=\"audio\" loaded=\"true\">";channel(out,bus.id,bus.name,bus.gainDb,bus.pan,bus.muted,false,bus.outputBusID.empty()?"master":bus.outputBusID,effect?"effect":"submix",nullptr,&bus.plugins);out+="</Track>";}
    out+="<Track id=\"master\" name=\"Master\" contentType=\"audio\" loaded=\"true\">";channel(out,"master","Master",model.master().gainDb,0,false,false,"","master",nullptr,&model.master().plugins);out+="</Track></Structure><Arrangement id=\"arrangement\"><Lanes id=\"arrangement-lanes\" timeUnit=\"seconds\">";
    for(const auto& track:model.tracks()){out+="<Lanes id=\"lanes-track-"+track.id+"\" track=\"track-"+track.id+"\"><Clips id=\"clips-track-"+track.id+"\">";for(const auto& region:track.regions)audioClip(out,region,track);out+="</Clips></Lanes>";automation(out,"automation-volume-"+track.id,"track-"+track.id,volumeID(track.id),"decibel",track.volumeAutomation,false);automation(out,"automation-pan-"+track.id,"track-"+track.id,panID(track.id),"normalized",track.panAutomation,true);for(const auto& plugin:track.plugins)pluginAutomation(out,plugin,"track-"+track.id);}
    for(const auto& bus:model.buses()){automation(out,"automation-volume-"+bus.id,"bus-"+bus.id,volumeID(bus.id),"decibel",bus.gainAutomation,false);for(const auto& plugin:bus.plugins)pluginAutomation(out,plugin,"bus-"+bus.id);}
    automation(out,"automation-volume-master","master",volumeID("master"),"decibel",model.master().gainAutomation,false);
    for(const auto& plugin:model.master().plugins)pluginAutomation(out,plugin,"master");
    out+="</Lanes></Arrangement><Scenes/></Project>";return out;
}
std::string metadataXml(std::string_view title){std::string out="<?xml version=\"1.0\" encoding=\"UTF-8\"?><MetaData>";if(!title.empty())out+="<Title>"+xml(title)+"</Title>";return out+="</MetaData>";}
std::string lossReportJson(const LossReport& report){std::string out="{\"format\":\"mydaw.dawproject-loss-report.v1\",\"items\":[";for(size_t index=0;index<report.items.size();++index){const auto& item=report.items[index];if(index)out+=',';out+="{\"severity\":\"";out+=severity(item.severity);out+="\",\"code\":\"";out+=json(item.code);out+="\",\"objectId\":\"";out+=json(item.objectID);out+="\",\"detail\":\"";out+=json(item.detail);out+="\"}";}out+="]}";return out;}
}
