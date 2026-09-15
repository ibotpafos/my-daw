#include "audio/output.hpp"
namespace daw {
std::unique_ptr<Output> makeOutput() { throw Error("Hardware output is available on macOS only"); }
}
