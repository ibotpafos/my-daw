// Same owner-thread/session lifetime/error boundary as the existing C ABI.
int daw_get_recording_clock(daw_session* s, daw_recording_clock* out) {
    return guard(s, [&] {
        if (!out || out->struct_size != sizeof(*out) || out->version != DAW_RECORDING_CLOCK_VERSION)
            throw daw::Error("Recording clock ABI mismatch");
        daw_recording_clock result{};
        result.struct_size = sizeof(result);
        result.version = DAW_RECORDING_CLOCK_VERSION;
        if (s->duplex) {
            const auto clock = s->duplex->clockReport();
            result.initial_flags = clock.initialFlags;
            result.fault = static_cast<uint32_t>(clock.fault);
            result.validated_frames = clock.validatedFrames;
            result.first_host_time = clock.firstHostTime;
            result.first_sample_time = clock.firstSampleTime;
            result.expected_sample_time = clock.expectedSampleTime;
            result.observed_sample_time = clock.observedSampleTime;
        }
        *out = result;
    });
}
