import AppKit

/// One reusable non-modal window. Providers read UI snapshots on the main thread;
/// no audio-thread objects, project copies or second mutation history live here.
@MainActor
final class MixerRoutingPresenter: NSObject, NSWindowDelegate {
    static let shared = MixerRoutingPresenter()
    private var controller: NSWindowController?
    private var matrix: MixerRoutingMatrixView?
    private var stripsProvider: (() -> [MixerStripModel])?
    private var editingProvider: (() -> Bool)?
    private var refreshTimer: Timer?
    var presentedWindow: NSWindow? { controller?.window }
    var isPolling: Bool { refreshTimer?.isValid == true }

    func present(strips: @escaping () -> [MixerStripModel],
                 editingAllowed: @escaping () -> Bool = { true },
                 onOutput: @escaping (UInt64, UInt64) -> Void,
                 onSend: @escaping (UInt64, MixerSendAction) -> Void) {
        let matrix: MixerRoutingMatrixView
        let window: NSWindow
        if let existing = self.matrix, let existingWindow = controller?.window {
            matrix = existing; window = existingWindow
        } else {
            matrix = MixerRoutingMatrixView(frame: NSRect(x: 0, y: 0, width: 900, height: 540))
            window = NSWindow(contentRect: matrix.frame, styleMask: [.titled, .closable, .resizable, .miniaturizable], backing: .buffered, defer: false)
            window.title = "My DAW · Routing Matrix"
            window.minSize = NSSize(width: 560, height: 300)
            window.contentView = matrix; window.isReleasedWhenClosed = false; window.delegate = self
            window.center()
            _ = window.setFrameAutosaveName("MyDAWMixerRoutingMatrix")
            controller = NSWindowController(window: window); self.matrix = matrix
        }
        stripsProvider = strips; editingProvider = editingAllowed
        matrix.editingAllowed = editingAllowed
        matrix.onWillInteract = { [weak self] in self?.reload() }
        matrix.onOutput = { [weak self] id, bus in onOutput(id, bus); self?.reload() }
        matrix.onSend = { [weak self] id, action in onSend(id, action); self?.reload() }
        reload()
        refreshTimer?.invalidate()
        let timer = Timer(timeInterval: 0.25, target: self, selector: #selector(poll), userInfo: nil, repeats: true)
        refreshTimer = timer
        RunLoop.main.add(timer, forMode: .common)
        controller?.showWindow(nil)
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
    }

    @objc private func poll() {
        guard let window = controller?.window, window.isVisible, !window.isMiniaturized else { return }
        reload()
    }
    func reload() {
        guard let matrix, let stripsProvider else { return }
        // Equal snapshots do not rebuild the view-based tables.
        matrix.editingEnabled = editingProvider?() ?? false
        matrix.strips = stripsProvider()
    }
    func close() {
        refreshTimer?.invalidate(); refreshTimer = nil
        controller?.window?.delegate = nil
        controller?.close()
        controller = nil; matrix = nil; stripsProvider = nil; editingProvider = nil
    }
    func windowDidBecomeKey(_ notification: Notification) { reload() }
    func windowWillClose(_ notification: Notification) {
        refreshTimer?.invalidate(); refreshTimer = nil
        controller = nil; matrix = nil; stripsProvider = nil; editingProvider = nil
    }
}
