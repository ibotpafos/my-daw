import AppKit

/// Native menu projects existing controls rather than introducing another
/// command model. Availability is re-read when the menu opens AND on dispatch.
@MainActor
final class WorkspaceCommandMenu: NSPopUpButton, NSMenuDelegate {
    let sourceButtons: [NSButton]
    private let heading: String

    init(_ title: String, sources: [NSButton]) {
        heading = title
        sourceButtons = sources
        super.init(frame: .zero, pullsDown: true)
        bezelStyle = .texturedRounded
        font = .systemFont(ofSize: 11, weight: .medium)
        setAccessibilityLabel(title)
        let menu = NSMenu(title: title)
        menu.autoenablesItems = false
        menu.delegate = self
        self.menu = menu
        menuNeedsUpdate(menu)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let header = NSMenuItem(title: heading, action: nil, keyEquivalent: "")
        header.isEnabled = false
        menu.addItem(header)
        for source in sourceButtons where !source.isHidden {
            let label = source.toolTip ?? (source.title.isEmpty ? "Команда" : source.title)
            let item = NSMenuItem(title: label, action: #selector(dispatch(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = source
            item.isEnabled = source.isEnabled
            item.image = source.image
            item.state = source.state
            menu.addItem(item)
        }
    }

    @objc func dispatch(_ item: NSMenuItem) {
        guard let source = item.representedObject as? NSButton,
              sourceButtons.contains(where: { $0 === source }),
              source.isEnabled, !source.isHidden else { return }
        // Keep the original sender, target, recording guard and Undo command.
        source.performClick(nil)
    }
}

@MainActor
enum WorkspaceControlStyle {
    static func icon(_ button: NSButton, height: CGFloat = 28) {
        button.bezelStyle = .regularSquare
        button.isBordered = false
        button.wantsLayer = true
        button.layer?.cornerRadius = 5
        button.layer?.backgroundColor = DAWDesignTokens.Color.raisedSurface.cgColor
        let configuration = NSImage.SymbolConfiguration(pointSize: 12, weight: .medium)
        if let configured = button.image?.withSymbolConfiguration(configuration) {
            button.image = configured
        }
        button.contentTintColor = DAWDesignTokens.Color.text
        button.heightAnchor.constraint(equalToConstant: height).isActive = true
    }
    static func separator() -> NSView {
        let view = NSView()
        view.wantsLayer = true
        view.layer?.backgroundColor = DAWDesignTokens.Color.border.cgColor
        view.widthAnchor.constraint(equalToConstant: 1).isActive = true
        view.heightAnchor.constraint(equalToConstant: 20).isActive = true
        return view
    }
}

/// Frequent commands remain visible at the minimum center width. Secondary
/// commands live in real AppKit menus; transient import controls stay visible.
@MainActor
final class WorkspaceCommandBar: NSView {
    let addMenu: WorkspaceCommandMenu
    let moreMenu: WorkspaceCommandMenu
    let scroll = NSScrollView()
    let commands: NSStackView
    override var isFlipped: Bool { true }

    init(importButton: NSButton, add: [NSButton], history: [NSButton], grid: NSPopUpButton,
         zoom: [NSButton], more: [NSButton], transient: [NSButton]) {
        addMenu = WorkspaceCommandMenu("＋ Дорожка", sources: add)
        moreMenu = WorkspaceCommandMenu("Ещё", sources: more)
        commands = NSStackView()
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        scroll.hasHorizontalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay
        scroll.drawsBackground = false
        scroll.documentView = commands
        addSubview(scroll)
        for button in [importButton] + history + zoom { WorkspaceControlStyle.icon(button) }
        for (index, button) in zoom.enumerated() {
            button.widthAnchor.constraint(equalToConstant: 28).isActive = true
            let labels = ["Уменьшить масштаб", "Показать проект целиком", "Увеличить масштаб"]
            let label = labels[min(index, labels.count - 1)]
            button.toolTip = label; button.setAccessibilityLabel(label)
        }
        importButton.toolTip = "Импорт аудио · ⌘I"
        addMenu.widthAnchor.constraint(equalToConstant: 96).isActive = true
        moreMenu.widthAnchor.constraint(equalToConstant: 58).isActive = true
        grid.controlSize = .regular
        grid.font = .monospacedDigitSystemFont(ofSize: 11, weight: .medium)
        grid.widthAnchor.constraint(equalToConstant: 90).isActive = true
        for popup in [addMenu, moreMenu] { popup.heightAnchor.constraint(equalToConstant: 28).isActive = true }
        var controls: [NSView] = [importButton, addMenu, WorkspaceControlStyle.separator()]
        controls.append(contentsOf: history)
        controls.append(contentsOf: [WorkspaceControlStyle.separator(), grid])
        controls.append(contentsOf: zoom)
        controls.append(contentsOf: [WorkspaceControlStyle.separator(), moreMenu])
        controls.append(contentsOf: transient)
        controls.forEach { commands.addArrangedSubview($0) }
        commands.orientation = .horizontal
        commands.alignment = .centerY
        commands.spacing = 5
        commands.edgeInsets = NSEdgeInsets(top: 6, left: 8, bottom: 6, right: 8)
        commands.translatesAutoresizingMaskIntoConstraints = false
        commands.heightAnchor.constraint(equalToConstant: 40).isActive = true
        setAccessibilityLabel("Команды аранжировки: импорт, дорожки, история, сетка, масштаб и дополнительные действия")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override func layout() { super.layout(); scroll.frame = bounds }
}
