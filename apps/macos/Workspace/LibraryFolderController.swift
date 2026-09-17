import AppKit

@MainActor
final class LibraryFolderController {
    struct State: Equatable {
        var title = "Папка не выбрана"
        var message = "WAV / AIFF · включая вложенные папки"
        var busy = false
        var warning = false
        var hasFolder = false
    }
    fileprivate struct Result: Sendable {
        let access: LibraryFolderAccess
        let scan: LibraryFolderScan
    }
    fileprivate enum Request: Sendable {
        case picked(URL), remembered(LibraryFolderBookmark), active(LibraryFolderAccess)
        func open() throws -> LibraryFolderAccess {
            switch self {
            case .picked(let url): return try LibraryFolderAccess(picked: url)
            case .remembered(let value): return try LibraryFolderAccess(remembered: value)
            case .active(let access): return access
            }
        }
    }
    private let scanner = LibraryFolderWorker()
    private let defaults: UserDefaults
    private var worker: Task<Result, Error>?
    private var generation = UUID()
    private var pending: Result?
    private var access: LibraryFolderAccess?
    private var panel: NSOpenPanel?
    private(set) var state = State() { didSet { onChange?(state) } }
    var onChange: ((State) -> Void)?
    var onPublish: (([URL]) -> Void)?
    var isPublishingAllowed = true { didSet { if isPublishingAllowed != oldValue { publishPending() } } }
    var isConfigured: Bool { onPublish != nil }
    var activeURL: URL? { access?.url }
    var waitingToPublish: Bool { pending != nil }

    init(defaults: UserDefaults) { self.defaults = defaults }
    deinit { worker?.cancel() }

    func choose(in window: NSWindow?) {
        guard isConfigured, isPublishingAllowed, panel == nil, let window else { return }
        let picker = NSOpenPanel()
        picker.canChooseDirectories = true; picker.canChooseFiles = false; picker.allowsMultipleSelection = false
        picker.prompt = "Открыть"
        picker.message = "Читать WAV/AIFF только в выбранной папке и её подпапках. Папка будет запомнена локально; содержимое проекта не меняется."
        panel = picker
        picker.beginSheetModal(for: window) { [weak self, weak picker] response in
            guard let self else { return }
            let selectedURL = picker?.url
            self.panel = nil
            guard response == .OK, self.isPublishingAllowed, let url = selectedURL else { return }
            self.open(url)
        }
    }
    func open(_ url: URL) {
        guard isConfigured, isPublishingAllowed, url.isFileURL else { return }
        start(.picked(url), title: url.lastPathComponent)
    }
    func restore() {
        guard isConfigured, worker == nil, access == nil else { return }
        guard let bytes = defaults.data(forKey: LibraryFolderBookmark.key) else { return }
        guard let bookmark = LibraryFolderBookmark.decode(bytes) else {
            state.message = "Сохранённая папка повреждена. Выберите её заново."; state.warning = true; state.hasFolder = true
            return
        }
        start(.remembered(bookmark), title: bookmark.title)
    }
    func rescan() {
        guard isConfigured, isPublishingAllowed, !state.busy else { return }
        if let access { start(.active(access), title: access.url.lastPathComponent) } else { restore() }
    }
    func cancel() {
        generation = UUID(); worker?.cancel(); worker = nil; pending = nil
        state.busy = false; state.title = access?.url.lastPathComponent ?? "Папка не выбрана"
        state.message = "Сканирование отменено. Каталог не изменён."; state.warning = false
    }
    func forget() {
        guard isConfigured, isPublishingAllowed else { return }
        cancel()
        // Stop audition / publish empty command targets before dropping the lease.
        onPublish?([]); access = nil
        defaults.removeObject(forKey: LibraryFolderBookmark.key)
        state = State()
    }
    private func start(_ request: Request, title: String) {
        worker?.cancel(); pending = nil
        let token = UUID(); generation = token
        state = State(title: title, message: "Сканирование…", busy: true, hasFolder: true)
        let scanner = self.scanner
        // Capture the weak UI owner only on its actor. The filesystem worker
        // passes immutable values to this Sendable hop, never a shared weak var.
        let reportProgress: @MainActor @Sendable (LibraryFolderScan.Progress) -> Void = { [weak self] progress in
            guard let self, self.generation == token, self.worker != nil else { return }
            self.state.message = "Найдено \(progress.found) · просмотрено \(progress.visited)"
        }
        let task = Task.detached(priority: .utility) {
            try await scanner.run(request) { progress in
                Task { await reportProgress(progress) }
            }
        }
        worker = task
        Task { [weak self] in
            do {
                let result = try await task.value
                guard let self, self.generation == token else { return }
                self.worker = nil; self.pending = result
                self.state.message = "Каталог готов · ожидается завершение записи / импорта"
                self.publishPending()
            } catch {
                guard let self, self.generation == token else { return }
                self.worker = nil; self.state.busy = false; self.state.warning = true
                self.state.title = self.access?.url.lastPathComponent ?? title
                self.state.message = "Папка недоступна: \(error.localizedDescription) Выберите её заново."
                DAWLog.lifecycle.error("Library folder scan failed; previous catalog retained")
            }
        }
    }
    private func publishPending() {
        guard isPublishingAllowed, let result = pending, let onPublish else { return }
        pending = nil
        // Keep the previous scope alive through the stop-preview callback.
        let previous = access
        access = result.access
        onPublish(result.scan.urls)
        withExtendedLifetime(previous) {}
        if let data = result.access.bookmark.encoded { defaults.set(data, forKey: LibraryFolderBookmark.key) }
        let partial = result.scan.limitedBy != nil
        let unreadable = result.scan.unreadable
        var message = "\(result.scan.urls.count) файлов · папка запомнена"
        if partial { message += " · достигнут лимит сканирования" }
        if unreadable > 0 { message += " · недоступно: \(unreadable)" }
        state = State(title: result.access.url.lastPathComponent, message: message, warning: partial || unreadable > 0, hasFolder: true)
    }
}

/// Serial isolation bounds outstanding filesystem work even when a slow
/// filesystem cannot interrupt its current metadata call on cancellation.
private actor LibraryFolderWorker {
    func run(_ request: LibraryFolderController.Request,
             progress: @Sendable (LibraryFolderScan.Progress) -> Void) throws -> LibraryFolderController.Result {
        try Task.checkCancellation()
        let access = try request.open()
        let scan = try LibraryFolderScanner.scan(access.url, progress: progress)
        try Task.checkCancellation()
        return .init(access: access, scan: scan)
    }
}
