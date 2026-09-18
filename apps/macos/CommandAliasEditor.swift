import AppKit

/// Inline, non-modal preferences editor. Return saves aliases, never invokes
/// the selected DAW command. Its command ID is owned by the palette controller.
@MainActor
final class DAWCommandAliasEditor: NSView, NSTextFieldDelegate {
    let input = NSTextField(string: "")
    private let titleLabel = NSTextField(labelWithString: "")
    private let feedback = NSTextField(labelWithString: "До 8 названий через ; · пустое поле удаляет названия")
    var onSave: (([String]) -> Void)?
    var onCancel: (() -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        identifier = NSUserInterfaceItemIdentifier("commandAliases.editor")
        titleLabel.font = .systemFont(ofSize: 11, weight: .medium)
        titleLabel.lineBreakMode = .byTruncatingMiddle
        input.identifier = NSUserInterfaceItemIdentifier("commandAliases.input")
        input.placeholderString = "Например: рендер; bounce"
        input.delegate = self
        input.setAccessibilityLabel("Свои названия выбранной команды")
        input.setAccessibilityHelp("До восьми названий через точку с запятой. Enter сохраняет, Escape отменяет редактирование. Пустое поле удаляет названия.")
        let save = NSButton(title: "Сохранить", target: self, action: #selector(saveAliases))
        let cancel = NSButton(title: "Отмена", target: self, action: #selector(cancelEditing))
        save.identifier = NSUserInterfaceItemIdentifier("commandAliases.save")
        cancel.identifier = NSUserInterfaceItemIdentifier("commandAliases.cancel")
        for button in [save, cancel] { button.bezelStyle = .rounded; button.controlSize = .small }
        feedback.font = .systemFont(ofSize: 10)
        feedback.textColor = .secondaryLabelColor
        feedback.lineBreakMode = .byTruncatingTail
        let row = NSStackView(views: [input, save, cancel])
        row.orientation = .horizontal; row.spacing = 6
        for view in [titleLabel, row, feedback] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            addSubview(view)
        }
        NSLayoutConstraint.activate([
            titleLabel.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            titleLabel.leadingAnchor.constraint(equalTo: leadingAnchor),
            titleLabel.trailingAnchor.constraint(equalTo: trailingAnchor),
            row.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 6),
            row.leadingAnchor.constraint(equalTo: leadingAnchor),
            row.trailingAnchor.constraint(equalTo: trailingAnchor),
            input.widthAnchor.constraint(greaterThanOrEqualToConstant: 160),
            feedback.topAnchor.constraint(equalTo: row.bottomAnchor, constant: 6),
            feedback.leadingAnchor.constraint(equalTo: leadingAnchor),
            feedback.trailingAnchor.constraint(equalTo: trailingAnchor)
        ])
    }
    required init?(coder: NSCoder) { nil }

    func edit(_ item: DAWCommandSearchItem) {
        titleLabel.stringValue = "Свои названия: \(item.title)"
        titleLabel.toolTip = [item.path, item.title].joined(separator: " › ")
        input.stringValue = item.aliases.joined(separator: "; ")
        feedback.stringValue = "До 8 названий через ; · пустое поле удаляет названия"
        feedback.textColor = .secondaryLabelColor
        feedback.toolTip = nil
        feedback.setAccessibilityLabel(nil)
    }
    func showError(_ message: String) {
        feedback.stringValue = message
        feedback.textColor = .systemRed
        feedback.toolTip = message
        feedback.setAccessibilityLabel(message)
    }
    @objc private func saveAliases() {
        if let editor = input.currentEditor() as? NSTextView, editor.hasMarkedText() { return }
        // Return is intercepted before AppKit ends field editing. Commit the
        // user's current editor text to the control before reading its value.
        input.validateEditing()
        onSave?(DAWCommandAliases.parse(input.stringValue))
    }
    @objc private func cancelEditing() { onCancel?() }
    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        guard !textView.hasMarkedText() else { return false }
        switch selector {
        case #selector(NSResponder.insertNewline(_:)): saveAliases(); return true
        case #selector(NSResponder.cancelOperation(_:)): onCancel?(); return true
        default: return false
        }
    }
}
