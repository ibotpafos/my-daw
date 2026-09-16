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

/// Native splitters own dragging; the bounded geometry also covers first layout,
/// resizing and corrupt preferences. Pane contents keep their identity throughout.
@MainActor
final class WorkspaceView: NSView, NSSplitViewDelegate {
    let columns: NSSplitView = WorkspaceSplit()
    let center: NSSplitView = WorkspaceSplit()
    private let libraryPane = NSView(), inspectorPane = NSView()
    private let arrangementPane = NSView(), dockPane = NSView()
    private var applying = false
    private let defaults: UserDefaults
    private(set) var preference: WorkspaceLayout
    var onChange: ((WorkspaceLayout) -> Void)?
    var geometry: WorkspaceLayout.Geometry {
        preference.geometry(width: bounds.width, height: bounds.height, divider: columns.dividerThickness)
    }
    override var isFlipped: Bool { true }

    init(library: NSView, arrangement: NSView, inspector: NSView, dock: NSView,
         defaults: UserDefaults = .standard) {
        self.defaults = defaults
        preference = WorkspaceLayout.load(from: defaults)
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
        mount(arrangement, in: arrangementPane); mount(dock, in: dockPane)
        center.addArrangedSubview(arrangementPane); center.addArrangedSubview(dockPane)
        columns.addArrangedSubview(libraryPane); columns.addArrangedSubview(center); columns.addArrangedSubview(inspectorPane)
        addSubview(columns)
        setAccessibilityLabel("Рабочее пространство: библиотека, аранжировка, инспектор, нижняя панель")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    override func layout() { super.layout(); applyGeometry() }
    func toggle(_ pane: WorkspaceLayout.Pane) {
        preference.toggle(pane); commitPreference()
    }
    func show(_ pane: WorkspaceLayout.Pane) {
        switch pane { case .library: preference.libraryVisible = true; case .inspector: preference.inspectorVisible = true; case .dock: preference.dockVisible = true }
        commitPreference()
    }
    func selectDock(_ tab: WorkspaceDockTab) {
        preference.dockTab = tab; preference.dockVisible = true; commitPreference()
    }
    func resetLayout() { preference = WorkspaceLayout(); commitPreference() }
    private func commitPreference() {
        preference.save(to: defaults); applyGeometry(); onChange?(preference)
    }
    func applyGeometry() {
        guard !applying, bounds.width > 0, bounds.height > 0 else { return }
        applying = true; defer { applying = false }
        columns.frame = bounds
        let g = geometry
        libraryPane.isHidden = g.library == 0; inspectorPane.isHidden = g.inspector == 0; dockPane.isHidden = g.dock == 0
        libraryPane.frame = NSRect(x: 0, y: 0, width: g.library, height: bounds.height)
        let centerX = g.library + (g.library > 0 ? g.gap : 0)
        center.frame = NSRect(x: centerX, y: 0, width: g.center, height: bounds.height)
        inspectorPane.frame = NSRect(x: centerX + g.center + (g.inspector > 0 ? g.gap : 0), y: 0,
                                     width: g.inspector, height: bounds.height)
        arrangementPane.frame = NSRect(x: 0, y: 0, width: g.center, height: g.arrangement)
        dockPane.frame = NSRect(x: 0, y: g.arrangement + (g.dock > 0 ? g.gap : 0), width: g.center, height: g.dock)
    }
    func splitView(_ splitView: NSSplitView, resizeSubviewsWithOldSize oldSize: NSSize) { applyGeometry() }
    func splitView(_ splitView: NSSplitView, canCollapseSubview subview: NSView) -> Bool { false }
    func splitView(_ splitView: NSSplitView, shouldHideDividerAt dividerIndex: Int) -> Bool {
        if splitView === center { return dockPane.isHidden }
        return dividerIndex == 0 ? libraryPane.isHidden : inspectorPane.isHidden
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
        guard !applying, bounds.width > 0, let split = notification.object as? NSSplitView else { return }
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
    let closeButton = NSButton(image: NSImage(systemSymbolName: "xmark", accessibilityDescription: "Скрыть нижнюю панель")!, target: nil, action: nil)
    private let content = NSView()
    private let views: [WorkspaceDockTab: NSView]
    private(set) var selected: WorkspaceDockTab = .devices
    var onSelect: ((WorkspaceDockTab) -> Void)?
    var onClose: (() -> Void)?
    override var isFlipped: Bool { true }
    init(devices: NSView, midi: NSView, mixer: NSView) {
        views = [.devices: devices, .midi: midi, .mixer: mixer]
        super.init(frame: .zero)
        wantsLayer = true; layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        tabs.target = self; tabs.action = #selector(changed); tabs.controlSize = .small
        tabs.setAccessibilityLabel("Содержимое нижней панели")
        closeButton.target = self; closeButton.action = #selector(close); closeButton.bezelStyle = .inline
        [tabs, closeButton, content].forEach(addSubview)
        for view in views.values {
            view.translatesAutoresizingMaskIntoConstraints = true; view.autoresizingMask = [.width, .height]
            content.addSubview(view)
        }
        select(.devices)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func select(_ tab: WorkspaceDockTab) {
        selected = tab; tabs.selectedSegment = tab.rawValue
        for (key, view) in views { view.isHidden = key != tab }
        needsLayout = true
    }
    override func layout() {
        super.layout()
        tabs.frame = NSRect(x: 8, y: 6, width: min(330, max(0, bounds.width - 50)), height: 26)
        closeButton.frame = NSRect(x: bounds.width - 32, y: 6, width: 24, height: 26)
        content.frame = NSRect(x: 0, y: 38, width: bounds.width, height: max(0, bounds.height - 38))
        for view in views.values { view.frame = content.bounds }
    }
    @objc private func changed() {
        guard let tab = WorkspaceDockTab(rawValue: tabs.selectedSegment) else { return }
        select(tab); onSelect?(tab)
    }
    @objc private func close() { onClose?() }
}
