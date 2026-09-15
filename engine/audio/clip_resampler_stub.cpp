#include "audio/clip_resampler.hpp"
#include "audio/clip.hpp"
#include "domain/session.hpp"

namespace daw {
std::vector<float> resampleStereoTo48k(const std::vector<float>& source, uint32_t sourceRate) {
    if (sourceRate == 48000) return source;
    throw Error("Sample-rate conversion is available on macOS only");
}
std::vector<float> resampleStereoTo48k(const std::vector<float>& source, uint32_t sourceRate,
                                       const ImportControl& control) {
    checkImportCanceled(control);
    auto result = resampleStereoTo48k(source, sourceRate);
    updateImportProgress(control, 1, 1);
    return result;
}
}
