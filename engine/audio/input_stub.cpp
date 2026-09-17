#include "audio/input.hpp"
#include "domain/session.hpp"
namespace daw {
std::unique_ptr<Input> makeInput(uint64_t,const std::string&,uint64_t,const AudioDeviceConfiguration&) { throw Error("Hardware input is available on macOS only"); }
}
