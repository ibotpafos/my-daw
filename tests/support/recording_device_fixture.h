#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Only linked into the dedicated recording tests. Never part of daw_core/app.
int recording_fixture_pump(const float* input, uint32_t frames, float* left, float* right);
int recording_fixture_pump_stereo(const float* input_left, const float* input_right, uint32_t frames, float* left, float* right);
void recording_fixture_failure(int mode); // 1: start refusal, 2: device loss, 3: sample gap, 4: invalid timestamp flag
int recording_fixture_active(void);
int recording_fixture_latency(uint32_t separation, uint32_t input, uint32_t output);
int recording_fixture_no_latency(void);
#ifdef __cplusplus
}
#endif
