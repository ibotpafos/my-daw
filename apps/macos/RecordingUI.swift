import AppKit

extension DraftApp {
    func beginRecording() {
        // Also recheck after the asynchronous microphone permission response.
        guard !isRecording, !midiTakeArmed, !exportBusy, !importBusy, !recordingWorkspace.isGesturing else { return }
        guard let recordingRoot else {
            setProjectMessage("Не удалось подготовить папку восстановления записи.")
            return
        }
        var transport = daw_transport()
        transport.struct_size = UInt32(MemoryLayout<daw_transport>.size)
        guard daw_get_transport(session, &transport) == 0 else {
            setProjectMessage(audioConfigurationError().localizedDescription)
            return
        }
        syncRevision()
        let recovery = recordingRoot.appendingPathComponent("\(ProcessInfo.processInfo.processIdentifier)-\(UUID().uuidString).mydawtake")
        let start = rangeStart ?? transport.frame
        let target = armedTrackID
        let result = target.map { daw_record_start_take(session, $0, start, recovery.path) }
            ?? daw_record_start(session, start, recovery.path)
        guard result == 0 else {
            // A failed device/graph preparation never claims to be recording.
            let message = audioConfigurationError().localizedDescription
            pollTransport() // a validation rejection may have preserved playback
            setProjectMessage(message)
            transportLabel.stringValue = "Запись не началась — проверьте настройки аудио"
            return
        }
        recordingStopPending = false
        activeRecordingURL = recovery
        activeRecordingRevision = revision
        activeRecordingTarget = target
        isRecording = true
        isPlaying = true
        updateRecordButton(true)
        setProjectControlsEnabled(false)
        syncRecordMonitorButton()
        pollTransport()
    }

    func finishRecording() {
        guard isRecording else { return }
        guard daw_record_request_stop(session) == 0 else {
            failRecording(audioConfigurationError().localizedDescription)
            return
        }
        recordingStopPending = true
        var timing = daw_recording_timing()
        timing.struct_size = UInt32(MemoryLayout<daw_recording_timing>.size)
        timing.version = UInt32(DAW_RECORDING_TIMING_VERSION)
        guard daw_get_recording_timing(session, &timing) == 0 else {
            failRecording(audioConfigurationError().localizedDescription)
            return
        }
        guard timing.can_finish != 0 else {
            isPlaying = false
            presentRecordingTiming(timing)
            return // Polling stays active; no sleep on AppKit's main thread.
        }
        let previousRevision = activeRecordingRevision
        let target = activeRecordingTarget
        guard daw_record_stop(session, "Запись \(recordingNumber)", previousRevision) == 0 else {
            failRecording(audioConfigurationError().localizedDescription)
            return
        }
        syncRevision()
        // Stopping in pre-roll succeeds without a clip, history entry or take.
        if revision != previousRevision {
            if let target { selectedTakes[target] = Int.max }
            recordingNumber += 1
        }
        activeRecordingURL = nil
        activeRecordingTarget = nil
        recordingStopPending = false
        isRecording = false
        isPlaying = false
        updateRecordButton(false)
        setProjectControlsEnabled(true)
        refresh()
        pollTransport()
    }

    func failRecording(_ reason: String) {
        let recovery = activeRecordingURL
        // Quiesce first; cancellation preserves confirmed raw audio.
        _ = daw_record_cancel(session)
        activeRecordingURL = nil
        activeRecordingTarget = nil
        recordingStopPending = false
        isRecording = false
        isPlaying = false
        updateRecordButton(false)
        setProjectControlsEnabled(true)
        let recoverable = recovery.map { FileManager.default.fileExists(atPath: $0.path) } ?? false
        let message = reason + (recoverable ? " Подтверждённое аудио оставлено для восстановления." : "")
        DAWLog.audio.error("Запись остановлена: \(message, privacy: .public)")
        setProjectMessage(message)
        transportLabel.stringValue = "Запись остановлена — " + message
    }

    func presentRecordingTiming(_ timing: daw_recording_timing) {
        guard timing.enabled != 0 else { return }
        if recordingStopPending || timing.drain_remaining_frames > 0 {
            isPlaying = false
            recordMonitorButton.isEnabled = false
            transportLabel.stringValue = "Завершение записи · принимаются последние входные кадры…"
        } else if timing.ready != 0 {
            transportLabel.stringValue += String(format: " · компенсация %.2f мс", Double(timing.compensation_frames) / 48)
        } else {
            transportLabel.stringValue = "Запись · ожидание временных меток устройства…"
        }
        transportLabel.toolTip = String(format:
            "Сэмплы: метки %llu + вход %u + поток %u + выход %u + поток %u + граф %llu. Буфер %u, safety %u/%u уже учтены в метках. Данные драйвера, не измеренная акустическая задержка.",
            timing.timestamp_separation_frames, timing.input_device_frames, timing.input_stream_frames,
            timing.output_device_frames, timing.output_stream_frames, timing.graph_frames,
            timing.buffer_frames, timing.input_safety_frames, timing.output_safety_frames)
    }

    func presentRecording(_ recording: daw_recording, progress: daw_recording_progress) {
        isRecording = true
        isPlaying = true
        updateRecordButton(true)
        playButton.isEnabled = false
        stopButton.isEnabled = true
        recordMonitorButton.isEnabled = true
        // The capture processor publishes the immutable compensated timeline.
        for wave in waveforms { wave.playhead = progress.timeline_frame }
        syncPlayhead(progress.timeline_frame)
        if progress.preroll_remaining_frames > 0 {
            transportLabel.stringValue = String(format: "◉ Преролл %.2f с · запись ещё не началась",
                Double(progress.preroll_remaining_frames) / 48000)
        } else if recording.loop_recording != 0 {
            transportLabel.stringValue = String(format: "● Loop recording %.1f с · дублей %d · playback + mono input",
                Double(recording.frames) / 48000, recording.pass_count)
        } else {
            let title = recording.target_track_id != 0 ? "Новый дубль" : "Запись"
            transportLabel.stringValue = String(format: "● %@ %.1f с · playback + mono input / 48 кГц",
                title, Double(recording.frames) / 48000)
        }
    }
}
