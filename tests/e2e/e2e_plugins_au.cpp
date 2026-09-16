// e2e: hosted Audio Units, from the approved catalog to audible WAV output.
//
// A user journey through the SAME public C ABI the macOS app uses:
//   scan the approved matrix -> add each AU to master -> read/set parameters ->
//   bypass/move/remove with exact undo fidelity -> the same strip API on track
//   and bus owners (owner pairs, hosting policy, runtime status) -> plugin
//   parameter automation -> save and reopen the captured AU state -> and
//   finally prove the hosted limiter really reshapes rendered audio.
//
// Platform policy: everything that needs a live Audio Unit is inside __APPLE__.
// Non-Apple builds link engine/audio/effect_stub.cpp, whose approved catalog is
// EMPTY, so the bridge answers daw_scan_supported_au with rc=0 and count=0 (NOT
// an error) and rejects every catalog-dependent call with human-readable text.
// The #else branch asserts exactly that honest stub behaviour.
//
// Owner and ABI-shape rejections need no plugin at all, so they run everywhere.
#include "e2e.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace e2e;

namespace {

#ifdef __APPLE__
uint32_t scanCatalog(daw_session* session) {
    uint32_t count = 0;
    CHECK_OK(session, daw_scan_supported_au(session, &count));
    return count;
}

daw_au_component componentAt(daw_session* session, uint32_t index) {
    auto component = abi<daw_au_component>();
    CHECK_OK(session, daw_get_supported_au(session, index, &component));
    return component;
}

daw_au_component componentNamed(daw_session* session, uint32_t count, const std::string& needle) {
    for (uint32_t index = 0; index < count; ++index) {
        const auto component = componentAt(session, index);
        if (std::string(component.name).find(needle) != std::string::npos) return component;
    }
    throw std::runtime_error("approved catalog has no component matching \"" + needle + "\"");
}

daw_plugin masterInsertAt(daw_session* session, uint32_t index) {
    auto plugin = abi<daw_plugin>();
    CHECK_OK(session, daw_get_master_insert(session, index, &plugin));
    return plugin;
}

daw_plugin busInsertAt(daw_session* session, uint64_t busId, uint32_t index) {
    auto plugin = abi<daw_plugin>();
    CHECK_OK(session, daw_get_insert(session, DAW_INSERT_OWNER_BUS, busId, index, &plugin));
    return plugin;
}

uint32_t parameterCountOf(daw_session* session, int32_t owner, uint64_t ownerId, uint64_t pluginId) {
    uint32_t count = 0;
    if (owner == DAW_INSERT_OWNER_MASTER)
        CHECK_OK(session, daw_get_master_insert_parameter_count(session, pluginId, &count));
    else
        CHECK_OK(session, daw_get_insert_parameter_count(session, owner, ownerId, pluginId, &count));
    return count;
}

daw_au_parameter parameterAt(daw_session* session, int32_t owner, uint64_t ownerId, uint64_t pluginId,
                             uint32_t index) {
    auto parameter = abi<daw_au_parameter>();
    if (owner == DAW_INSERT_OWNER_MASTER)
        CHECK_OK(session, daw_get_master_insert_parameter(session, pluginId, index, &parameter));
    else
        CHECK_OK(session, daw_get_insert_parameter(session, owner, ownerId, pluginId, index, &parameter));
    return parameter;
}

std::vector<daw_au_parameter> parametersOf(daw_session* session, int32_t owner, uint64_t ownerId,
                                           uint64_t pluginId) {
    std::vector<daw_au_parameter> parameters;
    const uint32_t count = parameterCountOf(session, owner, ownerId, pluginId);
    for (uint32_t index = 0; index < count; ++index)
        parameters.push_back(parameterAt(session, owner, ownerId, pluginId, index));
    return parameters;
}

// The first control a user may safely move: writable, with a sane native range,
// and not one of the documented level controls (this scenario keeps every
// rendered gain deterministic).
size_t firstSafeWritable(const std::vector<daw_au_parameter>& parameters) {
    for (size_t index = 0; index < parameters.size(); ++index) {
        const auto& parameter = parameters[index];
        if (!parameter.writable) continue;
        const std::string name = parameter.name;
        if (name.find("Gain") != std::string::npos || name.find("Level") != std::string::npos) continue;
        if (!std::isfinite(parameter.minimum) || !std::isfinite(parameter.maximum) ||
            parameter.maximum <= parameter.minimum)
            continue;
        return index;
    }
    throw std::runtime_error("plug-in publishes no safe writable parameter");
}

float upperQuarter(const daw_au_parameter& parameter) {
    return parameter.minimum + (parameter.maximum - parameter.minimum) * 0.75f;
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

daw_insert_hosting_status hostingOf(daw_session* session, int32_t owner, uint64_t ownerId, uint64_t pluginId) {
    auto status = abi<daw_insert_hosting_status>();
    status.version = DAW_INSERT_HOSTING_STATUS_VERSION;
    CHECK_OK(session, daw_get_insert_hosting_status(session, owner, ownerId, pluginId, &status));
    return status;
}

daw_insert_runtime_status runtimeOf(daw_session* session, int32_t owner, uint64_t ownerId, uint64_t pluginId) {
    auto status = abi<daw_insert_runtime_status>();
    status.version = DAW_INSERT_RUNTIME_STATUS_VERSION;
    CHECK_OK(session, daw_get_insert_runtime_status(session, owner, ownerId, pluginId, &status));
    return status;
}

// True when every sample of an exported WAV is a real number. The harness
// reader decodes IEEE float32 verbatim, so NaN/Inf from hosted DSP shows up.
void expectAllFinite(const Wav& wav, const std::string& what) {
    for (size_t index = 0; index < wav.samples.size(); ++index)
        if (!std::isfinite(wav.samples[index]))
            throw std::runtime_error("non-finite sample in " + what + " at index " + std::to_string(index));
}

uint32_t automationCount(daw_session* session, int32_t owner, uint64_t ownerId, uint64_t pluginId,
                         uint32_t parameterId) {
    uint32_t count = 0;
    CHECK_OK(session, daw_get_insert_parameter_automation_count(session, owner, ownerId, pluginId, parameterId,
                                                                &count));
    return count;
}
#endif  // __APPLE__

}  // namespace

int main() {
    try {
        TempRoot root("plugins-au");
        Bridge session;

        // 1. Insert chains belong to three owners on every platform, so the
        //    cross-platform shape is checked first: a real track, a real bus
        //    and the master, all with empty chains.
        const uint64_t track = addTrack(session.get(), "Guitar");
        const uint64_t bus = addBus(session.get(), "Drum Bus");
        const uint64_t quiet = rev(session.get());
        uint32_t stripCount = 12345;
        CHECK_OK(session.get(), daw_get_insert_count(session.get(), DAW_INSERT_OWNER_TRACK, track, &stripCount));
        CHECK(stripCount == 0);
        CHECK_OK(session.get(), daw_get_insert_count(session.get(), DAW_INSERT_OWNER_BUS, bus, &stripCount));
        CHECK(stripCount == 0);
        CHECK_OK(session.get(), daw_get_insert_count(session.get(), DAW_INSERT_OWNER_MASTER, 0, &stripCount));
        CHECK(stripCount == 0);
        CHECK(snapshotOf(session.get()).master_insert_count == 0);

        // 2. Malformed owner pairs and unknown owners reject with readable text
        //    and change nothing (docs/45: an invalid owner pair is refused
        //    before any catalog lookup or mutation).
        CHECK_REJ(session.get(), daw_get_insert_count(session.get(), DAW_INSERT_OWNER_MASTER, track, &stripCount));
        CHECK_REJ(session.get(), daw_get_insert_count(session.get(), DAW_INSERT_OWNER_TRACK, 0, &stripCount));
        CHECK_REJ(session.get(), daw_get_insert_count(session.get(), 7, track, &stripCount));
        CHECK_REJ(session.get(), daw_get_insert_count(session.get(), DAW_INSERT_OWNER_TRACK, bus, &stripCount));
        CHECK_REJ(session.get(), daw_get_insert_count(session.get(), DAW_INSERT_OWNER_BUS, track, &stripCount));
        auto probe = abi<daw_plugin>();
        CHECK_REJ(session.get(), daw_get_insert(session.get(), DAW_INSERT_OWNER_TRACK, track, 0, &probe));
        auto hosting = abi<daw_insert_hosting_status>();
        CHECK_REJ(session.get(), daw_get_insert_hosting_status(session.get(), DAW_INSERT_OWNER_TRACK, track, 999,
                                                               &hosting));
        auto runtime = abi<daw_insert_runtime_status>();
        CHECK_REJ(session.get(), daw_get_insert_runtime_status(session.get(), DAW_INSERT_OWNER_BUS, bus, 999,
                                                               &runtime));
        CHECK_REJ(session.get(), daw_set_insert_bypass(session.get(), DAW_INSERT_OWNER_TRACK, track, 999, 1,
                                                       quiet));
        CHECK_REJ(session.get(), daw_remove_insert(session.get(), DAW_INSERT_OWNER_BUS, bus, 999, quiet));
        // A component tuple that was never chosen from the approved catalog is
        // refused: the allowlist is mandatory (docs/36).
        CHECK_REJ(session.get(), daw_add_insert_au(session.get(), 99, track, 1, 2, 3, quiet));
        CHECK_REJ(session.get(), daw_add_insert_au(session.get(), DAW_INSERT_OWNER_TRACK, track, 0x1234, 0x5678,
                                                   0x9ABC, quiet));
        CHECK_REJ(session.get(), daw_add_master_au(session.get(), 0x1234, 0x5678, 0x9ABC, quiet));
        CHECK(rev(session.get()) == quiet);
        CHECK(snapshotOf(session.get()).master_insert_count == 0);

        // 3. Pure ABI-shape rejections (struct_size / NULL out) answer rc=1.
        auto undersized = abi<daw_plugin>();
        undersized.struct_size = sizeof(daw_plugin) - 1;
        CHECK(daw_get_insert(session.get(), DAW_INSERT_OWNER_TRACK, track, 0, &undersized) == 1);
        CHECK(daw_get_insert(session.get(), DAW_INSERT_OWNER_TRACK, track, 0, nullptr) == 1);
        CHECK(daw_get_insert_count(session.get(), DAW_INSERT_OWNER_TRACK, track, nullptr) == 1);
        auto undersizedStatus = abi<daw_insert_hosting_status>();
        undersizedStatus.struct_size = sizeof(daw_insert_hosting_status) - 1;
        CHECK(daw_get_insert_hosting_status(session.get(), DAW_INSERT_OWNER_TRACK, track, 999, &undersizedStatus) == 1);
        CHECK(daw_get_insert_hosting_status(session.get(), DAW_INSERT_OWNER_TRACK, track, 999, nullptr) == 1);
        CHECK(daw_get_insert_runtime_status(session.get(), DAW_INSERT_OWNER_TRACK, track, 999, nullptr) == 1);
        CHECK(rev(session.get()) == quiet);

#ifdef __APPLE__
        // ---------------------------------------------------------------------
        // 4. The approved matrix (docs/36): exactly three Apple effects.
        // ---------------------------------------------------------------------
        const uint32_t catalogCount = scanCatalog(session.get());
        CHECK(catalogCount == 3);
        std::vector<daw_au_component> catalog;
        for (uint32_t index = 0; index < catalogCount; ++index) {
            const auto component = componentAt(session.get(), index);
            CHECK(component.type != 0 && component.subtype != 0 && component.manufacturer != 0);
            CHECK(!std::string(component.name).empty());
            // core_tests precedent: the approved catalog is Apple's own components.
            CHECK(std::string(component.name).find("Apple") != std::string::npos);
            catalog.push_back(component);
        }
        // All three are effects of one type with distinct subtypes.
        CHECK(catalog[0].type == catalog[1].type && catalog[1].type == catalog[2].type);
        CHECK(catalog[0].subtype != catalog[1].subtype && catalog[0].subtype != catalog[2].subtype &&
              catalog[1].subtype != catalog[2].subtype);
        CHECK(catalog[0].manufacturer == catalog[1].manufacturer &&
              catalog[1].manufacturer == catalog[2].manufacturer);
        const auto limiter = componentNamed(session.get(), catalogCount, "PeakLimiter");
        const auto dynamics = componentNamed(session.get(), catalogCount, "DynamicsProcessor");
        const auto reverb = componentNamed(session.get(), catalogCount, "MatrixReverb");
        CHECK(limiter.subtype != dynamics.subtype && limiter.subtype != reverb.subtype);
        auto bogus = abi<daw_au_component>();
        CHECK_REJ(session.get(), daw_get_supported_au(session.get(), catalogCount, &bogus));
        CHECK_REJ(session.get(), daw_get_supported_au(session.get(), catalogCount + 50, &bogus));
        bogus.struct_size = sizeof(daw_au_component) - 1;
        CHECK(daw_get_supported_au(session.get(), 0, &bogus) == 1);
        CHECK(daw_get_supported_au(session.get(), 0, nullptr) == 1);
        CHECK(rev(session.get()) == quiet);
        CHECK_OK(session.get(), daw_add_insert_au(session.get(), DAW_INSERT_OWNER_TRACK, track, dynamics.type,
                                                 dynamics.subtype, dynamics.manufacturer, quiet));
        CHECK_OK(session.get(), daw_add_insert_au(session.get(), DAW_INSERT_OWNER_BUS, bus, reverb.type,
                                                 reverb.subtype, reverb.manufacturer, rev(session.get())));
        CHECK_OK(session.get(), daw_add_master_au(session.get(), limiter.type, limiter.subtype, limiter.manufacturer,
                                                  rev(session.get())));
        CHECK(snapshotOf(session.get()).master_insert_count == 1);

        // ---------------------------------------------------------------------
        // 5. Master chain: add each approved AU, read it back, duplicate-add,
        //    and the documented four-insert ceiling.
        // ---------------------------------------------------------------------
        Bridge master;
        CHECK(scanCatalog(master.get()) == catalogCount);
        std::vector<uint64_t> masterIds;
        for (uint32_t index = 0; index < catalogCount; ++index) {
            const auto before = snapshotOf(master.get());
            CHECK_OK(master.get(), daw_add_master_au(master.get(), catalog[index].type, catalog[index].subtype,
                                                     catalog[index].manufacturer, before.revision));
            const auto after = snapshotOf(master.get());
            CHECK(after.revision == before.revision + 1);
            CHECK(after.master_insert_count == index + 1);
            const auto plugin = masterInsertAt(master.get(), index);
            CHECK(plugin.id != 0);
            CHECK(std::find(masterIds.begin(), masterIds.end(), plugin.id) == masterIds.end());
            masterIds.push_back(plugin.id);
            CHECK(plugin.bypassed == 0);
            // docs/36 measured 256 / 96 / 0 frames at 48 kHz on the verified
            // machine; this scenario requires only a sane channel delay: under
            // one tenth of a second, and enumerated fields that mirror the
            // catalog entry the user picked.
            CHECK(plugin.latency_frames < kProjectRate / 10);
            CHECK(plugin.available == 1);
            CHECK(plugin.type == catalog[index].type && plugin.subtype == catalog[index].subtype &&
                  plugin.manufacturer == catalog[index].manufacturer);
            CHECK(std::string(plugin.name) == std::string(catalog[index].name));
            auto missing = abi<daw_plugin>();
            CHECK_REJ(master.get(), daw_get_master_insert(master.get(), index + 1, &missing));
        }
        // Re-adding an approved component is a second, independent insert: the
        // gate is catalog membership, not "not already present" (docs/36), so
        // the tuple is accepted again under its own durable id.
        const auto duplicateBefore = snapshotOf(master.get());
        CHECK_OK(master.get(), daw_add_master_au(master.get(), catalog[0].type, catalog[0].subtype,
                                                 catalog[0].manufacturer, duplicateBefore.revision));
        const auto duplicate = snapshotOf(master.get());
        CHECK(duplicate.revision == duplicateBefore.revision + 1);
        CHECK(duplicate.master_insert_count == 4);
        const auto duplicatePlugin = masterInsertAt(master.get(), 3);
        CHECK(duplicatePlugin.id != masterIds[0]);
        CHECK(duplicatePlugin.subtype == catalog[0].subtype &&
              duplicatePlugin.manufacturer == catalog[0].manufacturer);
        CHECK(duplicatePlugin.latency_frames == masterInsertAt(master.get(), 0).latency_frames);
        // The fifth insert crosses the master limit and mutates nothing.
        CHECK_REJ(master.get(), daw_add_master_au(master.get(), catalog[1].type, catalog[1].subtype,
                                                  catalog[1].manufacturer, rev(master.get())));
        CHECK(rev(master.get()) == duplicate.revision);
        CHECK(snapshotOf(master.get()).master_insert_count == 4);

        // ---------------------------------------------------------------------
        // 6. Parameters of a master insert: catalog shape, native semantics
        //    (docs/37), revision accounting and rejections.
        // ---------------------------------------------------------------------
        Bridge chain;
        CHECK(scanCatalog(chain.get()) == catalogCount);
        const auto addedAt = rev(chain.get());
        CHECK_OK(chain.get(), daw_add_master_au(chain.get(), dynamics.type, dynamics.subtype, dynamics.manufacturer,
                                                addedAt));
        const auto dyn = masterInsertAt(chain.get(), 0);
        CHECK(rev(chain.get()) == addedAt + 1);
        const auto dynParameters = parametersOf(chain.get(), DAW_INSERT_OWNER_MASTER, 0, dyn.id);
        CHECK(dynParameters.size() >= 3);
        uint32_t writable = 0;
        for (const auto& parameter : dynParameters) {
            CHECK(!std::string(parameter.name).empty());
            CHECK(std::isfinite(parameter.minimum) && std::isfinite(parameter.maximum) &&
                  std::isfinite(parameter.value));
            CHECK(parameter.maximum > parameter.minimum);
            CHECK(parameter.normalized_value >= 0.0f && parameter.normalized_value <= 1.0f);
            writable += parameter.writable != 0 ? 1u : 0u;
        }
        CHECK(writable == dynParameters.size());  // Apple's controls are all live
        auto badParameter = abi<daw_au_parameter>();
        CHECK_REJ(chain.get(), daw_get_master_insert_parameter(chain.get(), dyn.id,
                                                               static_cast<uint32_t>(dynParameters.size()),
                                                               &badParameter));
        badParameter.struct_size = sizeof(daw_au_parameter) - 1;
        CHECK(daw_get_master_insert_parameter(chain.get(), dyn.id, 0, &badParameter) == 1);
        CHECK(daw_get_master_insert_parameter(chain.get(), dyn.id, 0, nullptr) == 1);
        uint32_t unusedCount = 0;
        CHECK_REJ(chain.get(), daw_get_master_insert_parameter_count(chain.get(), 4242, &unusedCount));
        CHECK_REJ(chain.get(), daw_get_master_insert_parameter_count(chain.get(), dyn.id, nullptr));
        CHECK(rev(chain.get()) == addedAt + 1);

        const size_t safeIndex = firstSafeWritable(dynParameters);
        const auto safe = dynParameters[safeIndex];
        const auto hotValue = upperQuarter(safe);
        const auto beforeSet = rev(chain.get());
        CHECK_OK(chain.get(), daw_set_master_insert_parameter(chain.get(), dyn.id, safe.id, hotValue, beforeSet));
        CHECK(rev(chain.get()) == beforeSet + 1);
        const auto moved = parameterAt(chain.get(), DAW_INSERT_OWNER_MASTER, 0, dyn.id,
                                       static_cast<uint32_t>(safeIndex));
        // Native units (docs/37): the value read back is in the component's own
        // range and tracks the 0..1 normalized mirror.
        expectNear(moved.value, hotValue, (safe.maximum - safe.minimum) * 0.05 + 0.001,
                   std::string("AU parameter \"") + safe.name + "\" native value");
        expectNear(moved.normalized_value, (hotValue - safe.minimum) / (safe.maximum - safe.minimum), 0.05,
                   std::string("AU parameter \"") + safe.name + "\" normalized value");
        const auto afterSet = rev(chain.get());
        CHECK_REJ(chain.get(), daw_set_master_insert_parameter(chain.get(), dyn.id, 60606, hotValue, afterSet));
        CHECK(rev(chain.get()) == afterSet);
        CHECK_REJ(chain.get(), daw_set_master_insert_parameter(chain.get(), dyn.id, safe.id,
                                                               std::numeric_limits<float>::quiet_NaN(), afterSet));
        CHECK(rev(chain.get()) == afterSet);
        CHECK_REJ(chain.get(), daw_set_master_insert_parameter(chain.get(), dyn.id, safe.id, hotValue, beforeSet));
        CHECK(rev(chain.get()) == afterSet);
        CHECK_REJ(chain.get(), daw_set_master_insert_parameter(chain.get(), 4242, safe.id, hotValue, afterSet));
        CHECK(rev(chain.get()) == afterSet);
        // The AU parameter catalog is re-read from the restored state, so the
        // same index keeps naming the same control after the edit.
        CHECK(parametersOf(chain.get(), DAW_INSERT_OWNER_MASTER, 0, dyn.id).size() == dynParameters.size());

        // ---------------------------------------------------------------------
        // 7. Bypass / move / remove: one revision each, and undo/redo restores
        //    the whole insert list field-for-field.
        // ---------------------------------------------------------------------
        CHECK_OK(chain.get(), daw_add_master_au(chain.get(), reverb.type, reverb.subtype, reverb.manufacturer,
                                                rev(chain.get())));
        const auto verb = masterInsertAt(chain.get(), 1);
        CHECK(verb.id != dyn.id);
        const auto baseline = dumpOf(chain.get());
        CHECK(baseline.masterInserts.size() == 2);

        CHECK_OK(chain.get(), daw_set_master_insert_bypass(chain.get(), dyn.id, 1, baseline.revision));
        auto bypassed = withoutRevision(baseline);
        bypassed.masterInserts[0].bypassed = 1;
        CHECK(contentOf(chain.get()) == bypassed);
        // Re-applying the same bypass is a documented silent no-op (docs/45).
        const auto sameBypassRevision = rev(chain.get());
        CHECK_OK(chain.get(), daw_set_master_insert_bypass(chain.get(), dyn.id, 1, sameBypassRevision));
        CHECK(rev(chain.get()) == sameBypassRevision);
        CHECK(contentOf(chain.get()) == bypassed);
        CHECK_REJ(chain.get(), daw_set_master_insert_bypass(chain.get(), dyn.id, 2, sameBypassRevision));
        CHECK(contentOf(chain.get()) == bypassed);

        CHECK_OK(chain.get(), daw_undo(chain.get(), sameBypassRevision));
        CHECK(rev(chain.get()) == sameBypassRevision + 1);
        CHECK(contentOf(chain.get()) == withoutRevision(baseline));
        CHECK_OK(chain.get(), daw_redo(chain.get(), rev(chain.get())));
        CHECK(contentOf(chain.get()) == bypassed);

        const auto beforeMove = dumpOf(chain.get());
        CHECK_OK(chain.get(), daw_move_master_insert(chain.get(), dyn.id, 1, beforeMove.revision));
        auto movedOrder = withoutRevision(beforeMove);
        std::swap(movedOrder.masterInserts[0], movedOrder.masterInserts[1]);
        CHECK(contentOf(chain.get()) == movedOrder);
        CHECK(masterInsertAt(chain.get(), 0).id == verb.id && masterInsertAt(chain.get(), 1).id == dyn.id);
        // A same-position move succeeds without a revision (docs/45).
        const auto sameMoveRevision = rev(chain.get());
        CHECK_OK(chain.get(), daw_move_master_insert(chain.get(), dyn.id, 1, sameMoveRevision));
        CHECK(rev(chain.get()) == sameMoveRevision);
        CHECK(contentOf(chain.get()) == movedOrder);
        CHECK_REJ(chain.get(), daw_move_master_insert(chain.get(), dyn.id, 9, sameMoveRevision));
        CHECK_REJ(chain.get(), daw_move_master_insert(chain.get(), 4242, 0, sameMoveRevision));
        CHECK(contentOf(chain.get()) == movedOrder);

        CHECK_OK(chain.get(), daw_remove_master_insert(chain.get(), dyn.id, sameMoveRevision));
        CHECK(snapshotOf(chain.get()).master_insert_count == 1);
        CHECK(masterInsertAt(chain.get(), 0).id == verb.id);
        auto missing = abi<daw_plugin>();
        CHECK_REJ(chain.get(), daw_get_master_insert(chain.get(), 1, &missing));
        CHECK_OK(chain.get(), daw_undo(chain.get(), rev(chain.get())));
        CHECK(contentOf(chain.get()) == movedOrder);
        CHECK(snapshotOf(chain.get()).master_insert_count == 2);
        CHECK_OK(chain.get(), daw_redo(chain.get(), rev(chain.get())));
        CHECK(snapshotOf(chain.get()).master_insert_count == 1);

        // ---------------------------------------------------------------------
        // 8. The same strip API on TRACK and BUS owners: real chains, owner
        //    isolation, generic parameter/bypass/remove and undo fidelity.
        // ---------------------------------------------------------------------
        Bridge strips;
        CHECK(scanCatalog(strips.get()) == catalogCount);
        const auto tonePath = root / "strips-tone.wav";
        writeWavFixture(tonePath, sineInterleaved(kProjectRate, kProjectRate, 440.0, 0.5, 2), kProjectRate, 2, "f32");
        CHECK_OK(strips.get(), daw_import_wav(strips.get(), tonePath.string().c_str(), "Tone", rev(strips.get())));
        const auto tone = trackById(strips.get(), 1);
        CHECK(tone.clip_count == 1);
        const auto mixBus = addBus(strips.get(), "Mix Bus");

        const auto beforeTrackAdd = rev(strips.get());
        CHECK_OK(strips.get(), daw_add_insert_au(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, dynamics.type,
                                                 dynamics.subtype, dynamics.manufacturer, beforeTrackAdd));
        CHECK(rev(strips.get()) == beforeTrackAdd + 1);
        CHECK_OK(strips.get(), daw_add_insert_au(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, reverb.type,
                                                 reverb.subtype, reverb.manufacturer, rev(strips.get())));
        uint32_t trackInserts = 0;
        uint32_t busInserts = 0;
        CHECK_OK(strips.get(), daw_get_insert_count(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, &trackInserts));
        CHECK_OK(strips.get(), daw_get_insert_count(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, &busInserts));
        CHECK(trackInserts == 1 && busInserts == 1);
        const auto onTrack = [&] {
            auto plugin = abi<daw_plugin>();
            CHECK_OK(strips.get(), daw_get_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, 0, &plugin));
            return plugin;
        }();
        const auto onBus = busInsertAt(strips.get(), mixBus, 0);
        CHECK(onTrack.id != onBus.id);
        CHECK(onTrack.subtype == dynamics.subtype && onBus.subtype == reverb.subtype);
        CHECK(onTrack.available == 1 && onBus.available == 1);
        CHECK(onTrack.latency_frames < kProjectRate / 10 && onBus.latency_frames < kProjectRate / 10);
        // Owner isolation: a wrong owner kind or a borrowed id never reads
        // another chain.
        auto crossRead = abi<daw_plugin>();
        CHECK_REJ(strips.get(), daw_get_insert(strips.get(), DAW_INSERT_OWNER_BUS, tone.id, 0, &crossRead));
        CHECK_REJ(strips.get(), daw_get_insert(strips.get(), DAW_INSERT_OWNER_TRACK, mixBus, 0, &crossRead));
        CHECK_REJ(strips.get(), daw_get_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, 1, &crossRead));
        CHECK_REJ(strips.get(), daw_get_insert(strips.get(), DAW_INSERT_OWNER_MASTER, 0, 0, &crossRead));
        const auto stripDump = dumpOf(strips.get());
        CHECK(stripDump.tracks.size() == 1 && stripDump.tracks[0].inserts.size() == 1);
        CHECK(stripDump.buses.size() == 1 && stripDump.buses[0].inserts.size() == 1);

        const auto verbParameters = parametersOf(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id);
        CHECK(verbParameters.size() >= 3);
        const size_t verbSafeIndex = firstSafeWritable(verbParameters);
        const auto verbSafe = verbParameters[verbSafeIndex];
        const auto verbValue = upperQuarter(verbSafe);
        const auto beforeVerbSet = rev(strips.get());
        CHECK_OK(strips.get(), daw_set_insert_parameter(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id,
                                                        verbSafe.id, verbValue, beforeVerbSet));
        CHECK(rev(strips.get()) == beforeVerbSet + 1);
        const auto verbMoved = parameterAt(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id,
                                           static_cast<uint32_t>(verbSafeIndex));
        expectNear(verbMoved.value, verbValue, (verbSafe.maximum - verbSafe.minimum) * 0.05 + 0.001,
                   "bus insert parameter native value");
        CHECK_REJ(strips.get(), daw_set_insert_parameter(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id,
                                                         60606, verbValue, rev(strips.get())));
        CHECK(rev(strips.get()) == beforeVerbSet + 1);

        const auto preBypass = dumpOf(strips.get());
        CHECK_OK(strips.get(), daw_set_insert_bypass(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, 1,
                                                     preBypass.revision));
        auto busBypassed = withoutRevision(preBypass);
        busBypassed.buses[0].inserts[0].bypassed = 1;
        CHECK(contentOf(strips.get()) == busBypassed);
        CHECK_OK(strips.get(), daw_remove_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, onTrack.id,
                                                 rev(strips.get())));
        CHECK_OK(strips.get(), daw_get_insert_count(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, &trackInserts));
        CHECK(trackInserts == 0);
        CHECK_REJ(strips.get(), daw_get_insert_hosting_status(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id,
                                                              onTrack.id, &hosting));
        // Undo restores the removed strip exactly, bypass state included.
        CHECK_OK(strips.get(), daw_undo(strips.get(), rev(strips.get())));
        CHECK(contentOf(strips.get()) == busBypassed);
        CHECK_OK(strips.get(), daw_get_insert_count(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, &trackInserts));
        CHECK(trackInserts == 1);

        // 8b. Reordering a generic strip: daw_move_insert costs exactly one
        //     revision, PDC recomputes from the new order, and undo restores the
        //     previous chain byte-for-field (docs/45).
        const auto beforeSecondAdd = rev(strips.get());
        CHECK_OK(strips.get(), daw_add_insert_au(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, reverb.type,
                                                 reverb.subtype, reverb.manufacturer, beforeSecondAdd));
        CHECK(rev(strips.get()) == beforeSecondAdd + 1);
        auto second = abi<daw_plugin>();
        CHECK_OK(strips.get(), daw_get_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, 1, &second));
        CHECK(second.id != onTrack.id && second.subtype == reverb.subtype);
        const auto preMove = dumpOf(strips.get());
        CHECK(preMove.tracks[0].inserts.size() == 2);
        CHECK_OK(strips.get(), daw_move_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, second.id, 0,
                                               preMove.revision));
        CHECK(rev(strips.get()) == preMove.revision + 1);
        auto reordered = withoutRevision(preMove);
        std::swap(reordered.tracks[0].inserts[0], reordered.tracks[0].inserts[1]);
        CHECK(contentOf(strips.get()) == reordered);
        second = abi<daw_plugin>();
        CHECK_OK(strips.get(), daw_get_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, 0, &second));
        CHECK(second.subtype == reverb.subtype);
        // A same-position move is a silent success (docs/45).
        const auto samePosition = rev(strips.get());
        CHECK_OK(strips.get(), daw_move_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, second.id, 0,
                                               samePosition));
        CHECK(rev(strips.get()) == samePosition);
        CHECK(contentOf(strips.get()) == reordered);
        CHECK_REJ(strips.get(), daw_move_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, second.id, 4,
                                                samePosition));
        // An insert of another chain is never touched, whichever owner lies.
        CHECK_REJ(strips.get(), daw_move_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, 4242, 0, samePosition));
        CHECK_REJ(strips.get(), daw_move_insert(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, second.id, 0,
                                                samePosition));
        CHECK_REJ(strips.get(), daw_move_insert(strips.get(), DAW_INSERT_OWNER_TRACK, 9999, second.id, 0,
                                                samePosition));
        CHECK_REJ(strips.get(), daw_move_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, second.id, 0,
                                                samePosition - 1));
        CHECK(contentOf(strips.get()) == reordered);
        CHECK_OK(strips.get(), daw_undo(strips.get(), rev(strips.get())));
        CHECK(contentOf(strips.get()) == withoutRevision(preMove));
        CHECK_OK(strips.get(), daw_redo(strips.get(), rev(strips.get())));
        CHECK(contentOf(strips.get()) == reordered);
        // Drop the extra strip again so the rest of the journey owns one insert
        // per chain.
        CHECK_OK(strips.get(), daw_remove_insert(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, second.id,
                                                 rev(strips.get())));
        CHECK_OK(strips.get(), daw_get_insert_count(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, &trackInserts));
        CHECK(trackInserts == 1);

        // ---------------------------------------------------------------------
        // 9. Hosting policy (docs/52) and volatile runtime status (no graph).
        // ---------------------------------------------------------------------
        const auto policy = hostingOf(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id);
        CHECK(policy.version == DAW_INSERT_HOSTING_STATUS_VERSION);
        CHECK(policy.selected_mode == DAW_INSERT_HOSTING_MODE_IN_PROCESS);
        if (policy.format == DAW_INSERT_HOSTING_FORMAT_AUV2) {
            // The hosting policy contract: legacy AUv2 is in-process ONLY.
            CHECK(policy.supported_modes == DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS);
            const auto isolatedRevision = rev(strips.get());
            CHECK_REJ(strips.get(), daw_set_insert_hosting_mode(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id,
                                                                DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS,
                                                                isolatedRevision));
            CHECK(rev(strips.get()) == isolatedRevision);
            const auto unchanged = hostingOf(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id);
            CHECK(unchanged.selected_mode == DAW_INSERT_HOSTING_MODE_IN_PROCESS);
            CHECK(unchanged.supported_modes == DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS);
            CHECK(unchanged.format == DAW_INSERT_HOSTING_FORMAT_AUV2);
        } else {
            CHECK(policy.format == DAW_INSERT_HOSTING_FORMAT_AUV3);
            CHECK((policy.supported_modes & DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS) != 0);
            const auto beforeIsolate = rev(strips.get());
            CHECK_OK(strips.get(), daw_set_insert_hosting_mode(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id,
                                                               DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS, beforeIsolate));
            CHECK(rev(strips.get()) == beforeIsolate + 1);
            CHECK(hostingOf(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id).selected_mode ==
                  DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS);
            // core_tests 'isolatedAu' precedent: an isolated AU has no
            // in-process parameter proxy, so parameter access is refused rather
            // than silently hosted in-process.
            CHECK_REJ(strips.get(), daw_get_insert_parameter_count(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                   onBus.id, &unusedCount));
            CHECK_OK(strips.get(), daw_set_insert_hosting_mode(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id,
                                                               DAW_INSERT_HOSTING_MODE_IN_PROCESS, rev(strips.get())));
            CHECK(parameterCountOf(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id) ==
                  static_cast<uint32_t>(verbParameters.size()));
        }
        CHECK_REJ(strips.get(), daw_set_insert_hosting_mode(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, 77,
                                                            rev(strips.get())));
        CHECK_REJ(strips.get(), daw_set_insert_hosting_mode(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, 4242,
                                                            DAW_INSERT_HOSTING_MODE_IN_PROCESS, rev(strips.get())));
        hosting.struct_size = sizeof(daw_insert_hosting_status) - 1;
        CHECK(daw_get_insert_hosting_status(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, &hosting) == 1);

        const auto runtimeRevision = rev(strips.get());
        const auto live = runtimeOf(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id);
        // Header contract: with no prepared graph the read reports
        // UNPREPARED + RESTART_REQUIRED and never changes the revision.
        CHECK(live.version == DAW_INSERT_RUNTIME_STATUS_VERSION);
        CHECK(live.state == DAW_INSERT_RUNTIME_UNPREPARED);
        CHECK(live.fault_code == DAW_INSERT_RUNTIME_FAULT_RESTART_REQUIRED);
        CHECK(live.extra_pipeline_latency_frames == 0);
        CHECK(rev(strips.get()) == runtimeRevision);
        runtime.struct_size = sizeof(daw_insert_runtime_status) - 1;
        CHECK(daw_get_insert_runtime_status(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, &runtime) == 1);
        CHECK_REJ(strips.get(), daw_get_insert_runtime_status(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, 4242,
                                                              &runtime));
        CHECK(rev(strips.get()) == runtimeRevision);

        // ---------------------------------------------------------------------
        // 10. Plug-in parameter automation on a real catalog parameter
        //     (docs/46): normalized lanes, one revision per committed change.
        // ---------------------------------------------------------------------
        const auto lane = parameterAt(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, 0);
        CHECK(lane.writable == 1);
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 0);
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, 60606) == 0);
        const auto beforeLane = rev(strips.get());
        CHECK_OK(strips.get(), daw_upsert_insert_parameter_automation_point(
                                   strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, lane.name,
                                   kProjectRate / 10, 0.4, beforeLane));
        CHECK(rev(strips.get()) == beforeLane + 1);
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 1);
        auto point = abi<daw_plugin_parameter_automation_point>();
        CHECK_OK(strips.get(), daw_get_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                         onBus.id, lane.id, 0, &point));
        CHECK(point.frame == kProjectRate / 10 && point.normalized_value == 0.4);
        CHECK_REJ(strips.get(), daw_get_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                          onBus.id, lane.id, 1, &point));
        point.struct_size = sizeof(daw_plugin_parameter_automation_point) - 1;
        CHECK(daw_get_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id,
                                                        0, &point) == 1);
        point = abi<daw_plugin_parameter_automation_point>();
        CHECK_OK(strips.get(), daw_upsert_insert_parameter_automation_point(
                                   strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, lane.name,
                                   kProjectRate / 4, 0.2, rev(strips.get())));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 2);
        const auto beforeRepeat = rev(strips.get());
        CHECK_OK(strips.get(), daw_upsert_insert_parameter_automation_point(
                                   strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, lane.name,
                                   kProjectRate / 4, 0.2, beforeRepeat));
        CHECK(rev(strips.get()) == beforeRepeat);  // identical upsert is a no-op
        CHECK_OK(strips.get(), daw_upsert_insert_parameter_automation_point(
                                   strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, "Renamed Lane",
                                   kProjectRate / 4, 0.75, beforeRepeat));
        CHECK(rev(strips.get()) == beforeRepeat + 1);
        CHECK_OK(strips.get(), daw_get_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                         onBus.id, lane.id, 1, &point));
        CHECK(point.frame == kProjectRate / 4 && point.normalized_value == 0.75);
        for (const double badValue : {-0.01, 1.01, 2.0}) {
            CHECK_REJ(strips.get(), daw_upsert_insert_parameter_automation_point(
                                        strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, lane.name, 0,
                                        badValue, rev(strips.get())));
        }
        CHECK_REJ(strips.get(), daw_upsert_insert_parameter_automation_point(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, lane.name, 0,
                                    std::numeric_limits<double>::quiet_NaN(), rev(strips.get())));
        // Timeline limit: ten minutes at 48 kHz (docs/46).
        CHECK_REJ(strips.get(), daw_upsert_insert_parameter_automation_point(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, lane.name,
                                    kProjectRate * 600ULL + 1, 0.5, rev(strips.get())));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 2);
        CHECK_REJ(strips.get(), daw_upsert_insert_parameter_automation_point(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, lane.name,
                                    kProjectRate / 4, 0.9, beforeRepeat));  // stale revision
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 2);

        // 11. Deleting an insert deletes its lanes; undo brings both back.
        const auto beforeStripRemoval = rev(strips.get());
        CHECK_OK(strips.get(), daw_remove_insert(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id,
                                                 beforeStripRemoval));
        CHECK(rev(strips.get()) == beforeStripRemoval + 1);
        CHECK_REJ(strips.get(), daw_get_insert_parameter_automation_count(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, &unusedCount));
        CHECK_OK(strips.get(), daw_undo(strips.get(), rev(strips.get())));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 2);
        CHECK_OK(strips.get(), daw_remove_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                            onBus.id, lane.id, kProjectRate / 4,
                                                                            rev(strips.get())));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 1);
        CHECK_OK(strips.get(), daw_remove_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                            onBus.id, lane.id, kProjectRate / 10,
                                                                            rev(strips.get())));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 0);
        CHECK_REJ(strips.get(), daw_remove_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS,
                                                                             mixBus, onBus.id, lane.id,
                                                                             kProjectRate / 10, rev(strips.get())));

        // ---------------------------------------------------------------------
        // 11b. The live parameter gesture (docs/46): one open transaction that
        //      commits as ONE revision, writes nothing on cancel, and refuses
        //      every other mutation while it is open.
        // ---------------------------------------------------------------------
        CHECK_REJ(strips.get(), daw_write_insert_parameter_automation_gesture(strips.get(), 0, 0.5));
        CHECK_REJ(strips.get(), daw_end_insert_parameter_automation_gesture(strips.get(), 0, rev(strips.get())));
        const auto gestureRevision = rev(strips.get());
        const auto gestureBaseline = dumpOf(strips.get());
        // A gesture that cannot even address the strip must leave the session
        // unlocked: begin, then any failed call, then a clean retry.
        CHECK_REJ(strips.get(), daw_begin_insert_parameter_automation_gesture(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, 4242, lane.id, "Ghost",
                                    DAW_AUTOMATION_LATCH, gestureRevision));
        CHECK(rev(strips.get()) == gestureRevision);
        CHECK_REJ(strips.get(), daw_begin_insert_parameter_automation_gesture(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, "Ghost",
                                    77, gestureRevision));
        CHECK_REJ(strips.get(), daw_begin_insert_parameter_automation_gesture(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, "Ghost",
                                    DAW_AUTOMATION_LATCH, gestureRevision + 4));
        CHECK(rev(strips.get()) == gestureRevision);
        CHECK_OK(strips.get(), daw_begin_insert_parameter_automation_gesture(
                                   strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, "Gesture Lane",
                                   DAW_AUTOMATION_LATCH, gestureRevision));
        // Nothing is visible while the drag is open.
        CHECK(rev(strips.get()) == gestureRevision);
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 0);
        // The open transaction is the lock: every other mutation rejects.
        CHECK_REJ(strips.get(), daw_upsert_insert_parameter_automation_point(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, lane.name, 0, 0.5,
                                    gestureRevision));
        CHECK_REJ(strips.get(), daw_set_insert_bypass(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, 1,
                                                      gestureRevision));
        CHECK_REJ(strips.get(), daw_begin_insert_parameter_automation_gesture(
                                    strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, "Second",
                                    DAW_AUTOMATION_TOUCH, gestureRevision));
        CHECK_REJ(strips.get(), daw_undo(strips.get(), gestureRevision));
        CHECK_REJ(strips.get(), daw_rename_track(strips.get(), tone.id, "Locked", gestureRevision));
        CHECK(rev(strips.get()) == gestureRevision);
        CHECK(contentOf(strips.get()) == withoutRevision(gestureBaseline));
        // Values are normalized; the gesture writes into its private copy, so
        // the read-only view of the lane is still empty.
        CHECK_REJ(strips.get(), daw_write_insert_parameter_automation_gesture(strips.get(), 0, 1.5));
        CHECK_REJ(strips.get(), daw_write_insert_parameter_automation_gesture(strips.get(), 0,
                                                                             std::numeric_limits<double>::quiet_NaN()));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 0);
        CHECK_OK(strips.get(), daw_write_insert_parameter_automation_gesture(strips.get(), kProjectRate / 2, 0.5));
        CHECK_OK(strips.get(), daw_write_insert_parameter_automation_gesture(strips.get(), kProjectRate / 2 + 1, 0.6));
        CHECK_REJ(strips.get(), daw_write_insert_parameter_automation_gesture(strips.get(), kProjectRate / 4, 0.7));
        // Cancel drops the whole transaction without a revision.
        daw_cancel_insert_parameter_automation_gesture(strips.get());
        CHECK(rev(strips.get()) == gestureRevision);
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 0);
        CHECK_OK(strips.get(), daw_begin_insert_parameter_automation_gesture(
                                   strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id, "Gesture Lane",
                                   DAW_AUTOMATION_LATCH, gestureRevision));
        CHECK_OK(strips.get(), daw_write_insert_parameter_automation_gesture(strips.get(), kProjectRate / 2, 0.5));
        CHECK_OK(strips.get(), daw_write_insert_parameter_automation_gesture(strips.get(), kProjectRate / 2 + 1, 0.6));
        // Touch would end on the final written frame; latch holds to endFrame,
        // which is where the extra committed point comes from.
        CHECK_REJ(strips.get(), daw_end_insert_parameter_automation_gesture(strips.get(), kProjectRate / 2 - 1,
                                                                           gestureRevision));
        CHECK_OK(strips.get(), daw_end_insert_parameter_automation_gesture(strips.get(), kProjectRate,
                                                                           gestureRevision));
        // The whole stroke is ONE revision and ONE undo entry (docs/46).
        CHECK(rev(strips.get()) == gestureRevision + 1);
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 3);
        CHECK_OK(strips.get(), daw_get_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                         onBus.id, lane.id, 2, &point));
        CHECK(point.frame == kProjectRate && point.normalized_value == 0.6);
        const auto gestureCommitted = dumpOf(strips.get());
        CHECK_OK(strips.get(), daw_undo(strips.get(), rev(strips.get())));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 0);
        CHECK_OK(strips.get(), daw_redo(strips.get(), rev(strips.get())));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 3);
        CHECK(contentOf(strips.get()) == withoutRevision(gestureCommitted));
        // Back to an empty lane so the persistence step owns a deterministic
        // plug-in state.
        CHECK_OK(strips.get(), daw_remove_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                            onBus.id, lane.id, kProjectRate / 2,
                                                                            rev(strips.get())));
        CHECK_OK(strips.get(), daw_remove_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                            onBus.id, lane.id, kProjectRate / 2 + 1,
                                                                            rev(strips.get())));
        CHECK_OK(strips.get(), daw_remove_insert_parameter_automation_point(strips.get(), DAW_INSERT_OWNER_BUS, mixBus,
                                                                            onBus.id, lane.id, kProjectRate,
                                                                            rev(strips.get())));
        CHECK(automationCount(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, lane.id) == 0);

        // ---------------------------------------------------------------------
        // 12. Persistence: captured AU state survives save + reopen (docs/36),
        //     including the parameter value set through the ABI.
        // ---------------------------------------------------------------------
        CHECK_OK(strips.get(), daw_set_insert_bypass(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id, 0,
                                                     rev(strips.get())));
        const auto savedParameter = parameterAt(strips.get(), DAW_INSERT_OWNER_BUS, mixBus, onBus.id,
                                                static_cast<uint32_t>(verbSafeIndex));
        CHECK_OK(strips.get(), daw_add_insert_au(strips.get(), DAW_INSERT_OWNER_TRACK, tone.id, limiter.type,
                                                 limiter.subtype, limiter.manufacturer, rev(strips.get())));
        const auto built = dumpOf(strips.get());
        // Two AU inserts on the track (restored by undo, then the limiter) and
        // one on the bus: the chain order itself is part of what must survive.
        CHECK(built.masterInserts.empty() && built.tracks[0].inserts.size() == 2 &&
              built.buses[0].inserts.size() == 1);
        CHECK(built.tracks[0].inserts[0].subtype == dynamics.subtype &&
              built.tracks[0].inserts[1].subtype == limiter.subtype);
        const auto auDraft = root / "au-session.mydawdraft";
        saveDraftAndWait(strips.get(), auDraft);
        CHECK(fileNonEmpty(auDraft));
        Bridge restored;
        CHECK_OK(restored.get(), daw_open_draft(restored.get(), auDraft.string().c_str()));
        const auto reopened = dumpOf(restored.get());
        if (!(reopened == built))
            throw std::runtime_error("AU round-trip changed the project:\nBEFORE:\n" + describe(built) +
                                     "\nAFTER:\n" + describe(reopened));
        // The reopened strip is still hosted by this machine (available == 1 is
        // part of the equality above) and no scan was needed to prove it.
        const auto reopenedBus = busInsertAt(restored.get(), mixBus, 0);
        CHECK(reopenedBus.available == 1 && reopenedBus.bypassed == 0);
        CHECK(reopenedBus.latency_frames == built.buses[0].inserts[0].latencyFrames);
        CHECK(std::string(reopenedBus.name) == built.buses[0].inserts[0].name);
        const auto reopenedParameter = parameterAt(restored.get(), DAW_INSERT_OWNER_BUS, mixBus, reopenedBus.id,
                                                   static_cast<uint32_t>(verbSafeIndex));
        expectNear(reopenedParameter.value, savedParameter.value,
                   std::abs(savedParameter.maximum - savedParameter.minimum) * 0.05 + 0.001,
                   "reopened AU parameter value");
        CHECK(automationCount(restored.get(), DAW_INSERT_OWNER_BUS, mixBus, reopenedBus.id, lane.id) == 0);
        CHECK(snapshotOf(restored.get()).can_undo == 0);  // a fresh open owns no history

        // ---------------------------------------------------------------------
        // 13. Audible proof. Master gain is applied before the master chain
        //     (docs/45) and the render ends in the documented safety clamp
        //     [-1, 1] (docs/25, docs/30), so the hosted limiter is checked two
        //     ways: transparent below its ceiling, and unmistakably pulling a
        //     hot master down above it.
        // ---------------------------------------------------------------------
        Bridge audible;
        CHECK(scanCatalog(audible.get()) == catalogCount);
        const auto tonePath2 = root / "limiter-tone.wav";
        writeWavFixture(tonePath2, sineInterleaved(2 * kProjectRate, kProjectRate, 440.0, 0.9, 2), kProjectRate, 2,
                        "f32");
        CHECK_OK(audible.get(), daw_import_wav(audible.get(), tonePath2.string().c_str(), "Vocal Out",
                                               rev(audible.get())));
        const auto beforeLimiter = rev(audible.get());
        CHECK_OK(audible.get(), daw_add_master_au(audible.get(), limiter.type, limiter.subtype, limiter.manufacturer,
                                                  beforeLimiter));
        const auto limiterInsert = masterInsertAt(audible.get(), 0);
        CHECK(limiterInsert.available == 1);
        CHECK_OK(audible.get(), daw_set_master_insert_bypass(audible.get(), limiterInsert.id, 1,
                                                             rev(audible.get())));

        // docs/46: the Audio Unit host publishes no tail declaration, so the
        // finite tail summary stays zero with an AU in the master chain.
        const auto beforeSummary = rev(audible.get());
        auto options = abi<daw_export_options>();
        options.version = DAW_EXPORT_OPTIONS_VERSION;
        options.tail_mode = DAW_EXPORT_TAIL_AUTOMATIC;
        auto summary = abi<daw_export_tail_summary>();
        summary.version = DAW_EXPORT_TAIL_SUMMARY_VERSION;
        CHECK_OK(audible.get(), daw_get_export_tail_summary(audible.get(), &options, &summary));
        CHECK(summary.finite_tail_frames == 0);
        CHECK(summary.selected_tail_frames == 0);
        CHECK(summary.infinite_tail_detected == 0);
        CHECK(rev(audible.get()) == beforeSummary);  // a summary is not a mutation

        // (a) 0.9 peak at unity master gain: the limiter never triggers, so the
        // engaged render must be the very same bytes as the bypassed render -
        // including the graph-latency compensation of the insert's own delay.
        const auto openQuiet = exportProject(audible.get(), root / "au-quiet-bypassed.wav", 2);
        expectAllFinite(openQuiet, "bypassed quiet export");
        CHECK_OK(audible.get(), daw_set_master_insert_bypass(audible.get(), limiterInsert.id, 0,
                                                             rev(audible.get())));
        const auto engagedQuiet = exportProject(audible.get(), root / "au-quiet-engaged.wav", 2);
        expectAllFinite(engagedQuiet, "engaged quiet export");
        CHECK(engagedQuiet.frames() == openQuiet.frames());
        CHECK(engagedQuiet.samples == openQuiet.samples);
        const uint32_t quietFrom = kProjectRate / 2;
        const uint32_t quietTo = static_cast<uint32_t>(openQuiet.frames()) - kProjectRate / 10;
        expectNear(std::max(channelPeak(openQuiet, quietFrom, quietTo, 0), channelPeak(openQuiet, quietFrom, quietTo, 1)),
                   0.9, 0.002, "transparent limiter master peak");

        // (b) +12 dB master gain on the same 0.9 tone: 3.58 peak reaches the
        // limiter, and the un-hosted path instead lands on the clamp.
        CHECK_OK(audible.get(), daw_set_master_gain(audible.get(), 12.0, rev(audible.get())));
        const auto engaged = exportProject(audible.get(), root / "au-engaged.wav", 2);
        expectAllFinite(engaged, "engaged export");
        const auto engagedAgain = exportProject(audible.get(), root / "au-engaged-again.wav", 2);
        CHECK(engagedAgain.samples == engaged.samples);  // hosted DSP is deterministic
        CHECK_OK(audible.get(), daw_set_master_insert_bypass(audible.get(), limiterInsert.id, 1,
                                                             rev(audible.get())));
        const auto open = exportProject(audible.get(), root / "au-bypassed.wav", 2);
        expectAllFinite(open, "bypassed export");
        CHECK(engaged.frames() >= kProjectRate && open.frames() >= kProjectRate);

        const auto shared = static_cast<uint32_t>(std::min(engaged.frames(), open.frames()));
        const uint32_t from = kProjectRate / 4;  // past the limiter's attack ramp
        const uint32_t to = shared - kProjectRate / 10;
        CHECK(to > from);
        const double engagedPeak = std::max(channelPeak(engaged, from, to, 0), channelPeak(engaged, from, to, 1));
        const double openPeak = std::max(channelPeak(open, from, to, 0), channelPeak(open, from, to, 1));
        const double engagedRms = std::max(channelRms(engaged, from, to, 0), channelRms(engaged, from, to, 1));
        const double openRms = std::max(channelRms(open, from, to, 0), channelRms(open, from, to, 1));
        // The un-limited path really is over full scale: it is pinned exactly on
        // the documented [-1, 1] safety clamp, so the comparison below is not a
        // no-op between two silent or two identical renders.
        expectNear(openPeak, 1.0, 1e-6, "bypassed master peak sits on the safety clamp");
        CHECK(channelRms(open, from, to, 0) > 0.5);
        // Strongest defensible statement about a hosted limiter: it never makes
        // the master louder, it holds a hot master far below the clipped
        // bypass (0.001 vs 1.0 peak measured on the verified machine, so only a
        // wide 2x margin is asserted), it keeps the tone present instead of
        // muting the graph, and every sample stays finite.
        CHECK(engagedPeak <= openPeak + 1e-6);
        CHECK(engagedPeak < openPeak * 0.5);
        CHECK(engagedRms < openRms);
        CHECK(engagedPeak > 0.0);
        CHECK(toneMagnitude(engaged, from, to, 0, 440.0) > 0.0);
        CHECK(engaged.samples != open.samples);
        // Re-engaging costs a revision and lands back on the engaged render.
        CHECK_OK(audible.get(), daw_set_master_insert_bypass(audible.get(), limiterInsert.id, 0,
                                                             rev(audible.get())));
        CHECK(exportProject(audible.get(), root / "au-engaged-third.wav", 2).samples == engaged.samples);

#else
        // Non-Apple stub path (engine/audio/effect_stub.cpp): the approved
        // catalog is EMPTY, the scan itself still succeeds, and everything that
        // needs a component rejects with readable text and mutates nothing.
        uint32_t stubCount = 12345;
        CHECK_OK(session.get(), daw_scan_supported_au(session.get(), &stubCount));
        CHECK(stubCount == 0);
        auto stubComponent = abi<daw_au_component>();
        CHECK_REJ(session.get(), daw_get_supported_au(session.get(), 0, &stubComponent));
        const auto beforeStub = rev(session.get());
        CHECK_REJ(session.get(), daw_add_master_au(session.get(), 1, 2, 3, beforeStub));
        CHECK_REJ(session.get(), daw_add_insert_au(session.get(), DAW_INSERT_OWNER_TRACK, track, 1, 2, 3, beforeStub));
        CHECK_REJ(session.get(), daw_add_insert_au(session.get(), DAW_INSERT_OWNER_BUS, bus, 1, 2, 3, beforeStub));
        CHECK(rev(session.get()) == beforeStub);
        CHECK(snapshotOf(session.get()).master_insert_count == 0);
        // A plain 48 kHz WAV import plus export is the cross-platform backbone
        // and must keep working with no Audio Unit in the build at all.
        const auto plainPath = root / "plain-tone.wav";
        writeWavFixture(plainPath, sineInterleaved(kProjectRate, kProjectRate, 440.0, 0.5, 2), kProjectRate, 2, "f32");
        Bridge plain;
        CHECK_OK(plain.get(), daw_import_wav(plain.get(), plainPath.string().c_str(), "Plain", rev(plain.get())));
        const auto mixed = exportProject(plain.get(), root / "plain-export.wav", 2);
        CHECK(mixed.frames() >= kProjectRate);
        CHECK(channelPeak(mixed, 0, static_cast<uint32_t>(mixed.frames()), 0) > 0.05);
#endif

        std::cout << "PASS: e2e_plugins_au — "
#ifdef __APPLE__
                     "approved catalog, master/track/bus AU strips, parameters, hosting policy, "
                     "automation lanes, state round-trip, audible limiter\n";
#else
                     "owner/ABI rejections plus the honest empty stub catalog\n";
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "E2E FAIL: " << error.what() << '\n';
        return 1;
    }
}
