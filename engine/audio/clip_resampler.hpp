#pragma once

#include <cstdint>
#include <vector>

namespace daw {
std::vector<float> resampleStereoTo48k(const std::vector<float>& source, uint32_t sourceRate);
}
