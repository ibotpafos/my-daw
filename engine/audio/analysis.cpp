#include "audio/analysis.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
namespace {
const Clip& sourceFor(const Track& track,const Region& region) {
    if(region.take==0) return *track.audio;
    return *track.takes[region.take-1].audio;
}
float envelope(const Region& region,uint64_t local) {
    float value=1;
    if(region.fadeIn)value=std::min(value,region.fadeIn==1?0.0f:float(local)/float(region.fadeIn-1));
    if(region.fadeOut)value=std::min(value,region.fadeOut==1?0.0f:float(region.length-1-local)/float(region.fadeOut-1));
    return value;
}
}

TrackAnalysis analyzeTrack(const State& state,uint64_t trackID) {
    validate(state);
    const auto found=std::find_if(state.tracks.begin(),state.tracks.end(),[&](const auto& track){return track.id==trackID;});
    if(found==state.tracks.end())throw Error("Track not found");
    const auto& track=*found;TrackAnalysis result;
    if(!track.audio)return result;
    double leftEnergy=0,rightEnergy=0;
    auto observe=[&](float left,float right){
        if(!std::isfinite(left)||!std::isfinite(right))throw Error("Non-finite track sample during analysis");
        result.peakLeft=std::max(result.peakLeft,std::abs(left));result.peakRight=std::max(result.peakRight,std::abs(right));
        leftEnergy+=double(left)*left;rightEnergy+=double(right)*right;++result.analyzedFrames;
    };
    auto sample=[&](const Region& region,uint64_t local){
        const auto& pcm=sourceFor(track,region).samples();const auto offset=(region.sourceOffset+local)*2;const auto gain=envelope(region,local);
        return std::pair{pcm[offset]*gain,pcm[offset+1]*gain};
    };
    uint64_t skippedHead=0;
    for(size_t index=0;index<track.regions.size();++index){
        const auto& region=track.regions[index];uint64_t overlap=0;
        if(index+1<track.regions.size()){const auto& following=track.regions[index+1];const auto end=region.start+region.length;if(following.start<end)overlap=end-following.start;}
        const auto normalEnd=region.length-overlap;
        for(uint64_t local=skippedHead;local<normalEnd;++local){const auto [left,right]=sample(region,local);observe(left,right);}
        if(overlap){const auto& following=track.regions[index+1];for(uint64_t frame=0;frame<overlap;++frame){const auto [leftA,rightA]=sample(region,normalEnd+frame);const auto [leftB,rightB]=sample(following,frame);observe(leftA+leftB,rightA+rightB);}}
        skippedHead=overlap;
    }
    if(!result.analyzedFrames)return result;
    result.rmsLeft=static_cast<float>(std::sqrt(leftEnergy/double(result.analyzedFrames)));result.rmsRight=static_cast<float>(std::sqrt(rightEnergy/double(result.analyzedFrames)));
    if(!std::isfinite(result.rmsLeft)||!std::isfinite(result.rmsRight))throw Error("Non-finite track analysis result");
    return result;
}
}
