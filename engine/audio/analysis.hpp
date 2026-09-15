#pragma once
#include "domain/session.hpp"
#include <cstdint>

namespace daw {
// Pre-fader, pre-pan, pre-send track measurements over active timeline frames.
// Overlapping matching crossfades are mixed exactly once before measurement.
struct TrackAnalysis {
    float peakLeft=0;
    float peakRight=0;
    float rmsLeft=0;
    float rmsRight=0;
    uint64_t analyzedFrames=0;
};

TrackAnalysis analyzeTrack(const State&,uint64_t trackID);
}
