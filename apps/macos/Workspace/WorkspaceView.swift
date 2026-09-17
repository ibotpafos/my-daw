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
    private(set) var dockFocused = false
    private let defaults: UserDefaults
    private(set) var preference: WorkspaceLayout
    var onChange: ((WorkspaceLayout) -> Void)?
    var geometry: WorkspaceLayout.Geometry {
        if dockFocused {
            return WorkspaceLayout.Geometry(library: 0, center: bounds.width, inspector: 0,
                arrangement: 0, dock: bounds.height, gap: 0)
        }
        var adapted = preference
        if adapted.dockTab == .mixer { adapted.dockHeight = max(adapted.dockHeight, 400) }
        return adapted.geometry(width: bounds.width, height: bounds.height, divider: columns.dividerThickness)
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
    func setDockFocus(_ focused: Bool) {
        guard dockFocused != focused else { return }
        dockFocused = focused
        applyGeometry(); onChange?(preference)
    }
    func toggle(_ pane: WorkspaceLayout.Pane) {
        dockFocused = false
        preference.toggle(pane); commitPreference()
    }
    func show(_ pane: WorkspaceLayout.Pane) {
        dockFocused = false
        switch pane { case .library: preference.libraryVisible = true; case .inspector: preference.inspectorVisible = true; case .dock: preference.dockVisible = true }
        commitPreference()
    }
    func selectDock(_ tab: WorkspaceDockTab) {
        if tab != .mixer { dockFocused = false }
        preference.dockTab = tab; preference.dockVisible = true; commitPreference()
    }
    func resetLayout() { dockFocused = false; preference = WorkspaceLayout(); commitPreference() }
    private func commitPreference() {
        preference.save(to: defaults); applyGeometry(); onChange?(preference)
    }
    private func layoutColumns(_ geometry: WorkspaceLayout.Geometry) {
        let height = columns.bounds.height
        libraryPane.frame = NSRect(x: 0, y: 0, width: geometry.library, height: height)
        let centerX = geometry.library + (geometry.library > 0 ? geometry.gap : 0)
        center.frame = NSRect(x: centerX, y: 0, width: geometry.center, height: height)
        inspectorPane.frame = NSRect(
            x: centerX + geometry.center + (geometry.inspector > 0 ? geometry.gap : 0),
            y: 0,
            width: geometry.inspector,
            height: height)
    }
    private func layoutCenter(_ geometry: WorkspaceLayout.Geometry) {
        arrangementPane.frame = NSRect(x: 0, y: 0, width: center.bounds.width, height: geometry.arrangement)
        dockPane.frame = NSRect(
            x: 0,
            y: geometry.arrangement + (geometry.dock > 0 ? geometry.gap : 0),
            width: center.bounds.width,
            height: geometry.dock)
    }
    func applyGeometry() {
        guard !applying, bounds.width > 0, bounds.height > 0 else { return }
        applying = true; defer { applying = false }
        let g = geometry
        libraryPane.isHidden = g.library == 0
        inspectorPane.isHidden = g.inspector == 0
        dockPane.isHidden = g.dock == 0
        arrangementPane.isHidden = g.arrangement == 0
        columns.frame = bounds
        layoutColumns(g)
        layoutCenter(g)
    }
    func splitView(_ splitView: NSSplitView, resizeSubviewsWithOldSize oldSize: NSSize) {
        guard bounds.width > 0, bounds.height > 0 else { return }
        let g = geometry
        if splitView === columns {
            layoutColumns(g)
        } else if splitView === center {
            layoutCenter(g)
        }
    }
    func splitView(_ splitView: NSSplitView, canCollapseSubview subview: NSView) -> Bool { false }
    func splitView(_ splitView: NSSplitView, shouldHideDividerAt dividerIndex: Int) -> Bool {
        if splitView === center { return dockPane.isHidden || arrangementPane.isHidden }
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
        guard !applying, !dockFocused, bounds.width > 0, let split = notification.object as? NSSplitView else { return }
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
