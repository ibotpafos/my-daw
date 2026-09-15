#include "audio/effect.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
int main(){try{
    auto catalog=daw::supportedAudioUnits();if(catalog.size()!=3)throw daw::Error("Expected the 3 approved Apple Audio Units");
    for(const auto& component:catalog){
        auto snapshot=daw::snapshotAudioUnit(component);if(snapshot.state.empty())throw daw::Error("Audio Unit returned empty state");
        daw::PluginInsert plugin{1,component.type,component.subtype,component.manufacturer,snapshot.name,false,snapshot.latencyFrames,snapshot.state};auto effect=daw::prepareAudioUnit(plugin);
        std::vector<float> left(512),right(512);left[0]=right[0]=0.25f;bool ok=true;float peak=0;
        for(uint64_t block=0;block<4;++block){ok=effect->process(left.data(),right.data(),512,block*512)&&ok;for(size_t i=0;i<left.size();++i){if(!std::isfinite(left[i])||!std::isfinite(right[i]))throw daw::Error("Audio Unit returned non-finite audio");peak=std::max({peak,std::abs(left[i]),std::abs(right[i])});}std::fill(left.begin(),left.end(),0);std::fill(right.begin(),right.end(),0);}
        if(!ok)throw daw::Error("Audio Unit render failed");std::cout<<"PASS component=\""<<snapshot.name<<"\" state_bytes="<<snapshot.state.size()<<" latency_frames="<<snapshot.latencyFrames<<" peak="<<peak<<'\n';
    }
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
