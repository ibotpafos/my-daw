#include "audio/renderer.hpp"
#include "audio/input.hpp"
#include "daw.h"
#include <cmath>
#include <algorithm>
#include <sqlite3.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) throw std::runtime_error("Failed: " #x); } while(false)
template<class Fn> void rejects(Fn fn) { bool bad=false; try{fn();}catch(...){bad=true;} CHECK(bad); }
std::vector<unsigned char> wav() {
    std::vector<unsigned char> b(44+9600); std::memcpy(b.data(),"RIFF",4); std::memcpy(b.data()+8,"WAVEfmt ",8); std::memcpy(b.data()+36,"data",4);
    auto put=[&](size_t p,uint32_t v,int n){for(int i=0;i<n;++i)b[p+i]=static_cast<unsigned char>(v>>(8*i));};
    put(4,uint32_t(b.size()-8),4);put(16,16,4);put(20,1,2);put(22,1,2);put(24,48000,4);put(28,96000,4);put(32,2,2);put(34,16,2);put(40,9600,4);
    for(size_t p=44;p<b.size();p+=2)put(p,8192,2);
    return b;
}
std::vector<unsigned char> wavAtRate(uint32_t rate, uint32_t frames, uint16_t channels = 1) {
    const uint32_t align = channels * 2;
    std::vector<unsigned char> b(44 + size_t(frames) * align);
    std::memcpy(b.data(), "RIFF", 4); std::memcpy(b.data() + 8, "WAVEfmt ", 8); std::memcpy(b.data() + 36, "data", 4);
    auto put=[&](size_t p,uint32_t v,int n){for(int i=0;i<n;++i)b[p+i]=static_cast<unsigned char>(v>>(8*i));};
    put(4,uint32_t(b.size()-8),4); put(16,16,4); put(20,1,2); put(22,channels,2); put(24,rate,4);
    put(28,rate*align,4); put(32,align,2); put(34,16,2); put(40,uint32_t(frames)*align,4);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (uint16_t channel = 0; channel < channels; ++channel) {
            const int16_t sample = channels == 2 && channel == 1 ? -4096 : 8192;
            put(44 + (size_t(frame) * channels + channel) * 2, static_cast<uint16_t>(sample), 2);
        }
    }
    return b;
}
int main(){try{
    daw::CaptureBuffer capture(4); float captured[]={0.25f,-0.5f,NAN,20.0f,0.75f};
    capture.writeMono(captured,5); CHECK(capture.frames()==4 && capture.overflowed());
    auto capturedClip=capture.finish(); CHECK(capturedClip->samples()==std::vector<float>({0.25f,0.25f,-0.5f,-0.5f,0,0,16,16}));
    rejects([]{daw::CaptureBuffer invalid(0);});
    auto bytes=wav(); auto clip=daw::decodeWav(bytes); CHECK(clip->frames()==4800); CHECK(clip->samples()[0]==0.25f && clip->samples()[1]==0.25f);
    // Validate every advertised encoding with signed boundary samples.
    for(auto bits : {16,24,32}) {
        auto integer=bytes; const unsigned width=unsigned(bits/8); integer.resize(44+width);
        auto put=[&](size_t p,uint32_t value,int count){for(int i=0;i<count;++i)integer[p+i]=static_cast<unsigned char>(value>>(8*i));};
        if(width&1) integer.push_back(0);
        put(4,uint32_t(integer.size()-8),4);put(28,48000*width,4);put(32,width,2);put(34,unsigned(bits),2);put(40,width,4);put(44,uint32_t(1)<<(bits-1),int(width));
        CHECK(daw::decodeWav(integer)->samples()[0]==-1.0f);
        if(bits==32){put(20,3,2);put(44,0x3e800000,4);CHECK(daw::decodeWav(integer)->samples()[0]==0.25f);put(44,0x7fc00000,4);rejects([&]{daw::decodeWav(integer);});}
    }
    auto truncated=bytes; truncated.pop_back(); rejects([&]{daw::decodeWav(truncated);});
    auto wrongRate=bytes; wrongRate[24]=0x44; wrongRate[25]=0xac; rejects([&]{daw::decodeWav(wrongRate);});
    auto wrongAlign=bytes; wrongAlign[32]=7; rejects([&]{daw::decodeWav(wrongAlign);});
    // All product-supported source rates become deterministic stereo 48 kHz frames.
    for (uint32_t rate : {44100u, 48000u, 88200u, 96000u, 192000u}) {
        const uint32_t sourceFrames = rate;
#ifndef __APPLE__
        if (rate != 48000) { rejects([&]{daw::decodeWav(wavAtRate(rate, sourceFrames));}); continue; }
#endif
        auto converted = daw::decodeWav(wavAtRate(rate, sourceFrames, rate == 96000 ? 2 : 1));
        CHECK(converted->frames() == 48000);
        for (float value : converted->samples()) CHECK(std::isfinite(value) && std::abs(value) <= 16);
        const size_t middle = converted->frames() / 2;
        if (rate == 96000) CHECK(converted->samples()[middle * 2] > 0 && converted->samples()[middle * 2 + 1] < 0);
        else CHECK(converted->samples()[middle * 2] == converted->samples()[middle * 2 + 1]);
    }
    rejects([&]{daw::decodeWav(wavAtRate(32000, 320));});
    rejects([&]{daw::decodeWav(wavAtRate(0, 1));});
#ifdef __APPLE__
    CHECK(daw::decodeWav(wavAtRate(192000, 1))->frames() == 1);
#else
    rejects([&]{daw::decodeWav(wavAtRate(192000, 1));});
#endif
    // Source duration is checked before conversion, including high-rate PCM.
    rejects([&]{daw::decodeWav(wavAtRate(44100, 44100 * 60 + 1));});
    CHECK(clip->peaks().size()==512); for(auto peak:clip->peaks()) CHECK(peak==0.25f);
    std::vector<float> impulse(2000,0); impulse[1500]=0.5f;
    auto transient=std::make_shared<const daw::Clip>(std::move(impulse));
    CHECK(*std::max_element(transient->peaks().begin(),transient->peaks().end())==0.5f);
    daw::Session seekSession;seekSession.import("Transient",transient,0);
    daw::Renderer seekRenderer;seekRenderer.prepare(seekSession.state(),700);seekRenderer.playing=true;
    std::vector<float> seekLeft(512),seekRight(512);seekRenderer.render(seekLeft.data(),seekRight.data(),512);
    CHECK(seekLeft[49]==0 && seekLeft[50]>0 && seekRight[50]==0 && seekLeft[300]==0);
    CHECK(seekRenderer.position==1000 && !seekRenderer.playing);
    rejects([&]{seekRenderer.prepare(seekSession.state(),1001);});CHECK(seekRenderer.position==1000);
    seekRenderer.prepare(seekSession.state(),1000);seekRenderer.playing=true;seekRenderer.render(seekLeft.data(),seekRight.data(),512);
    CHECK(seekLeft[50]==0 && !seekRenderer.playing);
    std::vector<float> loopSamples(20);for(size_t i=0;i<10;++i)loopSamples[i*2]=loopSamples[i*2+1]=float(i+1)/20.0f;
    daw::Session loopSession;loopSession.import("Loop",std::make_shared<const daw::Clip>(std::move(loopSamples)),0);
    daw::Renderer loopRenderer;loopRenderer.prepare(loopSession.state(),5,3,7);loopRenderer.playing=true;
    std::vector<float> loopLeft(6),loopRight(6);loopRenderer.render(loopLeft.data(),loopRight.data(),6);
    const std::array<size_t,6> loopTimeline{5,6,3,4,5,6};float loopSmooth=0;
    for(size_t i=0;i<loopTimeline.size();++i){loopSmooth+=(1-loopSmooth)*0.004166667f;CHECK(std::abs(loopLeft[i]-(float(loopTimeline[i]+1)/20.0f)*loopSmooth)<0.000001f&&loopLeft[i]==loopRight[i]);}
    CHECK(loopRenderer.position==7&&loopRenderer.playing);loopLeft.resize(1);loopRight.resize(1);loopRenderer.render(loopLeft.data(),loopRight.data(),1);CHECK(loopRenderer.position==4&&loopRenderer.playing);
    rejects([&]{loopRenderer.prepare(loopSession.state(),0,7,3);});rejects([&]{loopRenderer.prepare(loopSession.state(),0,3,11);});
    seekSession.editClip(1,0,200,700,100,1);
    seekRenderer.prepare(seekSession.state()); seekRenderer.playing=true; seekRenderer.render(seekLeft.data(),seekRight.data(),512);
    CHECK(seekLeft[199]==0 && seekLeft[249]==0 && seekLeft[250]>0 && seekLeft[300]==0);
    CHECK(seekRenderer.duration()==300 && seekRenderer.position==300);
    auto editRevision=seekSession.state().revision;
    seekSession.editClip(1,0,200,700,100,editRevision);CHECK(seekSession.state().revision==editRevision);
    rejects([&]{seekSession.editClip(1,0,0,950,100,editRevision);});
    rejects([&]{seekSession.editClip(1,0,0,0,0,editRevision);});
    rejects([&]{seekSession.editClip(1,0,UINT64_MAX,0,100,editRevision);});
    rejects([&]{seekSession.editClip(1,0,0,UINT64_MAX,100,editRevision);});
    rejects([&]{seekSession.editClip(1,0,0,0,UINT64_MAX,editRevision);});
    rejects([&]{seekSession.editClip(1,0,0,0,100,0);});
    CHECK(seekSession.state().revision==editRevision);
    seekSession.undo(editRevision);CHECK(seekSession.state().tracks[0].regions[0].start==0 && seekSession.state().tracks[0].regions[0].length==1000);
    seekSession.redo(seekSession.state().revision);CHECK(seekSession.state().tracks[0].regions[0].sourceOffset==700);
    seekSession.splitClip(1,0,250,seekSession.state().revision);
    CHECK(seekSession.state().tracks[0].regions==std::vector<daw::Region>({{200,700,50},{250,750,50}}));
    seekRenderer.prepare(seekSession.state()); seekRenderer.playing=true; seekRenderer.render(seekLeft.data(),seekRight.data(),512);
    CHECK(seekLeft[249]==0 && seekLeft[250]>0 && seekLeft[299]==0); // split preserves the source/timeline mapping
    rejects([&]{seekSession.splitClip(1,0,200,seekSession.state().revision);});
    rejects([&]{seekSession.splitClip(1,1,300,seekSession.state().revision);});
    auto splitRevision=seekSession.state().revision; seekSession.undo(splitRevision); CHECK(seekSession.state().tracks[0].regions.size()==1);
    seekSession.redo(seekSession.state().revision); CHECK(seekSession.state().tracks[0].regions.size()==2);
    daw::Session clipOps; clipOps.import("Ops",transient,0); clipOps.splitClip(1,0,500,1);
    clipOps.setClipFades(1,0,10,20,2); CHECK(clipOps.state().tracks[0].regions[0].fadeIn==10);
    clipOps.editClip(1,0,0,0,15,3); CHECK(clipOps.state().tracks[0].regions[0].fadeIn==10 && clipOps.state().tracks[0].regions[0].fadeOut==5);
    clipOps.undo(4);
    rejects([&]{clipOps.setClipFades(1,0,300,300,5);}); CHECK(clipOps.state().revision==5);
    clipOps.duplicateClip(1,0,5); CHECK(clipOps.state().tracks[0].regions.size()==3 && clipOps.state().tracks[0].regions.back().start==1000);
    clipOps.deleteClip(1,1,6); CHECK(clipOps.state().tracks[0].regions.size()==2);
    clipOps.deleteClip(1,1,7); rejects([&]{clipOps.deleteClip(1,0,8);});
    clipOps.undo(8); CHECK(clipOps.state().tracks[0].regions.size()==2);
    auto constantClip=std::make_shared<const daw::Clip>(std::vector<float>(2000,0.5f));
    daw::Session crossfade;crossfade.import("X",constantClip,0);crossfade.splitClip(1,0,500,1);crossfade.setCrossfade(1,0,100,2);
    CHECK(crossfade.state().tracks[0].regions==std::vector<daw::Region>({{0,0,500,0,100},{400,400,600,100,0}}));
    crossfade.setCrossfade(1,0,100,3);CHECK(crossfade.state().revision==3);rejects([&]{crossfade.setCrossfade(1,0,1,3);});
    daw::Renderer crossfadeRenderer;crossfadeRenderer.prepare(crossfade.state());crossfadeRenderer.playing=true;std::vector<float> crossLeft(1000),crossRight(1000);crossfadeRenderer.render(crossLeft.data(),crossRight.data(),1000);
    for(size_t i=400;i<500;++i){const auto smoothed=1.0f-std::pow(1.0f-0.004166667f,float(i+1));CHECK(std::abs(crossLeft[i]-0.5f*smoothed)<0.00002f);}
    crossfade.setCrossfade(1,0,0,3);CHECK(crossfade.state().tracks[0].regions==std::vector<daw::Region>({{0,0,500},{500,500,500}}));
    crossfade.undo(4);CHECK(crossfade.state().tracks[0].regions[1].start==400);crossfade.redo(5);CHECK(crossfade.state().tracks[0].regions[1].start==500);
    crossfade.undo(6);CHECK(crossfade.state().tracks[0].regions[1].start==400);rejects([&]{crossfade.setCrossfade(1,0,600,7);});
    // Per-region clip gain folds into the voice mix: -6.0206 dB halves the level.
    {   daw::Session clipGain;clipGain.import("G",constantClip,0);
        clipGain.setClipGain(1,0,-6.020599913,1);CHECK(clipGain.state().tracks[0].regions[0].gain==-6.020599913);
        daw::Renderer gainRenderer;gainRenderer.prepare(clipGain.state());gainRenderer.playing=true;
        std::vector<float> gainLeft(300),gainRight(300);gainRenderer.render(gainLeft.data(),gainRight.data(),300);
        for(size_t i=0;i<300;++i){const auto smoothed=1.0f-std::pow(1.0f-0.004166667f,float(i+1));CHECK(std::abs(gainLeft[i]-0.25f*smoothed)<0.00002f&&gainLeft[i]==gainRight[i]);}
        clipGain.setClipGain(1,0,-6.020599913,2);CHECK(clipGain.state().revision==2);  // identical: silent no-op
        rejects([&]{clipGain.setClipGain(1,0,20.0,2);});
    }
    // An explicit fade-in ramp rides the same mix-in: env = local / (fadeIn - 1).
    {   daw::Session fades;fades.import("F",constantClip,0);fades.setClipFades(1,0,100,0,1);
        daw::Renderer fadeRenderer;fadeRenderer.prepare(fades.state());fadeRenderer.playing=true;
        std::vector<float> fadeLeft(200),fadeRight(200);fadeRenderer.render(fadeLeft.data(),fadeRight.data(),200);
        for(size_t i=0;i<200;++i){const auto smoothed=1.0f-std::pow(1.0f-0.004166667f,float(i+1));const auto env=i<99?float(i)/99.0f:1.0f;CHECK(std::abs(fadeLeft[i]-0.5f*smoothed*env)<0.00002f);}
    }
    // A muted region contributes no voice at all: pure silence in its window.
    {   daw::Session muteState;muteState.import("M",constantClip,0);            // region [0,1000) frames
        muteState.duplicateClip(1,0,1);                                          // rev 2: [1000,2000)
        muteState.setClipMuted(1,1,true,2);                                      // rev 3: mute the copy
        daw::Renderer muteRenderer;muteRenderer.prepare(muteState.state());muteRenderer.playing=true;
        std::vector<float> muteLeft(1500),muteRight(1500);muteRenderer.render(muteLeft.data(),muteRight.data(),1500);
        for(size_t i=0;i<300;++i){const auto smoothed=1.0f-std::pow(1.0f-0.004166667f,float(i+1));CHECK(std::abs(muteLeft[i]-0.5f*smoothed)<0.00002f);}
        { const auto smoothed=1.0f-std::pow(1.0f-0.004166667f,1000.0f);CHECK(std::abs(muteLeft[999]-0.5f*smoothed)<0.00002f&&std::abs(muteLeft[998]-muteLeft[997])<0.01f); }
        for(size_t i=1000;i<1500;++i)CHECK(muteLeft[i]==0.0f&&muteRight[i]==0.0f);
        muteState.setClipMuted(1,0,true,3);                                      // rev 4: everything muted
        rejects([&]{daw::Renderer dead;dead.prepare(muteState.state());});       // an all-muted project has nothing to play
        muteState.undo(4);CHECK(!muteState.state().tracks[0].regions[0].muted&&muteState.state().tracks[0].regions[1].muted);
    }
    // Clip pan rides the voice with the unity-center linear law of track pan:
    // hard side zeroes the other channel exactly, mid values scale one side.
    {   daw::Session panState;panState.import("P",constantClip,0);
        panState.setClipPan(1,0,-1.0,1);                                           // rev 2: hard left
        daw::Renderer panRenderer;panRenderer.prepare(panState.state());panRenderer.playing=true;
        std::vector<float> panLeft(1100),panRight(1100);panRenderer.render(panLeft.data(),panRight.data(),1100);
        { const auto smoothed=1.0f-std::pow(1.0f-0.004166667f,1000.0f);
          CHECK(panRight[999]==0.0f&&std::abs(panLeft[999]-0.5f*smoothed)<0.00002f);
          CHECK(panRight[400]==0.0f&&std::abs(panLeft[400]-0.5f*(1.0f-std::pow(1.0f-0.004166667f,401.0f)))<0.00002f); }
        panState.setClipPan(1,0,0.25,2);                                           // rev 3: L x0.75, R x1
        daw::Renderer pan2;pan2.prepare(panState.state());pan2.playing=true;
        pan2.render(panLeft.data(),panRight.data(),1100);
        { const auto smoothed=1.0f-std::pow(1.0f-0.004166667f,1000.0f);
          CHECK(std::abs(panLeft[999]-0.5f*0.75f*smoothed)<0.00002f&&std::abs(panRight[999]-0.5f*smoothed)<0.00002f); }
    }
    // A looped region reads [sourceOffset, frames) repeatedly: past the slice
    // end the ramp restarts from zero instead of falling silent.
    {   std::vector<float> ramp;for(size_t frame=0;frame<1000;++frame){ramp.push_back(float(frame)*0.001f);ramp.push_back(float(frame)*0.001f);}
        auto rampClip=std::make_shared<const daw::Clip>(std::move(ramp));        // 1000 stereo frames, L==R
        daw::Session loop;loop.import("L",rampClip,0);loop.setClipLooped(1,0,true,1);
        loop.editClip(1,0,0,0,1500,2);CHECK(loop.state().tracks[0].regions[0].length==1500);
        daw::Renderer loopRenderer;loopRenderer.prepare(loop.state());loopRenderer.playing=true;
        std::vector<float> loopLeft(1500),loopRight(1500);loopRenderer.render(loopLeft.data(),loopRight.data(),1500);
        for(size_t i=0;i<1500;++i){const auto smoothed=1.0f-std::pow(1.0f-0.004166667f,float(i+1));const auto level=float(i<1000?i:i-1000)*0.001f;CHECK(std::abs(loopLeft[i]-level*smoothed)<0.00002f&&loopLeft[i]==loopRight[i]);}
    }
    std::vector<float> mixA(6000),mixB(6000);for(size_t i=0;i<3000;++i){mixA[i*2]=0.4f;mixA[i*2+1]=0.2f;mixB[i*2]=0.1f;mixB[i*2+1]=0.3f;}
    daw::Session mixer;mixer.import("A",std::make_shared<const daw::Clip>(std::move(mixA)),0);mixer.import("B",std::make_shared<const daw::Clip>(std::move(mixB)),1);mixer.pan(1,-1,2);mixer.pan(2,1,3);mixer.masterGain(-6.020599913,4);
    auto mixedLast=[&]{daw::Renderer r;r.prepare(mixer.state());r.playing=true;std::vector<float> l(2048),rr(2048);r.render(l.data(),rr.data(),2048);return std::pair{l.back(),rr.back()};};
    auto stereo=mixedLast();CHECK(std::abs(stereo.first-0.2f)<0.0001f&&std::abs(stereo.second-0.15f)<0.0001f);
    mixer.solo(2,true,5);auto soloed=mixedLast();CHECK(std::abs(soloed.first)<0.0001f&&std::abs(soloed.second-0.15f)<0.0001f);
    mixer.mute(2,true,6);auto muted=mixedLast();CHECK(std::abs(muted.first)<0.0001f&&std::abs(muted.second)<0.0001f);mixer.undo(7);CHECK(!mixer.state().tracks[1].muted);
    auto routedClip=std::make_shared<const daw::Clip>(std::vector<float>(6000,0.5f));daw::Session routed;routed.import("Lead",routedClip,0);routed.addBus("Vocal Bus",1);routed.routeTrack(1,2,2);routed.busGain(2,-6.020599913,3);
    daw::Renderer routedRenderer;routedRenderer.prepare(routed.state());routedRenderer.playing=true;std::vector<float> routedLeft(2048),routedRight(2048);routedRenderer.render(routedLeft.data(),routedRight.data(),2048);CHECK(std::abs(routedLeft.back()-0.25f)<0.0002f&&std::abs(routedRight.back()-0.25f)<0.0002f);
    daw::Session sendMix;sendMix.import("Dry",routedClip,0);sendMix.addBus("Parallel",1);sendMix.upsertSend(1,2,-6.020599913,false,2);daw::Renderer sendRenderer;sendRenderer.prepare(sendMix.state());sendRenderer.playing=true;std::vector<float> sendLeft(2048),sendRight(2048);sendRenderer.render(sendLeft.data(),sendRight.data(),2048);CHECK(std::abs(sendLeft.back()-0.75f)<0.0003f&&std::abs(sendRight.back()-0.75f)<0.0003f);
    sendMix.gain(1,-6.020599913,3);sendMix.upsertSend(1,2,-6.020599913,true,4);sendRenderer.prepare(sendMix.state());sendRenderer.playing=true;sendRenderer.render(sendLeft.data(),sendRight.data(),2048);CHECK(std::abs(sendLeft.back()-0.5f)<0.0003f);sendMix.mute(1,true,5);sendRenderer.updateMix(sendMix.state());sendRenderer.render(sendLeft.data(),sendRight.data(),2048);CHECK(std::abs(sendLeft.back())<0.0003f);
#ifdef __APPLE__
    auto auCatalog=daw::supportedAudioUnits();CHECK(auCatalog.size()==3);auto auSnapshot=daw::snapshotAudioUnit(auCatalog.front());daw::Session auMix;auMix.import("AU source",routedClip,0);auMix.addMasterInsert({0,auCatalog.front().type,auCatalog.front().subtype,auCatalog.front().manufacturer,auSnapshot.name,false,auSnapshot.latencyFrames,auSnapshot.state},1);daw::Renderer auRenderer;auRenderer.prepare(auMix.state());auRenderer.playing=true;std::vector<float> auLeft(2048),auRight(2048);auRenderer.render(auLeft.data(),auRight.data(),2048);CHECK(auRenderer.pluginErrors==0&&std::isfinite(auLeft.back())&&std::abs(auLeft.back())>0.01f);
#endif
    auto baseTake=std::make_shared<const daw::Clip>(std::vector<float>(2000,0.1f));auto alternateTake=std::make_shared<const daw::Clip>(std::vector<float>(2000,0.8f));daw::Session comp;comp.import("Lead",baseTake,0);comp.addTake(1,"Take 2",alternateTake,0,1);comp.compRange(1,1,200,300,2);
    CHECK(comp.state().tracks[0].takes.size()==1&&comp.state().tracks[0].regions==std::vector<daw::Region>({{0,0,200},{200,200,300,0,0,1},{500,500,500}}));
    daw::Renderer compRenderer;compRenderer.prepare(comp.state());compRenderer.playing=true;std::vector<float> compLeft(1000),compRight(1000);compRenderer.render(compLeft.data(),compRight.data(),1000);CHECK(compLeft[100]<0.05f&&compLeft[350]>0.6f&&compLeft[700]<0.2f);
    comp.setClipFades(1,1,10,10,3);CHECK(comp.state().tracks[0].regions[1].take==1);comp.splitClip(1,1,350,4);CHECK(comp.state().tracks[0].regions[1].take==1&&comp.state().tracks[0].regions[2].take==1);comp.undo(5);comp.undo(6);comp.undo(7);CHECK(comp.state().tracks[0].regions.size()==1);comp.redo(8);CHECK(comp.state().tracks[0].regions[1].take==1);
    rejects([&]{comp.compRange(1,2,0,10,9);});rejects([&]{comp.compRange(1,1,900,200,9);});
    daw::Session passBatch;passBatch.import("Loop",baseTake,0);std::vector<daw::Take> passTakes{{"Pass 1",100,alternateTake},{"Pass 2",100,alternateTake}};passBatch.addTakes(1,std::move(passTakes),1);CHECK(passBatch.state().revision==2&&passBatch.state().tracks[0].takes.size()==2);passBatch.undo(2);CHECK(passBatch.state().tracks[0].takes.empty());passBatch.redo(3);CHECK(passBatch.state().tracks[0].takes.size()==2);
    daw::Session manyClips;manyClips.import("Many",transient,0);for(int i=1;i<256;++i)manyClips.duplicateClip(1,0,manyClips.state().revision);
    CHECK(manyClips.state().tracks[0].regions.size()==256);rejects([&]{manyClips.duplicateClip(1,0,manyClips.state().revision);});
    std::vector<float> shortSamples(200,0.25f); auto shortClip=std::make_shared<const daw::Clip>(std::move(shortSamples));
    daw::Session faded;faded.import("Fade",shortClip,0);faded.setClipFades(1,0,10,10,1);daw::Renderer fadeRenderer;fadeRenderer.prepare(faded.state());fadeRenderer.playing=true;
    std::vector<float> fadeLeft(128),fadeRight(128);fadeRenderer.render(fadeLeft.data(),fadeRight.data(),128);CHECK(fadeLeft[0]==0 && fadeLeft[10]>fadeLeft[5] && fadeLeft[99]==0);
    auto pcm=daw::encodePCM(*clip); CHECK(daw::decodePCM(pcm)->samples()==clip->samples());
    auto invalid=pcm; invalid[0]=0;invalid[1]=0;invalid[2]=0xc0;invalid[3]=0x7f;rejects([&]{daw::decodePCM(invalid);});
    daw::Session placed; placed.importAt("Take",capturedClip,12000,0); CHECK(placed.state().tracks[0].regions[0].start==12000); placed.undo(1); CHECK(placed.state().tracks.empty());
    daw::Session session; session.import("Audio",clip,0); session.gain(1,-6.020599913,1);
    daw::Renderer renderer; renderer.prepare(session.state()); renderer.playing=true;
    std::vector<float> left(512),right(512);
    for(int i=0;i<8;++i) renderer.render(left.data(),right.data(),512);
    CHECK(std::abs(left.back()-0.125f)<0.0001f); CHECK(left==right); CHECK(renderer.position==4096);
    renderer.render(left.data(),right.data(),512); renderer.render(left.data(),right.data(),512);
    CHECK(renderer.position==4800 && !renderer.playing); CHECK(left[191]>0 && left[192]==0);
    renderer.render(left.data(),right.data(),512); CHECK(left[0]==0);
    session.gain(1,24,2); renderer.prepare(session.state()); renderer.playing=true;
    for(int i=0;i<8;++i)renderer.render(left.data(),right.data(),512);
    CHECK(renderer.clipped>0 && left.back()==1);
    session.gain(1,-120,3); renderer.updateGains(session.state());
    renderer.render(left.data(),right.data(),512); CHECK(left.back()<0.6f);
    renderer.playing=false; renderer.render(left.data(),right.data(),512);CHECK(left.back()==0);
    session.undo(4); CHECK(session.state().tracks[0].gain==24); session.undo(5);session.undo(6);session.undo(7); CHECK(session.state().tracks.empty());
    session.redo(8); CHECK(session.state().tracks[0].audio==clip);
    auto path=(std::filesystem::temp_directory_path()/("mydaw-audio-"+std::to_string(getpid())+".mydawdraft")).string();
    struct Cleanup{std::string p,q,r;~Cleanup(){std::filesystem::remove(p);if(!q.empty())std::filesystem::remove(q);if(!r.empty())std::filesystem::remove(r);}} cleanup{path,{},{}};
    daw::writeDraft(passBatch.state(),path);auto loadedPassBatch=daw::readDraft(path);CHECK(loadedPassBatch.tracks[0].takes.size()==2&&loadedPassBatch.tracks[0].takes[0].start==100&&loadedPassBatch.tracks[0].takes[1].audio->samples()==alternateTake->samples());
    daw::writeDraft(session.state(),path); auto loaded=daw::readDraft(path); CHECK(loaded.tracks[0].audio->samples()==clip->samples());
    auto compPath=path+".comp";cleanup.q=compPath;daw::writeDraft(comp.state(),compPath);auto loadedComp=daw::readDraft(compPath);CHECK(loadedComp.tracks[0].takes.size()==1&&loadedComp.tracks[0].takes[0].audio->samples()==alternateTake->samples()&&loadedComp.tracks[0].regions[1].take==1);
    sqlite3* compDb=nullptr;CHECK(sqlite3_open(compPath.c_str(),&compDb)==SQLITE_OK);CHECK(sqlite3_exec(compDb,"UPDATE regions SET take_index=99 WHERE position=1",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(compDb);rejects([&]{daw::readDraft(compPath);});
    auto takeWav=path+".wav";cleanup.r=takeWav;{std::ofstream file(takeWav,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));}std::unique_ptr<daw_session,decltype(&daw_destroy)> takeBridge(daw_create(),daw_destroy);CHECK(takeBridge&&daw_open_draft(takeBridge.get(),path.c_str())==0);daw_snapshot takeSnapshot{};takeSnapshot.struct_size=sizeof(takeSnapshot);CHECK(daw_get_snapshot(takeBridge.get(),&takeSnapshot)==0);CHECK(daw_import_take_wav(takeBridge.get(),1,takeWav.c_str(),"Bridge take",0,takeSnapshot.revision)==0);daw_track takeTrack{};takeTrack.struct_size=sizeof(takeTrack);CHECK(daw_get_track(takeBridge.get(),0,&takeTrack)==0&&takeTrack.take_count==2);daw_take takeInfo{};takeInfo.struct_size=sizeof(takeInfo);CHECK(daw_get_take(takeBridge.get(),1,1,&takeInfo)==0&&takeInfo.frames==4800);CHECK(daw_comp_range(takeBridge.get(),1,1,100,200,takeSnapshot.revision+1)==0);daw_clip takeRegion{};takeRegion.struct_size=sizeof(takeRegion);CHECK(daw_get_clip(takeBridge.get(),1,1,&takeRegion)==0&&takeRegion.take_index==1&&takeRegion.source_offset==100);
    daw::Renderer restored;restored.prepare(loaded);restored.playing=true;restored.render(left.data(),right.data(),512);CHECK(left.back()>0.2f);
    std::unique_ptr<daw_session,decltype(&daw_destroy)> bridge(daw_create(),daw_destroy);CHECK(bridge);
    daw_recording idle{};idle.struct_size=sizeof(idle);CHECK(daw_get_recording(bridge.get(),&idle)==0 && !idle.recording && !idle.frames);
    CHECK(daw_open_draft(bridge.get(),path.c_str())==0);
    std::array<float,512> waveform{};CHECK(daw_get_waveform(bridge.get(),1,waveform.data(),512)==0 && waveform[0]==0.25f);
    CHECK(daw_get_waveform(bridge.get(),999,waveform.data(),512)==1);
    CHECK(daw_get_waveform(bridge.get(),1,nullptr,512)==1);
    CHECK(daw_get_waveform(bridge.get(),1,waveform.data(),511)==1);
    daw_snapshot before{};before.struct_size=sizeof(before);CHECK(daw_get_snapshot(bridge.get(),&before)==0);
    CHECK(daw_seek_frame(bridge.get(),4000)==0);
    daw_transport t{};t.struct_size=sizeof(t);CHECK(daw_get_transport(bridge.get(),&t)==0 && t.frame==4000 && t.duration==4800 && !t.playing);
    CHECK(daw_seek_frame(bridge.get(),4801)==1);
    CHECK(daw_get_transport(bridge.get(),&t)==0 && t.frame==4000);
    CHECK(daw_set_loop(bridge.get(),1,400,800)==0);CHECK(daw_get_transport(bridge.get(),&t)==0&&t.loop_enabled&&t.loop_start==400&&t.loop_end==800&&t.frame==400);
    CHECK(daw_set_loop(bridge.get(),1,800,400)==1);CHECK(daw_get_transport(bridge.get(),&t)==0&&t.loop_enabled);
    CHECK(daw_set_loop(bridge.get(),0,99,1)==0);CHECK(daw_get_transport(bridge.get(),&t)==0&&!t.loop_enabled&&!t.loop_start&&!t.loop_end);
    daw_snapshot after{};after.struct_size=sizeof(after);CHECK(daw_get_snapshot(bridge.get(),&after)==0 && before.revision==after.revision);
    CHECK(daw_open_draft(bridge.get(),path.c_str())==0);CHECK(daw_get_transport(bridge.get(),&t)==0 && t.frame==0);
    CHECK(daw_edit_clip(bridge.get(),1,0,200,700,100,session.state().revision)==0);
    CHECK(daw_get_transport(bridge.get(),&t)==0 && t.duration==300);
    daw_clip bridgeClip{};bridgeClip.struct_size=sizeof(bridgeClip);CHECK(daw_get_clip(bridge.get(),1,0,&bridgeClip)==0 && bridgeClip.start==200);
    daw_snapshot splitSnap{};splitSnap.struct_size=sizeof(splitSnap);CHECK(daw_get_snapshot(bridge.get(),&splitSnap)==0);
    CHECK(daw_split_clip(bridge.get(),1,0,250,splitSnap.revision)==0);CHECK(daw_get_clip(bridge.get(),1,1,&bridgeClip)==0 && bridgeClip.source_offset==750);
    CHECK(daw_set_clip_fades(bridge.get(),1,1,10,10,splitSnap.revision+1)==0);CHECK(daw_get_clip(bridge.get(),1,1,&bridgeClip)==0 && bridgeClip.fade_in==10);
    CHECK(daw_set_crossfade(bridge.get(),1,0,10,splitSnap.revision+2)==0);CHECK(daw_get_clip(bridge.get(),1,1,&bridgeClip)==0 && bridgeClip.start==240 && bridgeClip.fade_in==10);
    CHECK(daw_set_crossfade(bridge.get(),1,0,0,splitSnap.revision+3)==0);CHECK(daw_get_clip(bridge.get(),1,1,&bridgeClip)==0 && bridgeClip.start==250 && bridgeClip.fade_in==0);
    CHECK(daw_duplicate_clip(bridge.get(),1,1,splitSnap.revision+4)==0);CHECK(daw_delete_clip(bridge.get(),1,2,splitSnap.revision+5)==0);
    daw::writeDraft(seekSession.state(),path);auto edited=daw::readDraft(path);
    CHECK(edited.tracks[0].regions.size()==2 && edited.tracks[0].regions[0].start==200 && edited.tracks[0].regions[1].sourceOffset==750);
    daw::Renderer persisted;persisted.prepare(edited);persisted.playing=true;persisted.render(seekLeft.data(),seekRight.data(),512);CHECK(seekLeft[250]>0 && seekLeft[300]==0);
    daw::writeDraft(clipOps.state(),path);auto persistedOps=daw::readDraft(path);CHECK(persistedOps.tracks[0].regions.size()==2 && persistedOps.tracks[0].regions[0].fadeIn==10 && persistedOps.tracks[0].regions[0].fadeOut==20);
    daw::writeDraft(crossfade.state(),path);auto persistedCrossfade=daw::readDraft(path);CHECK(persistedCrossfade.tracks[0].regions[0].fadeOut==100 && persistedCrossfade.tracks[0].regions[1].start==400 && persistedCrossfade.tracks[0].regions[1].fadeIn==100);
    daw::writeDraft(session.state(),path);
    sqlite3* db=nullptr;CHECK(sqlite3_open(path.c_str(),&db)==SQLITE_OK);
    CHECK(sqlite3_exec(db,"PRAGMA user_version=2;",nullptr,nullptr,nullptr)==SQLITE_OK);
    auto version2=daw::readDraft(path);CHECK(version2.tracks[0].regions[0].start==0 && version2.tracks[0].regions[0].length==4800);
    CHECK(sqlite3_exec(db,"PRAGMA user_version=1; ALTER TABLE tracks DROP COLUMN pcm;",nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
    auto legacy=daw::readDraft(path);CHECK(legacy.tracks.size()==1 && !legacy.tracks[0].audio);
    daw::Session limits;for(int i=0;i<8;++i)limits.import("A",clip,limits.state().revision);rejects([&]{limits.import("B",clip,limits.state().revision);});
    // Deterministic malformed-input smoke corpus; not a replacement for sustained fuzzing.
    std::mt19937 rng(7);for(int i=0;i<400;++i){auto mutation=bytes;for(int n=0;n<5;++n)mutation[rng()%44]=static_cast<unsigned char>(rng());try{daw::decodeWav(mutation);}catch(const std::exception&){} }
    // MIDI plan: frame-exact offsets, off-before-on ordering, capacity carry,
    // loop wrap with held-note cut-offs, and prepare()-integration.
    {
        std::vector<daw::MidiClip> clips;
        clips.push_back({1000,2000,{{1000,500,60,2,100},{1200,4000,64,2,90}},0}); // on@2000 off@2500, on@2200 off@6200
        daw::MidiTrackPlan plan; plan.build(clips);
        std::array<daw::PreparedMidiEvent,16> buf{};
        CHECK(plan.drain(1536,2048,buf.data(),16)==1 && buf[0].sampleOffset==464 && !buf[0].noteOff && buf[0].pitch==60 && buf[0].channel==2 && buf[0].velocity==100);
        CHECK(plan.drain(2048,2560,buf.data(),16)==2 && buf[0].sampleOffset==152 && !buf[0].noteOff && buf[0].pitch==64 && buf[1].sampleOffset==452 && buf[1].noteOff && buf[1].pitch==60);
        CHECK(plan.drain(2560,3072,buf.data(),16)==0);
        plan.reset(1536); CHECK(plan.drain(1536,2048,buf.data(),16)==1);
        // Equal-frame ordering: touching clips emit the off before the on.
        std::vector<daw::MidiClip> touching;
        touching.push_back({0,1000,{{0,1000,61,0,100}},0}); touching.push_back({1000,1000,{{0,500,62,0,100}},0});
        daw::MidiTrackPlan pairPlan; pairPlan.build(touching);
        CHECK(pairPlan.drain(0,2000,buf.data(),16)==4 && buf[0].sampleOffset==0 && !buf[0].noteOff && buf[1].noteOff && buf[1].pitch==61 && buf[2].sampleOffset==1000 && !buf[2].noteOff && buf[2].pitch==62 && buf[3].noteOff);
        // Capacity overflow carries to following blocks, never loses events.
        std::vector<daw::MidiClip> dense; std::vector<daw::MidiNote> ten;
        for(int i=0;i<10;++i)ten.push_back({uint64_t(i*10),10,70,0,100});
        dense.push_back({0,100000,ten,0});
        daw::MidiTrackPlan flood; flood.build(dense);
        std::array<daw::PreparedMidiEvent,8> small{};
        uint32_t delivered=0, first=0;
        for(uint64_t block=0;block<4000&&delivered<20;++block){
            const auto n=flood.drain(block*1000,(block+1)*1000,small.data(),8);
            if(delivered==8&&n>0)first=small[0].sampleOffset;
            delivered+=n;}
        CHECK(delivered==20 && first==0); // 8+8+4 across blocks; late events clamp to offset 0
        // Loop wrap: held note is cut off at the new pass start and replays.
        std::vector<daw::MidiClip> longNote; longNote.push_back({0,20000,{{100,8900,63,3,101}},0}); // off@9000 beyond loopEnd
        daw::MidiTrackPlan looped; looped.build(longNote);
        CHECK(looped.drain(0,512,buf.data(),16)==1 && !buf[0].noteOff); // on@100, now sounding
        CHECK(looped.wrapCutOffs(buf.data(),16)==1 && buf[0].sampleOffset==0 && buf[0].noteOff && buf[0].pitch==63 && buf[0].channel==3 && buf[0].velocity==0);
        looped.reset(0);
        CHECK(looped.drain(0,512,buf.data(),16)==1 && !buf[0].noteOff && buf[0].sampleOffset==100); // replays next pass
        CHECK(looped.drain(512,1024,buf.data(),16)==0); // off@9000 unreachable in loop: wrap owns the cut-off
        // Renderer integration: audio+MIDI track prepares, plays, advances.
        daw::Session midiSession; midiSession.import("T",clip,midiSession.state().revision);
        midiSession.addMidiClip(1,daw::MidiClip{0,4800,{{0,2400,60,0,100}},0},midiSession.state().revision);
        daw::Renderer midiRender; midiRender.prepare(midiSession.state()); midiRender.playing=true;
        midiRender.render(seekLeft.data(),seekRight.data(),512);
        CHECK(midiRender.position==512 && midiRender.pluginErrors.load()==0);
    }
    // Instrument-source arc: a MIDI-only track with an insert becomes audible.
    struct TestInstrument final : daw::PreparedEffect {
      struct Slot { bool on=false, releasing=false; uint32_t releaseAt=0; float amp=0; double step=0, phase=0; uint8_t pitch=0, channel=0; };
      bool process(float *left, float *right, uint32_t frames, uint64_t,
                   std::span<const daw::PreparedParameterEvent>,
                   std::span<const daw::PreparedMidiEvent> midi) noexcept override {
        size_t next=0;
        for (uint32_t f=0;f<frames;++f) {
          while (next<midi.size() && midi[next].sampleOffset<=f) {
            const auto &e=midi[next++];
            if (e.noteOff) { for (auto &v : voices) if (v.on&&!v.releasing&&v.pitch==e.pitch&&v.channel==e.channel) { v.releasing=true; v.releaseAt=f; } }
            else if (e.velocity) {
              Slot *slot=nullptr;
              for (auto &v : voices) if (!v.on) { slot=&v; break; }
              if (!slot) for (auto &v : voices) if (v.releasing) { slot=&v; break; }
              if (slot) { slot->on=true; slot->releasing=false; slot->pitch=e.pitch; slot->channel=e.channel; slot->amp=float(e.velocity)/127.0f*0.5f; slot->phase=0;
                slot->step=440.0*std::pow(2.0,(double(e.pitch)-69.0)/12.0)/48000.0; }
            }
          }
          float mix=0;
          for (auto &v : voices) {
            if (!v.on) continue;
            double env=1;
            if (v.releasing) { env=1.0-double(f-v.releaseAt)/240.0; if (env<=0) { v.on=false; continue; } }
            mix+=float(v.amp*env*std::sin(v.phase*6.283185307179586)); v.phase+=v.step;
          }
          left[f]+=mix; right[f]+=mix;
        }
        return true;
      }
      uint32_t latencyFrames() const noexcept override { return 0; }
      std::array<Slot,16> voices{};
    };
    const auto synthFactory=[](const daw::PluginInsert &){ return std::unique_ptr<daw::PreparedEffect>(std::make_unique<TestInstrument>()); };
    const auto peakOf=[&](const std::vector<float> &buffer,size_t from,size_t to){ float m=0; for(size_t i=from;i<to;++i)m=std::max(m,std::abs(buffer[i])); return m; };
    {
        daw::Session synth; synth.add("Synth",0);
        synth.addMidiClip(1,daw::MidiClip{0,5000,{{0,4800,60,0,100}},0},synth.state().revision);
        daw::PluginInsert insert; insert.name="Test synth"; insert.type=1; insert.subtype=2; insert.manufacturer=3;
        synth.addTrackInsert(1,insert,synth.state().revision);
        CHECK(synth.state().tracks[0].midiClips.size()==1 && !synth.state().tracks[0].audio);
        daw::Renderer r; r.insertFactoryForTest=synthFactory; r.prepare(synth.state()); r.playing=true;
        std::vector<float> l(5120),rr(5120); r.render(l.data(),rr.data(),5120);
        CHECK(r.pluginErrors.load()==0);
        CHECK(peakOf(l,500,4000)>0.15f);                    // note sounds while held
        CHECK(peakOf(l,5050,5120)<0.02f);                   // off@4800 plus a 240-frame release tail
        // Loop wrap kills the held note: cutoff at the wrap, silent after the tail.
        daw::Session loopy; loopy.add("Loop",0);
        loopy.addMidiClip(1,daw::MidiClip{0,4600,{{2500,2000,60,0,100}},0},loopy.state().revision);
        daw::PluginInsert insert2; insert2.name="Test synth 2"; insert2.type=1; insert2.subtype=2; insert2.manufacturer=3;
        loopy.addTrackInsert(1,insert2,loopy.state().revision);
        daw::Renderer w; w.insertFactoryForTest=synthFactory; w.prepare(loopy.state(),0,0,3000); w.playing=true;
        std::vector<float> wl(4600),wr(4600); w.render(wl.data(),wr.data(),4600);
        CHECK(peakOf(wl,2600,2950)>0.15f && peakOf(wl,3400,4600)<0.02f && w.pluginErrors.load()==0);
        // Empty track (no audio, no MIDI, no inserts) is still not a voice.
        daw::Session empty; empty.add("Nothing",0);
        daw::Renderer e; bool gate=false; std::string message;
        try { e.prepare(empty.state()); } catch (const daw::Error &x) { gate=true; message=x.what(); } catch (...) { gate=true; }
        CHECK(gate && message.find("Import audio or add a MIDI/instrument track")!=std::string::npos);
        // Inserts-only track (no MIDI) is a voice and honestly renders silence.
        daw::Session quiet; quiet.add("Silent chain",0);
        daw::PluginInsert insert3; insert3.name="Test synth 3"; insert3.type=1; insert3.subtype=2; insert3.manufacturer=3;
        quiet.addTrackInsert(1,insert3,quiet.state().revision);
        int built=0; daw::Renderer q; q.insertFactoryForTest=[&](const daw::PluginInsert &p){ ++built; return synthFactory(p); };
        q.prepare(quiet.state()); q.playing=true;
        std::vector<float> ql(1024),qr(1024); q.render(ql.data(),qr.data(),1024);
        CHECK(built==1 && q.pluginErrors.load()==0 && peakOf(ql,0,1024)==0.0f);
        // Regression: a plain audio track is unaffected.
        daw::Session audio; audio.import("Audio",clip,audio.state().revision);
        daw::Renderer a; a.prepare(audio.state()); a.playing=true;
        std::vector<float> al(512),ar(512); a.render(al.data(),ar.data(),512);
        CHECK(a.position==512 && al[300]>0.15f && al[480]>0.2f); // gain ramp smooths from zero
    }
    // --- METRONOME-ENGINE: post-master click track on the domain beat map ---
    {
        auto silent=[](uint64_t frames){std::vector<float> pcm(size_t(frames)*2,0.0f);return std::make_shared<const daw::Clip>(std::move(pcm));};
        auto peakWindow=[](const std::vector<float> &buffer,size_t from,size_t to){float m=0;for(size_t i=from;i<to;++i)m=std::max(m,std::abs(buffer[i]));return m;};
        // Envelope-aware zero-crossing density: 1000 Hz -> 9 interior sign
        // flips per 240-frame blip, 1500 Hz -> 14. Sub-threshold samples at
        // exact sine zeros are skipped so float error cannot fake a flip.
        auto crossings=[](const std::vector<float> &buffer,size_t from,size_t to){
            int flips=0,previous=0;
            for(size_t i=from;i<to;++i){const float x=buffer[i];
                if(std::abs(x)<1e-4f) continue;
                const int sign=x>0?1:-1;
                if(previous!=0&&sign!=previous) ++flips;
                previous=sign;}
            return flips;
        };
        auto renderAll=[](daw::Renderer &r,size_t frames,bool exportPath){
            std::vector<float> left(frames),right(frames);
            for(size_t off=0;off<frames;off+=512){const auto count=uint32_t(std::min<size_t>(512,frames-off));
                if(exportPath) r.renderExport(left.data()+off,right.data()+off,count);
                else r.render(left.data()+off,right.data()+off,count);}
            return std::pair{left,right};
        };
        const size_t span=192000; // four seconds at the fixed 48 kHz project rate
        daw::Session metro; metro.import("Silence",silent(span),0);
        CHECK(metro.state().tempo.size()==1&&metro.state().tempo[0].bpm==120.0); // default map
        daw::Renderer on; on.prepare(metro.state()); on.setMetronome(true); on.playing=true;
        auto [left,right]=renderAll(on,span,false);
        CHECK(on.position==span);
        // (a) 120 BPM default: eight quarter-note beats at k*24000 frames.
        for(int k=0;k<8;++k){
            const size_t beat=size_t(k)*24000;
            CHECK(peakWindow(left,beat,beat+300)>0.05f);   // -18 dBFS >> threshold
            CHECK(peakWindow(right,beat,beat+300)>0.05f);  // both channels
        }
        CHECK(std::memcmp(left.data(),right.data(),span*sizeof(float))==0); // mono click, L==R
        for(size_t i=240;i<12000;++i) CHECK(left[i]==0.0f); // inter-blip silence preserved
        // Default signature numerator 4 -> beats 0 and 4 are bar starts (1500 Hz),
        // beats 1..3 and 5 ordinary (1000 Hz); accent judged by crossing density.
        const int accent=crossings(left,0,240),accent4=crossings(left,96000,96240);
        const int ordinary1=crossings(left,24000,24240),ordinary2=crossings(left,48000,48240),
                  ordinary3=crossings(left,72000,72240),ordinary5=crossings(left,120000,120240);
        CHECK(accent>=12&&accent<=17&&accent4>=12&&accent4<=17);
        CHECK(ordinary1>=7&&ordinary1<=11&&ordinary2>=7&&ordinary2<=11&&ordinary3>=7&&ordinary3<=11&&ordinary5>=7&&ordinary5<=11);
        daw::Renderer never; never.prepare(metro.state()); never.playing=true;
        auto [controlL,controlR]=renderAll(never,span,false);
        bool anyClick=false; for(size_t i=0;i<span;++i) if(controlL[i]!=0.0f) {anyClick=true;break;}
        CHECK(!anyClick); // (b) control render with the switch untouched is pure silence
        daw::Renderer cycled; cycled.prepare(metro.state());
        cycled.setMetronome(true); cycled.setMetronome(false); cycled.playing=true;
        auto [cycledL,cycledR]=renderAll(cycled,span,false);
        CHECK(std::memcmp(cycledL.data(),controlL.data(),span*sizeof(float))==0&&
              std::memcmp(cycledR.data(),controlR.data(),span*sizeof(float))==0); // off == control byte-for-byte
        CHECK(std::memcmp(left.data(),controlL.data(),span*sizeof(float))!=0);    // on == audible
        // (c) tempo map {0:120, 96000:60}: beat spacing doubles after 96000.
        metro.setTempoAt(96000,60.0,metro.state().revision);
        daw::Renderer map; map.prepare(metro.state()); map.setMetronome(true); map.playing=true;
        auto [mapL,mapR]=renderAll(map,span,false);
        for(size_t beat:{0ull,24000ull,48000ull,72000ull,96000ull,144000ull})
            CHECK(peakWindow(mapL,beat,beat+300)>0.05f);
        for(size_t i=96240;i<119000;++i) CHECK(mapL[i]==0.0f); // no 24000-frame echo
        CHECK(peakWindow(mapL,120000,120300)==0.0f);
        CHECK(peakWindow(mapL,168000,168300)==0.0f);           // 144000+48000=192000 falls outside
        CHECK(crossings(mapL,96000,96240)>=12&&crossings(mapL,144000,144240)<=11); // beat 4 accent, beat 5 ordinary
        // (d) export-path suppression: renderExport stays click-free with the
        // switch on; engine/audio/export.cpp's only render-loop call site uses
        // renderExport (grep-verified in the arc report).
        daw::Renderer bounce; bounce.prepare(metro.state()); bounce.setMetronome(true); bounce.playing=true;
        CHECK(bounce.metronome());
        auto [bounceL,bounceR]=renderAll(bounce,span,true);
        daw::Renderer bounceControl; bounceControl.prepare(metro.state()); bounceControl.playing=true;
        auto [quietL,quietR]=renderAll(bounceControl,span,false);
        CHECK(std::memcmp(bounceL.data(),quietL.data(),span*sizeof(float))==0&&
              std::memcmp(bounceR.data(),quietR.data(),span*sizeof(float))==0);
    }
    // Realtime master loudness tracker: BS.1770-4 momentary/short-term windows
    // fed by the render path, with silent sentinel and meter reset.
    {   std::vector<float> meterTone; for(size_t i=0;i<48000*3;++i){const float v=std::sin(2.0f*3.14159265f*1000.0f*float(i)/48000.0f);meterTone.push_back(v);meterTone.push_back(v);}
        auto toneClip=std::make_shared<const daw::Clip>(std::move(meterTone));
        daw::Session toneSession;toneSession.import("Meter",toneClip,0);
        daw::Renderer meterRenderer;meterRenderer.prepare(toneSession.state());meterRenderer.playing=true;
        std::vector<float> meterL(19200),meterR(19200);float momentary=1,shortTerm=1;
        for(int pass=0;pass<8;++pass){meterRenderer.render(meterL.data(),meterR.data(),19200);meterRenderer.masterLoudness(momentary,shortTerm);}
        CHECK(std::abs(momentary-0.0f)<0.8f&&std::abs(shortTerm-0.0f)<0.8f);  // full-scale 1 kHz: -3.01 RMS + K-weighting ~= 0 LUFS
        CHECK(meterRenderer.masterLatencyFrames()==0);
        meterRenderer.resetMeters();meterRenderer.masterLoudness(momentary,shortTerm);
        CHECK(momentary<-100.0f&&shortTerm<-100.0f);  // reset returns to the -200 sentinel
        std::vector<float> dead(48000*3*2,0.0f);auto deadClip=std::make_shared<const daw::Clip>(std::move(dead));
        daw::Session deadSession;deadSession.import("Silent",deadClip,0);
        daw::Renderer quietRenderer;quietRenderer.prepare(deadSession.state());quietRenderer.playing=true;
        for(int pass=0;pass<8;++pass)quietRenderer.render(meterL.data(),meterR.data(),19200);
        quietRenderer.masterLoudness(momentary,shortTerm);
        CHECK(momentary<-100.0f&&shortTerm<-100.0f);  // digital silence never leaves the sentinel
    }
    std::cout<<"PASS: WAV bounds/formats, PCM persistence, cached peaks, sample-accurate seek/loop, seek revision invariance, gain/smoothing, EOF silence, clipping, stop, audio undo/redo, import limits, 400 malformed headers, MIDI plan offsets/carry/loop-wrap, instrument voice with release and loop cutoffs, metronome click/accent/tempo-map/export-suppress\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
