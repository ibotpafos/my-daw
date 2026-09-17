import AppKit

/// The main DAW window's keyboard boundary. The embedding controller owns the
/// command implementations; this class only decides whether a key is safe to
/// treat as a global DAW command.
final class DAWWindow: NSWindow {
    var onPlayStop: (() -> Void)?
    var onRewind: (() -> Void)?
    var onDeleteSelectedClip: (() -> Void)?
    var onDeleteSelectedTrack: (() -> Void)?
    var onZoomIn: (() -> Void)?
    var onZoomOut: (() -> Void)?
    var onZoomReset: (() -> Void)?
    private var commandPalette: DAWCommandPaletteController?

    override func sendEvent(_ event: NSEvent) {
        if event.type == .keyDown,
           !defersToTextInput,
           handleUnmodifiedGlobalCommand(event) {
            return
        }
        super.sendEvent(event)
    }

    /// The palette remains reachable during rename/search, but never interrupts
    /// marked-text composition. All other command equivalents keep menu routing.
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if DAWCommandPaletteKey.isToggle(event) {
            if let textView = firstResponder as? NSTextView, textView.hasMarkedText() {
                return super.performKeyEquivalent(with: event)
            }
            if !event.isARepeat {
                if commandPalette == nil { commandPalette = DAWCommandPaletteController() }
                commandPalette?.toggle(from: self)
            }
            return true
        }
        guard !defersToTextInput else { return super.performKeyEquivalent(with: event) }
        if super.performKeyEquivalent(with: event) { return true }
        if handleTrackDeleteCommand(event) { return true }
        return handleZoomCommand(event)
    }

    override func close() {
        commandPalette?.dismiss(restoreFocus: false)
        super.close()
    }

    private var defersToTextInput: Bool {
        var responder = firstResponder
        while let current = responder {
            if current is NSTextView || current is NSTextField || current is NSSearchField {
                return true
            }
            if let inputClient = current as? NSTextInputClient, inputClient.hasMarkedText() {
                return true
            }
            responder = current.nextResponder
        }
        return false
    }

    private func handleUnmodifiedGlobalCommand(_ event: NSEvent) -> Bool {
        guard event.modifierFlags.intersection(.deviceIndependentFlagsMask).isEmpty else { return false }
        switch event.keyCode {
        case 49: return invoke(onPlayStop)
        case 115: return invoke(onRewind)
        case 51, 117: return invoke(onDeleteSelectedClip)
        default: return false
        }
    }

    private func handleZoomCommand(_ event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        guard modifiers.contains(.command),
              modifiers.subtracting([.command, .shift]).isEmpty,
              let characters = event.charactersIgnoringModifiers else { return false }
        switch characters {
        case "+", "=": return invoke(onZoomIn)
        case "-", "_": return invoke(onZoomOut)
        case "0": return invoke(onZoomReset)
        default: return false
        }
    }

    /// Unmodified Delete stays scoped to clips; Command-Delete deletes a track.
    private func handleTrackDeleteCommand(_ event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        guard modifiers == [.command], event.keyCode == 51 || event.keyCode == 117 else { return false }
        return invoke(onDeleteSelectedTrack)
    }

    private func invoke(_ command: (() -> Void)?) -> Bool {
        guard let command else { return false }
        command()
        return true
    }
}
