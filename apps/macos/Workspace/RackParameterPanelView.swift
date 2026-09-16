import AppKit

struct RackParameterRequest: Equatable {
    let token = UUID()
    let target: RackTarget
    let pluginID: UInt64
    let revision: UInt64
    let offset: UInt32
}
struct RackParameterPage {
    let total: UInt32
    let parameters: [RackParameter]
}
enum RackParameterRead {
    case loaded(RackParameterPage)
    case unavailable(String)
}

/// Four native controls per page. The bridge owns all plug-in state; the panel
/// only holds an immutable snapshot and rejects detached/stale control actions.
@MainActor
final class RackParameterPanelView: NSView {
    static let pageSize: UInt32 = 4
    private(set) var request: RackParameterRequest?
    private(set) var page: RackParameterPage?
    private(set) var controls: [RackParameterControl] = []
    private(set) var previousButton: NSButton!
    private(set) var nextButton: NSButton!
    private let message = NSTextField(wrappingLabelWithString: "")
    private let pages = NSTextField(labelWithString: "")
    private var editingEnabled = false
    var onPage: ((UInt32) -> Void)?
    var onCommit: ((RackParameterRequest, UInt32, Double) -> Bool)?
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        heightAnchor.constraint(equalToConstant: 110).isActive = true
        previousButton = WorkspaceActionButton("‹") { [weak self] in
            guard let self, let request = self.request, self.editingEnabled,
                  request.offset >= Self.pageSize else { return }
            self.onPage?(request.offset - Self.pageSize)
        }
        nextButton = WorkspaceActionButton("›") { [weak self] in
            guard let self, let request = self.request, let page = self.page, self.editingEnabled,
                  request.offset + Self.pageSize < page.total else { return }
            self.onPage?(request.offset + Self.pageSize)
        }
        previousButton.setAccessibilityLabel("Предыдущие параметры")
        nextButton.setAccessibilityLabel("Следующие параметры")
        for button in [previousButton!, nextButton!] {
            button.bezelStyle = .regularSquare; button.isBordered = false
            button.font = .systemFont(ofSize: 16); button.contentTintColor = DAWDesignTokens.Color.text
        }
        message.font = .systemFont(ofSize: 11); message.textColor = DAWDesignTokens.Color.secondaryText
        pages.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular)
        pages.textColor = DAWDesignTokens.Color.secondaryText; pages.alignment = .center
        [message, pages, previousButton!, nextButton!].forEach(addSubview)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func begin(_ request: RackParameterRequest) {
        self.request = request; page = nil
        controls.forEach { $0.removeFromSuperview() }; controls = []
        message.stringValue = "Чтение сохранённых параметров…"; message.isHidden = false
        setEditingEnabled(false)
    }
    func show(_ result: RackParameterRead, for request: RackParameterRequest) {
        guard self.request == request else { return }
        switch result {
        case let .unavailable(reason): message.stringValue = reason
        case let .loaded(page):
            self.page = page
            message.stringValue = "Плагин не публикует параметры."
            message.isHidden = !page.parameters.isEmpty
            for parameter in page.parameters {
                let control = RackParameterControl(parameter)
                control.onCommit = { [weak self] value in
                    self?.commit(parameter.id, value: value, request: request) ?? false
                }
                controls.append(control); addSubview(control)
            }
        }
        setEditingEnabled(true); needsLayout = true
    }
    func setEditingEnabled(_ enabled: Bool) {
        editingEnabled = enabled
        controls.forEach { $0.setEditingEnabled(enabled) }
        previousButton.isEnabled = enabled && (request?.offset ?? 0) >= Self.pageSize
        nextButton.isEnabled = enabled && (request?.offset ?? 0) + Self.pageSize < (page?.total ?? 0)
    }
    @discardableResult
    func commit(_ id: UInt32, value: Double, request: RackParameterRequest) -> Bool {
        guard editingEnabled, self.request == request,
              let parameter = page?.parameters.first(where: { $0.id == id }), parameter.accepts(value) else { return false }
        return onCommit?(request, id, value) ?? false
    }
    override func layout() {
        super.layout()
        let cellWidth = bounds.width / CGFloat(Self.pageSize)
        for (index, control) in controls.enumerated() {
            control.frame = NSRect(x: CGFloat(index) * cellWidth, y: 0, width: cellWidth, height: 84)
        }
        message.frame = NSRect(x: 4, y: 12, width: max(0, bounds.width - 8), height: 68)
        previousButton.frame = NSRect(x: 0, y: 88, width: 26, height: 22)
        nextButton.frame = NSRect(x: max(0, bounds.width - 26), y: 88, width: 26, height: 22)
        pages.frame = NSRect(x: 28, y: 91, width: max(0, bounds.width - 56), height: 16)
        if let request, let page, page.total > 0 {
            pages.stringValue = "\(request.offset + 1)–\(min(request.offset + Self.pageSize, page.total)) / \(page.total)"
        } else { pages.stringValue = "" }
    }
}

@MainActor
final class RackParameterControl: NSView {
    let parameter: RackParameter
    let slider = NSSlider(value: 0, minValue: 0, maxValue: 1, target: nil, action: nil)
    let valueField = NSTextField(string: "")
    private let name = NSTextField(wrappingLabelWithString: "")
    var onCommit: ((Double) -> Bool)?
    override var isFlipped: Bool { true }
    init(_ parameter: RackParameter) {
        self.parameter = parameter
        super.init(frame: .zero)
        name.stringValue = parameter.name; name.font = .systemFont(ofSize: 10, weight: .medium)
        name.textColor = DAWDesignTokens.Color.text; name.alignment = .center
        name.maximumNumberOfLines = 2; name.lineBreakMode = .byTruncatingTail
        name.toolTip = parameter.name
        slider.sliderType = .circular; slider.controlSize = .small; slider.doubleValue = parameter.position
        slider.isContinuous = false; slider.target = self; slider.action = #selector(changeKnob)
        slider.setAccessibilityLabel(parameter.name)
        slider.setAccessibilityHelp("Сохранённый параметр. Применяется после отпускания; останавливает воспроизведение. Не запись автоматизации.")
        valueField.stringValue = parameter.displayValue; valueField.alignment = .center
        valueField.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular)
        valueField.textColor = DAWDesignTokens.Color.text; valueField.backgroundColor = DAWDesignTokens.Color.canvas
        valueField.isBezeled = false; valueField.drawsBackground = true
        valueField.target = self; valueField.action = #selector(changeNumber)
        valueField.setAccessibilityLabel("\(parameter.name) — числовое значение")
        valueField.toolTip = "\(parameter.minimum)…\(parameter.maximum) · Enter — применить"
        [name, slider, valueField].forEach(addSubview)
        setEditingEnabled(true)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func setEditingEnabled(_ enabled: Bool) {
        slider.isEnabled = enabled && parameter.writable; valueField.isEnabled = enabled && parameter.writable
    }
    @objc private func changeKnob() {
        guard slider.isEnabled, let value = parameter.nativeValue(at: slider.doubleValue), onCommit?(value) == true else {
            slider.doubleValue = parameter.position; return
        }
    }
    @objc private func changeNumber() {
        guard valueField.isEnabled, let value = parameter.parse(valueField.stringValue), onCommit?(value) == true else {
            valueField.stringValue = parameter.displayValue
            valueField.toolTip = "Введите число в диапазоне \(parameter.minimum)…\(parameter.maximum)"
            return
        }
    }
    override func layout() {
        super.layout()
        name.frame = NSRect(x: 2, y: 0, width: max(0, bounds.width - 4), height: 26)
        slider.frame = NSRect(x: (bounds.width - 32) / 2, y: 28, width: 32, height: 32)
        valueField.frame = NSRect(x: 3, y: 64, width: max(0, bounds.width - 6), height: 18)
    }
}
