#include "audio/duplex.hpp"
#include "domain/session.hpp"

namespace daw {
// Match the complete shared factory signature, including preroll and monitoring.
// Unsupported platforms must reject explicitly rather than leave an unresolved
// symbol or pretend to have opened a real audio device.
std::unique_ptr<Duplex> makeDuplex(const State &, uint64_t, const std::string &, uint64_t,
                                   uint64_t, uint64_t, uint64_t, bool, const AudioDeviceConfiguration&) {
    throw Error("Full-duplex audio is available on macOS only");
}
} // namespace daw
