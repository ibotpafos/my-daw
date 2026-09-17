import AppKit

@MainActor
final class PRProWindowController: NSWindowController, NSWindowDelegate {
    let state: PRProState
    let workspace: PRProWorkspaceView
    var onClose: (() -> Void)?
    private var playheadTimer: Timer?
    private var fittedInitialContent = false

    init(state: PRProState) {
        self.state = state
        workspace = PRProWorkspaceView(state: state)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 760),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = "Piano Roll — My DAW"
        window.appearance = NSAppearance(named: .darkAqua)
        window.minSize = NSSize(width: 980, height: 560)
        window.isReleasedWhenClosed = false
        window.contentView = workspace
        window.setFrameAutosaveName("MyDAW.PianoRoll.v1")
        super.init(window: window)
        window.delegate = self
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    func present(relativeTo owner: NSWindow?, title: String?) {
        if let title, !title.isEmpty { window?.title = "Piano Roll — \(title)" }
        if let owner, let window, window.parent == nil {
            owner.addChildWindow(window, ordered: .above)
        }
        showWindow(nil)
        window?.makeKeyAndOrderFront(nil)
        prepareForPresentation()
        startPlayheadPolling()
    }

    /// Initial framing belongs to presentation, after the real viewport has
    /// been laid out. A new editor must not start at MIDI pitch 127.
    func prepareForPresentation() {
        window?.layoutIfNeeded()
        workspace.layoutSubtreeIfNeeded()
        workspace.refreshFromState()
        if !fittedInitialContent {
            workspace.fit(selectionOnly: false)
            workspace.scrollInspectorToTop()
            fittedInitialContent = true
        }
        workspace.focusCanvas()
    }

    private func startPlayheadPolling() {
        playheadTimer?.invalidate()
        let timer = Timer(timeInterval: 1.0 / 30.0,
                          target: self,
                          selector: #selector(pollProjectPlayhead),
                          userInfo: nil,
                          repeats: true)
        playheadTimer = timer
        RunLoop.main.add(timer, forMode: .common)
        pollProjectPlayhead()
    }

    @objc private func pollProjectPlayhead() {
        guard let app = NSApp.delegate as? DraftApp else { return }
        workspace.updatePlayhead(app.playheadFrame)
    }

    func detachFromParent() {
        if let window, let parent = window.parent { parent.removeChildWindow(window) }
    }

    func windowWillClose(_ notification: Notification) {
        playheadTimer?.invalidate()
        playheadTimer = nil
        detachFromParent()
        if state.isGesturing { state.cancelGesture() }
        onClose?()
    }
}
