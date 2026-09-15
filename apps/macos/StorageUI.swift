import AppKit
import Darwin

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
    @objc func exportMix() {
        guard !exportBusy else { storageMessage("Экспорт уже выполняется."); return }
        if isRecording { finishRecording(); guard !isRecording else { return } }
        finishEditing()
        guard hasAudio else { storageMessage("Добавь или запиши аудио перед экспортом."); return }
        let choice = NSAlert(); choice.messageText = "Формат WAV"
        let scope = rangeEnd != nil ? rangeLabel.stringValue : "весь проект"
        choice.informativeText = "Область: \(scope). 24-bit подходит для сведения и обмена. Float32 сохраняет результат рендера без целочисленного квантования."
        choice.addButton(withTitle: "WAV 24-bit"); choice.addButton(withTitle: "WAV float32"); choice.addButton(withTitle: "Отмена")
        let response = choice.runModal()
        guard response != .alertThirdButtonReturn else { return }
        let format: Int32 = response == .alertSecondButtonReturn ? 2 : 1
        let panel = NSSavePanel(); panel.allowedContentTypes = [.wav]
        if let currentURL { panel.directoryURL = currentURL.deletingLastPathComponent() }
        panel.nameFieldStringValue = "\(currentURL?.deletingPathExtension().lastPathComponent ?? "Микс").wav"
        panel.message = rangeEnd != nil ? "Экспортируется выбранный диапазон из зафиксированного снимка. Можно продолжать редактирование." : "Экспортируется весь проект из зафиксированного снимка. Можно продолжать редактирование."
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let job: OpaquePointer?
        if let start=rangeStart,let end=rangeEnd { job=daw_begin_export_range(session,url.path,format,start,end) }
        else { job=daw_begin_export(session,url.path,format) }
        guard let job else { _ = check(1); return }
        exportJob = job; exportURL = url; exportStarted = Date(); exportMessage = nil; exportMessageUntil = .distantPast
        exportButton.isEnabled = false; cancelExportButton.isEnabled = true; recordButton.isEnabled = false
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
        guard let job = daw_begin_dawproject_export(session,url.path,Double(tempo),4,4,title) else { _ = check(1); return }
        dawprojectJob=job;exportURL=url;exportStarted=Date();exportMessage=nil;exportMessageUntil = .distantPast
        exportButton.isEnabled=false;dawprojectButton.isEnabled=false;cancelExportButton.isEnabled=true;recordButton.isEnabled=false
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
            self.rotateRecovery(); daw_destroy(self.session); self.session = fresh
            self.currentURL = nil; self.savedRevision = 0; self.saveError = nil; self.rangeStart=nil;self.rangeEnd=nil;self.loopEnabled=false;self.armedTrackID=nil;self.selectedTakes.removeAll();self.refresh();self.updateTimelineTools()
        }
    }
    @objc func openDraft() {
        requestLeave { [weak self] in
            guard let self else { return }
            let panel = NSOpenPanel(); panel.allowedContentTypes = [self.draftType, .data]
            panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
            guard panel.runModal() == .OK, let url = panel.url else { return }
            guard self.check(daw_open_draft(self.session, url.path)) else { return }
            var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
            guard self.check(daw_get_snapshot(self.session, &snapshot)) else { return }
            self.rotateRecovery(); self.currentURL = url; self.savedRevision = snapshot.revision; self.saveError = nil; self.rangeStart=nil;self.rangeEnd=nil;self.loopEnabled=false;self.armedTrackID=nil;self.selectedTakes.removeAll();self.refresh();self.updateTimelineTools()
        }
    }
    func pollStorage() {
        if let job = exportJob {
            var result = daw_export_status(); result.struct_size = UInt32(MemoryLayout<daw_export_status>.size)
            if daw_poll_export(job, &result) == 0 && result.status != 0 {
                daw_release_export(job); exportJob = nil
                cancelExportButton.isEnabled = false; recordButton.isEnabled = true
                if result.status == 1 {
                    exportMessage = "Экспорт готов: \(exportURL?.lastPathComponent ?? "WAV") · снимок ревизии \(result.revision)"
                } else if result.status == 3 {
                    exportMessage = "Экспорт отменён · готовый файл не заменён"
                } else {
                    let error = withUnsafeBytes(of: result.error) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
                    exportMessage = "Ошибка экспорта: \(error)"
                    storageMessage("Не удалось экспортировать WAV: \(error)")
                }
                exportMessageUntil = Date().addingTimeInterval(6)
                exportURL = nil; exportButton.isEnabled = hasAudio && !isRecording
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
                exportButton.isEnabled=hasAudio && !isRecording;dawprojectButton.isEnabled = !isRecording
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
    }
    func updateStorageStatus() {
        if let job = exportJob {
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
