// e2e: the built-in vocal preparation workflow (EXT-01) through the public ABI.
//
// A user journey: import three "vocal" takes with analytically known levels,
// preview the module's deterministic pre-fader plan, check its measurements and
// gain suggestions against fixture math AND against peak/RMS recomputed here by
// the harness's own WAV reader, prove preview is read-only, apply the plan as
// ONE atomic revision (undo restores the exact pre-commit model), then
// save/reopen and export — the committed fader moves must be audible, measured
// as a before/after pair of renders so "closer to the documented target" is a
// fact this file checks rather than a claim it repeats. The generic workflow API
// (preview/commit of a rename + gain batch) is covered on the same session,
// because vocal.prepare is documented to decompose into it.
//
// Everything here is 48 kHz PCM WAV plus plain model calls, so the whole
// scenario runs identically on Linux and macOS: no Audio Unit, no device.
#include "e2e.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace e2e;

namespace {

constexpr uint32_t kTakeFrames = 2 * kProjectRate;  // every fixture is two seconds
constexpr double kTargetRms = -18.0;
constexpr double kCeiling = -1.0;
constexpr double kDoubleOffset = -6.0;

struct Fixture {
    const char* name;
    double amplitude;
    double hz;
};

// Deterministic takes with power-of-two amplitudes, so peak and RMS in dB are
// exact anchors: peak_db = 20*log10(a), rms_db = 20*log10(a/sqrt(2)).
const Fixture kFixtures[3] = {{"Take A", 0.25, 440.0}, {"Take B", 0.125, 330.0}, {"Take C", 0.0625, 550.0}};

double dbfs(double amplitude) { return 20.0 * std::log10(amplitude); }

// Peak/RMS of a file, measured by THIS scenario with the harness's independent
// WAV reader (never by the engine's own analysis code) so the ABI's reported
// measurements can be checked against the bytes the media really holds. The
// definitions are the ones docs/40 promises: a sample peak over both channels
// and a stereo RMS from the summed channel energies, sqrt((rmsL^2+rmsR^2)/2).
struct IndependentLevels {
    double peakDb = 0.0;
    double rmsDb = 0.0;
    uint64_t frames = 0;
};

IndependentLevels independentLevels(const std::filesystem::path& file) {
    const Wav wav = readWavFixture(file);
    CHECK(wav.channels >= 1 && !wav.samples.empty());
    double peak = 0.0, energy = 0.0;
    for (const float sample : wav.samples) {
        peak = std::max(peak, static_cast<double>(std::abs(sample)));
        energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    IndependentLevels levels;
    levels.frames = wav.frames();
    levels.peakDb = dbfs(peak);
    levels.rmsDb = dbfs(std::sqrt(energy / static_cast<double>(wav.samples.size())));
    return levels;
}

// One take identified by its tone frequency, measured inside a rendered mix by
// the harness's own Goertzel; reported as sine RMS dBFS, the quantity docs/40
// normalizes to. Frequencies are far apart, so leakage between takes is nil.
double toneRmsDb(const Wav& mix, uint32_t fromFrame, uint32_t toFrame, double hz) {
    const double amplitude = std::max(toneMagnitude(mix, fromFrame, toFrame, 0, hz),
                                      toneMagnitude(mix, fromFrame, toFrame, 1, hz));
    if (!(amplitude > 0.0)) throw std::runtime_error("fixture tone vanished from the render");
    return dbfs(amplitude / std::sqrt(2.0));
}

struct VocalPreview {
    std::vector<daw_vocal_preview> items;
    uint32_t queriedCount = 0;
    uint64_t afterRevision = 0;
};

// Queries the item count with items=NULL/capacity=0 first, then reads the real
// buffer, exactly like the app's Workflow dialog does.
VocalPreview previewVocal(daw_session* session, const std::vector<uint64_t>& selected, const std::string& base,
                          double target, double ceiling, double offset, uint64_t expected) {
    VocalPreview result;
    uint64_t after = 0;
    uint32_t count = 0;
    CHECK_OK(session, daw_preview_vocal_preparation(session, selected.data(), static_cast<uint32_t>(selected.size()),
                                                    base.c_str(), target, ceiling, offset, expected, nullptr, 0,
                                                    &count, &after));
    result.queriedCount = count;
    result.items.resize(count);
    for (auto& item : result.items) item.struct_size = sizeof(daw_vocal_preview);
    CHECK_OK(session, daw_preview_vocal_preparation(session, selected.data(), static_cast<uint32_t>(selected.size()),
                                                    base.c_str(), target, ceiling, offset, expected,
                                                    result.items.data(), count, &count, &after));
    result.afterRevision = after;
    return result;
}

// A Dump comparison that ignores the revision undo/redo mints for itself; the
// exact revision arithmetic is asserted separately with rev().
Dump contentOf(daw_session* session) {
    auto dump = dumpOf(session);
    dump.revision = 0;
    return dump;
}

Dump withoutRevision(const Dump& base) {
    Dump dump = base;
    dump.revision = 0;
    return dump;
}

daw_workflow_operation renameOperation(uint64_t trackId, const std::string& name) {
    auto operation = abi<daw_workflow_operation>();
    operation.kind = 1;  // header: 1 = rename track
    operation.track_id = trackId;
    std::memcpy(operation.name, name.data(), name.size());
    return operation;
}

daw_workflow_operation gainOperation(uint64_t trackId, double gainDb) {
    auto operation = abi<daw_workflow_operation>();
    operation.kind = 2;  // header: 2 = set track gain
    operation.track_id = trackId;
    operation.gain_db = gainDb;
    return operation;
}

// Reads a workflow preview with the NULL-query then full-read pattern.
struct WorkflowPreview {
    std::vector<daw_workflow_change> changes;
    uint32_t queriedCount = 0;
    uint64_t afterRevision = 0;
};

WorkflowPreview previewWorkflow(daw_session* session, const std::vector<daw_workflow_operation>& operations,
                                uint64_t expected) {
    WorkflowPreview result;
    uint32_t count = 0;
    uint64_t after = 0;
    const uint32_t size = static_cast<uint32_t>(operations.size());
    CHECK_OK(session, daw_preview_workflow(session, operations.data(), size, expected, nullptr, 0, &count, &after));
    result.queriedCount = count;
    result.changes.resize(count);
    for (auto& change : result.changes) change.struct_size = sizeof(daw_workflow_change);
    CHECK_OK(session, daw_preview_workflow(session, operations.data(), size, expected, result.changes.data(),
                                           static_cast<uint32_t>(result.changes.size()), &count, &after));
    result.afterRevision = after;
    return result;
}

}  // namespace

int main() {
    try {
        TempRoot root("vocal-workflow");
        Bridge session;
        CHECK(snapshotOf(session.get()).revision == 0);

        // 1. Three imported takes, one per track, in the order the user will
        //    select them: Lead first, then the doubles.
        for (const auto& fixture : kFixtures) {
            const auto path = root / (std::string(fixture.name) + ".wav");
            writeWavFixture(path, sineInterleaved(kTakeFrames, kProjectRate, fixture.hz, fixture.amplitude, 2),
                            kProjectRate, 2, "f32");
        }
        const auto importStart = rev(session.get());
        for (uint32_t index = 0; index < 3; ++index) {
            const auto before = rev(session.get());
            CHECK_OK(session.get(), daw_import_wav(session.get(),
                                                   (root / (std::string(kFixtures[index].name) + ".wav")).string().c_str(),
                                                   kFixtures[index].name, before));
            CHECK(rev(session.get()) == before + 1);
        }
        CHECK(rev(session.get()) == importStart + 3);
        std::vector<uint64_t> selected;
        for (uint32_t index = 0; index < 3; ++index) {
            const auto track = trackById(session.get(), index + 1);
            CHECK(std::string(track.name) == kFixtures[index].name);
            CHECK(track.clip_count == 1 && track.take_count == 1);
            CHECK(track.audio_frames == kTakeFrames);
            selected.push_back(track.id);
        }
        const auto built = dumpOf(session.get());
        CHECK(built.tracks.size() == 3 && built.buses.empty());

        // 2. Preview with items=NULL/capacity=0 reports the item count, then a
        //    real buffer carries one item per selected track, Lead first.
        const uint64_t modelRevision = rev(session.get());
        const auto plan = previewVocal(session.get(), selected, "Vocal", kTargetRms, kCeiling, kDoubleOffset,
                                       modelRevision);
        CHECK(plan.queriedCount == 3);
        CHECK(plan.items.size() == 3);
        CHECK(plan.afterRevision == modelRevision + 1);  // a plan would cost exactly one revision
        for (uint32_t index = 0; index < 3; ++index) {
            const auto& item = plan.items[index];
            const auto& fixture = kFixtures[index];
            const double amplitude = fixture.amplitude;
            CHECK(item.track_id == selected[index]);
            CHECK(item.analyzed_frames == kTakeFrames);
            // docs/40: honest peak/RMS of the pre-fader, pre-pan track signal.
            expectNear(item.peak_db, dbfs(amplitude), 0.3, "measured peak_db");
            expectNear(item.rms_db, dbfs(amplitude / std::sqrt(2.0)), 0.3, "measured rms_db");
            // ...and the SAME two numbers measured from the imported file by the
            // harness's own reader: the plan must describe the material, not a
            // model the engine invented. Independent of the closed-form anchor
            // above, this is the check that would catch an analysis lying about
            // offsets, fades or the channel it reports.
            const auto onDisk = independentLevels(root / (std::string(fixture.name) + ".wav"));
            CHECK(onDisk.frames == item.analyzed_frames);
            expectNear(onDisk.peakDb, item.peak_db, 0.05, "harness peak vs reported peak_db");
            expectNear(onDisk.rmsDb, item.rms_db, 0.05, "harness RMS vs reported rms_db");
            CHECK(std::isfinite(item.proposed_gain_db));
            // The suggested absolute fader moves this track's RMS onto its own
            // target (Lead at target, doubles at target + offset).
            const double roleTarget = kTargetRms + (index == 0 ? 0.0 : kDoubleOffset);
            expectNear(item.proposed_gain_db, roleTarget - item.rms_db, 0.05, "proposed_gain_db");
            expectNear(item.predicted_rms_db, roleTarget, 0.5, "predicted_rms_db");
            // Peak-ceiling policy (docs/40): the proposal is the SMALLER of the
            // RMS gain and the ceiling gain, so the predicted peak stays at or
            // below the ceiling and only flags peak_limited when it bound.
            CHECK(item.predicted_peak_db <= kCeiling + 0.05);
            CHECK(item.peak_limited == 0);
            // Algebraic invariants of the ABI: predictions are measured values
            // plus the one proposed fader.
            expectNear(item.predicted_rms_db, item.rms_db + item.proposed_gain_db, 1e-9, "rms prediction identity");
            expectNear(item.predicted_peak_db, item.peak_db + item.proposed_gain_db, 1e-9, "peak prediction identity");
            // Rename: first selection is the Lead, the rest are numbered doubles.
            const std::string expectedName = std::string("Vocal") + (index == 0 ? " Lead" : " Double " + std::to_string(index));
            CHECK(std::string(item.before_name) == fixture.name);
            CHECK(std::string(item.after_name) == expectedName);
        }
        // Every halving of amplitude costs 6.02 dB of RMS, so with the doubles
        // aimed at -24 dBFS the quietest take needs the biggest lift: -2.93 dB
        // for a -21.07 dBFS RMS take versus +3.09 dB for a -27.09 dBFS one,
        // while the hot Lead is pulled down as well.
        CHECK(plan.items[1].proposed_gain_db < plan.items[2].proposed_gain_db);
        CHECK(plan.items[0].proposed_gain_db < 0.0);
        expectNear(plan.items[2].proposed_gain_db, 3.09, 0.2, "quiet take needs a lift");

        // 3. Preview is read-only: the revision and the whole model dump are
        //    untouched, however often it runs.
        CHECK(rev(session.get()) == modelRevision);
        CHECK(dumpOf(session.get()) == built);
        const auto again = previewVocal(session.get(), selected, "Vocal", kTargetRms, kCeiling, kDoubleOffset,
                                        modelRevision);
        CHECK(again.items.size() == 3);
        for (uint32_t index = 0; index < 3; ++index) {
            CHECK(again.items[index].proposed_gain_db == plan.items[index].proposed_gain_db);
            CHECK(again.items[index].peak_db == plan.items[index].peak_db);
            CHECK(again.items[index].rms_db == plan.items[index].rms_db);
        }
        CHECK(dumpOf(session.get()) == built);
        // A different base name changes only the plan, never the project.
        const auto otherNames = previewVocal(session.get(), selected, "Дубль", kTargetRms, kCeiling, kDoubleOffset,
                                             modelRevision);
        CHECK(std::string(otherNames.items[0].after_name) == "Дубль Lead");
        CHECK(std::string(otherNames.items[2].after_name) == "Дубль Double 2");
        CHECK(dumpOf(session.get()) == built);

        // 4. The peak ceiling is a real limit: ask for a target the ceiling
        //    cannot afford and the plan reports the bound instead of the wish.
        const auto bound = previewVocal(session.get(), selected, "Vocal", -6.0, -18.0, 0.0, modelRevision);
        CHECK(bound.items.size() == 3);
        for (const auto& item : bound.items) {
            CHECK(item.peak_limited == 1);
            expectNear(item.predicted_peak_db, -18.0, 0.3, "ceiling-bound predicted peak");
            CHECK(item.predicted_rms_db < -6.0 + 0.05);  // the RMS target was NOT reached
            expectNear(item.predicted_rms_db, item.rms_db + item.proposed_gain_db, 1e-9, "bound rms identity");
        }
        CHECK(rev(session.get()) == modelRevision);
        CHECK(dumpOf(session.get()) == built);

        // 5. Invalid plans are refused before anything is written, and every
        //    rejection leaves a human-readable reason (docs/40 + the header).
        auto tryPreview = [&](const std::vector<uint64_t>& ids, const std::string& base, double target,
                              double ceiling, double offset, uint64_t expected, daw_vocal_preview* items,
                              uint32_t capacity) {
            uint32_t count = 0;
            uint64_t after = 0;
            return daw_preview_vocal_preparation(session.get(), ids.data(), static_cast<uint32_t>(ids.size()),
                                                 base.c_str(), target, ceiling, offset, expected, items, capacity,
                                                 &count, &after);
        };
        std::vector<daw_vocal_preview> buffer(3);
        for (auto& item : buffer) item.struct_size = sizeof(daw_vocal_preview);
        const std::vector<uint64_t> single;
        const std::vector<uint64_t> unknown = {selected[0], 999, selected[1]};
        const std::vector<uint64_t> duplicated = {selected[0], selected[0], selected[1]};
        CHECK_REJ(session.get(), tryPreview(single, "Vocal", kTargetRms, kCeiling, kDoubleOffset, modelRevision,
                                            nullptr, 0));
        CHECK_REJ(session.get(), tryPreview(unknown, "Vocal", kTargetRms, kCeiling, kDoubleOffset, modelRevision,
                                            buffer.data(), 3));
        CHECK_REJ(session.get(), tryPreview(duplicated, "Vocal", kTargetRms, kCeiling, kDoubleOffset, modelRevision,
                                            buffer.data(), 3));
        CHECK_REJ(session.get(), tryPreview(selected, "", kTargetRms, kCeiling, kDoubleOffset, modelRevision,
                                            buffer.data(), 3));
        CHECK_REJ(session.get(), tryPreview(selected, std::string(200, 'x'), kTargetRms, kCeiling, kDoubleOffset,
                                            modelRevision, buffer.data(), 3));
        // The numeric ranges probed below (target RMS -60…-6, peak ceiling -18…0,
        // double offset -24…0, names of 1…120 characters, 1…32 selected tracks)
        // appear NOWHERE in daw.h or docs/40 — they are observed bridge behavior.
        // They are frozen here as an executable contract so a future change is a
        // deliberate edit of this file instead of a silent regression, and they
        // are reported as a documentation gap alongside the scenario.
        for (const double target : {-61.0, -5.0, std::numeric_limits<double>::quiet_NaN(),
                                    std::numeric_limits<double>::infinity()}) {
            CHECK_REJ(session.get(), tryPreview(selected, "Vocal", target, kCeiling, kDoubleOffset, modelRevision,
                                                buffer.data(), 3));
        }
        for (const double ceiling : {-20.0, 1.0, std::numeric_limits<double>::quiet_NaN()}) {
            CHECK_REJ(session.get(), tryPreview(selected, "Vocal", kTargetRms, ceiling, kDoubleOffset, modelRevision,
                                                buffer.data(), 3));
        }
        for (const double offset : {1.0, -25.0, std::numeric_limits<double>::quiet_NaN()}) {
            CHECK_REJ(session.get(), tryPreview(selected, "Vocal", kTargetRms, kCeiling, offset, modelRevision,
                                                buffer.data(), 3));
        }
        CHECK_REJ(session.get(), daw_preview_vocal_preparation(session.get(), selected.data(), 3, "Vocal", kTargetRms,
                                                               kCeiling, kDoubleOffset, modelRevision, nullptr, 3,
                                                               nullptr, nullptr));
        CHECK_REJ(session.get(), tryPreview(selected, "Vocal", kTargetRms, kCeiling, kDoubleOffset,
                                            modelRevision + 5, buffer.data(), 3));
        CHECK_REJ(session.get(), tryPreview(selected, "Vocal", kTargetRms, kCeiling, kDoubleOffset, modelRevision,
                                            buffer.data(), 2));
        buffer[1].struct_size = sizeof(daw_vocal_preview) - 1;
        CHECK_REJ(session.get(), tryPreview(selected, "Vocal", kTargetRms, kCeiling, kDoubleOffset, modelRevision,
                                            buffer.data(), 3));
        buffer[1].struct_size = sizeof(daw_vocal_preview);
        CHECK_REJ(session.get(), daw_commit_vocal_preparation(session.get(), selected.data(), 3, "", kTargetRms,
                                                              kCeiling, kDoubleOffset, modelRevision));
        CHECK_REJ(session.get(), daw_commit_vocal_preparation(session.get(), unknown.data(), 3, "Vocal", kTargetRms,
                                                              kCeiling, kDoubleOffset, modelRevision));
        CHECK_REJ(session.get(), daw_commit_vocal_preparation(session.get(), selected.data(), 3, "Vocal", kTargetRms,
                                                              kCeiling, kDoubleOffset, modelRevision + 5));
        CHECK(rev(session.get()) == modelRevision);
        CHECK(dumpOf(session.get()) == built);

        // 6. Apply: the whole plan is ONE revision and ONE undo step.
        const auto preCommit = dumpOf(session.get());
        // The honest "before" of the loudness claim, rendered by the same
        // offline path the app exports with and read back by this file. It is
        // taken BEFORE the commit and must not touch the model: an export is
        // not a project event.
        const auto mixBefore = exportProject(session.get(), root / "vocal-mix-before.wav", 2);
        CHECK(fileNonEmpty(root / "vocal-mix-before.wav"));
        CHECK(rev(session.get()) == modelRevision);
        CHECK(dumpOf(session.get()) == preCommit);
        CHECK_OK(session.get(), daw_commit_vocal_preparation(session.get(), selected.data(), 3, "Vocal", kTargetRms,
                                                             kCeiling, kDoubleOffset, modelRevision));
        CHECK(rev(session.get()) == modelRevision + 1);
        const auto committed = dumpOf(session.get());
        CHECK(committed.tracks.size() == 3);
        for (uint32_t index = 0; index < 3; ++index) {
            const std::string expectedName = "Vocal" + (index == 0 ? std::string(" Lead")
                                                                    : " Double " + std::to_string(index));
            CHECK(committed.tracks[index].name == expectedName);
            expectNear(committed.tracks[index].gainDb, plan.items[index].proposed_gain_db, 1e-9,
                       "committed fader equals the previewed proposal");
            CHECK(committed.tracks[index].clips == preCommit.tracks[index].clips);  // audio untouched
        }
        CHECK(snapshotOf(session.get()).can_undo == 1);
        const auto committedRevision = rev(session.get());
        CHECK_OK(session.get(), daw_undo(session.get(), committedRevision));
        CHECK(rev(session.get()) == committedRevision + 1);  // one step back, one step forward
        CHECK(contentOf(session.get()) == withoutRevision(preCommit));
        CHECK_OK(session.get(), daw_redo(session.get(), rev(session.get())));
        CHECK(contentOf(session.get()) == withoutRevision(committed));
        // Re-applying the same plan is now a no-op: identical names and gains
        // mint no second revision (the module never double-stacks).
        const auto idempotentRevision = rev(session.get());
        CHECK_OK(session.get(), daw_commit_vocal_preparation(session.get(), selected.data(), 3, "Vocal", kTargetRms,
                                                             kCeiling, kDoubleOffset, idempotentRevision));
        CHECK(rev(session.get()) == idempotentRevision);

        // 7. The prepared project is durable and audible.
        const auto saved = dumpOf(session.get());
        const auto draft = root / "vocal.mydawdraft";
        saveDraftAndWait(session.get(), draft);
        CHECK(fileNonEmpty(draft));
        Bridge reopened;
        CHECK_OK(reopened.get(), daw_open_draft(reopened.get(), draft.string().c_str()));
        const auto restored = dumpOf(reopened.get());
        if (!(restored == saved))
            throw std::runtime_error("vocal round-trip changed the project:\nBEFORE:\n" + describe(saved) +
                                     "\nAFTER:\n" + describe(restored));
        const auto mix = exportProject(reopened.get(), root / "vocal-mix.wav", 2);
        CHECK(mix.frames() >= kTakeFrames);
        for (size_t index = 0; index < mix.samples.size(); ++index)
            if (!std::isfinite(mix.samples[index]))
                throw std::runtime_error("non-finite sample in the vocal mix at index " + std::to_string(index));
        // Each committed fader must be measurable in the rendered mix: the
        // module promised RMS -18 dBFS on the Lead and -24 dBFS on both
        // doubles, i.e. sine amplitudes of 10^(dB/20)*sqrt(2).
        const double leadAmplitude = std::pow(10.0, kTargetRms / 20.0) * std::sqrt(2.0);
        const double doubleAmplitude = std::pow(10.0, (kTargetRms + kDoubleOffset) / 20.0) * std::sqrt(2.0);
        const uint32_t steadyFrom = kProjectRate / 10;
        const uint32_t steadyTo = static_cast<uint32_t>(mix.frames()) - kProjectRate / 20;
        const uint32_t channels = mix.channels;
        for (uint32_t index = 0; index < 3; ++index) {
            const double wanted = index == 0 ? leadAmplitude : doubleAmplitude;
            const double measured = std::max(toneMagnitude(mix, steadyFrom, steadyTo, 0, kFixtures[index].hz),
                                             channels > 1 ? toneMagnitude(mix, steadyFrom, steadyTo, 1,
                                                                          kFixtures[index].hz)
                                                          : 0.0);
            expectNear(20.0 * std::log10(measured), 20.0 * std::log10(wanted), 1.0,
                       std::string("rendered level of ") + kFixtures[index].name);
        }
        const double leadTone = toneMagnitude(mix, steadyFrom, steadyTo, 0, kFixtures[0].hz);
        const double doubleTone = toneMagnitude(mix, steadyFrom, steadyTo, 0, kFixtures[1].hz);
        CHECK(leadTone > doubleTone * 1.4);  // the Lead really sits above its doubles
        CHECK(channelPeak(mix, steadyFrom, steadyTo, 0) > 0.05);
        CHECK(channelPeak(mix, steadyFrom, steadyTo, 0) < 1.0);  // nothing landed on the safety clamp

        // Before/after gain staging measured end to end by this file alone: two
        // exports of the same project, read back by the harness. Every take
        // starts off-target, ends on the docs/40 normalization (Lead at the RMS
        // target, doubles at target + offset) and moves by exactly the fader the
        // preview had proposed — so it is the committed plan that changed the
        // sound, not a coincidence of the render path.
        CHECK(mixBefore.frames() == mix.frames());
        for (uint32_t index = 0; index < 3; ++index) {
            const double roleTarget = kTargetRms + (index == 0 ? 0.0 : kDoubleOffset);
            const double beforeDb = toneRmsDb(mixBefore, steadyFrom, steadyTo, kFixtures[index].hz);
            const double afterDb = toneRmsDb(mix, steadyFrom, steadyTo, kFixtures[index].hz);
            const double distanceBefore = std::abs(beforeDb - roleTarget);
            const double distanceAfter = std::abs(afterDb - roleTarget);
            CHECK(distanceBefore > 2.0);  // the unprepared take really needed the module
            expectNear(afterDb, roleTarget, 0.5, "rendered RMS vs the documented target");
            CHECK(distanceAfter * 4.0 < distanceBefore);  // and now measurably closer to the goal
            expectNear(afterDb - beforeDb, plan.items[index].proposed_gain_db, 0.25,
                       "rendered move vs the proposed fader");
        }

        // 8. The generic workflow API behind vocal.prepare: kind 1 renames and
        //    kind 2 sets gain, previewed as a diff and committed atomically.
        const uint64_t synth = addTrack(session.get(), "Synth");
        const uint64_t bass = addTrack(session.get(), "Bass");
        const uint64_t workflowRevision = rev(session.get());
        const std::vector<daw_workflow_operation> operations = {renameOperation(synth, "Analog Lead"),
                                                               gainOperation(bass, -9.0)};
        const auto workflow = previewWorkflow(session.get(), operations, workflowRevision);
        CHECK(workflow.queriedCount == 2);
        CHECK(workflow.afterRevision == workflowRevision + 1);
        CHECK(rev(session.get()) == workflowRevision);  // preview stays read-only
        CHECK(workflow.changes.size() == 2);
        CHECK(workflow.changes[0].track_id == synth);
        CHECK(std::string(workflow.changes[0].before_name) == "Synth");
        CHECK(std::string(workflow.changes[0].after_name) == "Analog Lead");
        expectNear(workflow.changes[0].before_gain_db, 0.0, 1e-12, "rename leaves gain (before)");
        expectNear(workflow.changes[0].after_gain_db, 0.0, 1e-12, "rename leaves gain (after)");
        CHECK(workflow.changes[1].track_id == bass);
        CHECK(std::string(workflow.changes[1].before_name) == "Bass");
        CHECK(std::string(workflow.changes[1].after_name) == "Bass");
        expectNear(workflow.changes[1].before_gain_db, 0.0, 1e-12, "gain before");
        expectNear(workflow.changes[1].after_gain_db, -9.0, 1e-12, "gain after");
        // A batch that changes nothing reports zero changes and no revision.
        const auto noop = previewWorkflow(session.get(), std::vector<daw_workflow_operation>{renameOperation(synth, "Synth")},
                                          workflowRevision);
        CHECK(noop.queriedCount == 0 && noop.changes.empty());
        CHECK(noop.afterRevision == workflowRevision);
        // Buffer and argument contract, plus the batch-level validation rules.
        daw_workflow_change one[1];
        one[0].struct_size = sizeof(daw_workflow_change);
        uint32_t changeCount = 0;
        uint64_t afterRevision = 0;
        CHECK_REJ(session.get(), daw_preview_workflow(session.get(), operations.data(), 2, workflowRevision, one, 1,
                                                      &changeCount, &afterRevision));
        CHECK_REJ(session.get(), daw_preview_workflow(session.get(), operations.data(), 2, workflowRevision, nullptr, 2,
                                                      &changeCount, &afterRevision));
        CHECK_REJ(session.get(), daw_preview_workflow(session.get(), operations.data(), 2, workflowRevision + 4,
                                                      nullptr, 0, &changeCount, &afterRevision));
        CHECK_REJ(session.get(), daw_preview_workflow(session.get(), nullptr, 0, workflowRevision, nullptr, 0,
                                                      &changeCount, &afterRevision));
        auto badKind = renameOperation(synth, "Nope");
        badKind.kind = 3;
        CHECK_REJ(session.get(), daw_preview_workflow(session.get(), &badKind, 1, workflowRevision, nullptr, 0,
                                                      &changeCount, &afterRevision));
        CHECK_REJ(session.get(), daw_commit_workflow(session.get(), &badKind, 1, workflowRevision));
        const std::vector<daw_workflow_operation> orphan = {gainOperation(9999, -3.0)};
        CHECK_REJ(session.get(), daw_commit_workflow(session.get(), orphan.data(), 1, workflowRevision));
        // A stale expected_revision rejects the GENERIC commit as well — with a
        // human-readable reason — and the batch is completely inert: same
        // revision, byte-identical model dump. The very same operations commit
        // successfully a few lines below, so this is the revision gate alone.
        const auto staleBaseline = dumpOf(session.get());
        CHECK_REJ(session.get(), daw_commit_workflow(session.get(), operations.data(), 2, workflowRevision - 1));
        CHECK_REJ(session.get(), daw_preview_workflow(session.get(), operations.data(), 2, workflowRevision - 1,
                                                      nullptr, 0, &changeCount, &afterRevision));
        CHECK(rev(session.get()) == workflowRevision);
        CHECK(dumpOf(session.get()) == staleBaseline);
        std::vector<daw_workflow_operation> oversize;
        for (int index = 0; index < 101; ++index) oversize.push_back(gainOperation(synth, -1.0));
        CHECK_REJ(session.get(), daw_preview_workflow(session.get(), oversize.data(), 101, workflowRevision, nullptr, 0,
                                                      &changeCount, &afterRevision));
        CHECK_REJ(session.get(), daw_commit_workflow(session.get(), oversize.data(), 101, workflowRevision));
        // One bad member rejects the whole batch (docs/07: no partial commit).
        const std::vector<daw_workflow_operation> halfValid = {renameOperation(synth, "Fine"),
                                                              renameOperation(bass, "\xED\xA0\x80")};
        const auto beforeAtomic = dumpOf(session.get());
        CHECK_REJ(session.get(), daw_commit_workflow(session.get(), halfValid.data(), 2, workflowRevision));
        CHECK(dumpOf(session.get()) == beforeAtomic);
        CHECK(rev(session.get()) == workflowRevision);

        // Commit the valid batch: exactly one revision for both operations.
        const auto workflowBaseline = dumpOf(session.get());
        CHECK_OK(session.get(), daw_commit_workflow(session.get(), operations.data(), 2, workflowRevision));
        CHECK(rev(session.get()) == workflowRevision + 1);
        const auto applied = dumpOf(session.get());
        CHECK(applied.tracks.size() == 5);
        CHECK(applied.tracks[3].name == "Analog Lead");
        CHECK(applied.tracks[4].name == "Bass");
        expectNear(applied.tracks[3].gainDb, 0.0, 1e-12, "rename-only op left the fader alone");
        expectNear(applied.tracks[4].gainDb, -9.0, 1e-12, "gain op applied");
        CHECK(applied.tracks[0].name == workflowBaseline.tracks[0].name);  // the vocal strip is untouched
        CHECK_OK(session.get(), daw_undo(session.get(), rev(session.get())));
        CHECK(contentOf(session.get()) == withoutRevision(workflowBaseline));
        CHECK_OK(session.get(), daw_redo(session.get(), rev(session.get())));
        CHECK(contentOf(session.get()) == withoutRevision(applied));
        // A committed no-op batch mints no revision (preview said 0 changes).
        const auto afterRedo = rev(session.get());
        CHECK_OK(session.get(), daw_commit_workflow(session.get(), operations.data(), 2, afterRedo));
        CHECK(rev(session.get()) == afterRedo);

        std::cout << "PASS: e2e_workflow_vocal — three-take vocal plan previewed against fixture math and the "
                     "harness's own peak/RMS reader, read-only previews, inert stale/invalid batches, one-revision "
                     "atomic commit, whole-operation undo/redo, save+reopen, before/after renders closer to target, "
                     "generic workflow batch\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}

