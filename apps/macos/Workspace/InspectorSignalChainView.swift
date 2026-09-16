import AppKit

struct InspectorSend: Equatable {
    let busID: UInt64
    let name: String
    let gainDb: Double
    let preFader: Bool
}
struct InspectorSendDestination: Equatable {
    let id: UInt64
    let name: String
}
/// A command belongs to the exact channel and project revision that displayed it.
struct InspectorChainContext: Equatable {
    let target: RackTarget
    let revision: UInt64
}
enum InspectorSendAction {
    case add(UInt64), gain(UInt64, Double), pre(UInt64), remove(UInt64)
}

/// Native controls over the existing rack and send commands. No plugin instances
/// or graph state are owned by this view; stale row/menu callbacks are rejected.
@MainActor
final class InspectorSignalChainView: NSStackView {
    private(set) var context: InspectorChainContext?
    private(set) var devices: [RackDevice] = []
    private(set) var sends: [InspectorSend] = []
    private(set) var destinations: [InspectorSendDestination] = []
    private(set) var editable = false
    var onRackAction: ((InspectorChainContext, RackAction) -> Void)?
    var onSendAction: ((InspectorChainContext, InspectorSendAction) -> Void)?
    private(set) var pluginButtons: [NSButton] = []
    private(set) var bypassButtons: [NSButton] = []
    private(set) var sendSliders: [NSSlider] = []
    private(set) var sendPreButtons: [NSButton] = []
    private(set) var sendRemoveButtons: [NSButton] = []
    private(set) var addPluginButton: NSButton?
    private(set) var addSendMenu: NSPopUpButton?

    override init(frame: NSRect) {
        super.init(frame: frame)
        orientation = .vertical; alignment = .leading; spacing = 5
        setAccessibilityLabel("Плагины и посылы выбранного канала")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    func update(context: InspectorChainContext?, devices: [RackDevice], sends: [InspectorSend],
                destinations: [InspectorSendDestination], editable: Bool) {
        guard self.context != context || self.devices != devices || self.sends != sends ||
                self.destinations != destinations || self.editable != editable else { return }
        self.context = context; self.devices = devices; self.sends = sends
        self.destinations = destinations; self.editable = editable
        rebuild()
    }
    func setEditingEnabled(_ enabled: Bool) {
        update(context: context, devices: devices, sends: sends, destinations: destinations, editable: enabled)
    }
    func performRack(_ action: RackAction, context captured: InspectorChainContext) {
        guard editable, context == captured else { return }
        switch action {
        case .add: break
        case let .edit(id): guard devices.contains(where: { $0.id == id && $0.available && $0.canEdit }) else { return }
        case let .bypass(id): guard devices.contains(where: { $0.id == id && $0.available }) else { return }
        case let .remove(id): guard devices.contains(where: { $0.id == id }) else { return }
        case let .move(id, index): guard devices.contains(where: { $0.id == id }), devices.indices.contains(index) else { return }
        }
        onRackAction?(captured, action)
    }
    func performSend(_ action: InspectorSendAction, context captured: InspectorChainContext) {
        guard editable, context == captured, captured.target.owner == Int32(DAW_INSERT_OWNER_TRACK) else { return }
        switch action {
        case let .add(id):
            guard destinations.contains(where: { $0.id == id }), !sends.contains(where: { $0.busID == id }) else { return }
        case let .gain(id, value):
            guard sends.contains(where: { $0.busID == id }), value.isFinite, (-120...24).contains(value) else { return }
        case let .pre(id), let .remove(id): guard sends.contains(where: { $0.busID == id }) else { return }
        }
        onSendAction?(captured, action)
    }
    private func append(_ view: NSView) {
        addArrangedSubview(view)
        view.widthAnchor.constraint(equalTo: widthAnchor).isActive = true
    }
    private func label(_ text: String, size: CGFloat = 11, secondary: Bool = false) -> NSTextField {
        let field = NSTextField(labelWithString: text)
        field.font = .systemFont(ofSize: size, weight: .medium)
        field.textColor = secondary ? DAWDesignTokens.Color.secondaryText : DAWDesignTokens.Color.text
        field.lineBreakMode = .byTruncatingTail
        field.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return field
    }
    private func actionButton(_ title: String, symbol: String? = nil, enabled: Bool,
                              action: @escaping () -> Void) -> NSButton {
        let button = WorkspaceActionButton(title, symbol: symbol, action: action)
        // Avoid inline bezels that visually dim the entire row in an inactive
        // window. Keep native target/action and real disabled state intact.
        button.bezelStyle = .regularSquare; button.isBordered = false; button.controlSize = .small
        button.isEnabled = enabled; button.toolTip = title
        let ink = enabled ? DAWDesignTokens.Color.text : DAWDesignTokens.Color.secondaryText.withAlphaComponent(0.5)
        button.contentTintColor = ink
        let paragraph = NSMutableParagraphStyle(); paragraph.lineBreakMode = .byTruncatingTail
        button.attributedTitle = NSAttributedString(string: title, attributes: [
            .font: NSFont.systemFont(ofSize: 11, weight: .medium),
            .foregroundColor: ink, .paragraphStyle: paragraph
        ])
        if symbol != nil {
            button.imagePosition = .imageOnly
            button.image = button.image?.withSymbolConfiguration(.init(pointSize: 12, weight: .medium))
            button.widthAnchor.constraint(equalToConstant: 24).isActive = true
        }
        button.heightAnchor.constraint(equalToConstant: 24).isActive = true
        return button
    }
    private func section(_ title: String, count: Int, trailing: NSView) {
        let line = NSBox(); line.boxType = .separator; append(line)
        let name = label(title, size: 12)
        let countLabel = label(String(count), size: 10, secondary: true)
        let space = NSView(); space.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let row = NSStackView(views: [name, countLabel, space, trailing]); row.spacing = 6
        row.heightAnchor.constraint(equalToConstant: 28).isActive = true
        append(row)
    }
    private func rebuild() {
        for view in arrangedSubviews { removeArrangedSubview(view); view.removeFromSuperview() }
        pluginButtons = []; bypassButtons = []; sendSliders = []; sendPreButtons = []; sendRemoveButtons = []
        let captured = context
        let add = actionButton("Добавить плагин", symbol: "plus", enabled: editable && captured != nil) { [weak self] in
            guard let captured else { return }; self?.performRack(.add, context: captured)
        }
        addPluginButton = add
        section("Плагины", count: devices.count, trailing: add)
        if devices.isEmpty { append(label("Нет плагинов — добавьте через +", secondary: true)) }
        for (index, device) in devices.enumerated() {
            let power = actionButton("\(device.bypassed ? "Включить" : "Обход") · \(device.name)", symbol: "power",
                                     enabled: editable && device.available) { [weak self] in
                guard let captured else { return }; self?.performRack(.bypass(device.id), context: captured)
            }
            power.contentTintColor = !device.available ? DAWDesignTokens.Color.coral :
                (device.bypassed ? DAWDesignTokens.Color.secondaryText : DAWDesignTokens.Color.accent)
            power.setAccessibilityValue(device.bypassed ? "Обход" : "Включён")
            let edit = actionButton(device.name, enabled: editable && device.available && device.canEdit) { [weak self] in
                guard let captured else { return }; self?.performRack(.edit(device.id), context: captured)
            }
            edit.alignment = .left; edit.cell?.lineBreakMode = .byTruncatingTail
            edit.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            edit.setContentHuggingPriority(.defaultLow, for: .horizontal)
            edit.toolTip = "\(device.name) · \(device.format) · \(device.available ? "Параметры плагина" : "Недоступен")"
            let menu = NSPopUpButton(frame: .zero, pullsDown: true)
            menu.bezelStyle = .inline; menu.controlSize = .small
            menu.widthAnchor.constraint(equalToConstant: 26).isActive = true
            menu.setAccessibilityLabel("Действия плагина \(device.name)")
            menu.addItem(withTitle: "•••"); menu.menu?.autoenablesItems = false
            func item(_ title: String, _ action: RackAction, enabled: Bool = true) {
                let entry = InspectorChainMenuItem(title: title, enabled: editable && enabled) { [weak self] in
                    guard let captured else { return }; self?.performRack(action, context: captured)
                }
                menu.menu?.addItem(entry)
            }
            item("Раньше в цепочке", .move(device.id, index - 1), enabled: index > 0)
            item("Позже в цепочке", .move(device.id, index + 1), enabled: index + 1 < devices.count)
            menu.menu?.addItem(.separator()); item("Удалить плагин", .remove(device.id))
            let number = label(String(index + 1), size: 10, secondary: true)
            number.widthAnchor.constraint(equalToConstant: 14).isActive = true
            let row = NSStackView(views: [number, power, edit, menu]); row.spacing = 3
            row.edgeInsets = NSEdgeInsets(top: 2, left: 5, bottom: 2, right: 3)
            row.wantsLayer = true; row.layer?.cornerRadius = 4
            row.layer?.backgroundColor = DAWDesignTokens.Color.raisedSurface.cgColor
            append(row); pluginButtons.append(edit); bypassButtons.append(power)
        }
        let choices = NSPopUpButton(frame: .zero, pullsDown: true)
        choices.bezelStyle = .inline; choices.controlSize = .small
        choices.setAccessibilityLabel("Добавить посыл на шину")
        choices.addItem(withTitle: "+"); choices.menu?.autoenablesItems = false
        choices.isEnabled = editable && !destinations.isEmpty && captured?.target.owner == Int32(DAW_INSERT_OWNER_TRACK)
        for destination in destinations {
            let item = InspectorChainMenuItem(title: destination.name, enabled: choices.isEnabled) { [weak self] in
                guard let captured else { return }; self?.performSend(.add(destination.id), context: captured)
            }
            choices.menu?.addItem(item)
        }
        addSendMenu = choices
        section("Посылы", count: sends.count, trailing: choices)
        if sends.isEmpty {
            let hint = captured?.target.owner == Int32(DAW_INSERT_OWNER_TRACK) ?
                (destinations.isEmpty ? "Создайте шину для посыла" : "Нет посылов — выберите шину через +") : "Посылы доступны у дорожек"
            append(label(hint, size: 10, secondary: true))
        }
        for (index, send) in sends.enumerated() {
            let badge = label(String(index + 1), size: 10, secondary: true)
            let name = label(send.name)
            let pre = actionButton(send.preFader ? "PRE" : "POST", enabled: editable) { [weak self] in
                guard let captured else { return }; self?.performSend(.pre(send.busID), context: captured)
            }
            pre.setAccessibilityLabel("Посыл \(send.name): до или после фейдера")
            pre.toolTip = send.preFader ? "До фейдера" : "После фейдера"
            let remove = actionButton("Удалить посыл \(send.name)", symbol: "xmark", enabled: editable) { [weak self] in
                guard let captured else { return }; self?.performSend(.remove(send.busID), context: captured)
            }
            let header = NSStackView(views: [badge, name, pre, remove]); header.spacing = 5
            name.setContentHuggingPriority(.defaultLow, for: .horizontal)
            let value = label(String(format: "%+.1f dB", send.gainDb), size: 10, secondary: true)
            value.alignment = .right; value.widthAnchor.constraint(equalToConstant: 64).isActive = true
            let slider = InspectorSendSlider(frame: .zero)
            slider.minValue = -120; slider.maxValue = 24; slider.doubleValue = send.gainDb
            slider.isContinuous = false; slider.controlSize = .mini; slider.isEnabled = editable
            slider.setAccessibilityLabel("Уровень посыла \(send.name), dB")
            slider.onCommit = { [weak self] gain in
                guard let captured else { return }; self?.performSend(.gain(send.busID, gain), context: captured)
            }
            let controls = NSStackView(views: [slider, value]); controls.spacing = 7
            let row = NSStackView(views: [header, controls]); row.orientation = .vertical; row.spacing = 0
            header.widthAnchor.constraint(equalTo: row.widthAnchor).isActive = true
            controls.widthAnchor.constraint(equalTo: row.widthAnchor).isActive = true
            append(row); sendSliders.append(slider); sendPreButtons.append(pre); sendRemoveButtons.append(remove)
        }
    }
}

@MainActor
private final class InspectorChainMenuItem: NSMenuItem {
    private var handler: (() -> Void)?
    init(title: String, enabled: Bool, handler: @escaping () -> Void) {
        self.handler = handler
        super.init(title: title, action: #selector(invoke), keyEquivalent: "")
        target = self; isEnabled = enabled
    }
    required init(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc private func invoke() { if isEnabled { handler?() } }
}
@MainActor
private final class InspectorSendSlider: NSSlider {
    var onCommit: ((Double) -> Void)?
    override init(frame: NSRect) { super.init(frame: frame); target = self; action = #selector(commit) }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc private func commit() { if isEnabled { onCommit?(doubleValue) } }
}
