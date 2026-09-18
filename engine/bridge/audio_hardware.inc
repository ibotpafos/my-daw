// Included by daw.cpp: keeps the existing owner-thread session/error boundary.
struct daw_audio_hardware_job {
    std::shared_ptr<daw::AudioHardwareJob> result;
};
int daw_get_audio_hardware_settings(daw_session* s, const char* uid, daw_audio_hardware_settings* out) {
    return guard(s, [&] {
        if (!uid || !out || out->struct_size != sizeof(*out) || out->version != DAW_AUDIO_HARDWARE_VERSION)
            throw daw::Error("Audio hardware settings ABI mismatch");
        const auto value = daw::readAudioHardwareSettings(uid);
        daw_audio_hardware_settings result{};
        result.struct_size = sizeof(result); result.version = DAW_AUDIO_HARDWARE_VERSION;
        result.device_id = value.deviceID; result.buffer_frames = value.bufferFrames;
        result.minimum_buffer = value.minimumBuffer; result.maximum_buffer = value.maximumBuffer;
        result.sample_rate = value.sampleRate; result.supports_48k = value.supports48k;
        result.rate_writable = value.rateWritable; result.buffer_writable = value.bufferWritable;
        copyText(result.uid, value.uid); *out = result;
    });
}
daw_audio_hardware_job* daw_begin_audio_hardware_change(daw_session* s, const daw_audio_hardware_settings* raw, uint32_t buffer) {
    std::unique_ptr<daw_audio_hardware_job> handle;
    if (guard(s, [&] {
        if (!raw || raw->struct_size != sizeof(*raw) || raw->version != DAW_AUDIO_HARDWARE_VERSION)
            throw daw::Error("Audio hardware request ABI mismatch");
        const auto end = static_cast<const char*>(std::memchr(raw->uid, 0, sizeof(raw->uid)));
        if (!end || raw->supports_48k < 0 || raw->supports_48k > 1 ||
            raw->rate_writable < 0 || raw->rate_writable > 1 || raw->buffer_writable < 0 || raw->buffer_writable > 1)
            throw daw::Error("Invalid hardware request values");
        const daw::AudioHardwareSettings expected{raw->device_id, std::string(raw->uid, end), raw->sample_rate,
            raw->buffer_frames, raw->minimum_buffer, raw->maximum_buffer,
            raw->supports_48k != 0, raw->rate_writable != 0, raw->buffer_writable != 0};
        daw::validateAudioHardwareRequest(expected, buffer);
        if (recordingActive(s) || s->midiRecorder || s->playbackPreparation ||
            (s->output && s->output->renderer.playing.load(std::memory_order_acquire)))
            throw daw::Error("Stop playback, recording and MIDI capture before changing hardware format");
        handle = std::make_unique<daw_audio_hardware_job>();
        // A stopped output can still own AUHAL. Dispose it before the worker
        // obtains exclusivity; other sessions/late prepares remain guarded.
        s->output.reset();
        handle->result = daw::beginAudioHardwareChange(expected, buffer);
    })) return nullptr;
    return handle.release();
}
int daw_poll_audio_hardware_change(daw_audio_hardware_job* handle, daw_audio_hardware_status* out) {
    if (!handle || !out || out->struct_size != sizeof(*out) || out->version != DAW_AUDIO_HARDWARE_VERSION) return 1;
    daw_audio_hardware_status result{};
    result.struct_size = sizeof(result); result.version = DAW_AUDIO_HARDWARE_VERSION;
    if (handle->result->done.load(std::memory_order_acquire)) {
        const auto& value = handle->result->outcome;
        result.status = value.success ? 1 : 2; result.restored = value.restored;
        result.actual_known = value.actualKnown; result.actual_rate = value.actualRate; result.actual_buffer = value.actualBuffer;
        std::snprintf(result.error, sizeof(result.error), "%s", value.error.c_str());
    }
    *out = result; return 0;
}
void daw_release_audio_hardware_change(daw_audio_hardware_job* handle) {
    if (!handle) return;
    handle->result->cancel.store(true, std::memory_order_release);
    delete handle;
}
