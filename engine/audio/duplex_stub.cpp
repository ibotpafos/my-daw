#include "audio/duplex.hpp"
#include "domain/session.hpp"
namespace daw {
std::unique_ptr<Duplex> makeDuplex(const State&,uint64_t,const std::string&,uint64_t,uint64_t,uint64_t){throw Error("Full-duplex audio is available on macOS only");}
}
