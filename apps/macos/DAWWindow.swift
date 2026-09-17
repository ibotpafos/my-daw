import AppKit

/// The main DAW window's keyboard boundary.  The embedding controller owns the
/// command implementations; this class only decides whether a key is safe to
/// treat as a global DAW command.
final class DAWWindow: NSWindow {
    var onFocusedKeyDown: ((NSEvent) -> Bool)?
    var onPlayStop: (() -> Void)?
    var onRewind: (() -> Void)?
    var shouldHandleClipDelete: (() -> Bool)?
    var onDeleteSelectedClip: (() -> Void)?
    var onDeleteSelectedTrack: (() -> Void)?
    var onZoomIn: (() -> Void)?
    var onZoomOut: (() -> Void)?
    var onZoomReset: (() -> Void)?

    /// Capture unmodified transport/edit keys before focused canvas views.
    /// Command equivalents deliberately remain in AppKit's menu routing first.
    override func sendEvent(_ event: NSEvent) {
        if event.type == .keyDown,
           !defersToTextInput,
           (onFocusedKeyDown?(event) == true || handleUnmodifiedGlobalCommand(event)) {
            return
        }
        super.sendEvent(event)
    }

    /// Preserve the responder chain and standard menu shortcuts.  If no menu
    /// consumes a DAW zoom shortcut, use the closure supplied by the app.
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        guard !defersToTextInput else {
            return super.performKeyEquivalent(with: event)
        }
        if onFocusedKeyDown?(event) == true { return true }
        if super.performKeyEquivalent(with: event) {
            return true
        }
        if handleTrackDeleteCommand(event) {
            return true
        }
        return handleZoomCommand(event)
    }

    private var defersToTextInput: Bool {
        var responder = firstResponder
        while let current = responder {
            if current is NSTextView || current is NSTextField || current is NSSearchField {
                return true
            }
            if let inputClient = current as? NSTextInputClient,
               inputClient.hasMarkedText() {
                return true
            }
            responder = current.nextResponder
        }
        return false
    }

    private func handleUnmodifiedGlobalCommand(_ event: NSEvent) -> Bool {
        guard event.modifierFlags.intersection(.deviceIndependentFlagsMask).isEmpty else {
            return false
        }

        switch event.keyCode {
        case 49: // Space
            return invoke(onPlayStop)
        case 115: // Home
            return invoke(onRewind)
        case 51, 117: // Backspace/Delete and forward Delete
            guard shouldHandleClipDelete?() ?? true else { return false }
            return invoke(onDeleteSelectedClip)
        default:
            return false
        }
    }

    private func handleZoomCommand(_ event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        guard modifiers.contains(.command),
              modifiers.subtracting([.command, .shift]).isEmpty,
              let characters = event.charactersIgnoringModifiers else {
            return false
        }

        switch characters {
        case "+", "=":
            return invoke(onZoomIn)
        case "-", "_":
            return invoke(onZoomOut)
        case "0":
            return invoke(onZoomReset)
        default:
            return false
        }
    }

    /// Keep the unmodified Delete key scoped to clips.  Command-Delete is a
    /// distinct, explicitly reversible project command for the selected track.
    private func handleTrackDeleteCommand(_ event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        guard modifiers == [.command],
              event.keyCode == 51 || event.keyCode == 117 else {
            return false
        }
        return invoke(onDeleteSelectedTrack)
    }

    private func invoke(_ command: (() -> Void)?) -> Bool {
        guard let command else { return false }
        command()
        return true
    }
}
