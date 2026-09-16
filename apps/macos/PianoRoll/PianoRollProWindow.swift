import AppKit

@MainActor
final class PRProWindowController: NSWindowController, NSWindowDelegate {
    let state: PRProState
    let workspace: PRProWorkspaceView
    var onClose: (() -> Void)?

    init(state: PRProState) {
        self.state = state
        workspace = PRProWorkspaceView(state: state)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 760),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = "Piano Roll — My DAW"
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
        if let owner, window?.parent == nil { owner.addChildWindow(window!, ordered: .above) }
        showWindow(nil)
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        workspace.refreshFromState()
        workspace.focusCanvas()
    }

    func detachFromParent() {
        if let window, let parent = window.parent { parent.removeChildWindow(window) }
    }

    func windowWillClose(_ notification: Notification) {
        detachFromParent()
        if state.isGesturing { state.cancelGesture() }
        onClose?()
    }
}
