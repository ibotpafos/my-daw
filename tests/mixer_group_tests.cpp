#include "e2e/e2e.hpp"
#include "domain/session.hpp"
#include "audio/renderer.hpp"
#include <array>
#include <cmath>
#include <limits>

using namespace e2e;
static void bridgeAndStorage() {
    TempRoot root("mixer-groups");
    Bridge owner;
    auto* s=owner.get();
    const auto a=addTrack(s,"Lead"), b=addTrack(s,"Double"), other=addTrack(s,"Unselected");
    uint64_t bus=0; CHECK_OK(s,daw_create_bus(s,"Return",&bus,rev(s)));
    CHECK_OK(s,daw_set_gain(s,a,-6,rev(s))); CHECK_OK(s,daw_set_gain(s,b,-12,rev(s)));
    CHECK_OK(s,daw_upsert_send(s,a,bus,-18,0,rev(s)));
    const std::array<uint64_t,2> ids{a,b}, duplicate{a,a}, missing{a,999}, notTrack{a,bus}, master{a,0};
    const auto initial=rev(s);
    for(const auto& bad:{duplicate,missing,notTrack,master})
        CHECK_REJ(s,daw_begin_track_gain_group(s,bad.data(),2,initial));
    CHECK_REJ(s,daw_begin_track_gain_group(s,nullptr,2,initial));
    CHECK_REJ(s,daw_begin_track_gain_group(s,ids.data(),1,initial));
    CHECK_REJ(s,daw_begin_track_gain_group(s,ids.data(),257,initial));
    CHECK_REJ(s,daw_begin_track_gain_group(s,ids.data(),2,initial+1));
    CHECK(daw_begin_track_gain_group(nullptr,ids.data(),2,0)!=0);
    CHECK(rev(s)==initial);
    CHECK_OK(s,daw_begin_track_gain_group(s,ids.data(),2,initial));
    CHECK_REJ(s,daw_begin_track_gain_group(s,ids.data(),2,initial));
    CHECK_REJ(s,daw_set_gain(s,other,-5,initial));
    CHECK_REJ(s,daw_remove_track(s,a,initial));
    CHECK_REJ(s,daw_undo(s,initial));
    CHECK_REJ(s,daw_begin_automation_gesture(s,1,a,1,initial));
    for (int i=0;i<1000;++i) CHECK_OK(s,daw_write_mixer_gesture(s,static_cast<double>(i)/100));
    CHECK_REJ(s,daw_write_mixer_gesture(s,std::numeric_limits<double>::quiet_NaN()));
    CHECK_REJ(s,daw_write_mixer_gesture(s,std::numeric_limits<double>::infinity()));
    CHECK_OK(s,daw_write_mixer_gesture(s,3));
    CHECK(rev(s)==initial && trackById(s,a).gain_db==-6 && trackById(s,b).gain_db==-12);
    CHECK_OK(s,daw_save_draft(s,(root/"preview.daw").c_str()));
    Bridge opened;
    CHECK_OK(opened.get(),daw_open_draft(opened.get(),(root/"preview.daw").c_str()));
    CHECK(trackById(opened.get(),a).gain_db==-6 && trackById(opened.get(),b).gain_db==-12);
    CHECK_REJ(s,daw_end_mixer_gesture(s,initial+1));
    CHECK_REJ(s,daw_set_mute(s,a,1,initial)); // Failed end leaves the gesture open.
    CHECK_OK(s,daw_end_mixer_gesture(s,initial));
    CHECK(rev(s)==initial+1 && trackById(s,a).gain_db==-3 && trackById(s,b).gain_db==-9);
    CHECK(trackById(s,other).gain_db==0);
    auto send=abi<daw_send>(); CHECK_OK(s,daw_get_send(s,a,0,&send)); CHECK(send.gain_db==-18);
    CHECK_OK(s,daw_undo(s,rev(s)));
    CHECK(trackById(s,a).gain_db==-6 && trackById(s,b).gain_db==-12);
    const auto undoRevision=rev(s);
    // No-op/cancel must preserve the redo branch, not simply the levels.
    CHECK_OK(s,daw_begin_track_gain_group(s,ids.data(),2,rev(s)));
    CHECK_OK(s,daw_write_mixer_gesture(s,-20)); CHECK_OK(s,daw_write_mixer_gesture(s,0));
    CHECK_OK(s,daw_end_mixer_gesture(s,rev(s))); CHECK(rev(s)==undoRevision);
    CHECK_OK(s,daw_begin_track_gain_group(s,ids.data(),2,rev(s)));
    CHECK_OK(s,daw_write_mixer_gesture(s,-100)); daw_cancel_mixer_gesture(s); daw_cancel_mixer_gesture(s);
    CHECK(rev(s)==undoRevision);
    CHECK_OK(s,daw_redo(s,rev(s))); CHECK(trackById(s,a).gain_db==-3 && trackById(s,b).gain_db==-9);
    CHECK_OK(s,daw_save_draft(s,(root/"committed.daw").c_str()));
    CHECK_OK(opened.get(),daw_open_draft(opened.get(),(root/"committed.daw").c_str()));
    CHECK(trackById(opened.get(),a).gain_db==-3 && trackById(opened.get(),b).gain_db==-9);
    CHECK_OK(s,daw_upsert_track_volume_automation_point(s,b,0,-12,rev(s)));
    CHECK_REJ(s,daw_begin_track_gain_group(s,ids.data(),2,rev(s)));
    CHECK_OK(s,daw_set_gain(s,a,-2,rev(s))); // Rejection did not leave a partial lock.
}
static void boundsAndRender() {
    daw::Session session;
    session.add("A",0);session.add("B",1);
    const auto a=session.state().tracks[0].id,b=session.state().tracks[1].id;
    session.gain(a,20,session.state().revision);session.gain(b,-119,session.state().revision);
    const auto initial=session.state().revision;
    session.beginTrackGainGroup({b,a},initial); // Deliberately not project order.
    session.writeMixerGesture(1000);
    CHECK(session.mixerPreview().tracks[0].gain==24 && session.mixerPreview().tracks[1].gain==-115);
    session.writeMixerGesture(-1000);
    CHECK(session.mixerPreview().tracks[0].gain==19 && session.mixerPreview().tracks[1].gain==-120);
    for(int i=0;i<1000;++i) session.writeMixerGesture(0.25);
    CHECK(session.mixerPreview().tracks[0].gain==20.25 && session.mixerPreview().tracks[1].gain==-118.75);
    session.writeMixerGesture(0);session.endMixerGesture(initial);CHECK(session.state().revision==initial);
    auto state=session.state();
    for(auto& track:state.tracks) {
        track.gain=-12;
        track.audio=std::make_shared<daw::Clip>(std::vector<float>(48000*2,0.04f));
        track.regions={{0,0,48000,0,0}};
    }
    session.replace(state);
    daw::Renderer renderer;renderer.prepare(session.state());renderer.playing.store(true);
    std::array<float,4096> left{},right{};
    renderer.render(left.data(),right.data(),4096);
    const float old=left.back();const auto pos=renderer.position.load();
    session.beginTrackGainGroup({a,b},session.state().revision);session.writeMixerGesture(6);
    renderer.updateMix(session.mixerPreview());
    CHECK(renderer.position.load()==pos && renderer.playing.load());
    renderer.render(left.data(),right.data(),4096);
    const auto target=0.08*std::pow(10.,-6./20.);
    CHECK(left[0]>old && left[0]<target);CHECK(std::abs(left.back()-target)<1e-5);CHECK(left==right);
    CHECK(renderer.position.load()==pos+4096);
    session.cancelMixerGesture();renderer.updateMix(session.state());renderer.render(left.data(),right.data(),4096);
    CHECK(std::abs(left.back()-old)<1e-5);
    // Full selection is bounded, IDs are resolved once at begin, not during writes.
    daw::Session large;
    for(int i=0;i<256;++i)large.add("Track "+std::to_string(i),large.state().revision);
    std::vector<uint64_t> all; for(const auto& t:large.state().tracks)all.push_back(t.id);
    const auto r=large.state().revision;large.beginTrackGainGroup(all,r);large.writeMixerGesture(-6);large.endMixerGesture(r);
    CHECK(large.state().revision==r+1);
    for(const auto& t:large.state().tracks)CHECK(t.gain==-6);
    large.undo(large.state().revision);for(const auto& t:large.state().tracks)CHECK(t.gain==0);
}
int main() {
    try { bridgeAndStorage();boundsAndRender();std::cout<<"Mixer group gain: IDs, guards, relative limits, preview, one Undo, persistence, automation rejection, live render and 256 tracks PASS\n"; }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
