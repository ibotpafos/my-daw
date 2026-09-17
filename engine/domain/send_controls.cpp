#include "domain/session.hpp"
#include <algorithm>
#include <cmath>

namespace daw {
namespace {
Send& resolveSend(State& state, uint64_t trackID, uint64_t busID) {
    auto track = std::find_if(state.tracks.begin(), state.tracks.end(),
                             [trackID](const auto& t) { return t.id == trackID; });
    if (track == state.tracks.end()) throw Error("Track not found");
    auto send = std::find_if(track->sends.begin(), track->sends.end(),
                            [busID](const auto& s) { return s.bus == busID; });
    if (send == track->sends.end()) throw Error("Send not found");
    return *send;
}
}
void Session::setSendMuted(uint64_t trackID, uint64_t busID, bool muted, uint64_t expected) {
    check(expected);
    State next = current;
    auto& send = resolveSend(next, trackID, busID);
    if (send.muted == muted) return;
    send.muted = muted;
    commit(std::move(next));
}
void Session::setSendPan(uint64_t trackID, uint64_t busID, double pan, bool independent, uint64_t expected) {
    check(expected);
    if (!std::isfinite(pan) || pan < -1 || pan > 1)
        throw Error("Send balance must be between -1 and +1");
    State next = current;
    auto& send = resolveSend(next, trackID, busID);
    if (send.pan == pan && send.independentPan == independent) return;
    send.pan = pan;
    send.independentPan = independent;
    commit(std::move(next));
}
}
