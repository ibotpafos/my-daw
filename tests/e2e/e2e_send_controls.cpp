#include "e2e.hpp"

using namespace e2e;
int main() {
    try {
        TempRoot root("send-render");
        writeWavFixture(root/"source.wav", constantInterleaved(24000,2,.1f,.05f),48000,2,"f32");
        Bridge session; auto* s=session.get();
        CHECK_OK(s,daw_import_wav(s,(root/"source.wav").c_str(),"Voice",rev(s)));
        const auto track=trackById(s,1).id;
        const auto dry=addBus(s,"Dry"), wet=addBus(s,"Wet");
        CHECK_OK(s,daw_set_track_output(s,track,dry,rev(s)));
        CHECK_OK(s,daw_set_bus_mute(s,dry,1,rev(s))); // Isolate the wet edge in the exported signal.
        CHECK_OK(s,daw_set_gain(s,track,-6.020599913,rev(s)));
        CHECK_OK(s,daw_set_pan(s,track,-1,rev(s)));
        CHECK_OK(s,daw_upsert_send(s,track,wet,0,0,rev(s)));
        CHECK_OK(s,daw_set_send_pan(s,track,wet,1,1,rev(s)));
        const auto gestureRevision=rev(s);
        CHECK_OK(s,daw_begin_mixer_gesture(s,DAW_MIXER_SEND_PAN,track,wet,gestureRevision));
        CHECK_OK(s,daw_write_mixer_gesture(s,-1));
        daw_cancel_mixer_gesture(s);
        CHECK(rev(s)==gestureRevision);
        CHECK_OK(s,daw_begin_mixer_gesture(s,DAW_MIXER_SEND_PAN,track,wet,gestureRevision));
        for(int step=0;step<=32;++step) CHECK_OK(s,daw_write_mixer_gesture(s,double(step)/32));
        CHECK_OK(s,daw_end_mixer_gesture(s,gestureRevision)); // Ends at original value: no history.
        CHECK(rev(s)==gestureRevision);
        CHECK_OK(s,daw_set_solo_exclusive(s,track,1,rev(s)));
        CHECK(trackById(s,track).solo);
        CHECK_OK(s,daw_set_solo_exclusive(s,0,0,rev(s)));
        CHECK(!trackById(s,track).solo);
        const auto panRevision=rev(s);
        auto wav=exportProject(s,root/"right.wav");
        CHECK(channelPeak(wav,8000,20000,0)<1e-6);
        CHECK(std::abs(channelRms(wav,8000,20000,1)-.025)<1e-4);
        CHECK(rev(s)==panRevision);
        CHECK_OK(s,daw_set_send_muted(s,track,wet,1,rev(s)));
        wav=exportProject(s,root/"muted.wav");
        CHECK(channelPeak(wav,0,24000,0)==0 && channelPeak(wav,0,24000,1)==0);
        auto old=abi<daw_send>(); CHECK_OK(s,daw_get_send(s,track,0,&old));
        CHECK(old.bus_id==wet && old.gain_db==0 && old.pre_fader==0);
        CHECK_OK(s,daw_undo(s,rev(s)));
        wav=exportProject(s,root/"undo.wav");
        CHECK(channelPeak(wav,8000,20000,0)<1e-6 && std::abs(channelRms(wav,8000,20000,1)-.025)<1e-4);
        CHECK_OK(s,daw_redo(s,rev(s)));
        saveDraftAndWait(s,root/"saved.daw");
        Bridge loaded; CHECK_OK(loaded.get(),daw_open_draft(loaded.get(),(root/"saved.daw").c_str()));
        auto restored=abi<daw_send_controls>();
        CHECK_OK(loaded.get(),daw_get_send_controls(loaded.get(),track,wet,&restored));
        CHECK(restored.muted && restored.independent_pan && restored.pan==1);
        wav=exportProject(loaded.get(),root/"loaded-muted.wav");
        CHECK(channelPeak(wav,0,24000,1)==0);
        CHECK_OK(loaded.get(),daw_set_send_muted(loaded.get(),track,wet,0,rev(loaded.get())));
        wav=exportProject(loaded.get(),root/"loaded-right.wav");
        CHECK(channelPeak(wav,8000,20000,0)<1e-6 && std::abs(channelRms(wav,8000,20000,1)-.025)<1e-4);
        CHECK(trackById(loaded.get(),track).pan==-1);
        std::cout<<"E2E send controls PASS: actual exported stereo samples, send-only mute, Undo/Redo, save/open/unmute\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
