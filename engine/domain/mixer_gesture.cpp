#include "domain/session.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
namespace {
double& mixerValue(State& state, MixerEditTarget target, uint64_t id, uint64_t busID) {
    if (target != MixerEditTarget::SendGain && target != MixerEditTarget::SendPan && busID != 0)
        throw Error("Send destination is valid only for a send gesture");
    switch (target) {
    case MixerEditTarget::MasterGain:
        if (id != 0) throw Error("Master ID must be zero");
        return state.masterGain;
    case MixerEditTarget::TrackGain:
    case MixerEditTarget::TrackPan:
    case MixerEditTarget::SendPan:
    case MixerEditTarget::SendGain: {
        auto track = std::find_if(state.tracks.begin(), state.tracks.end(),
                                  [id](const auto& t) { return t.id == id; });
        if (track == state.tracks.end()) throw Error("Track not found");
        if (target == MixerEditTarget::TrackGain) return track->gain;
        if (target == MixerEditTarget::TrackPan) return track->pan;
        auto send = std::find_if(track->sends.begin(), track->sends.end(),
                                 [busID](const auto& s) { return s.bus == busID; });
        if (send == track->sends.end()) throw Error("Send not found");
        if (target == MixerEditTarget::SendPan) {
            if (!send->independentPan) throw Error("Enable independent send balance before a pan gesture");
            return send->pan;
        }
        return send->gain;
    }
    case MixerEditTarget::BusGain:
    case MixerEditTarget::BusPan: {
        auto bus = std::find_if(state.buses.begin(), state.buses.end(),
                                [id](const auto& b) { return b.id == id; });
        if (bus == state.buses.end()) throw Error("Bus not found");
        return target == MixerEditTarget::BusGain ? bus->gain : bus->pan;
    }
    }
    throw Error("Unsupported mixer edit target");
}
}
void Session::exclusiveSolo(uint64_t id,bool solo,uint64_t expected) {
    check(expected);
    if(id==0 && solo)throw Error("Select a track for exclusive solo");
    if(id && std::none_of(current.tracks.begin(),current.tracks.end(),[&](const auto& t){return t.id==id;}))
        throw Error("Track not found");
    State next=current;
    bool changed=false;
    for(auto& track:next.tracks) {
        const bool value=solo && track.id==id;
        changed=changed || track.solo!=value;
        track.solo=value;
    }
    if(changed)commit(std::move(next));
}
void Session::beginMixerGesture(MixerEditTarget target, uint64_t id, uint64_t busID, uint64_t expected) {
    check(expected);
    State working = current;
    const auto initial = mixerValue(working, target, id, busID);
    mixerGesture = MixerGesture{target, id, busID, expected, std::move(working), initial};
}
void Session::beginTrackGainGroup(const std::vector<uint64_t>& ids, uint64_t expected) {
    check(expected);
    if (ids.size() < 2 || ids.size() > 256)
        throw Error("Select between 2 and 256 track faders");
    MixerGesture preview{MixerEditTarget::TrackGain, 0, 0, expected, current, 0};
    preview.group.reserve(ids.size());
    preview.minimumDelta = -144;
    preview.maximumDelta = 144;
    for (const auto id : ids) {
        const auto track = std::find_if(current.tracks.begin(), current.tracks.end(),
                                       [id](const auto& t) { return t.id == id; });
        if (track == current.tracks.end()) throw Error("Group member is not an existing track");
        const auto index = static_cast<size_t>(track - current.tracks.begin());
        if (std::any_of(preview.group.begin(), preview.group.end(),
                        [index](const auto& member) { return member.trackIndex == index; }))
            throw Error("Group selection contains a duplicate track");
        // A static gain cannot override a playback automation lane. Refuse the
        // entire edit rather than moving only some members or erasing automation.
        if (!track->volumeAutomation.empty())
            throw Error("Linked static levels require tracks without volume automation");
        preview.group.push_back({index, track->gain});
        preview.minimumDelta = std::max(preview.minimumDelta, -120 - track->gain);
        preview.maximumDelta = std::min(preview.maximumDelta, 24 - track->gain);
    }
    // Publish only after all IDs, automation lanes and allocations have succeeded.
    mixerGesture = std::move(preview);
}
void Session::writeMixerGesture(double value) {
    if (!mixerGesture) throw Error("No active mixer gesture");
    if (!mixerGesture->group.empty()) {
        if (!std::isfinite(value)) throw Error("Group gain delta must be finite");
        auto& g = *mixerGesture;
        const auto delta = std::clamp(value, g.minimumDelta, g.maximumDelta);
        for (const auto& member : g.group)
            g.working.tracks[member.trackIndex].gain =
                std::clamp(member.initialGain + delta, -120.0, 24.0);
        g.delta = delta;
        return;
    }
    const bool pan = mixerGesture->target == MixerEditTarget::TrackPan || mixerGesture->target == MixerEditTarget::BusPan || mixerGesture->target == MixerEditTarget::SendPan;
    if (!std::isfinite(value) || value < (pan ? -1.0 : -120.0) || value > (pan ? 1.0 : 24.0))
        throw Error(pan ? "Balance must be between -1 and +1" : "Gain must be between -120 and +24 dB");
    // Only a scalar changes. No snapshots/history allocation at mouse frequency;
    // the private state was copied once before the first preview.
    mixerValue(mixerGesture->working, mixerGesture->target, mixerGesture->targetID, mixerGesture->sendBusID) = value;
}
void Session::endMixerGesture(uint64_t expected) {
    if (!mixerGesture) throw Error("No active mixer gesture");
    auto& g = *mixerGesture;
    if (expected != g.baseRevision || current.revision != g.baseRevision)
        throw Error("Revision conflict: refresh the project");
    const bool changed = g.group.empty()
        ? mixerValue(g.working, g.target, g.targetID, g.sendBusID) != g.initialValue
        : std::any_of(g.group.begin(), g.group.end(), [&](const auto& member) {
              return g.working.tracks[member.trackIndex].gain != member.initialGain;
          });
    // Keep the preview cancelable if validation/history allocation throws.
    if (changed) commit(g.working);
    mixerGesture.reset();
}
void Session::cancelMixerGesture() noexcept { mixerGesture.reset(); }
const State& Session::mixerPreview() const noexcept { return mixerGesture ? mixerGesture->working : current; }
bool Session::mixerGestureActive() const noexcept { return mixerGesture.has_value(); }
}
