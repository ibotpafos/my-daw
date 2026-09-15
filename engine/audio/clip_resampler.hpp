#pragma once

#include <cstdint>
#include <vector>

namespace daw {
struct ImportControl;
std::vector<float> resampleStereoTo48k(const std::vector<float>& source, uint32_t sourceRate);
std::vector<float> resampleStereoTo48k(const std::vector<float>& source, uint32_t sourceRate,
                                       const ImportControl& control);
}
