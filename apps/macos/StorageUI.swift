import AppKit
import Darwin

@MainActor
final class WavExportTailDialog: NSObject {
    let view = NSView(frame: NSRect(x: 0, y: 0, width: 430, height: 84))
    private let mode = NSPopUpButton(frame: .zero, pullsDown: false)
    private let limit = NSPopUpButton(frame: .zero, pullsDown: false)
    private let explanation = NSTextField(wrappingLabelWithString: "")
    private let finiteTailFrames: UInt32
    private let infiniteTail: Bool

    init(storedMode: UInt32, storedLimitSeconds: UInt32, summary: daw_export_tail_summary) {
        finiteTailFrames = summary.finite_tail_frames
        infiniteTail = summary.infinite_tail_detected != 0
        super.init()
        mode.addItems(withTitles: ["Автоматически", "Только конечный", "Ограничить"])
        mode.selectItem(at: Int(max(1, min(3, storedMode))) - 1)
        mode.target = self; mode.action = #selector(updateExplanation)
        mode.setAccessibilityLabel("Хвост после диапазона")
        mode.setAccessibilityHelp("Автоматически сохраняет конечный хвост и применяет безопасный предел к бесконечному. Второй вариант исключает только бесконечный хвост. Ограничить задаёт его предел после диапазона.")

        limit.addItems(withTitles: ["2 с", "5 с", "15 с", "30 с"])
        let preset = [2, 5, 15, 30]
        limit.selectItem(at: preset.firstIndex(of: Int(storedLimitSeconds)) ?? 3)
        limit.target = self; limit.action = #selector(updateExplanation)
        limit.setAccessibilityLabel("Предел хвоста")
        limit.setAccessibilityHelp("Длительность хвоста после выбранного диапазона: 2, 5, 15 или 30 секунд.")

        let modeRow = NSStackView(views: [NSTextField(labelWithString: "Хвост после диапазона"), mode, limit])
        modeRow.spacing = 8; modeRow.alignment = .centerY
        explanation.font = .systemFont(ofSize: 11); explanation.textColor = .secondaryLabelColor
        explanation.maximumNumberOfLines = 2
        let content = NSStackView(views: [modeRow, explanation])
        content.orientation = .vertical; content.spacing = 6; content.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: view.leadingAnchor), content.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            content.topAnchor.constraint(equalTo: view.topAnchor), content.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            mode.widthAnchor.constraint(equalToConstant: 142), limit.widthAnchor.constraint(equalToConstant: 76)
        ])
        updateExplanation()
    }

    @objc private func updateExplanation() {
        let isManual = mode.indexOfSelectedItem == 2
        limit.isEnabled = isManual
        if mode.indexOfSelectedItem == 1 {
            explanation.stringValue = finiteTailFrames == 0 ? "Бесконечный хвост не будет добавлен; WAV завершится в конце области после компенсации задержки." : String(format: "Бесконечный хвост не будет добавлен; конечный хвост эффектов %.1f с сохранится.", Double(finiteTailFrames) / 48_000)
        } else if isManual {
            explanation.stringValue = infiniteTail ? "После области будет обработано до \(limitSeconds) с тишины для бесконечного хвоста. Конечный хвост сохранится." : "Бесконечный хвост не обнаружен; выбранный предел не обрезает конечный спад."
        } else if infiniteTail {
            explanation.stringValue = "Плагин объявил бесконечный хвост. Автоматический экспорт использует безопасный конечный предел."
        } else if finiteTailFrames == 0 {
            explanation.stringValue = "Подключённые эффекты не объявили дополнительный хвост."
        } else {
            explanation.stringValue = String(format: "Будет добавлен конечный хвост эффектов: %.1f с.", Double(finiteTailFrames) / 48_000)
        }
    }

    private var limitSeconds: Int { [2, 5, 15, 30][max(0, min(3, limit.indexOfSelectedItem))] }
    var options: daw_export_options {
        var value = daw_export_options()
        value.struct_size = UInt32(MemoryLayout<daw_export_options>.size)
        value.version = UInt32(DAW_EXPORT_OPTIONS_VERSION)
        switch mode.indexOfSelectedItem {
        case 1: value.tail_mode = UInt32(DAW_EXPORT_TAIL_NONE)
        case 2: value.tail_mode = UInt32(DAW_EXPORT_TAIL_MANUAL_LIMIT); value.manual_tail_frames = UInt32(limitSeconds * 48_000)
        default: value.tail_mode = UInt32(DAW_EXPORT_TAIL_AUTOMATIC)
        }
        return value
    }

    func persist() {
        let defaults = UserDefaults.standard
        defaults.set(options.tail_mode, forKey: "export.tail.mode.v1")
        defaults.set(limitSeconds, forKey: "export.tail.limitSeconds.v1")
    }

    var savePanelMessage: String {
        switch mode.indexOfSelectedItem {
        case 1: return "Экспортируется зафиксированный снимок без бесконечного хвоста; конечный хвост сохранится. Можно продолжать редактирование."
        case 2: return "Экспортируется зафиксированный снимок с пределом бесконечного хвоста \(limitSeconds) с; конечный хвост сохранится. Можно продолжать редактирование."
        default: return infiniteTail ? "Экспортируется зафиксированный снимок с автоматическим безопасным пределом бесконечного хвоста. Можно продолжать редактирование." : "Экспортируется зафиксированный снимок с объявленным хвостом эффектов. Можно продолжать редактирование."
        }
    }
}

@MainActor
extension DraftApp {
    func storageMessage(_ text: String) {
        let alert = NSAlert(); alert.messageText = text; alert.runModal()
    }
    func setupRecovery() {
        do {
            let support = try FileManager.default.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            let root = support.appendingPathComponent("My DAW/Recovery", isDirectory: true)
            let recordings = support.appendingPathComponent("My DAW/Recording Recovery", isDirectory: true)
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            try FileManager.default.createDirectory(at: recordings, withIntermediateDirectories: true)
            recoveryRoot = root; recordingRoot = recordings; rotateRecovery()
            auCacheURL = support.appendingPathComponent("My DAW/au-scan-cache-v1.txt")
        } catch { recoveryError = "Резервные копии недоступны: \(error.localizedDescription)" }
    }
    func rotateRecovery() {
        if let old = recoveryURL {
            if recoveryJobURL == old { retiredRecovery.insert(old) }
            else { try? FileManager.default.removeItem(at: old) }
        }
        if let source = recoveredFrom { try? FileManager.default.removeItem(at: source); recoveredFrom = nil }
        recoveryURL = recoveryRoot?.appendingPathComponent("\(ProcessInfo.processInfo.processIdentifier)-\(UUID().uuidString).mydawdraft")
        recoveredRevision = nil; nextRecovery = Date().addingTimeInterval(5); recoveryError = nil
    }
    func importPhaseText(_ phase: Int32) -> String {
        switch phase {
        case Int32(DAW_IMPORT_PHASE_READING): return "чтение файла"
        case Int32(DAW_IMPORT_PHASE_DECODING): return "декодирование PCM"
        case Int32(DAW_IMPORT_PHASE_CONVERTING): return "конвертация к 48 кГц"
        case Int32(DAW_IMPORT_PHASE_READY): return "готово к добавлению"
        default: return "подготовка"
        }
    }
    func setImportMessage(_ text: String, duration: TimeInterval = 8) {
        importMessage = text
        importMessageUntil = Date().addingTimeInterval(duration)
    }
    func setProjectMessage(_ text: String, duration: TimeInterval = 8) {
        projectMessage = text
        projectMessageUntil = Date().addingTimeInterval(duration)
    }
    func releaseImportJob(cancel: Bool) {
        guard let job = importJob else { return }
        if cancel { daw_cancel_import(job) }
        daw_release_import(job)
        importJob = nil; importIntent = nil; importSession = nil; importStatus = nil
        libraryBrowser.isImportBusy = false
        cancelImportButton.isEnabled = false; cancelImportButton.isHidden = true
        resolveImportButton.isEnabled = false; resolveImportButton.isHidden = true
    }
    func selectAppliedImport(_ intent: BackgroundImportIntent) {
        refresh()
        switch intent {
        case .track:
            if let track = mixerWorkspace.strips.first(where: { $0.kind == .track && !importExistingTrackIDs.contains($0.id) }) {
                selectedMixerID = track.id; inspectorTrackID = track.id; inspectorClipIndex = 0; selectedClips[track.id] = 0
                refresh()
            }
        case let .take(_, _, trackID, _):
            var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
            if daw_get_snapshot(session, &snapshot) == 0 {
                for index in 0..<snapshot.track_count {
                    var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
                    if daw_get_track(session, index, &track) == 0, track.id == trackID, track.take_count > 0 {
                        selectedTakes[trackID] = Int(track.take_count - 1)
                        selectedMixerID = trackID; inspectorTrackID = trackID; inspectorClipIndex = 0
                        refresh()
                        break
                    }
                }
            }
        }
        importExistingTrackIDs.removeAll()
    }
    func applyReadyImport(explicitly: Bool) {
        guard let job = importJob, let intent = importIntent else { return }
        guard importSession == session else {
            releaseImportJob(cancel: true)
            setImportMessage("Импорт отменён: проект уже сменился")
            updateStorageStatus()
            return
        }
        guard explicitly || revision == importBaseRevision else {
            resolveImportButton.title = "＋ WAV/AIFF"
            resolveImportButton.toolTip = "Добавить готовый WAV/AIFF в текущую ревизию проекта"
            resolveImportButton.setAccessibilityLabel("Добавить готовый WAV/AIFF в текущую ревизию проекта")
            resolveImportButton.isHidden = false; resolveImportButton.isEnabled = true
            setImportMessage("WAV/AIFF готов. Проект изменился во время импорта — добавь его явно в текущую ревизию или отмени.", duration: .infinity)
            updateStorageStatus()
            return
        }
        guard check(daw_apply_import(session, job, revision)) else {
            setImportMessage("Не удалось добавить готовый WAV. Можно повторить или отменить импорт.", duration: .infinity)
            resolveImportButton.title = "Повторить"
            resolveImportButton.toolTip = "Повторить добавление готового WAV/AIFF"
            resolveImportButton.setAccessibilityLabel("Повторить добавление готового WAV/AIFF")
            resolveImportButton.isHidden = false; resolveImportButton.isEnabled = true
            updateStorageStatus()
            return
        }
        releaseImportJob(cancel: false)
        setImportMessage("WAV/AIFF добавлен в проект")
        selectAppliedImport(intent)
        updateStorageStatus()
    }
    @objc func resolveReadyImport() { applyReadyImport(explicitly: true) }
    @objc func cancelImport() {
        guard importJob != nil else { return }
        cancelImportButton.isEnabled = false
        setImportMessage("Отмена импорта WAV/AIFF…", duration: .infinity)
        if let job = importJob { daw_cancel_import(job) }
        updateStorageStatus()
    }
    func pollImport() {
        guard let job = importJob else { return }
        var result = daw_import_status(); result.struct_size = UInt32(MemoryLayout<daw_import_status>.size); result.version = UInt32(DAW_IMPORT_STATUS_VERSION)
        guard daw_poll_import(job, &result) == 0 else { return }
        importStatus = result
        switch result.status {
        case Int32(DAW_IMPORT_RUNNING):
            break
        case Int32(DAW_IMPORT_READY):
            if revision == result.base_revision && importSession == session { applyReadyImport(explicitly: false) }
            else {
                resolveImportButton.title = "＋ WAV/AIFF"
                resolveImportButton.toolTip = "Добавить готовый WAV/AIFF/AIFF в текущую ревизию проекта"
                resolveImportButton.setAccessibilityLabel("Добавить готовый WAV/AIFF/AIFF в текущую ревизию проекта")
                resolveImportButton.isHidden = false; resolveImportButton.isEnabled = true
                setImportMessage("WAV/AIFF готов. Проект изменился во время импорта — добавь его явно в текущую ревизию или отмени.", duration: .infinity)
            }
        case Int32(DAW_IMPORT_APPLIED):
            // UI releases immediately after its own successful apply. This branch
            // keeps the bridge contract safe if a future caller applies elsewhere.
            releaseImportJob(cancel: false); setImportMessage("WAV/AIFF добавлен в проект")
        case Int32(DAW_IMPORT_CANCELED):
            releaseImportJob(cancel: false); setImportMessage("Импорт WAV/AIFF отменён")
        case Int32(DAW_IMPORT_FAILED):
            let error = withUnsafeBytes(of: result.error) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
            releaseImportJob(cancel: false); DAWLog.jobs.error("Импорт WAV/AIFF сорвался: \(error.isEmpty ? "неизвестная ошибка" : error, privacy: .public)"); setImportMessage("Ошибка импорта WAV/AIFF: \(error.isEmpty ? "неизвестная ошибка" : error)")
        default:
            releaseImportJob(cancel: true); setImportMessage("Импорт WAV/AIFF остановлен из-за неизвестного состояния")
        }
    }
    func beginSave(to url: URL, completion: (() -> Void)? = nil) {
        guard saveJob == nil else { storageMessage("Сохранение уже выполняется. Дождись завершения."); return }
        guard let job = daw_begin_save(session, url.path) else { _ = check(1); return }
        saveJob = job; saveURL = url; saveCompletion = completion; saveStarted = Date(); saveError = nil
        updateStorageStatus()
    }
    func chooseSave(completion: (() -> Void)? = nil) {
        let panel = NSSavePanel(); panel.allowedContentTypes = [draftType]
        if let currentURL { panel.directoryURL = currentURL.deletingLastPathComponent() }
        else if let root = recoveryRoot?.deletingLastPathComponent().appendingPathComponent("Projects", isDirectory: true) {
            do {
                try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
                panel.directoryURL = root
            } catch { storageMessage("Не удалось подготовить папку проектов: \(error.localizedDescription)"); return }
        }
        panel.nameFieldStringValue = currentURL?.lastPathComponent ?? "Моя сессия.mydawdraft"
        panel.message = "Черновик включает аудио и монтаж. Можно продолжать работу во время сохранения."
        guard panel.runModal() == .OK, let url = panel.url else { return }
        beginSave(to: url, completion: completion)
    }
    @objc func saveDraft() {
        if isRecording { finishRecording(); guard !isRecording else { return } }
        finishEditing()
        if let url = currentURL { beginSave(to: url) } else { chooseSave() }
    }
    @objc func saveAs() { if isRecording { finishRecording(); guard !isRecording else{return} }; finishEditing(); chooseSave() }
    @objc func exportMix() { beginMixExport() }
    @objc func exportStems() {
        guard !exportBusy else { storageMessage("Экспорт уже выполняется."); return }
        if isRecording { finishRecording(); guard !isRecording else { return } }
        finishEditing()
        let defaults = UserDefaults.standard
        let storedModeRaw = defaults.integer(forKey: "export.tail.mode.v1")
        let storedLimitRaw = defaults.integer(forKey: "export.tail.limitSeconds.v1")
        let storedMode = UInt32(max(0, storedModeRaw))
        let storedLimit = UInt32(max(0, storedLimitRaw))
        var initialOptions = daw_export_options(); initialOptions.struct_size = UInt32(MemoryLayout<daw_export_options>.size); initialOptions.version = UInt32(DAW_EXPORT_OPTIONS_VERSION)
        initialOptions.tail_mode = storedMode >= UInt32(DAW_EXPORT_TAIL_AUTOMATIC) && storedMode <= UInt32(DAW_EXPORT_TAIL_MANUAL_LIMIT) ? storedMode : UInt32(DAW_EXPORT_TAIL_AUTOMATIC)
        initialOptions.manual_tail_frames = [2, 5, 15, 30].contains(Int(storedLimit)) ? storedLimit * 48_000 : 30 * 48_000
        var tailSummary = daw_export_tail_summary(); tailSummary.struct_size = UInt32(MemoryLayout<daw_export_tail_summary>.size)
        guard check(daw_get_export_tail_summary(session, &initialOptions, &tailSummary)) else { return }
        let tailDialog = WavExportTailDialog(storedMode: initialOptions.tail_mode, storedLimitSeconds: initialOptions.manual_tail_frames / 48_000, summary: tailSummary)
        let choice = NSAlert(); choice.messageText = "Экспорт стемов"
        choice.informativeText = "По WAV-файлу на слышимую выбранную дорожку: мастер-гейн и мастер-цепочка не применяются (сумма стемов даёт микс до мастера). Замьюченные и пустые дорожки пропускаются."
        var trackChecks: [(id: UInt64, box: NSButton)] = []
        var stemsSnapshot = daw_snapshot(); stemsSnapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        if check(daw_get_snapshot(session, &stemsSnapshot)) {
            let trackList = NSStackView(); trackList.orientation = .vertical; trackList.alignment = .leading; trackList.spacing = 2
            for index in 0..<Int(stemsSnapshot.track_count) {
                var row = daw_track(); row.struct_size = UInt32(MemoryLayout<daw_track>.size)
                guard check(daw_get_track(session, UInt32(index), &row)) else { continue }
                let box = NSButton(checkboxWithTitle: withUnsafeBytes(of: row.name) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }, target: nil, action: nil); box.state = .on
                trackChecks.append((row.id, box)); trackList.addArrangedSubview(box)
            }
            let trackScroll = NSScrollView(); trackScroll.hasVerticalScroller = true; trackScroll.borderType = .bezelBorder; trackScroll.drawsBackground = false
            trackScroll.documentView = trackList
            trackScroll.frame = NSRect(x: 0, y: 0, width: 260, height: min(120, CGFloat(22 * max(1, trackChecks.count) + 8)))
            trackScroll.setAccessibilityLabel("Список дорожек для экспорта стемов")
            let stemsAccessory = NSStackView(views: [tailDialog.view, trackScroll]); stemsAccessory.orientation = .vertical; stemsAccessory.alignment = .leading
            choice.accessoryView = stemsAccessory
        }
        choice.addButton(withTitle: "WAV 24-bit"); choice.addButton(withTitle: "WAV float32"); choice.addButton(withTitle: "Отмена")
        let response = choice.runModal()
        guard response != .alertThirdButtonReturn else { return }
        let format: Int32 = response == .alertSecondButtonReturn ? 2 : 1
        var options = tailDialog.options; tailDialog.persist()
        let panel = NSOpenPanel(); panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
        if let currentURL { panel.directoryURL = currentURL.deletingLastPathComponent() }
        panel.message = "Папка для стемов — файлы получат имена «01 − дорожка.wav», «02 − …»"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let job: OpaquePointer?
        let chosen = trackChecks.filter { $0.box.state == .on }.map { $0.id }
        if chosen.isEmpty { storageMessage("Выбери хотя бы одну дорожку для стемов."); return }
        if chosen.count == trackChecks.count {
            job = daw_begin_stem_export(session, url.path, format, &options)
        } else {
            job = chosen.withUnsafeBufferPointer { daw_begin_stem_export_tracks(session, url.path, format, &options, $0.baseAddress, UInt32(chosen.count)) }
        }
        guard let job else { _ = check(1); return }
        exportJob = job; exportURL = url; exportStarted = Date(); exportMessage = nil; exportMessageUntil = .distantPast
        updateMixExportAvailability(); cancelExportButton.isEnabled = true; recordButton.isEnabled = false
        updateStorageStatus()
    }
    @objc func exportDawproject() {
        guard !exportBusy else { storageMessage("Экспорт уже выполняется."); return }
        if isRecording { finishRecording(); guard !isRecording else { return } }
        finishEditing()
        let panel = NSSavePanel(); panel.allowedContentTypes = [dawprojectType]
        if let currentURL { panel.directoryURL = currentURL.deletingLastPathComponent() }
        let title = currentURL?.deletingPathExtension().lastPathComponent ?? "Моя сессия"
        panel.nameFieldStringValue = "\(title).dawproject"
        panel.message = "Переносит дорожки, монтаж, routing, automation, аудио и состояния Audio Unit. Отчёт о преобразованиях сохраняется внутри файла."
        guard panel.runModal() == .OK, let url = panel.url else { return }
        // Темп и размер берутся из темпо-карты проекта (якорь на frame 0), а не из локального стейта UI.
        let anchorTempo = tempoMap.tempoPoint(atFrame: 0).bpm
        let anchorSignature = tempoMap.signature(atFrame: 0)
        guard let job = daw_begin_dawproject_export(session,url.path,anchorTempo,UInt32(anchorSignature.numerator),UInt32(anchorSignature.denominator),title) else { _ = check(1); return }
        dawprojectJob=job;exportURL=url;exportStarted=Date();exportMessage=nil;exportMessageUntil = .distantPast
        updateMixExportAvailability();dawprojectButton.isEnabled=false;cancelExportButton.isEnabled=true;recordButton.isEnabled=false
        updateStorageStatus()
    }
    @objc func cancelExport() {
        if let exportJob { daw_cancel_export(exportJob) }
        else if let dawprojectJob { daw_cancel_dawproject_export(dawprojectJob) }
        else { return }
        cancelExportButton.isEnabled = false
        exportMessage = "Отмена экспорта…"
        updateStorageStatus()
    }
    func requestLeave(_ action: @escaping () -> Void) {
        if isRecording { finishRecording(); guard !isRecording else { return } }
        finishEditing()
        guard saveJob == nil && recoveryJob == nil else { storageMessage("Сохранение ещё выполняется. После завершения можно закрыть или сменить проект."); return }
        guard dirty else { action(); return }
        leavePromptActive = true
        defer { leavePromptActive = false }
        let alert = NSAlert(); alert.messageText = "Сохранить изменения черновика?"
        alert.informativeText = "Несохранённое аудио и монтаж будут потеряны при выборе «Не сохранять»."
        alert.addButton(withTitle: "Сохранить"); alert.addButton(withTitle: "Отмена"); alert.addButton(withTitle: "Не сохранять")
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            let continuation: () -> Void = { [weak self] in self?.requestLeave(action) }
            if let url = currentURL { beginSave(to: url, completion: continuation) } else { chooseSave(completion: continuation) }
        case .alertThirdButtonReturn: action()
        default: break
        }
    }
    @objc func newDraft() {
        requestLeave { [weak self] in
            guard let self, let fresh = daw_create() else { return }
            // Do not discard the current document when hardware settings are
            // temporarily exclusive (including cancellation still rolling back).
            guard self.restoreAudioDeviceConfiguration(fresh) else { daw_destroy(fresh); return }
            self.stopBrowserAudioPreview()
            self.releaseImportJob(cancel: true)
            self.rotateRecovery(); daw_destroy(self.session); self.session = fresh
            self.mixExportDocumentID = UUID()
            self.midiDocumentID = UUID()
            self.currentURL = nil; self.savedRevision = 0; self.saveError = nil; self.rangeStart=nil;self.rangeEnd=nil;self.loopEnabled=false;self.armedTrackID=nil;self.selectedTakes.removeAll();self.refresh();self.updateTimelineTools()
        }
    }
    @objc func openDraft() {
        requestLeave { [weak self] in
            guard let self else { return }
            let panel = NSOpenPanel(); panel.allowedContentTypes = [self.draftType, .data]
            panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
            guard panel.runModal() == .OK, let url = panel.url else { return }
            self.openDraftFile(url)
        }
    }
    func openDraftFile(_ url: URL) {
            self.stopBrowserAudioPreview()
            self.releaseImportJob(cancel: true)
            guard self.check(daw_open_draft(self.session, url.path)) else { return }
            self.mixExportDocumentID = UUID()
            self.midiDocumentID = UUID()
            var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
            guard self.check(daw_get_snapshot(self.session, &snapshot)) else { return }
            self.rotateRecovery(); self.currentURL = url; self.savedRevision = snapshot.revision; self.saveError = nil; self.rangeStart=nil;self.rangeEnd=nil;self.loopEnabled=false;self.armedTrackID=nil;self.selectedTakes.removeAll();self.refresh();self.updateTimelineTools()
    }
    @objc func packageProject() {
        guard !exportBusy else { storageMessage("Дождитесь окончания текущего фонового задания."); return }
        if isRecording { finishRecording(); guard !isRecording else { return } }
        finishEditing()
        guard let draft = currentURL else { storageMessage("Сначала сохраните проект — архив упаковывает сохранённый файл."); return }
        let panel = NSSavePanel(); panel.allowedContentTypes = [.zip]
        panel.nameFieldStringValue = draft.deletingPathExtension().lastPathComponent + ".mydawzip"
        panel.message = "Архив .mydawzip содержит черновик со всем аудио и манифест."
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if check(daw_package_project(session, draft.path, url.path)) { storageMessage("Проект упакован: \(url.lastPathComponent)") }
    }
    @objc func openPackage() {
        if isRecording { storageMessage("Сначала остановите запись."); return }
        guard !exportBusy else { storageMessage("Дождитесь окончания текущего фонового задания."); return }
        finishEditing()
        requestLeave { [weak self] in
            guard let self else { return }
            let panel = NSOpenPanel(); panel.allowedContentTypes = [.zip]
            panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
            guard panel.runModal() == .OK, let zip = panel.url else { return }
            var target = zip.deletingPathExtension().appendingPathExtension("mydawdraft")
            var counter = 2
            while FileManager.default.fileExists(atPath: target.path) { target = zip.deletingPathExtension().appendingPathExtension("mydawdraft-\(counter)"); counter += 1 }
            guard self.check(daw_extract_package(self.session, zip.path, target.path)) else { return }
            self.openDraftFile(target)
            self.storageMessage("Проект извлечён из архива: \(target.lastPathComponent)")
        }
    }
    func pollStorage() {
        var exportFailureToReport: String?
        defer { updateMixExportAvailability() }
        pollImport()
        if let job = exportJob {
            var result = daw_export_status(); result.struct_size = UInt32(MemoryLayout<daw_export_status>.size)
            if daw_poll_export(job, &result) == 0 && result.status != 0 {
                daw_release_export(job); exportJob = nil
                cancelExportButton.isEnabled = false; recordButton.isEnabled = true
                if result.status == 1 {
                    var message = "Экспорт готов: \(exportURL?.lastPathComponent ?? "WAV") · снимок ревизии \(result.revision)"
                    if let url = exportURL {
                        if url.pathExtension.lowercased() == "wav" {
                            var report = daw_loudness_report(); report.struct_size = UInt32(MemoryLayout<daw_loudness_report>.size)
                            if daw_measure_wav(session, url.path, &report) == 0 {
                                message += report.gated_silence != 0 ? " · тишина или ниже −70 LUFS — проверьте mute и MIDI-инструменты" : String(format: " · %.1f LUFS · %.1f dBTP", report.integrated_lufs, report.true_peak_db)
                            }
                        } else if ((try? FileManager.default.contentsOfDirectory(atPath: url.path))?.contains(where: { $0.hasSuffix(".wav") }) ?? false) {
                            message += " · стемы в папке"
                        }
                    }
                    exportMessage = message
                    DAWLog.jobs.info("Экспорт готов: статус \(result.status, privacy: .public), ревизия \(result.revision, privacy: .public)")
                } else if result.status == 3 {
                    DAWLog.jobs.info("Экспорт отменён пользователем")
                    exportMessage = "Экспорт отменён · готовый файл не заменён"
                } else {
                    let error = withUnsafeBytes(of: result.error) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
                    DAWLog.jobs.error("Экспорт сорвался: \(error, privacy: .public)")
                    exportMessage = "Ошибка экспорта: \(error)"
                    exportFailureToReport = "Не удалось экспортировать WAV: \(error)"
                }
                exportMessageUntil = Date().addingTimeInterval(6)
                exportURL = nil; updateMixExportAvailability()
            }
        }
        if let job=dawprojectJob {
            var result=daw_dawproject_status();result.struct_size=UInt32(MemoryLayout<daw_dawproject_status>.size)
            if daw_poll_dawproject_export(job,&result)==0 && result.status != 0 {
                daw_release_dawproject_export(job);dawprojectJob=nil
                cancelExportButton.isEnabled=false;recordButton.isEnabled=true
                if result.status==1 {
                    let notes=result.warning_count==0 ? "без предупреждений" : "предупреждений: \(result.warning_count)"
                    exportMessage="DAWproject готов: \(exportURL?.lastPathComponent ?? "проект") · \(notes)"
                    if result.warning_count>0 { storageMessage("DAWproject экспортирован. Внутри файла сохранён loss-report.json: предупреждений — \(result.warning_count), информационных заметок — \(result.info_count).") }
                } else if result.status==3 { exportMessage="Экспорт отменён · готовый файл не заменён" }
                else {
                    let error=withUnsafeBytes(of:result.error){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)}
                    exportMessage="Ошибка DAWproject: \(error)";storageMessage("Не удалось экспортировать DAWproject: \(error)")
                }
                exportMessageUntil=Date().addingTimeInterval(8);exportURL=nil
                updateMixExportAvailability();dawprojectButton.isEnabled = !isRecording
            }
        }
        if let job = saveJob {
            var result = daw_save_status(); result.struct_size = UInt32(MemoryLayout<daw_save_status>.size)
            if daw_poll_save(job, &result) == 0 && result.status != 0 {
                daw_release_save(job); saveJob = nil
                let completion = saveCompletion; saveCompletion = nil
                if result.status == 1 {
                    currentURL = saveURL; savedRevision = result.revision; saveError = nil
                    if !dirty { rotateRecovery() }
                    window.title = (currentURL?.deletingPathExtension().lastPathComponent ?? "Черновик") + " — My DAW"
                    window.isDocumentEdited = dirty
                    completion?()
                } else {
                    saveError = withUnsafeBytes(of: result.error) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
                    DAWLog.jobs.error("Сохранение проекта не удалось: \(self.saveError ?? "ошибка записи", privacy: .public)")
                    storageMessage("Не удалось сохранить: \(saveError ?? "ошибка записи"). Проект остаётся в памяти.")
                }
            }
        }
        if let job = recoveryJob {
            var result = daw_save_status(); result.struct_size = UInt32(MemoryLayout<daw_save_status>.size)
            if daw_poll_save(job, &result) == 0 && result.status != 0 {
                daw_release_save(job); recoveryJob = nil
                if let url = recoveryJobURL, retiredRecovery.remove(url) != nil {
                    try? FileManager.default.removeItem(at: url)
                } else if recoveryJobURL == recoveryURL {
                    if result.status == 1 { recoveredRevision = result.revision; recoveryError = nil }
                    else { recoveryError = "Не удалось обновить резервный черновик" }
                }
                recoveryJobURL = nil
            }
        }
        if !leavePromptActive && dirty && recoveredRevision != revision && recoveryJob == nil && Date() >= nextRecovery, let url = recoveryURL {
            nextRecovery = Date().addingTimeInterval(5)
            if let job = daw_begin_save(session, url.path) { recoveryJob = job; recoveryJobURL = url }
            else { recoveryError = "Не удалось запустить резервное сохранение" }
        }
        updateStorageStatus()
        // Reporting may enter a nested AppKit loop. Detach all old job state
        // first, so a retry started from that dialog cannot lose its URL/status.
        if let exportFailureToReport { mixExportInteraction.report(exportFailureToReport) }
    }
    func updateStorageStatus() {
        if importJob != nil, let result = importStatus {
            let percent = min(100, max(0, Int(result.progress)))
            status.stringValue = "Импорт WAV/AIFF: \(percent)% · \(importPhaseText(result.phase)) · можно продолжать работу"
            status.setAccessibilityLabel("Статус импорта WAV/AIFF")
            status.setAccessibilityValue(status.stringValue)
        } else if let importMessage, Date() < importMessageUntil {
            status.stringValue = importMessage
            status.setAccessibilityLabel("Статус импорта WAV/AIFF")
            status.setAccessibilityValue(status.stringValue)
        } else if let job = exportJob {
            var result = daw_export_status(); result.struct_size = UInt32(MemoryLayout<daw_export_status>.size)
            if daw_poll_export(job, &result) == 0 {
                let percent = result.total_frames == 0 ? 100 : Int((result.rendered_frames * 100) / result.total_frames)
                status.stringValue = Date().timeIntervalSince(exportStarted) > 10 ? "Экспорт WAV: \(percent)% · можно продолжать работу" : "Экспорт WAV: \(percent)%"
            }
        } else if let job=dawprojectJob {
            var result=daw_dawproject_status();result.struct_size=UInt32(MemoryLayout<daw_dawproject_status>.size)
            if daw_poll_dawproject_export(job,&result)==0 {
                let percent=result.total_entries==0 ? 100:Int((result.completed_entries*100)/result.total_entries)
                status.stringValue="Экспорт DAWproject: \(percent)% · можно продолжать работу"
            }
        } else if saveJob != nil {
            status.stringValue = Date().timeIntervalSince(saveStarted) > 10 ? "Запись занимает больше времени. Проект остаётся доступен для работы." : "Сохраняется снимок проекта… можно продолжать редактирование"
        } else if let error = saveError { status.stringValue = "Ошибка сохранения: \(error)" }
        else if let exportMessage, Date() < exportMessageUntil { status.stringValue = exportMessage }
        else if let projectMessage, Date() < projectMessageUntil {
            status.stringValue = projectMessage
            status.setAccessibilityLabel("Статус проекта")
            status.setAccessibilityValue(status.stringValue)
        }
        else if dirty {
            if let error = recoveryError { status.stringValue = "Есть несохранённые изменения · \(error)" }
            else if recoveredRevision == revision { status.stringValue = "Есть несохранённые изменения · резервный черновик обновлён" }
            else { status.stringValue = "Есть несохранённые изменения · резервная копия ожидается" }
        } else { status.stringValue = currentURL == nil ? "Создай первую дорожку, чтобы начать." : "Черновик сохранён на этом Mac" }
    }
    @objc func restoreDraft() { requestLeave { [weak self] in self?.offerRecovery() } }
    func offerRecovery() {
        if offerRecordingRecovery() { return }
        guard let root = recoveryRoot, let urls = try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: [.contentModificationDateKey]) else { return }
        let candidates = urls.filter { url in
            guard url.pathExtension == "mydawdraft", url != recoveryURL, let prefix = url.lastPathComponent.split(separator: "-").first, let pid = Int32(prefix) else { return false }
            return kill(pid, 0) != 0 && errno == ESRCH
        }.sorted { a, b in
            let da = (try? a.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            let db = (try? b.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            return da > db
        }
        guard let url = candidates.first else { return }
        let alert = NSAlert(); alert.messageText = "Найден резервный черновик"
        alert.informativeText = "Можно восстановить последнюю резервную копию как новый проект. Исходный файл не будет перезаписан. История Undo не восстанавливается."
        alert.addButton(withTitle: "Восстановить"); alert.addButton(withTitle: "Позже")
        guard alert.runModal() == .alertFirstButtonReturn, check(daw_open_draft(session, url.path)) else { return }
        mixExportDocumentID = UUID()
        midiDocumentID = UUID()
        rotateRecovery(); recoveredFrom = url; currentURL = nil; savedRevision = UInt64.max; saveError = nil;armedTrackID=nil;selectedTakes.removeAll(); refresh()
    }
    func offerRecordingRecovery() -> Bool {
        guard let root=recordingRoot,let urls=try? FileManager.default.contentsOfDirectory(at:root,includingPropertiesForKeys:[.contentModificationDateKey]) else{return false}
        let candidates=urls.filter { url in
            guard url.pathExtension=="mydawtake",let prefix=url.lastPathComponent.split(separator:"-").first,let pid=Int32(prefix) else{return false}
            return kill(pid,0) != 0 && errno == ESRCH
        }.sorted { a,b in
            let da=(try? a.resourceValues(forKeys:[.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            let db=(try? b.resourceValues(forKeys:[.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            return da>db
        }
        guard let url=candidates.first else{return false}
        let alert=NSAlert();alert.messageText="Найдена незавершённая запись";alert.informativeText="My DAW может восстановить подтверждённую часть дубля как новую дорожку. Последние доли секунды перед сбоем могли не сохраниться.";alert.addButton(withTitle:"Восстановить");alert.addButton(withTitle:"Позже")
        guard alert.runModal() == .alertFirstButtonReturn else{return true}
        if check(daw_recover_take(session,url.path,"Восстановленная запись",revision)) {
            currentURL=nil;savedRevision=UInt64.max;saveError=nil;refresh();return true
        }
        let unreadable=url.appendingPathExtension("unreadable");try? FileManager.default.moveItem(at:url,to:unreadable);return true
    }
}
