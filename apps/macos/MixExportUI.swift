import AppKit
import UniformTypeIdentifiers

struct MixExportPrompt {
    var currentURL: URL?
    var scope: String
    var midiOnly: Bool
    var options: daw_export_options
    var summary: daw_export_tail_summary
}

struct MixExportDestination {
    var url: URL
    var format: Int32
    var options: daw_export_options
}

/// Only the human answers are injectable. Tests still invoke exportMix(), the
/// real session, the production background job and pollStorage() completion.
@MainActor
protocol MixExportInteraction {
    func choose(_ prompt: MixExportPrompt) -> MixExportDestination?
    func report(_ message: String)
}

@MainActor
final class AppKitMixExportInteraction: MixExportInteraction {
    func choose(_ prompt: MixExportPrompt) -> MixExportDestination? {
        let tail = WavExportTailDialog(storedMode: prompt.options.tail_mode,
                                      storedLimitSeconds: prompt.options.manual_tail_frames / 48_000,
                                      summary: prompt.summary)
        let choice = NSAlert()
        choice.messageText = "Экспорт WAV"
        choice.informativeText = "Область: \(prompt.scope). 24-bit подходит для сведения и обмена. Float32 сохраняет результат рендера без целочисленного квантования."
        if prompt.midiOnly {
            choice.informativeText += "\nMIDI сам по себе не звучит: необходим активный инструмент. Без него или при bypass/mute WAV может содержать тишину."
        }
        choice.accessoryView = tail.view
        choice.addButton(withTitle: "WAV 24-bit")
        choice.addButton(withTitle: "WAV float32")
        choice.addButton(withTitle: "Отмена")
        let format: Int32
        switch choice.runModal() {
        case .alertFirstButtonReturn: format = 1
        case .alertSecondButtonReturn: format = 2
        default: return nil
        }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.wav]
        panel.directoryURL = prompt.currentURL?.deletingLastPathComponent()
        panel.nameFieldStringValue = "\(prompt.currentURL?.deletingPathExtension().lastPathComponent ?? "Микс").wav"
        panel.message = tail.savePanelMessage
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        tail.persist()
        return MixExportDestination(url: url, format: format, options: tail.options)
    }
    func report(_ message: String) {
        let alert = NSAlert()
        alert.messageText = message
        alert.runModal()
    }
}

@MainActor
extension DraftApp {
    /// Shared by button, menu, initial action, post-dialog validation and job
    /// completion. Capture flags are read from the ABI as well as the UI cache.
    func mixExportPolicy(allowingDialog token: UUID? = nil) -> MixExportPolicy {
        var policy = MixExportPolicy(hasAudio: hasAudio, hasMIDI: hasMidiContent,
            busy: exportBusy || importBusy,
            dialogOpen: mixExportDialogToken != nil && mixExportDialogToken != token,
            recording: isRecording, midiCapture: midiTakeArmed,
            gesture: consoleGesture != nil || automationGesture != nil || pluginParameterGesture != nil
                || inspectorBrowser.midiEditor.hasUncommittedEdit
                || timelineRuler.cycleRange.isGesturing || waveforms.contains { $0.isGesturing },
            start: rangeStart, end: rangeEnd)
        guard let session else { policy.available = false; return policy }
        var transport = daw_transport()
        transport.struct_size = UInt32(MemoryLayout<daw_transport>.size)
        var recording = daw_recording()
        recording.struct_size = UInt32(MemoryLayout<daw_recording>.size)
        var capture = daw_midi_record_status_t()
        capture.struct_size = UInt32(MemoryLayout<daw_midi_record_status_t>.size)
        capture.version = UInt32(DAW_MIDI_RECORD_STATUS_VERSION)
        guard daw_get_transport(session, &transport) == 0,
              daw_get_recording(session, &recording) == 0,
              daw_midi_record_status(session, &capture) == 0 else {
            policy.available = false
            return policy
        }
        policy.duration = transport.duration
        policy.recording = policy.recording || recording.recording != 0
        policy.midiCapture = policy.midiCapture || capture.armed != 0
        return policy
    }

    func updateMixExportAvailability() {
        let policy = mixExportPolicy()
        exportButton.isEnabled = policy.canExport
        exportButton.toolTip = policy.block?.rawValue ?? (policy.midiOnly
            ? "Экспорт MIDI-инструментов в WAV · без активного инструмента возможна тишина"
            : "Экспортировать микс в WAV")
    }

    func beginMixExport() {
        if let block = mixExportPolicy().block { mixExportInteraction.report(block.rawValue); return }
        finishEditing()
        let policy = mixExportPolicy()
        if let block = policy.block { mixExportInteraction.report(block.rawValue); return }
        guard let sourceSession = session else { return }
        var snapshot = daw_snapshot()
        snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        guard daw_get_snapshot(sourceSession, &snapshot) == 0 else {
            mixExportInteraction.report(MixExportPolicy.Block.unavailable.rawValue); return
        }
        let document = mixExportDocumentID, start = rangeStart, end = rangeEnd
        let token = UUID()
        mixExportDialogToken = token
        updateMixExportAvailability()
        defer {
            if mixExportDialogToken == token { mixExportDialogToken = nil }
            updateMixExportAvailability()
        }
        let defaults = UserDefaults.standard
        let storedMode = UInt32(clamping: defaults.integer(forKey: "export.tail.mode.v1"))
        let storedLimit = UInt32(clamping: defaults.integer(forKey: "export.tail.limitSeconds.v1"))
        var options = daw_export_options()
        options.struct_size = UInt32(MemoryLayout<daw_export_options>.size)
        options.version = UInt32(DAW_EXPORT_OPTIONS_VERSION)
        options.tail_mode = (1...3).contains(storedMode) ? storedMode : UInt32(DAW_EXPORT_TAIL_AUTOMATIC)
        options.manual_tail_frames = ([2, 5, 15, 30].contains(storedLimit) ? storedLimit : 30) * 48_000
        var summary = daw_export_tail_summary()
        summary.struct_size = UInt32(MemoryLayout<daw_export_tail_summary>.size)
        guard daw_get_export_tail_summary(sourceSession, &options, &summary) == 0 else {
            reportMixExportBridgeError(); return
        }
        let prompt = MixExportPrompt(currentURL: currentURL,
            scope: end == nil ? "весь проект" : rangeLabel.stringValue,
            midiOnly: policy.midiOnly, options: options, summary: summary)
        guard let destination = mixExportInteraction.choose(prompt) else { return }
        // AppKit modal panels run a nested event loop. Never export a different
        // document, revision or selection than the one offered in that dialog.
        guard mixExportDialogToken == token, mixExportDocumentID == document,
              session == sourceSession, rangeStart == start, rangeEnd == end else {
            mixExportInteraction.report("Документ или диапазон изменился. Откройте экспорт заново."); return
        }
        if let block = mixExportPolicy(allowingDialog: token).block {
            mixExportInteraction.report(block.rawValue); return
        }
        var latest = daw_snapshot()
        latest.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        guard daw_get_snapshot(sourceSession, &latest) == 0, latest.revision == snapshot.revision else {
            mixExportInteraction.report("Проект изменился во время выбора файла. Откройте экспорт заново."); return
        }
        guard destination.url.isFileURL else {
            mixExportInteraction.report("Выберите локальный WAV-файл."); return
        }
        var chosenOptions = destination.options
        let job: OpaquePointer?
        if let start, let end {
            job = daw_begin_export_range_with_options(sourceSession, destination.url.path,
                destination.format, start, end, &chosenOptions)
        } else {
            job = daw_begin_export_with_options(sourceSession, destination.url.path,
                destination.format, &chosenOptions)
        }
        guard let job else { reportMixExportBridgeError(); return }
        exportJob = job; exportURL = destination.url; exportStarted = Date()
        exportMessage = nil; exportMessageUntil = .distantPast
        updateMixExportAvailability()
        cancelExportButton.isEnabled = true; recordButton.isEnabled = false
        updateStorageStatus()
    }

    private func reportMixExportBridgeError() {
        var bytes = [CChar](repeating: 0, count: 512)
        daw_error(session, &bytes, bytes.count)
        let text = String(decoding: bytes.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
        mixExportInteraction.report(text.isEmpty ? MixExportPolicy.Block.unavailable.rawValue : text)
    }
}
