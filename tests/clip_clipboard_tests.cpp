#include "domain/clip_clipboard.hpp"
#include <iostream>
#include <type_traits>

namespace {
int checks = 0;
#define CHECK(...)                                                                                 \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(__VA_ARGS__))                                                                        \
            throw std::runtime_error(#__VA_ARGS__);                                                \
    } while (false)
template <class F> void rejects(F &&f) {
    bool failed = false;
    try {
        f();
    } catch (const daw::Error &) {
        failed = true;
    }
    CHECK(failed);
}
std::shared_ptr<const daw::Clip> audio(float value) {
    return std::make_shared<const daw::Clip>(std::vector<float>(48000 * 2, value));
}
}
int main() {
    try {
        using namespace daw;
        static_assert(std::is_nothrow_move_assignable_v<ClipClipboard>);
        const auto source = audio(.1f), other = audio(.2f), take = audio(.3f);
        Session session;
        auto rev = [&] {
            return session.state().revision;
        };
        session.import("Source", source, rev());
        session.import("Foreign", other, rev());
        session.add("Empty", rev());
        session.setClipGain(1, 0, -6, rev());
        session.setClipPan(1, 0, .4, rev());
        session.setClipColor(1, 0, 0xABCDEF, rev());
        session.setClipFades(1, 0, 100, 200, rev());
        const auto original = session.state().tracks[0];
        auto captureRevision = rev();
        const auto board = session.captureClips(1, {0}, false, false, rev());
        CHECK(rev() == captureRevision && board.count() == 1 && board.length() == 48000);
        session.setClipGain(1, 0, 3, rev());
        session.deleteClip(1, 0, rev());
        CHECK(session.state().tracks[0].audio == source &&
              session.state().tracks[0].regions.empty());
        session.pasteClips(board, 3, 96000, rev());
        CHECK(session.state().tracks[2].audio == source); // no sample copy
        auto pasted = session.state().tracks[2].regions[0];
        pasted.start = 0;
        CHECK(pasted == original.regions[0]); // gain/pan/color/offset/fades survive source edit
        session.pasteClips(board, 2, 96000, rev());
        CHECK(session.state().tracks[1].audio == other &&
              session.state().tracks[1].takes.size() == 1);
        CHECK(session.state().tracks[1].takes[0].audio == source &&
              session.state().tracks[1].regions[1].take == 1);
        session.pasteClips(board, 2, 192000, rev());
        CHECK(session.state().tracks[1].takes.size() == 1); // source interned once
        CHECK(session.state().tracks[1].regions.size() == 3);
        auto before = session.state();
        rejects([&] {
            session.pasteClips(board, 2, 100000, rev());
        });
        CHECK(session.state().tracks == before.tracks && rev() == before.revision);
        rejects([&] {
            session.pasteClips(board, 2, UINT64_MAX, rev());
        });
        CHECK(session.state().tracks == before.tracks && rev() == before.revision);
        session.removeTrack(1, rev());
        session.pasteClips(board, 3, 192000, rev());
        CHECK(session.state().tracks[1].regions.size() == 2); // original owner removed

        Session groups;
        auto gr = [&] {
            return groups.state().revision;
        };
        groups.import("Group", source, gr());
        groups.splitClip(1, 0, 24000, gr());
        groups.nudgeClips(1, {1}, 48000, gr());
        const auto groupBefore = groups.state().tracks;
        const auto groupRevision = gr();
        const auto cut = groups.captureClips(1, {1, 0, 1}, false, true, gr());
        CHECK(gr() == groupRevision + 1 && cut.count() == 2 && cut.length() == 96000);
        CHECK(groups.state().tracks[0].regions.empty() && groups.state().tracks[0].audio == source);
        groups.undo(gr());
        CHECK(groups.state().tracks == groupBefore);
        groups.redo(gr());
        CHECK(groups.state().tracks[0].regions.empty());
        groups.pasteClips(cut, 1, 144000, gr());
        CHECK(groups.state().tracks[0].regions[0].start == 144000);
        CHECK(groups.state().tracks[0].regions[1].start == 216000);
        CHECK(groups.state().tracks[0].regions[1].sourceOffset == 24000);
        before = groups.state();
        rejects([&] {
            groups.captureClips(1, {0, 256}, false, true, gr());
        });
        CHECK(groups.state().tracks == before.tracks && gr() == before.revision);
        rejects([&] {
            groups.captureClips(1, {}, false, false, gr());
        });
        rejects([&] {
            groups.captureClips(1, {0}, false, false, gr() - 1);
        });

        Session takes;
        auto tr = [&] {
            return takes.state().revision;
        };
        takes.import("Base", source, tr());
        takes.addTake(1, "Alternate", take, 0, tr());
        takes.compRange(1, 1, 0, 48000, tr());
        takes.add("New", tr());
        const auto alternate = takes.captureClips(1, {0}, false, false, tr());
        takes.pasteClips(alternate, 2, 0, tr());
        CHECK(takes.state().tracks[1].audio == take &&
              takes.state().tracks[1].regions[0].take == 0);
        takes.moveClipToTrack(1, 0, 2, 96000, tr());
        CHECK(takes.state().tracks[0].regions.empty());
        CHECK(takes.state().tracks[0].audio == source &&
              takes.state().tracks[0].takes[0].audio == take);
        auto noOp = tr();
        takes.moveClipToTrack(2, 1, 2, 96000, tr());
        CHECK(tr() == noOp);
        // Capacity failure cannot leave an attached source or an erased original.
        takes.import("Full", other, tr());
        for (int i = 0; i < 15; ++i)
            takes.addTake(3, "Stored", audio(.01f * float(i)), 0, tr());
        before = takes.state();
        rejects([&] {
            takes.moveClipToTrack(2, 0, 3, 192000, tr());
        });
        CHECK(takes.state().tracks == before.tracks && tr() == before.revision);

        Session midi;
        auto mr = [&] {
            return midi.state().revision;
        };
        midi.add("Keys", mr());
        midi.add("Target", mr());
        MidiClip a{24000, 48000, {{6000, 12000, 65, 3, 81}}, 7, 0x123456};
        MidiClip b{120000, 24000, {{0, 12000, 50, 0, 120}}, 4, 0x445566};
        midi.addMidiClip(1, a, mr());
        midi.addMidiClip(1, b, mr());
        const auto notes = midi.captureClips(1, {1, 0}, true, true, mr());
        CHECK(notes.midi() && notes.length() == 120000 && midi.state().tracks[0].midiClips.empty());
        midi.removeTrack(1, mr());
        for (uint64_t start : {0ULL, 240000ULL, 480000ULL})
            midi.pasteClips(notes, 2, start, mr());
        CHECK(midi.state().tracks[0].midiClips.size() == 6);
        CHECK(midi.state().tracks[0].midiClips[4].notes == a.notes);
        CHECK(midi.state().tracks[0].midiClips[4].track == a.track);
        CHECK(midi.state().tracks[0].midiClips[5].start == 576000);
        before = midi.state();
        rejects([&] {
            midi.pasteClips(notes, 2, 240000, mr());
        });
        CHECK(midi.state().tracks == before.tracks && mr() == before.revision);
        std::cout << "PASS: " << checks << " clipboard model assertions\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
