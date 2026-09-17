#include "e2e/e2e.hpp"
#include "domain/session.hpp"
#include "audio/renderer.hpp"
#include "export/dawproject_model.hpp"
#include <array>
#include <limits>
#include <sqlite3.h>

using namespace e2e;

static daw_send_controls controls(daw_session* s, uint64_t track, uint64_t bus) {
    auto value = abi<daw_send_controls>();
    CHECK_OK(s, daw_get_send_controls(s, track, bus, &value));
    CHECK(value.version == DAW_SEND_CONTROLS_VERSION);
    return value;
}
static void mutateDB(const std::filesystem::path& path, const char* sql) {
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    CHECK(sqlite3_exec(raw, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
}
static void bridgeAndStorage() {
    TempRoot root("send-controls");
    Bridge owner; auto* s = owner.get();
    const auto track = addTrack(s, "Lead"), other = addTrack(s, "Other");
    const auto bus = addBus(s, "Plate");
    CHECK_OK(s, daw_upsert_send(s, track, bus, -18, 0, rev(s)));
    const auto base = rev(s);
    auto initial = controls(s, track, bus);
    CHECK(initial.pan == 0 && !initial.muted && !initial.independent_pan);
    CHECK_OK(s, daw_set_send_muted(s, track, bus, 0, base));
    CHECK_OK(s, daw_set_send_pan(s, track, bus, 0, 0, base));
    CHECK(rev(s) == base);
    CHECK_REJ(s, daw_set_send_muted(s, track, bus, 2, base));
    CHECK_REJ(s, daw_set_send_pan(s, track, bus, 0, -1, base));
    for (const auto bad : {1.01, -1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        CHECK_REJ(s, daw_set_send_pan(s, track, bus, bad, 1, base));
    CHECK_REJ(s, daw_set_send_muted(s, other, bus, 1, base));
    CHECK_REJ(s, daw_set_send_pan(s, track, 999, 0, 1, base));
    CHECK_REJ(s, daw_set_send_muted(s, track, bus, 1, base + 1));
    CHECK_REJ(s, daw_get_send_controls(s, track, bus, nullptr));
    auto invalid = abi<daw_send_controls>(); invalid.struct_size--; invalid.pan = .123;
    CHECK_REJ(s, daw_get_send_controls(s, track, bus, &invalid));
    CHECK(invalid.pan == .123 && rev(s) == base);
    CHECK(daw_set_send_pan(nullptr, track, bus, 0, 1, base) != 0);
    CHECK(daw_set_send_muted(nullptr, track, bus, 1, base) != 0);
    CHECK(daw_get_send_controls(nullptr, track, bus, &invalid) != 0);

    CHECK_OK(s, daw_set_send_pan(s, track, bus, .75, 1, base));
    CHECK(rev(s) == base + 1 && controls(s, track, bus).pan == .75);
    CHECK_OK(s, daw_set_send_muted(s, track, bus, 1, rev(s)));
    CHECK(controls(s, track, bus).muted == 1);
    CHECK_OK(s, daw_undo(s, rev(s)));
    CHECK(controls(s, track, bus).pan == .75 && !controls(s, track, bus).muted);
    CHECK_OK(s, daw_redo(s, rev(s)));
    CHECK(controls(s, track, bus).muted == 1);
    // Legacy gain/tap editing does not discard newly persisted controls.
    CHECK_OK(s, daw_upsert_send(s, track, bus, -6, 1, rev(s)));
    auto legacy = abi<daw_send>(); CHECK_OK(s, daw_get_send(s, track, 0, &legacy));
    CHECK(legacy.gain_db == -6 && legacy.pre_fader == 1 && controls(s, track, bus).pan == .75 && controls(s, track, bus).muted);
    CHECK(trackById(s, track).gain_db == 0 && trackById(s, track).pan == 0 && !trackById(s, track).muted);

    const auto beforeGesture = rev(s);
    CHECK_OK(s, daw_begin_mixer_gesture(s, DAW_MIXER_SEND_PAN, track, bus, beforeGesture));
    for (int n=0; n<256; ++n) CHECK_OK(s, daw_write_mixer_gesture(s, -double(n)/255));
    CHECK_REJ(s, daw_write_mixer_gesture(s, 1.1));
    CHECK_REJ(s, daw_set_send_muted(s, track, bus, 0, beforeGesture));
    CHECK_REJ(s, daw_begin_automation_gesture(s, DAW_AUTOMATION_TRACK_VOLUME, track, DAW_AUTOMATION_TOUCH, beforeGesture));
    CHECK(controls(s, track, bus).pan == .75 && rev(s) == beforeGesture);
    CHECK_OK(s, daw_save_draft(s, (root/"preview.daw").c_str()));
    Bridge preview; CHECK_OK(preview.get(), daw_open_draft(preview.get(), (root/"preview.daw").c_str()));
    CHECK(controls(preview.get(), track, bus).pan == .75);
    CHECK_REJ(s, daw_end_mixer_gesture(s, beforeGesture + 1));
    CHECK_OK(s, daw_end_mixer_gesture(s, beforeGesture));
    CHECK(rev(s) == beforeGesture + 1 && controls(s, track, bus).pan == -1);
    CHECK_OK(s, daw_undo(s, rev(s))); CHECK(controls(s, track, bus).pan == .75);
    CHECK_OK(s, daw_redo(s, rev(s))); CHECK(controls(s, track, bus).pan == -1);
    const auto noop = rev(s);
    CHECK_OK(s, daw_begin_mixer_gesture(s, DAW_MIXER_SEND_PAN, track, bus, noop));
    CHECK_OK(s, daw_write_mixer_gesture(s, 1));
    daw_cancel_mixer_gesture(s);
    CHECK(controls(s, track, bus).pan == -1 && rev(s) == noop);
    CHECK_OK(s, daw_begin_mixer_gesture(s, DAW_MIXER_SEND_PAN, track, bus, noop));
    CHECK_OK(s, daw_write_mixer_gesture(s, -1));
    CHECK_OK(s, daw_end_mixer_gesture(s, noop)); CHECK(rev(s) == noop);
    CHECK_OK(s, daw_set_send_pan(s, track, bus, -1, 0, rev(s)));
    CHECK_REJ(s, daw_begin_mixer_gesture(s, DAW_MIXER_SEND_PAN, track, bus, rev(s)));
    CHECK_OK(s, daw_set_send_pan(s, track, bus, -1, 1, rev(s)));

    const auto native = root/"v22.daw";
    CHECK_OK(s, daw_save_draft(s, native.c_str()));
    const auto saved = daw::readDraft(native);
    CHECK(saved.tracks[0].sends[0].pan == -1 && saved.tracks[0].sends[0].muted && saved.tracks[0].sends[0].independentPan);
    Bridge reopened; CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), native.c_str()));
    CHECK(controls(reopened.get(), track, bus).pan == -1 && controls(reopened.get(), track, bus).muted == 1);
    CHECK_OK(s, daw_delete_bus(s, bus, rev(s)));
    CHECK_REJ(s, daw_get_send_controls(s, track, bus, &initial));
    CHECK_OK(s, daw_undo(s, rev(s)));
    CHECK(controls(s, track, bus).pan == -1 && controls(s, track, bus).muted);

    // A real legacy schema, without any new columns, must retain its old sound.
    const auto old = root/"v21.daw";
    std::filesystem::copy_file(native, old);
    mutateDB(old, "ALTER TABLE sends DROP COLUMN pan; ALTER TABLE sends DROP COLUMN muted; ALTER TABLE sends DROP COLUMN independent_pan; PRAGMA user_version=21;");
    const auto loaded = daw::readDraft(old);
    CHECK(loaded.tracks[0].sends[0].gain == -6 && loaded.tracks[0].sends[0].preFader);
    CHECK(loaded.tracks[0].sends[0].pan == 0 && !loaded.tracks[0].sends[0].muted && !loaded.tracks[0].sends[0].independentPan);
    for (const char* sql : {
        "PRAGMA ignore_check_constraints=ON; UPDATE sends SET muted=2;",
        "PRAGMA ignore_check_constraints=ON; UPDATE sends SET independent_pan=-1;",
        "UPDATE sends SET pan='invalid';", "UPDATE sends SET pan=1.1;",
        "UPDATE sends SET gain='invalid';", "UPDATE sends SET pan=1e999;"}) {
        const auto bad = root/"bad.daw";
        std::filesystem::copy_file(native, bad, std::filesystem::copy_options::overwrite_existing);
        mutateDB(bad, sql);
        const auto before = rev(reopened.get());
        CHECK_REJ(reopened.get(), daw_open_draft(reopened.get(), bad.c_str()));
        CHECK(rev(reopened.get()) == before && controls(reopened.get(), track, bus).muted);
    }
    CHECK_OK(s, daw_package_project(s, native.c_str(), (root/"sends.mydawzip").c_str()));
    CHECK_OK(s, daw_extract_package(s, (root/"sends.mydawzip").c_str(), (root/"unpacked.daw").c_str()));
    CHECK(daw::readDraft(root/"unpacked.daw").tracks[0].sends == saved.tracks[0].sends);
    auto interchange = daw::dawproject::makeExportModel(saved);
    CHECK(interchange.tracks()[0].sends.empty());
    CHECK(std::any_of(interchange.losses().items.begin(), interchange.losses().items.end(), [](const auto& l){return l.code=="muted-send-omitted";}));
}

static void liveDSP() {
    daw::Session session; session.add("Source",0); session.addBus("Dry",1); session.addBus("Wet",2);
    session.routeTrack(1,2,3); session.busMute(2,true,4); session.upsertSend(1,3,0,false,5);
    auto state=session.state();
    std::vector<float> samples(48000*4);
    for(size_t i=0;i<samples.size();i+=2){samples[i]=.04f;samples[i+1]=.02f;}
    state.tracks[0].audio=std::make_shared<daw::Clip>(samples); state.tracks[0].regions={{0,0,96000,0,0}};
    state.tracks[0].pan=-1; state.tracks[0].gain=-6.020599913;
    daw::Renderer renderer; renderer.prepare(state); renderer.playing.store(true);
    std::array<float,4096> l{},r{};
    auto render=[&]{renderer.render(l.data(),r.data(),4096);};
    render(); CHECK(std::abs(l.back()-.02f)<1e-6 && std::abs(r.back())<1e-7);
    const auto oldPosition=renderer.position.load();
    auto& send=state.tracks[0].sends[0]; send.independentPan=true; send.pan=1;
    renderer.updateMix(state);
    CHECK(renderer.position.load()==oldPosition && renderer.playing.load());
    render(); CHECK(renderer.position.load()==oldPosition+4096);
    CHECK(l.front()>0 && r.front()<.01f); // Crossfade rather than discontinuous switch.
    CHECK(std::abs(l.back())<1e-6 && std::abs(r.back()-.01f)<1e-6);
    // Track pan moves cannot recolor an independent send. The dry path is unchanged.
    state.tracks[0].pan=1; renderer.updateMix(state); render();
    CHECK(std::abs(l.back())<1e-6 && std::abs(r.back()-.01f)<1e-6);
    send.muted=true; renderer.updateMix(state); render();
    CHECK(r.front()>0 && r.front()<.01f && std::abs(r.back())<1e-7);
    CHECK(send.gain==0 && send.pan==1 && send.independentPan && send.bus==3);
    send.gain=-6.020599913; renderer.updateMix(state); render(); CHECK(std::abs(r.back())<1e-7);
    send.muted=false; renderer.updateMix(state); render(); CHECK(std::abs(r.back()-.005f)<1e-6);
    send.pan=0; renderer.updateMix(state); render();
    CHECK(std::abs(l.back()-.01f)<1e-6 && std::abs(r.back()-.005f)<1e-6);
    state.tracks[0].muted=true; renderer.updateMix(state); render(); CHECK(std::abs(l.back())<1e-7 && std::abs(r.back())<1e-7);
    state.tracks[0].muted=false;
    // PRE ignores fader and main pan; independent send balance still applies.
    send.preFader=true; send.pan=-1; state.tracks[0].gain=-120;
    renderer.prepare(state); renderer.playing=true; render();
    CHECK(std::abs(l.back()-.02f)<1e-6 && std::abs(r.back())<1e-7);
    send.muted=true; renderer.prepare(state); renderer.playing=true; render();
    CHECK(std::all_of(l.begin(),l.end(),[](auto x){return x==0;})); // No startup burst.
    send.muted=false; send.preFader=false; send.pan=1;
    state.tracks[0].volumeAutomation={{0,-6.020599913},{95999,-6.020599913}};
    state.tracks[0].panAutomation={{0,-1},{95999,-1}};
    renderer.prepare(state); renderer.playing=true; render();
    CHECK(std::abs(l.back())<1e-7 && std::abs(r.back()-.005f)<1e-6); // POST follows fader automation, bypasses pan automation.
    send.independentPan=false; renderer.updateMix(state); render();
    CHECK(std::abs(l.back()-.01f)<1e-6 && std::abs(r.back())<1e-6);
    // Report the pan interchange limitation instead of silently claiming parity.
    send.independentPan=true;
    const auto interchange=daw::dawproject::makeExportModel(state);
    CHECK(std::any_of(interchange.losses().items.begin(),interchange.losses().items.end(),[](const auto& loss){return loss.code=="independent-send-pan-omitted";}));
}
int main() {
    try { bridgeAndStorage(); liveDSP(); std::cout<<"Send controls PASS: ABI, old/new storage, history, mute, pan, automation, smoothing, live transport, interchange losses\n"; }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
