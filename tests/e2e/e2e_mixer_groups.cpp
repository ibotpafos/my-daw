#include "e2e.hpp"
#include <array>
#include <cmath>

using namespace e2e;

// Black-box journey: the rendered audio, persisted gains and history must agree.
// Unlike mixer_group_tests this scenario uses only the public bridge and independently
// reads the exported PCM; it also participates in the strict public-ABI coverage gate.
int main() {
    try {
        TempRoot root("linked-mix");
        writeWavFixture(root/"source.wav", constantInterleaved(24000, 2, .04f, .04f), 48000, 2, "f32");
        Bridge project;
        auto* s = project.get();
        for (const char* name : {"Lead", "Double"})
            CHECK_OK(s, daw_import_wav(s, (root/"source.wav").c_str(), name, rev(s)));
        auto first = abi<daw_track>(), second = abi<daw_track>();
        CHECK_OK(s, daw_get_track(s, 0, &first));
        CHECK_OK(s, daw_get_track(s, 1, &second));
        CHECK_OK(s, daw_set_gain(s, first.id, -6, rev(s)));
        CHECK_OK(s, daw_set_gain(s, second.id, -12, rev(s)));
        const std::array<uint64_t, 2> ids{first.id, second.id};
        const auto baseline = exportProject(s, root/"baseline.wav");
        const double rms = channelRms(baseline, 8000, 20000, 0);
        CHECK(rms > .02 && rms < .04);
        const auto before = rev(s);
        CHECK_OK(s, daw_begin_track_gain_group(s, ids.data(), 2, before));
        for (int step = 0; step <= 100; ++step)
            CHECK_OK(s, daw_write_mixer_gesture(s, 3.0 * step / 100));
        CHECK(rev(s) == before);
        CHECK(trackById(s, first.id).gain_db == -6 && trackById(s, second.id).gain_db == -12);
        // Saving during a preview may not bake uncommitted fader values into the draft.
        saveDraftAndWait(s, root/"preview.daw");
        Bridge reloaded;
        CHECK_OK(reloaded.get(), daw_open_draft(reloaded.get(), (root/"preview.daw").c_str()));
        CHECK(trackById(reloaded.get(), first.id).gain_db == -6);
        const auto previewPCM = exportProject(reloaded.get(), root/"preview.wav");
        CHECK(std::abs(channelRms(previewPCM, 8000, 20000, 0) - rms) < 1e-6);
        CHECK_OK(s, daw_end_mixer_gesture(s, before));
        CHECK(rev(s) == before + 1);
        CHECK(trackById(s, first.id).gain_db == -3 && trackById(s, second.id).gain_db == -9);
        const auto committed = exportProject(s, root/"committed.wav");
        CHECK(std::abs(channelRms(committed, 8000, 20000, 0) / rms - std::pow(10., 3./20.)) < 1e-4);
        CHECK_OK(s, daw_undo(s, rev(s)));
        const auto undoRevision = rev(s);
        CHECK(trackById(s, first.id).gain_db == -6 && trackById(s, second.id).gain_db == -12);
        CHECK_OK(s, daw_begin_track_gain_group(s, ids.data(), 2, rev(s)));
        CHECK_OK(s, daw_write_mixer_gesture(s, -20));
        daw_cancel_mixer_gesture(s);
        CHECK(rev(s) == undoRevision && snapshotOf(s).can_redo);
        CHECK_OK(s, daw_redo(s, rev(s)));
        saveDraftAndWait(s, root/"committed.daw");
        CHECK_OK(reloaded.get(), daw_open_draft(reloaded.get(), (root/"committed.daw").c_str()));
        CHECK(trackById(reloaded.get(), first.id).gain_db == -3 && trackById(reloaded.get(), second.id).gain_db == -9);
        const auto loadedPCM = exportProject(reloaded.get(), root/"loaded.wav");
        CHECK(std::abs(channelRms(loadedPCM, 8000, 20000, 0) - channelRms(committed, 8000, 20000, 0)) < 1e-6);
        std::cout << "E2E linked mixer PASS: imported audio, isolated preview/save, relative rendered gain, one Undo, cancel/Redo, reopen/export\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
