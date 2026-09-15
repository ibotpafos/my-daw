#pragma once

// The host-neutral public VST3 surface lives beside PreparedEffect so audio
// code does not need to include Steinberg SDK headers.  This header exists as
// the macOS runtime ownership boundary; consumers should include effect.hpp.
#include "audio/effect.hpp"
