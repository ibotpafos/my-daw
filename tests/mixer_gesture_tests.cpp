#include "e2e/e2e.hpp"
#include "domain/session.hpp"
#include "audio/renderer.hpp"
#include <array>
#include <limits>

using namespace e2e;

static void bridgeGesture() {
    TempRoot root("mixer-gesture");
    Bridge owner;
    auto* s=owner.get();
    const auto a=addTrack(s,"Lead"), b=addTrack(s,"Double");
    uint64_t bus=0;
    CHECK_OK(s,daw_create_bus(s,"Reverb",&bus,rev(s)));
    CHECK_OK(s,daw_upsert_send(s,a,bus,-12,1,rev(s)));
    const auto initial=rev(s);
    CHECK_OK(s,daw_begin_mixer_gesture(s,DAW_MIXER_TRACK_GAIN,a,0,initial));
    CHECK_REJ(s,daw_begin_mixer_gesture(s,DAW_MIXER_MASTER_GAIN,0,0,initial));
    CHECK_REJ(s,daw_set_pan(s,a,.3,initial));
    CHECK_REJ(s,daw_undo(s,initial));
    CHECK_REJ(s,daw_begin_automation_gesture(s,1,a,1,initial));
    CHECK_REJ(s,daw_write_mixer_gesture(s,std::numeric_limits<double>::quiet_NaN()));
    CHECK_REJ(s,daw_write_mixer_gesture(s,25));
    for(int i=1;i<=256;++i) CHECK_OK(s,daw_write_mixer_gesture(s,-double(i)/32));
    CHECK(rev(s)==initial);
    CHECK(trackById(s,a).gain_db==0); // Preview never leaks into snapshot/save.
    CHECK_OK(s,daw_save_draft(s,(root/"preview.daw").c_str()));
    Bridge saved;
    CHECK_OK(saved.get(),daw_open_draft(saved.get(),(root/"preview.daw").c_str()));
    CHECK(trackById(saved.get(),a).gain_db==0);
    CHECK_REJ(s,daw_end_mixer_gesture(s,initial+1));
    CHECK_REJ(s,daw_set_gain(s,a,-6,initial)); // Failed end stays cancelable/open.
    CHECK_OK(s,daw_end_mixer_gesture(s,initial));
    CHECK(rev(s)==initial+1 && trackById(s,a).gain_db==-8);
    CHECK_OK(s,daw_undo(s,rev(s)));
    CHECK(trackById(s,a).gain_db==0);
    CHECK_OK(s,daw_redo(s,rev(s)));
    CHECK(trackById(s,a).gain_db==-8);
    const auto noOp=rev(s);
    CHECK_OK(s,daw_begin_mixer_gesture(s,DAW_MIXER_TRACK_GAIN,a,0,noOp));
    CHECK_OK(s,daw_write_mixer_gesture(s,-4));
    CHECK_OK(s,daw_write_mixer_gesture(s,-8));
    CHECK_OK(s,daw_end_mixer_gesture(s,noOp));
    CHECK(rev(s)==noOp);
    CHECK_OK(s,daw_begin_mixer_gesture(s,DAW_MIXER_SEND_GAIN,a,bus,rev(s)));
    CHECK_OK(s,daw_write_mixer_gesture(s,-3));
    daw_cancel_mixer_gesture(s); daw_cancel_mixer_gesture(s);
    auto send=abi<daw_send>(); CHECK_OK(s,daw_get_send(s,a,0,&send));
    CHECK(send.gain_db==-12 && send.pre_fader==1 && rev(s)==noOp);
    CHECK_OK(s,daw_begin_mixer_gesture(s,DAW_MIXER_SEND_GAIN,a,bus,rev(s)));
    CHECK_OK(s,daw_write_mixer_gesture(s,-6));
    CHECK_OK(s,daw_end_mixer_gesture(s,rev(s)));
    CHECK_OK(s,daw_get_send(s,a,0,&send));
    CHECK(send.gain_db==-6 && send.pre_fader==1 && trackById(s,a).gain_db==-8);
    // All supported scalar targets share the same bounds/interlock contract.
    for(const auto target:{DAW_MIXER_TRACK_PAN,DAW_MIXER_BUS_PAN,DAW_MIXER_BUS_GAIN,DAW_MIXER_MASTER_GAIN}) {
        const uint64_t id=target==DAW_MIXER_TRACK_PAN?a:target==DAW_MIXER_MASTER_GAIN?0:bus;
        CHECK_OK(s,daw_begin_mixer_gesture(s,target,id,0,rev(s)));
        CHECK_OK(s,daw_write_mixer_gesture(s,.5));
        CHECK_REJ(s,daw_write_mixer_gesture(s,std::numeric_limits<double>::infinity()));
        CHECK_OK(s,daw_end_mixer_gesture(s,rev(s)));
    }
    CHECK_REJ(s,daw_begin_mixer_gesture(s,99,a,0,rev(s)));
    CHECK_REJ(s,daw_begin_mixer_gesture(s,DAW_MIXER_MASTER_GAIN,a,0,rev(s)));
    CHECK_REJ(s,daw_begin_mixer_gesture(s,DAW_MIXER_TRACK_GAIN,a,bus,rev(s)));
    CHECK_REJ(s,daw_begin_mixer_gesture(s,DAW_MIXER_SEND_GAIN,b,bus,rev(s)));
    CHECK(daw_begin_mixer_gesture(nullptr,1,a,0,0)!=0);
    CHECK(daw_write_mixer_gesture(nullptr,0)!=0);
    daw_cancel_mixer_gesture(nullptr);
    CHECK_OK(s,daw_save_draft(s,(root/"committed.daw").c_str()));
    Bridge opened; CHECK_OK(opened.get(),daw_open_draft(opened.get(),(root/"committed.daw").c_str()));
    CHECK_OK(opened.get(),daw_get_send(opened.get(),a,0,&send));
    CHECK(send.gain_db==-6 && trackById(opened.get(),a).pan==.5);
    // Exclusive solo is atomic, including Undo restoring multiple solo tracks.
    CHECK_OK(s,daw_set_solo(s,a,1,rev(s))); CHECK_OK(s,daw_set_solo(s,b,1,rev(s)));
    const auto beforeSolo=rev(s);
    CHECK_OK(s,daw_set_solo_exclusive(s,a,1,rev(s)));
    CHECK(rev(s)==beforeSolo+1 && trackById(s,a).solo && !trackById(s,b).solo);
    CHECK_OK(s,daw_undo(s,rev(s)));
    CHECK(trackById(s,a).solo && trackById(s,b).solo);
    CHECK_OK(s,daw_set_solo_exclusive(s,0,0,rev(s)));
    CHECK(!trackById(s,a).solo && !trackById(s,b).solo);
    CHECK_REJ(s,daw_set_solo_exclusive(s,999,1,rev(s)));
}

static void livePreparedSend() {
    daw::Session session;
    session.add("Source",0);
    const auto id=session.state().tracks[0].id;
    session.addBus("Return",session.state().revision);
    const auto bus=session.state().buses[0].id;
    session.upsertSend(id,bus,-12,true,session.state().revision);
    daw::State state=session.state();
    state.tracks[0].gain=-120;
    state.tracks[0].audio=std::make_shared<daw::Clip>(std::vector<float>(48000*2,.04f));
    state.tracks[0].regions={{0,0,48000,0,0}};
    daw::Renderer renderer;
    renderer.prepare(state); renderer.playing.store(true);
    std::array<float,4096> left{},right{};
    renderer.render(left.data(),right.data(),4096);
    const auto position=renderer.position.load();
    const float old=left.back();
    state.tracks[0].sends[0].gain=-6;
    renderer.updateMix(state);
    CHECK(renderer.playing.load() && renderer.position.load()==position);
    renderer.render(left.data(),right.data(),4096);
    CHECK(renderer.playing.load() && renderer.position.load()==position+4096);
    CHECK(left[0]>old && left[0]<.04f*float(std::pow(10.,-6./20.)));
    CHECK(std::abs(left.back()-.04f*std::pow(10.,-6./20.))<1e-5);
    CHECK(left==right);
    // Existing PRE tap ignores main fader; its gate still obeys mute/solo.
    state.tracks[0].muted=true;
    renderer.updateMix(state); renderer.render(left.data(),right.data(),4096);
    CHECK(std::abs(left.back())<1e-7);
    state.tracks[0].muted=false;
    state.tracks[0].sends[0].gain=-12;
    renderer.updateMix(state); renderer.render(left.data(),right.data(),4096);
    CHECK(std::abs(left.back()-old)<1e-5);
}

int main() {
    try { bridgeGesture(); livePreparedSend(); std::cout<<"mixer gesture / persistence / exclusive solo / live send: PASS\n"; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
