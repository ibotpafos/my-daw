import AppKit

struct RackTarget: Equatable { let owner: Int32; let ownerID: UInt64 }
struct RackDevice: Equatable {
    let id: UInt64
    var name: String
    var format: String
    var bypassed: Bool
    var available: Bool
    var isolated: Bool
    var canEdit: Bool
    var latencyFrames: UInt64
}
enum RackAction { case add, edit(UInt64), bypass(UInt64), remove(UInt64), move(UInt64, Int) }

@MainActor
final class WorkspaceActionButton: NSButton {
    var onClick: (() -> Void)?
    init(_ title: String, symbol: String? = nil, action: @escaping () -> Void) {
        super.init(frame: .zero)
        self.title = title; self.onClick = action
        bezelStyle = .texturedRounded; font = .systemFont(ofSize: 11, weight: .medium)
        target = self; self.action = #selector(clicked)
        if let symbol { image = NSImage(systemSymbolName: symbol, accessibilityDescription: title); imagePosition = .imageLeading }
        setAccessibilityLabel(title)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc private func clicked() { if isEnabled { onClick?() } }
}

/// Honest device rack: displays actual inserts and their reported state. It does
/// not draw invented EQ/compressor curves or imply a vendor editor is embedded.
@MainActor
final class ChannelRackView: NSView {
    var onAction: ((RackTarget, RackAction) -> Void)?
    var onReadParameters: ((RackParameterRequest) -> RackParameterRead)?
    let parameterPanel = RackParameterPanelView()
    private(set) var parameterPluginID: UInt64?
    private(set) var parameterButtons: [NSButton] = []
    private var parameterOffset: UInt32 = 0
    private var displayRevision: UInt64 = 0
    private var generation = UUID()
    private var cardWidths: [CGFloat] = []
    private(set) var target: RackTarget?
    private(set) var devices: [RackDevice] = []
    private var editable = false
    private let title = NSTextField(labelWithString: "Устройства канала")
    private let routing = NSTextField(labelWithString: "")
    private let scroll = NSScrollView()
    private let canvas = WorkspaceSurface(DAWDesignTokens.Color.canvas)
    private var cards: [NSView] = []
    private var accent = DAWDesignTokens.Color.accent
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        title.font = .systemFont(ofSize: 12, weight: .semibold); title.textColor = DAWDesignTokens.Color.text
        routing.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular)
        routing.textColor = DAWDesignTokens.Color.secondaryText; routing.alignment = .right
        routing.lineBreakMode = .byTruncatingTail
        scroll.documentView = canvas; scroll.hasHorizontalScroller = true; scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true; scroll.scrollerStyle = .overlay; scroll.drawsBackground = false
        [title, routing, scroll].forEach(addSubview)
        parameterPanel.onPage = { [weak self] offset in
            self?.parameterOffset = offset; self?.reloadParameters()
        }
        renderCards()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func update(target: RackTarget?, title: String, output: String, accent: NSColor, devices: [RackDevice], editable: Bool, revision: UInt64 = 0) {
        self.title.stringValue = target == nil ? "Устройства канала" : title
        routing.stringValue = target == nil ? "" : "\(title) → \(output)"
        if self.target != target || !devices.contains(where: { $0.id == parameterPluginID }) {
            parameterPluginID = nil; parameterOffset = 0
        }
        self.target = target; self.devices = devices; self.editable = editable; self.accent = accent
        displayRevision = revision
        // Every refresh invalidates old controls, even reopening a project whose
        // numeric revision and insert IDs happen to match the previous project.
        renderCards()
        reloadParameters()
    }
    func openParameters(_ id: UInt64) {
        guard editable, devices.contains(where: { $0.id == id && $0.available && $0.canEdit }) else { return }
        parameterPluginID = id; parameterOffset = 0; renderCards(); reloadParameters()
    }
    func reloadParameters() {
        guard let target, let id = parameterPluginID else { return }
        let request = RackParameterRequest(target: target, pluginID: id, revision: displayRevision, offset: parameterOffset)
        parameterPanel.begin(request)
        guard editable else {
            parameterPanel.show(.unavailable("Параметры недоступны во время записи."), for: request)
            parameterPanel.setEditingEnabled(false); return
        }
        let result = onReadParameters?(request) ?? .unavailable("Параметры недоступны.")
        parameterPanel.show(result, for: request)
    }
    func setParameterEditingEnabled(_ enabled: Bool) {
        parameterPanel.setEditingEnabled(enabled && editable)
    }
    private func perform(_ action: RackAction, generation: UUID) {
        guard self.generation == generation else { return }; perform(action)
    }
    func perform(_ action: RackAction) {
        guard editable, let target else { return }
        switch action {
        case .add: break
        case let .edit(id): guard devices.contains(where: { $0.id == id && $0.available && $0.canEdit }) else { return }
        case let .bypass(id): guard devices.contains(where: { $0.id == id && $0.available }) else { return }
        case let .remove(id): guard devices.contains(where: { $0.id == id }) else { return }
        case let .move(id, index): guard devices.contains(where: { $0.id == id }), devices.indices.contains(index) else { return }
        }
        onAction?(target, action)
    }
    override func layout() {
        super.layout()
        title.frame = NSRect(x: 12, y: 5, width: max(0, bounds.width * 0.5 - 12), height: 20)
        routing.frame = NSRect(x: bounds.width * 0.5, y: 7, width: max(0, bounds.width * 0.5 - 12), height: 18)
        scroll.frame = NSRect(x: 6, y: 32, width: max(0, bounds.width - 12), height: max(0, bounds.height - 38))
        let height = max(204, scroll.contentSize.height)
        let width = cardWidths.reduce(CGFloat(8)) { $0 + $1 + 8 }
        canvas.frame = NSRect(x: 0, y: 0, width: max(scroll.contentSize.width, width), height: height)
        var x: CGFloat = 6
        for (index, card) in cards.enumerated() {
            card.frame = NSRect(x: x, y: 6, width: cardWidths[index], height: height - 12)
            x += cardWidths[index] + 8
        }
    }
    private func renderCards() {
        generation = UUID()
        parameterPanel.removeFromSuperview()
        cards.forEach { $0.removeFromSuperview() }; cards.removeAll()
        cardWidths = []; parameterButtons = []
        let captured = generation
        for (index, device) in devices.enumerated() {
            let card = WorkspaceSurface(DAWDesignTokens.Color.raisedSurface)
            let heading = NSTextField(labelWithString: "\(index + 1)  \(device.name)")
            heading.font = .systemFont(ofSize: 13, weight: .semibold); heading.lineBreakMode = .byTruncatingTail
            let badge = NSTextField(labelWithString: "\(device.format)  ·  \(device.isolated ? "Отдельный процесс" : "В процессе")")
            badge.font = .systemFont(ofSize: 10); badge.textColor = DAWDesignTokens.Color.secondaryText
            let state = NSTextField(labelWithString: !device.available ? "Недоступен" : (device.bypassed ? "Обход / Bypass" : "Включён"))
            state.font = .systemFont(ofSize: 12, weight: .medium)
            state.textColor = !device.available ? DAWDesignTokens.Color.coral : (device.bypassed ? DAWDesignTokens.Color.warning : DAWDesignTokens.Color.mint)
            let latency = NSTextField(labelWithString: String(format: "Задержка плагина: %.2f ms", Double(device.latencyFrames) / 48))
            latency.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular); latency.textColor = DAWDesignTokens.Color.secondaryText
            let edit = WorkspaceActionButton("Параметры…", symbol: "slider.horizontal.3") { [weak self] in self?.perform(.edit(device.id), generation: captured) }
            edit.isEnabled = editable && device.available && device.canEdit
            edit.toolTip = "Открыть существующий редактор параметров; это не встроенный интерфейс производителя"
            let bypass = WorkspaceActionButton(device.bypassed ? "Включить" : "Обход", symbol: "power") { [weak self] in self?.perform(.bypass(device.id), generation: captured) }
            bypass.isEnabled = editable && device.available
            let up = WorkspaceActionButton("←") { [weak self] in self?.perform(.move(device.id, index - 1), generation: captured) }
            let down = WorkspaceActionButton("→") { [weak self] in self?.perform(.move(device.id, index + 1), generation: captured) }
            up.isEnabled = editable && index > 0; down.isEnabled = editable && index + 1 < devices.count
            up.setAccessibilityLabel("Переместить \(device.name) раньше в цепочке")
            down.setAccessibilityLabel("Переместить \(device.name) позже в цепочке")
            let remove = WorkspaceActionButton("Удалить") { [weak self] in self?.perform(.remove(device.id), generation: captured) }
            remove.isEnabled = editable
            let controls = NSStackView(views: [bypass, up, down, remove]); controls.spacing = 2
            for button in [bypass, up, down, remove] { button.font = .systemFont(ofSize: 10); button.controlSize = .small }
            let inline = WorkspaceActionButton("В панели", symbol: "dial.low") { [weak self] in
                guard self?.generation == captured else { return }; self?.openParameters(device.id)
            }
            inline.isEnabled = editable && device.available && device.canEdit
            inline.toolTip = "Показать сохранённые параметры без отдельного окна"
            parameterButtons.append(inline)
            let expanded = parameterPluginID == device.id
            let body: NSView
            if expanded { body = parameterPanel }
            else {
                let summary = NSStackView(views: [state, latency, inline, edit])
                summary.orientation = .vertical; summary.alignment = .leading; summary.spacing = 9
                inline.widthAnchor.constraint(equalTo: summary.widthAnchor).isActive = true
                edit.widthAnchor.constraint(equalTo: summary.widthAnchor).isActive = true
                body = summary
            }
            let stack = NSStackView(views: [heading, badge, body, controls])
            stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 5
            body.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
            if expanded {
                let all = WorkspaceActionButton("Все…") { [weak self] in self?.perform(.edit(device.id), generation: captured) }
                all.toolTip = "Полный редактор параметров и автоматизации"; all.isEnabled = edit.isEnabled
                all.controlSize = .small; controls.addArrangedSubview(all)
                badge.stringValue += device.format == "VST3" ? " · значения 0…1" : " · сохранённые значения"
            }
            for button in [inline, edit, bypass, up, down, remove] {
                button.bezelStyle = .regularSquare; button.isBordered = false
                button.contentTintColor = DAWDesignTokens.Color.text
            }
            stack.translatesAutoresizingMaskIntoConstraints = false; card.addSubview(stack)
            NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 10),
                                         stack.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -10),
                                         stack.topAnchor.constraint(equalTo: card.topAnchor, constant: 10)])
            cardWidths.append(expanded ? 332 : 244)
            cards.append(card); canvas.addSubview(card)
        }
        let addCard = WorkspaceSurface()
        let label = NSTextField(wrappingLabelWithString: target == nil ? "Выберите канал в аранжировке или микшере." : "Добавьте установленный AU или VST3 в цепочку выбранного канала.")
        label.font = .systemFont(ofSize: 12); label.textColor = DAWDesignTokens.Color.secondaryText; label.maximumNumberOfLines = 4
        let add = WorkspaceActionButton("Добавить плагин…", symbol: "plus") { [weak self] in self?.perform(.add, generation: captured) }
        add.isEnabled = editable && target != nil; add.contentTintColor = accent
        let stack = NSStackView(views: [label, add]); stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 18
        stack.translatesAutoresizingMaskIntoConstraints = false; addCard.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: addCard.leadingAnchor, constant: 14),
                                     stack.trailingAnchor.constraint(equalTo: addCard.trailingAnchor, constant: -14),
                                     stack.topAnchor.constraint(equalTo: addCard.topAnchor, constant: 24),
                                     label.widthAnchor.constraint(equalTo: stack.widthAnchor)])
        cardWidths.append(220); cards.append(addCard); canvas.addSubview(addCard); needsLayout = true
    }
}
