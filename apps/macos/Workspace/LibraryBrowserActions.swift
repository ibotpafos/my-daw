import AppKit

@MainActor
final class LibraryTableView: NSTableView {
    var contextMenuForRow: ((Int) -> NSMenu?)?
    override func menu(for event: NSEvent) -> NSMenu? {
        let point = convert(event.locationInWindow, from: nil)
        guard bounds.contains(point) else { return nil }
        let index = row(at: point)
        return index >= 0 ? contextMenuForRow?(index) : nil
    }
}

@MainActor
final class LibraryItemCell: NSTableCellView {
    let resourceIcon = NSImageView()
    let nameLabel = NSTextField(labelWithString: "")
    let detailLabel = NSTextField(labelWithString: "")
    let favoriteButton = NSButton()
    var onFavorite: (() -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        identifier = NSUserInterfaceItemIdentifier("library.resource")
        nameLabel.font = .systemFont(ofSize: 12, weight: .medium)
        detailLabel.font = .systemFont(ofSize: 10)
        for label in [nameLabel, detailLabel] {
            label.lineBreakMode = .byTruncatingTail
            label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        }
        let text = NSStackView(views: [nameLabel, detailLabel])
        text.orientation = .vertical; text.alignment = .leading; text.spacing = 3
        favoriteButton.bezelStyle = .regularSquare; favoriteButton.isBordered = false
        favoriteButton.target = self; favoriteButton.action = #selector(toggleFavorite)
        let stack = NSStackView(views: [resourceIcon, text, favoriteButton])
        stack.spacing = 7; stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            resourceIcon.widthAnchor.constraint(equalToConstant: 18),
            resourceIcon.heightAnchor.constraint(equalToConstant: 20),
            favoriteButton.widthAnchor.constraint(equalToConstant: 24),
            favoriteButton.heightAnchor.constraint(equalToConstant: 28),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 4),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -4),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor)
        ])
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func configure(_ item: InspectorBrowserItem, favorite: Bool) {
        resourceIcon.image = NSImage(systemSymbolName: item.category.symbol, accessibilityDescription: item.category.title)
        resourceIcon.contentTintColor = item.available ? DAWDesignTokens.Color.accent : DAWDesignTokens.Color.coral
        nameLabel.stringValue = item.title
        nameLabel.textColor = DAWDesignTokens.Color.text
        let format = item.format == .unknown ? "" : item.format.title + " · "
        detailLabel.stringValue = (item.available ? "" : "Недоступен · ") + (item.detail.hasPrefix(format) ? item.detail : format + item.detail)
        detailLabel.textColor = item.available ? DAWDesignTokens.Color.secondaryText : DAWDesignTokens.Color.coral
        favoriteButton.image = NSImage(systemSymbolName: favorite ? "star.fill" : "star", accessibilityDescription: nil)
        favoriteButton.contentTintColor = favorite ? DAWDesignTokens.Color.accent : DAWDesignTokens.Color.secondaryText
        favoriteButton.isEnabled = item.resourceKey != nil
        let label = favorite ? "Убрать из избранного" : "В избранное"
        favoriteButton.setAccessibilityLabel(label + ": " + item.title)
        favoriteButton.toolTip = item.resourceKey == nil ? "Идентификатор ресурса недоступен" : label
        toolTip = [item.title, item.detail, item.sourceURL?.path ?? ""].filter { !$0.isEmpty }.joined(separator: "\n")
    }
    @objc private func toggleFavorite() { guard favoriteButton.isEnabled else { return }; onFavorite?() }
}

@MainActor
final class LibraryMenuPayload: NSObject {
    enum Action { case favorite, add, preview }
    let itemID: UUID
    let resourceKey: String?
    let action: Action
    init(_ item: InspectorBrowserItem, _ action: Action) {
        itemID = item.id; resourceKey = item.resourceKey; self.action = action
    }
}
