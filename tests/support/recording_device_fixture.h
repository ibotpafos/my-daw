#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Only linked into the dedicated recording tests. Never part of daw_core/app.
int recording_fixture_pump(const float* input, uint32_t frames, float* left, float* right);
int recording_fixture_pump_timestamped(const float* input, uint32_t frames, float* left,
    float* right, double sample_time, uint64_t host_time, uint32_t flags);
void recording_fixture_failure(int mode); // 1: start refusal, 2: device loss
int recording_fixture_active(void);
#ifdef __cplusplus
}
#endif
