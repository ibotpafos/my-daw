import AppKit

@MainActor
final class WorkspaceSurface: NSView {
    override var isFlipped: Bool { true }
    init(_ color: NSColor = DAWDesignTokens.Color.surface) {
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = color.cgColor
        layer?.borderColor = DAWDesignTokens.Color.border.cgColor
        layer?.borderWidth = 1
        layer?.cornerRadius = 5
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
}

@MainActor
private final class WorkspaceSplit: NSSplitView {
    override var isFlipped: Bool { true }
    override var dividerColor: NSColor { DAWDesignTokens.Color.border }
}

@MainActor
private final class WorkspaceScrollBookmark {
    weak var scroll: NSScrollView?
    let origin: NSPoint
    init(_ scroll: NSScrollView) { self.scroll = scroll; origin = scroll.contentView.bounds.origin }
    func restore() {
        guard let scroll, !scroll.isHiddenOrHasHiddenAncestor, scroll.documentView != nil else { return }
        let clip = scroll.contentView
        clip.scroll(to: clip.constrainBoundsRect(NSRect(origin: origin, size: clip.bounds.size)).origin)
        scroll.reflectScrolledClipView(clip)
    }
}

@MainActor
private final class WorkspaceFocusBookmark {
    weak var view: NSView?
    init(_ view: NSView) { self.view = view }
}

/// Native splitters own dragging. Dedicated screens reuse the exact mounted
/// views; switching neither reparents editors nor reconstructs the audio graph.
@MainActor
final class WorkspaceView: NSView, NSSplitViewDelegate {
    let overview = ArrangementOverviewView(frame: .zero)
    let columns: NSSplitView = WorkspaceSplit()
    let center: NSSplitView = WorkspaceSplit()
    private let libraryPane = NSView(), inspectorPane = NSView()
    private let arrangementPane = NSView(), dockPane = NSView()
    private var applying = false
    private let defaults: UserDefaults
    private let recordingContent: NSView, dockContent: NSView
    private var scrollBookmarks: [WorkspaceScreen: [WorkspaceScrollBookmark]] = [:]
    private var focusBookmarks: [WorkspaceScreen: WorkspaceFocusBookmark] = [:]
    private(set) var preference: WorkspaceLayout
    private(set) var screen: WorkspaceScreen
    var dockFocused: Bool { screen.dockTab != nil }
    var activeDockTab: WorkspaceDockTab { screen.dockTab ?? preference.dockTab }
    var onChange: ((WorkspaceLayout) -> Void)?
    /// Allows the application to reject navigation during an unfinished gesture
    /// or invalid field edit instead of hiding its active control.
    var mayNavigate: (() -> Bool)?
    var onFocusRequested: ((WorkspaceScreen) -> Void)?
    var geometry: WorkspaceLayout.Geometry {
        screen.geometry(preference: preference, width: bounds.width, height: bounds.height,
                        divider: columns.dividerThickness)
    }
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    init(library: NSView, arrangement: NSView, inspector: NSView, dock: NSView,
         recording: NSView = NSView(), defaults: UserDefaults = .standard) {
        self.defaults = defaults
        recordingContent = recording; dockContent = dock
        preference = WorkspaceLayout.load(from: defaults)
        screen = WorkspaceScreen.load(from: defaults)
        super.init(frame: .zero)
        columns.isVertical = true; center.isVertical = false
        for split in [columns, center] { split.dividerStyle = .thin; split.delegate = self }
        func mount(_ child: NSView, in parent: NSView) {
            child.translatesAutoresizingMaskIntoConstraints = true
            child.frame = parent.bounds
            child.autoresizingMask = [.width, .height]
            parent.addSubview(child)
        }
        mount(library, in: libraryPane); mount(inspector, in: inspectorPane)
        mount(ArrangementOverviewContainer(arrangement: arrangement, overview: overview), in: arrangementPane)
        mount(dock, in: dockPane); mount(recording, in: dockPane)
        center.addArrangedSubview(arrangementPane); center.addArrangedSubview(dockPane)
        columns.addArrangedSubview(libraryPane); columns.addArrangedSubview(center); columns.addArrangedSubview(inspectorPane)
        addSubview(columns)
        setAccessibilityLabel("Рабочее пространство DAW")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    override func layout() { super.layout(); applyGeometry() }

    private func canNavigate() -> Bool {
        guard mayNavigate?() ?? true else {
            // A segmented control changes its state before dispatching an action.
            // Re-project the accepted selection when the action is rejected.
            onChange?(preference)
            return false
        }
        return true
    }
    private func capturePresentation() {
        func collect(_ view: NSView) -> [WorkspaceScrollBookmark] {
            guard !view.isHiddenOrHasHiddenAncestor else { return [] }
            let own = (view as? NSScrollView).map { [WorkspaceScrollBookmark($0)] } ?? []
            return own + view.subviews.flatMap(collect)
        }
        scrollBookmarks[screen] = collect(self)
        var focused = window?.firstResponder as? NSView
        if let editor = focused as? NSTextView, editor.isFieldEditor {
            focused = editor.delegate as? NSView
        }
        if let focused, focused.isDescendant(of: self), !focused.isHiddenOrHasHiddenAncestor {
            focusBookmarks[screen] = WorkspaceFocusBookmark(focused)
        }
    }
    private func restorePresentation() {
        window?.contentView?.layoutSubtreeIfNeeded()
        for bookmark in scrollBookmarks[screen] ?? [] { bookmark.restore() }
        guard let window else { return }
        if let view = focusBookmarks[screen]?.view, view.window === window,
           !view.isHiddenOrHasHiddenAncestor, window.makeFirstResponder(view) { return }
        // Never leave Delete/typing routed to a hidden editor.
        _ = window.makeFirstResponder(self)
        onFocusRequested?(screen)
    }
    @discardableResult
    func present(_ destination: WorkspaceScreen) -> Bool {
        guard destination != screen else { onChange?(preference); return true }
        capturePresentation()
        guard canNavigate() else { return false }
        screen = destination
        screen.save(to: defaults)
        applyGeometry(); onChange?(preference)
        restorePresentation()
        NSAccessibility.post(element: self, notification: .layoutChanged)
        return true
    }
    func setDockFocus(_ focused: Bool) {
        present(focused ? WorkspaceScreen(dockTab: activeDockTab) : .arrange)
    }
    func toggle(_ pane: WorkspaceLayout.Pane) {
        if screen != .arrange {
            guard present(.arrange) else { return }
            // Closing a dedicated editor restores the previous arrangement,
            // rather than also closing the user's previously visible dock.
            if pane != .dock { show(pane) }
            return
        }
        guard canNavigate() else { return }
        preference.toggle(pane); commitPreference()
    }
    func show(_ pane: WorkspaceLayout.Pane) {
        guard screen == .arrange || present(.arrange) else { return }
        switch pane {
        case .library: preference.libraryVisible = true
        case .inspector: preference.inspectorVisible = true
        case .dock: preference.dockVisible = true
        }
        commitPreference()
    }
    func selectDock(_ tab: WorkspaceDockTab) {
        // Clicking the active focused tab is a no-op. Choosing another dock tab
        // retains the established behaviour: return to the arrangement + dock.
        if screen.dockTab == tab { onChange?(preference); return }
        if screen == .arrange {
            guard canNavigate() else { return }
        } else if !present(.arrange) { return }
        preference.dockTab = tab; preference.dockVisible = true; commitPreference()
    }
    func resetLayout() {
        if screen == .arrange {
            guard canNavigate() else { return }
        } else if !present(.arrange) { return }
        preference = WorkspaceLayout()
        screen.save(to: defaults)
        scrollBookmarks.removeAll(); focusBookmarks.removeAll()
        commitPreference()
    }
    override func keyDown(with event: NSEvent) {
        let modifiers = event.modifierFlags.intersection([.command, .control, .option, .shift])
        if event.keyCode == 53, modifiers.isEmpty, screen != .arrange {
            present(.arrange)
        } else { super.keyDown(with: event) }
    }
    private func commitPreference() {
        preference.save(to: defaults); applyGeometry(); onChange?(preference)
    }
    private func layoutColumns(_ geometry: WorkspaceLayout.Geometry) {
        let height = columns.bounds.height
        // Hidden panes retain their ordinary dimensions. Otherwise AppKit would
        // clamp their scroll origins and rewrite nested splitter preferences.
        if screen == .arrange || geometry.library > 0 {
            libraryPane.frame = NSRect(x: 0, y: 0, width: geometry.library, height: height)
        }
        let centerX = geometry.library + (geometry.library > 0 ? geometry.gap : 0)
        if geometry.center > 0 {
            center.frame = NSRect(x: centerX, y: 0, width: geometry.center, height: height)
        }
        if screen == .arrange {
            inspectorPane.frame = NSRect(
                x: centerX + geometry.center + (geometry.inspector > 0 ? geometry.gap : 0),
                y: 0, width: geometry.inspector, height: height)
        }
    }
    private func layoutCenter(_ geometry: WorkspaceLayout.Geometry) {
        if screen == .arrange {
            arrangementPane.frame = NSRect(x: 0, y: 0, width: center.bounds.width, height: geometry.arrangement)
        }
        if geometry.dock > 0 {
            dockPane.frame = NSRect(
                x: 0, y: geometry.arrangement + (geometry.arrangement > 0 ? geometry.gap : 0),
                width: center.bounds.width, height: geometry.dock)
        }
    }
    func applyGeometry() {
        guard !applying, bounds.width > 0, bounds.height > 0 else { return }
        applying = true; defer { applying = false }
        let g = geometry
        dockContent.isHidden = screen == .recording
        recordingContent.isHidden = screen != .recording
        libraryPane.isHidden = g.library == 0
        inspectorPane.isHidden = g.inspector == 0
        center.isHidden = g.center == 0
        dockPane.isHidden = g.dock == 0
        arrangementPane.isHidden = g.arrangement == 0
        columns.frame = bounds
        layoutColumns(g); layoutCenter(g)
    }
    func splitView(_ splitView: NSSplitView, resizeSubviewsWithOldSize oldSize: NSSize) {
        guard bounds.width > 0, bounds.height > 0 else { return }
        if splitView === columns { layoutColumns(geometry) }
        else if splitView === center { layoutCenter(geometry) }
    }
    func splitView(_ splitView: NSSplitView, canCollapseSubview subview: NSView) -> Bool { false }
    func splitView(_ splitView: NSSplitView, shouldHideDividerAt dividerIndex: Int) -> Bool {
        guard dividerIndex >= 0, dividerIndex + 1 < splitView.subviews.count else { return true }
        return splitView.subviews[dividerIndex].isHidden || splitView.subviews[dividerIndex + 1].isHidden
    }
    func splitView(_ splitView: NSSplitView, constrainMinCoordinate proposed: CGFloat, ofSubviewAt index: Int) -> CGFloat {
        if splitView === center { return 220 }
        return index == 0 ? 200 : center.frame.minX + 520
    }
    func splitView(_ splitView: NSSplitView, constrainMaxCoordinate proposed: CGFloat, ofSubviewAt index: Int) -> CGFloat {
        if splitView === center { return max(220, splitView.bounds.height - 250) }
        if index == 0 { return min(340, inspectorPane.frame.minX - 520 - splitView.dividerThickness) }
        return max(center.frame.minX + 520, splitView.bounds.width - 240)
    }
    func splitViewDidResizeSubviews(_ notification: Notification) {
        guard !applying, screen == .arrange, bounds.width > 0,
              let split = notification.object as? NSSplitView else { return }
        if split === center, !dockPane.isHidden { preference.dockHeight = dockPane.frame.height }
        if split === columns {
            if !libraryPane.isHidden { preference.libraryWidth = libraryPane.frame.width }
            if !inspectorPane.isHidden { preference.inspectorWidth = inspectorPane.frame.width }
        }
        preference.save(to: defaults)
    }
}

@MainActor
final class WorkspaceDockView: NSView {
    let tabs = NSSegmentedControl(labels: WorkspaceDockTab.allCases.map(\.title), trackingMode: .selectOne, target: nil, action: nil)
    let focusButton = NSButton(title: "Развернуть", target: nil, action: nil)
    let closeButton = NSButton(image: NSImage(systemSymbolName: "xmark", accessibilityDescription: "Скрыть нижнюю панель")!, target: nil, action: nil)
    private let content = NSView()
    private let views: [WorkspaceDockTab: NSView]
    private(set) var selected: WorkspaceDockTab = .devices
    private(set) var expanded = false
    var onSelect: ((WorkspaceDockTab) -> Void)?
    var onClose: (() -> Void)?
    var onToggleFocus: (() -> Void)?
    override var isFlipped: Bool { true }
    init(devices: NSView, midi: NSView, mixer: NSView) {
        views = [.devices: devices, .midi: midi, .mixer: mixer]
        super.init(frame: .zero)
        wantsLayer = true; layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        tabs.target = self; tabs.action = #selector(changed); tabs.controlSize = .small
        tabs.setAccessibilityLabel("Содержимое нижней панели")
        closeButton.target = self; closeButton.action = #selector(close); closeButton.bezelStyle = .inline
        focusButton.target = self; focusButton.action = #selector(toggleFocus)
        focusButton.bezelStyle = .texturedRounded
        focusButton.font = .systemFont(ofSize: 11, weight: .medium)
        [tabs, focusButton, closeButton, content].forEach(addSubview)
        for view in views.values {
            view.translatesAutoresizingMaskIntoConstraints = true; view.autoresizingMask = [.width, .height]
            content.addSubview(view)
        }
        select(.devices); setExpanded(false)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func select(_ tab: WorkspaceDockTab) {
        selected = tab; tabs.selectedSegment = tab.rawValue
        for (key, view) in views { view.isHidden = key != tab }
        needsLayout = true
    }
    func setExpanded(_ expanded: Bool) {
        self.expanded = expanded
        focusButton.title = expanded ? "К проекту" : "Развернуть"
        focusButton.image = NSImage(systemSymbolName: expanded ? "arrow.down.right.and.arrow.up.left" : "arrow.up.left.and.arrow.down.right", accessibilityDescription: nil)
        focusButton.imagePosition = .imageLeading
        let help = expanded ? "Вернуться к аранжировке, сохранив её компоновку" : "Открыть этот редактор на всю рабочую область"
        focusButton.toolTip = help; focusButton.setAccessibilityLabel(help)
        closeButton.toolTip = expanded ? "Вернуться к проекту" : "Скрыть нижнюю панель"
        closeButton.setAccessibilityLabel(closeButton.toolTip)
    }
    override func layout() {
        super.layout()
        tabs.frame = NSRect(x: 8, y: 6, width: min(330, max(0, bounds.width - 180)), height: 28)
        focusButton.frame = NSRect(x: max(8, bounds.width - 164), y: 6, width: 124, height: 28)
        closeButton.frame = NSRect(x: max(0, bounds.width - 32), y: 6, width: 24, height: 28)
        content.frame = NSRect(x: 0, y: 40, width: bounds.width, height: max(0, bounds.height - 40))
        for view in views.values { view.frame = content.bounds }
    }
    @objc private func changed() {
        guard let tab = WorkspaceDockTab(rawValue: tabs.selectedSegment) else { return }
        if let onSelect { onSelect(tab) } else { select(tab) }
    }
    @objc private func close() { onClose?() }
    @objc private func toggleFocus() { onToggleFocus?() }
}
