import AppKit
import AudioToolbox
import AVFoundation
import Foundation

@MainActor
private final class ExportAnswers: MixExportInteraction {
    var destination: MixExportDestination?
    var onChoose: (() -> Void)?
    var onReport: (() -> Void)?
    var prompts: [MixExportPrompt] = []
    var messages: [String] = []
    func choose(_ prompt: MixExportPrompt) -> MixExportDestination? {
        prompts.append(prompt)
        let callback = onChoose; onChoose = nil
        callback?()
        return destination
    }
    func report(_ message: String) {
        messages.append(message)
        let callback = onReport; onReport = nil
        callback?()
    }
}

/// Production AppKit controller/action/jobs; only human dialog answers and
/// hardware-dependent startup (recovery, scanning, timers) are replaced.
@MainActor
func runMixExportIntegrationTests() {
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ name: String) {
        guard value() else {
            FileHandle.standardError.write(Data("FAIL \(name)\n".utf8))
            fatalError(name)
        }
        checks += 1
        FileHandle.standardOutput.write(Data("PASS \(name)\n".utf8))
    }
    _ = NSApplication.shared
    NSApp.setActivationPolicy(.prohibited)
    let app = DraftApp()
    NSApp.delegate = app
    app.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
    let answers = ExportAnswers()
    app.mixExportInteraction = answers
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("mydaw-export-\(UUID())")
    try! FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    defer {
        if let job = app.exportJob { daw_cancel_export(job); daw_release_export(job); app.exportJob = nil }
        if let job = app.saveJob { daw_release_save(job); app.saveJob = nil }
        app.window.orderOut(nil); app.window.delegate = nil
        NotificationCenter.default.removeObserver(app)
        daw_destroy(app.session); app.session = nil
        try? FileManager.default.removeItem(at: directory)
    }
    func revision() -> UInt64 {
        var value = daw_snapshot(); value.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        guard daw_get_snapshot(app.session, &value) == 0 else { fatalError("snapshot") }
        return value.revision
    }
    func choose(_ name: String, format: Int32 = 2) -> URL {
        let url = directory.appendingPathComponent(name)
        var options = daw_export_options()
        options.struct_size = UInt32(MemoryLayout<daw_export_options>.size)
        options.version = UInt32(DAW_EXPORT_OPTIONS_VERSION)
        options.tail_mode = UInt32(DAW_EXPORT_TAIL_NONE)
        answers.destination = MixExportDestination(url: url, format: format, options: options)
        return url
    }
    func waitForJob() {
        let deadline = Date().addingTimeInterval(30)
        while app.exportJob != nil, Date() < deadline {
            app.pollStorage()
            RunLoop.current.run(until: Date().addingTimeInterval(0.005))
        }
        expect(app.exportJob == nil, "Background job reaches a terminal state")
        expect(app.mixExportDialogToken == nil && !app.cancelExportButton.isEnabled,
               "Dialog and cancel controls return to idle")
    }
    func pcm(_ url: URL) -> (frames: Int, peak: Float) {
        do {
            let file = try AVAudioFile(forReading: url)
            expect(file.fileFormat.sampleRate == 48_000 && file.fileFormat.channelCount == 2,
                   "Export is stereo 48 kHz")
            guard file.length > 0, file.length < 5_000_000,
                  let buffer = AVAudioPCMBuffer(pcmFormat: file.processingFormat,
                                               frameCapacity: AVAudioFrameCount(file.length)) else {
                fatalError("Invalid WAV length")
            }
            try file.read(into: buffer)
            guard let channels = buffer.floatChannelData else { fatalError("Expected float processing format") }
            var peak: Float = 0
            for channel in 0..<Int(buffer.format.channelCount) {
                for index in 0..<Int(buffer.frameLength) {
                    let sample = channels[channel][index]
                    guard sample.isFinite else { fatalError("Nonfinite WAV") }
                    peak = max(peak, abs(sample))
                }
            }
            return (Int(buffer.frameLength), peak)
        } catch { fatalError("Cannot read exported WAV: \(error)") }
    }
    func addMIDI(_ track: UInt64, length: UInt64 = 48_000, notes: Bool = true) {
        var clip = daw_midi_clip()
        clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        clip.version = UInt32(DAW_MIDI_CLIP_VERSION); clip.length = length
        clip.note_count = notes ? 1 : 0
        clip.start = notes ? 0 : 48_000 // Do not overlap the original one-second clip.
        var note = daw_midi_note()
        note.struct_size = UInt32(MemoryLayout<daw_midi_note>.size)
        note.version = UInt32(DAW_MIDI_NOTE_VERSION)
        note.start = 137; note.length = 20_000; note.pitch = 60; note.velocity = 100
        let result = notes ? daw_add_midi_clip(app.session, track, &clip, &note, 1, revision())
                           : daw_add_midi_clip(app.session, track, &clip, nil, 0, revision())
        expect(result == 0, "MIDI fixture enters actual session through C ABI")
    }
    let menu = NSMenuItem(title: "Экспорт WAV", action: #selector(DraftApp.exportMix), keyEquivalent: "")
    menu.target = app
    expect(!app.exportButton.isEnabled && !app.validateMenuItem(menu), "Empty project disables button and menu")
    app.exportMix()
    expect(answers.prompts.isEmpty && app.exportJob == nil, "Direct empty export also refuses")
    expect(daw_add_track(app.session, "MIDI source", revision()) == 0, "Create instrument track")
    var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
    expect(daw_get_track(app.session, 0, &track) == 0, "Read instrument track")
    let midiTrack = track.id
    addMIDI(midiTrack)
    // Use the real installed-AU scanner and catalog: the normal startup scan
    // was suppressed for this harness, and the public insert API requires it.
    let helper = URL(fileURLWithPath: "build/debug/daw_au_scan_helper").standardizedFileURL.path
    guard let scan = daw_begin_installed_au_scan(helper, 3_000) else { fatalError("AU scan did not start") }
    var scanStatus = daw_au_scan_status()
    scanStatus.struct_size = UInt32(MemoryLayout<daw_au_scan_status>.size)
    let scanDeadline = Date().addingTimeInterval(75)
    repeat {
        guard daw_poll_installed_au_scan(scan, &scanStatus) == 0 else { fatalError("AU scan poll failed") }
        if scanStatus.status != 0 { break }
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
    } while Date() < scanDeadline
    var available: UInt32 = 0, quarantined: UInt32 = 0
    let applied = scanStatus.status == 1 ? daw_apply_installed_au_scan(app.session, scan, &available, &quarantined) : 1
    daw_release_installed_au_scan(scan)
    expect(applied == 0 && available > 0, "Apply real scanned AU catalog before inserting instrument")
    // No custom test synthesizer. Apple DLS MusicDevice must actually exist and
    // render; absence is a failure of this named acceptance, not a silent skip.
    expect(daw_add_insert_au(app.session, Int32(DAW_INSERT_OWNER_TRACK), midiTrack,
        kAudioUnitType_MusicDevice, kAudioUnitSubType_DLSSynth,
        kAudioUnitManufacturer_Apple, revision()) == 0, "Insert real system DLS Synth")
    var synth = daw_plugin(); synth.struct_size = UInt32(MemoryLayout<daw_plugin>.size)
    expect(daw_get_insert(app.session, Int32(DAW_INSERT_OWNER_TRACK), midiTrack, 0, &synth) == 0,
           "Read real instrument insert")
    app.refresh()
    expect(!app.hasAudio && app.hasMidiContent, "Fixture has MIDI and no WAV source")
    expect(app.exportButton.isEnabled && app.validateMenuItem(menu), "MIDI-only button and menu are enabled")
    expect(app.exportButton.toolTip?.contains("инструмента") == true, "MIDI-only tooltip does not promise sound")
    app.selectedMixerID = midiTrack
    app.updateMixerInspector(midiTrack)
    let editor = app.inspectorBrowser.midiEditor
    let piano = editor.integrationEditState
    expect(piano.context?.documentID == app.midiDocumentID && piano.timeMap != nil,
           "Docked Piano Roll receives the real document binding and tempo map")
    let originalNotes = piano.entities.map(\.note)
    let originalRevision = revision()
    let beforePreviewPrompts = answers.prompts.count
    piano.selectAll()
    piano.previewVelocityRamp(from: 70, to: 70)
    expect(piano.isTransformPreview && editor.hasUncommittedEdit,
           "Actual Piano Roll transform has an uncommitted preview")
    expect(!app.exportButton.isEnabled && !app.validateMenuItem(menu),
           "Preview change immediately disables WAV button and menu")
    app.exportMix()
    expect(answers.prompts.count == beforePreviewPrompts && app.exportJob == nil && piano.isTransformPreview,
           "Direct WAV action cannot finish or silently ignore Piano Roll preview")
    expect(revision() == originalRevision, "Preview and rejected export do not modify project")
    piano.cancelTransformPreview()
    expect(!editor.hasUncommittedEdit && app.exportButton.isEnabled,
           "Cancel restores export without changing note data")
    expect(piano.entities.map(\.note) == originalNotes, "Cancelled preview retains original notes")

    // A missing host echo is not accepted as a committed edit.
    let commitCallback = editor.onCommitRequest
    var delayedRequest: PRCommitRequest?
    editor.onCommitRequest = { delayedRequest = $0 }
    piano.selectAll(); piano.previewVelocityRamp(from: 71, to: 71); piano.applyTransformPreview()
    expect(piano.awaitingCommit && delayedRequest != nil, "Production editor waits for authoritative host echo")
    app.exportMix()
    expect(!app.exportButton.isEnabled && app.exportJob == nil && piano.awaitingCommit,
           "Pending Piano Roll commit also blocks export")
    expect(answers.prompts.count == beforePreviewPrompts && revision() == originalRevision,
           "Pending commit cannot export or consume a revision")
    editor.onCommitRequest = commitCallback
    app.loadMidiInspector(midiTrack) // Echo the unchanged snapshot, rejecting the pending write.
    expect(!piano.awaitingCommit && piano.entities.map(\.note) == originalNotes && app.exportButton.isEnabled,
           "Rejected pending edit receives one atomic authoritative snapshot")

    // The same inspector callback now performs an actual revision-bound C ABI write.
    piano.selectAll(); piano.previewVelocityRamp(from: 72, to: 72); piano.applyTransformPreview()
    expect(revision() == originalRevision + 1 && !piano.awaitingCommit && piano.entities.first?.note.velocity == 72,
           "Docked Piano Roll commit reaches real C ABI with exactly one revision")
    app.undo()
    expect(piano.entities.map(\.note) == originalNotes && app.exportButton.isEnabled,
           "Project Undo restores the editor and export availability")
    let identityDraft = directory.appendingPathComponent("piano-identity.mydawdraft")
    expect(daw_save_draft(app.session, identityDraft.path) == 0, "Save integration identity fixture")
    let oldPianoID = app.midiDocumentID, oldExportID = app.mixExportDocumentID
    let beforeReopenRevision = revision()
    app.openDraftFile(identityDraft)
    expect(revision() == beforeReopenRevision && app.midiDocumentID != oldPianoID && app.mixExportDocumentID != oldExportID,
           "Equal-revision reopen invalidates both Piano Roll and export document tokens")
    if let delayedRequest { app.commitMidiEdit(delayedRequest) }
    expect(revision() == beforeReopenRevision && piano.entities.map(\.note) == originalNotes,
           "Late Piano Roll callback cannot edit a reopened document")

    let midiWav = choose("midi-only.wav")
    let beforeExport = revision()
    app.exportButton.performClick(nil)
    expect(app.exportJob != nil && !app.exportButton.isEnabled && !app.validateMenuItem(menu),
           "Actual toolbar action starts one job and disables duplicate export")
    let promptCount = answers.prompts.count, job = app.exportJob
    app.exportMix()
    expect(answers.prompts.count == promptCount && app.exportJob == job, "Busy command cannot replace running job")
    waitForJob()
    let first = pcm(midiWav)
    expect(first.frames >= 48_000 && first.peak > 0.000001, "Actual MIDI-only action produces finite nonzero PCM")
    expect(revision() == beforeExport, "Export is not a project edit")
    expect(app.exportButton.isEnabled && app.validateMenuItem(menu), "Completion restores MIDI-only availability")
    expect(answers.prompts.last?.midiOnly == true, "Dialog receives MIDI warning context")

    // Guards apply equally to explicit selectors, not just disabled buttons.
    let guardPrompts = answers.prompts.count
    app.isRecording = true; app.updateMixExportAvailability(); app.exportMix()
    expect(app.isRecording && !app.exportButton.isEnabled, "Export does not stop audio capture")
    app.isRecording = false; app.midiTakeArmed = true; app.exportMix()
    expect(app.midiTakeArmed && !app.mixExportPolicy().canExport, "MIDI capture remains armed and blocks export")
    app.midiTakeArmed = false; app.automationGesture = (target: 0, id: 0); app.exportMix()
    expect(app.automationGesture != nil, "Export does not finish/cancel automation gesture")
    app.automationGesture = nil
    app.pluginParameterGesture = (owner: 0, ownerID: 0, plugin: 1, parameter: 0); app.exportMix()
    expect(app.pluginParameterGesture != nil, "Parameter gesture remains untouched")
    app.pluginParameterGesture = nil
    expect(answers.prompts.count == guardPrompts && app.exportJob == nil, "Guarded commands never open dialogs/jobs")
    app.pollTransport()
    expect(app.exportButton.isEnabled, "Availability returns after guards clear")

    answers.destination = nil; app.exportMix()
    expect(app.exportJob == nil && app.exportButton.isEnabled && app.mixExportDialogToken == nil,
           "Cancel dialog leaves editor ready without a job")
    let stale = choose("stale.wav")
    answers.onChoose = {
        expect(!app.exportButton.isEnabled && !app.validateMenuItem(menu), "Dialog disables parallel requests")
        expect(daw_set_master_gain(app.session, -1, revision()) == 0, "Simulate actual revision change during modal loop")
    }
    app.exportMix()
    expect(app.exportJob == nil && !FileManager.default.fileExists(atPath: stale.path), "Stale dialog does not write a changed project")
    expect(answers.messages.last?.contains("изменился") == true, "Stale export is explained")
    expect(daw_set_master_gain(app.session, 0, revision()) == 0, "Restore master gain")
    app.refresh()
    let changedRange = choose("changed-range.wav")
    answers.onChoose = { app.rangeStart = 0; app.rangeEnd = 24_000 }
    app.exportMix()
    expect(app.exportJob == nil && !FileManager.default.fileExists(atPath: changedRange.path), "Changed range requires a fresh dialog")
    app.rangeStart = nil; app.rangeEnd = nil; app.refresh()

    let saved = directory.appendingPathComponent("source.mydawdraft")
    expect(daw_save_draft(app.session, saved.path) == 0, "Save actual MIDI/instrument project")
    let epoch = app.mixExportDocumentID
    let reopened = choose("stale-document.wav")
    answers.onChoose = { app.openDraftFile(saved) }
    app.exportMix()
    expect(app.mixExportDocumentID != epoch, "Opening same saved revision changes document identity")
    expect(app.exportJob == nil && !FileManager.default.fileExists(atPath: reopened.path), "Same-revision document switch rejects stale export")
    let afterOpen = choose("after-open.wav")
    app.exportMix(); waitForJob()
    expect(pcm(afterOpen).peak > 0.000001, "Save/open preserves actual audible MIDI export")

    FileHandle.standardOutput.write(Data("PHASE first-range/update-timeline\n".utf8))
    app.rangeStart = 0; app.rangeEnd = 24_000; app.updateTimelineTools()
    FileHandle.standardOutput.write(Data("PHASE first-range/begin-export\n".utf8))
    let ranged = choose("range.wav")
    app.exportMix(); waitForJob()
    let rangePCM = pcm(ranged)
    expect(rangePCM.frames >= 24_000 && rangePCM.frames < first.frames && rangePCM.peak > 0.000001,
           "Selected range reaches real export-range path")
    app.rangeEnd = UInt64.max
    let invalidRange = choose("invalid-range.wav"); app.exportMix()
    expect(app.exportJob == nil && !FileManager.default.fileExists(atPath: invalidRange.path), "Invalid range never starts a job")
    app.rangeStart = nil; app.rangeEnd = nil; app.refresh()

    // Fixed repetitions of the exact Save/Open → timeline update → range
    // export sequence implicated by #30. A failure stops immediately; this is
    // not a retry-until-green wrapper. All original cases below still run.
    for iteration in 0..<8 {
        FileHandle.standardOutput.write(Data("PHASE save-open-range/\(iteration)/save\n".utf8))
        expect(daw_save_draft(app.session, saved.path) == 0, "Stress save preserves source")
        let document = app.mixExportDocumentID
        app.openDraftFile(saved)
        expect(app.mixExportDocumentID != document, "Stress reopen rotates document identity")
        let before = revision()
        FileHandle.standardOutput.write(Data("PHASE save-open-range/\(iteration)/timeline\n".utf8))
        app.rangeStart = 0; app.rangeEnd = 24_000; app.updateTimelineTools()
        let url = choose("stress-range-\(iteration).wav")
        FileHandle.standardOutput.write(Data("PHASE save-open-range/\(iteration)/export\n".utf8))
        app.exportMix(); waitForJob()
        let rendered = pcm(url)
        expect(rendered.frames == rangePCM.frames && rendered.peak > 0.000001,
               "Repeated range export retains exact frame count and audible PCM")
        expect(revision() == before, "Repeated range export does not edit the document")
        app.rangeStart = nil; app.rangeEnd = nil; app.refresh()
    }

    let reentrant = choose("one-request.wav")
    let beforePrompts = answers.prompts.count
    answers.onChoose = { app.exportMix() }
    app.exportMix(); waitForJob()
    expect(answers.prompts.count == beforePrompts + 1 && pcm(reentrant).peak > 0.000001, "Nested action cannot launch a second dialog/export")

    // A valid file containing silence is reported as such, not evidence that
    // absent/bypassed instruments have worked.
    expect(daw_set_insert_bypass(app.session, Int32(DAW_INSERT_OWNER_TRACK), midiTrack, synth.id, 1, revision()) == 0, "Bypass actual synth")
    app.refresh(); let bypassWav = choose("bypassed.wav"); app.exportMix(); waitForJob()
    expect(pcm(bypassWav).peak == 0, "Bypassed instrument produces honestly measured silence")
    expect(app.exportMessage?.contains("ниже −70 LUFS") == true, "Completion warns about silent WAV")
    expect(daw_remove_insert(app.session, Int32(DAW_INSERT_OWNER_TRACK), midiTrack, synth.id, revision()) == 0, "Remove instrument")
    app.refresh(); let absentWav = choose("no-instrument.wav"); app.exportMix(); waitForJob()
    expect(pcm(absentWav).peak == 0, "Missing instrument is not confused with audible success")
    app.undo()
    expect(daw_set_insert_bypass(app.session, Int32(DAW_INSERT_OWNER_TRACK), midiTrack, synth.id, 0, revision()) == 0, "Restore synth after project Undo")
    app.refresh()

    // Cancellation keeps an existing destination unchanged and restores the
    // same availability policy. Long duration leaves time to request cancellation before publication.
    addMIDI(midiTrack, length: 48_000 * 120, notes: false); app.refresh()
    let cancelWav = choose("cancel.wav")
    let originalData = try! Data(contentsOf: midiWav)
    try! originalData.write(to: cancelWav)
    app.exportMix(); expect(app.exportJob != nil, "Start cancellable long export")
    app.cancelExport(); waitForJob()
    expect(try! Data(contentsOf: cancelWav) == originalData, "Cancel preserves previously completed destination")
    expect(app.exportMessage?.contains("отменён") == true && app.exportButton.isEnabled, "Cancellation restores MIDI-only command")
    expect(daw_remove_midi_clip(app.session, midiTrack, 1, revision()) == 0, "Remove long cancellation fixture")
    app.refresh()

    // Error path is real worker filesystem failure, not a fabricated job status.
    let failureURL = choose("missing-parent/failure.wav")
    app.exportMix(); waitForJob()
    expect(!FileManager.default.fileExists(atPath: failureURL.path) && app.exportMessage?.contains("Ошибка экспорта") == true,
           "Real worker error is surfaced without claiming success")
    expect(app.exportButton.isEnabled && answers.messages.last?.contains("Не удалось экспортировать WAV") == true,
           "Failed job restores availability and reports its reason")
    let retry = choose("retry.wav"); app.exportMix(); waitForJob()
    expect(pcm(retry).peak > 0.000001, "Retry after worker failure works")

    // Error reporting, like the file picker, may enter a nested event loop.
    // A retry from there must own independent URL/job state after the old poll returns.
    let nestedRetry = directory.appendingPathComponent("nested-retry.wav")
    _ = choose("missing-parent/again.wav")
    answers.onReport = {
        expect(app.exportJob == nil && app.exportURL == nil,
               "Failed job state is detached before entering error dialog")
        _ = choose("nested-retry.wav")
        app.exportMix()
        expect(app.exportJob != nil && app.exportURL == nestedRetry,
               "Error dialog can start a fresh independent retry")
    }
    app.exportMix(); waitForJob()
    expect(pcm(nestedRetry).peak > 0.000001 && app.exportMessage?.contains("nested-retry.wav") == true,
           "Old error completion cannot erase a reentrant retry URL or status")

    expect(daw_import_wav(app.session, midiWav.path, "Audio source", revision()) == 0, "Add actual audio to create mixed project")
    app.refresh(); let mixed = choose("mixed.wav", format: 1); app.exportMix(); waitForJob()
    expect(app.hasAudio && app.hasMidiContent && pcm(mixed).peak > 0.000001, "Mixed project still exports PCM24")
    expect(daw_remove_track(app.session, midiTrack, revision()) == 0, "Remove MIDI track for audio-only regression")
    app.refresh(); let audioOnly = choose("audio-only.wav"); app.exportMix(); waitForJob()
    expect(app.hasAudio && !app.hasMidiContent && pcm(audioOnly).peak > 0.000001, "Audio-only export unchanged")
    var audioTrack = daw_track(); audioTrack.struct_size = UInt32(MemoryLayout<daw_track>.size)
    expect(daw_get_track(app.session, 0, &audioTrack) == 0, "Read last track")
    expect(daw_remove_track(app.session, audioTrack.id, revision()) == 0, "Delete last content")
    app.refresh()
    expect(!app.exportButton.isEnabled && !app.validateMenuItem(menu), "Deleting last clip/track disables export")
    app.undo(); expect(app.exportButton.isEnabled, "Project Undo restores export availability")

    do {
        let evidence = URL(fileURLWithPath: "build/mix-export-ui")
        try FileManager.default.createDirectory(at: evidence, withIntermediateDirectories: true)
        let output = evidence.appendingPathComponent("midi-only.wav")
        try? FileManager.default.removeItem(at: output)
        try FileManager.default.copyItem(at: midiWav, to: output)
        let proof: [String: Any] = ["instrument": "Apple DLS Synth", "entry": "DraftApp.exportMix / NSButton.performClick",
            "frames": first.frames, "sampleRate": 48_000, "channels": 2, "peak": first.peak,
            "dialogAnswersInjected": true, "physicalHardwareTested": false, "assertions": checks]
        try JSONSerialization.data(withJSONObject: proof, options: [.prettyPrinted, .sortedKeys])
            .write(to: evidence.appendingPathComponent("proof.json"))
    } catch { fatalError("Evidence: \(error)") }
    print("RESULT \(checks) native mix-export checks passed; actual AU-to-WAV through production action")
}
