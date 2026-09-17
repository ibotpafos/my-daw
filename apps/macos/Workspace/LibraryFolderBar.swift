import AppKit

@MainActor
final class LibraryFolderBar: NSView {
    let title = NSTextField(labelWithString: "Папка не выбрана")
    let status = NSTextField(wrappingLabelWithString: "")
    let refreshButton = NSButton()
    let cancelButton = NSButton()
    let forgetButton = NSButton()
    private weak var controller: LibraryFolderController?
    private var state = LibraryFolderController.State()
    var enabled = true { didSet { render(state) } }

    init(controller: LibraryFolderController) {
        self.controller = controller
        super.init(frame: .zero)
        title.font = .systemFont(ofSize: 11, weight: .medium)
        title.textColor = DAWDesignTokens.Color.text
        title.lineBreakMode = .byTruncatingMiddle
        title.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        title.setContentHuggingPriority(.defaultLow, for: .horizontal)
        status.font = .systemFont(ofSize: 10); status.maximumNumberOfLines = 2
        let specifications: [(NSButton, String, String, Selector)] = [
            (refreshButton, "arrow.clockwise", "Пересканировать папку", #selector(refresh)),
            (cancelButton, "xmark.circle", "Отменить сканирование", #selector(cancel)),
            (forgetButton, "eject", "Забыть папку, не удаляя файлы", #selector(forget))
        ]
        for (button, symbol, help, action) in specifications {
            button.image = NSImage(systemSymbolName: symbol, accessibilityDescription: help)
            button.bezelStyle = .regularSquare; button.isBordered = false
            button.contentTintColor = DAWDesignTokens.Color.secondaryText
            button.setAccessibilityLabel(help); button.toolTip = help
            button.target = self; button.action = action
            button.widthAnchor.constraint(equalToConstant: 24).isActive = true
            button.heightAnchor.constraint(equalToConstant: 24).isActive = true
        }
        let row = NSStackView(views: [title, refreshButton, cancelButton, forgetButton]); row.spacing = 4
        let stack = NSStackView(views: [row, status])
        stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 2
        stack.translatesAutoresizingMaskIntoConstraints = false; addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor), stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor), stack.bottomAnchor.constraint(equalTo: bottomAnchor),
            row.widthAnchor.constraint(equalTo: stack.widthAnchor), status.widthAnchor.constraint(equalTo: stack.widthAnchor)
        ])
        controller.onChange = { [weak self] state in self?.render(state) }
        render(controller.state)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    private func render(_ state: LibraryFolderController.State) {
        self.state = state
        title.stringValue = state.title; title.toolTip = controller?.activeURL?.path ?? state.title
        status.stringValue = state.message; status.toolTip = state.message
        status.textColor = state.warning ? DAWDesignTokens.Color.coral : DAWDesignTokens.Color.secondaryText
        refreshButton.isHidden = state.busy; cancelButton.isHidden = !state.busy
        refreshButton.isEnabled = enabled && state.hasFolder
        cancelButton.isEnabled = state.busy
        forgetButton.isEnabled = enabled && state.hasFolder
    }
    @objc private func refresh() { guard refreshButton.isEnabled else { return }; controller?.rescan() }
    @objc private func cancel() { guard cancelButton.isEnabled else { return }; controller?.cancel() }
    @objc private func forget() { guard forgetButton.isEnabled else { return }; controller?.forget() }
}
