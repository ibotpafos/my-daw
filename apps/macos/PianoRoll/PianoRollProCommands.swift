import AppKit

@MainActor
extension PRProState {
    func duplicateSelection() {
        guard let map = timeMap, !selection.isEmpty else { return }
        let clipboard = PRClipboard(entities: entities, selected: selection, map: map)
        let selected = selectedEntities
        let origin = selected.map { map.start($0.note) }.min() ?? insertionBeat
        let step = max(1.0 / 64, grid.step(pixelsPerBeat))
        let distance = max(step, ceil(clipboard.span / step - 1e-9) * step)
        pasteClipboard(clipboard, at: origin + distance)
    }

    func pasteClipboard(_ clipboard: PRClipboard, at beat: Double? = nil) {
        guard let map = timeMap, editable, !isGesturing else {
            fail(PREditError.unavailable)
            return
        }
        do {
            let result = try clipboard.inserted(into: entities,
                                                at: beat ?? insertionBeat,
                                                nextID: nextID,
                                                map: map)
            perform({ _, _ in result.entities }, selection: result.selection)
        } catch { fail(error) }
    }

    func copyToPasteboard() {
        guard let map = timeMap, !selection.isEmpty else { return }
        do {
            let data = try PRClipboard(entities: entities, selected: selection, map: map).encoded()
            NSPasteboard.general.clearContents()
            if NSPasteboard.general.setData(data, forType: NSPasteboard.PasteboardType(PRClipboard.typeIdentifier)) {
                status = "Скопировано нот: \(selection.count)"
                changed()
            }
        } catch { fail(error) }
    }

    func cutToPasteboard() {
        guard canPerformEdit, !selection.isEmpty else { return }
        guard let map = timeMap else { fail(PREditError.unavailable); return }
        do {
            let data = try PRClipboard(entities: entities, selected: selection, map: map).encoded()
            NSPasteboard.general.clearContents()
            guard NSPasteboard.general.setData(data, forType: NSPasteboard.PasteboardType(PRClipboard.typeIdentifier)) else { return }
            deleteSelected()
            // The host echo owns success/rejection status; do not overwrite it.
        } catch { fail(error) }
    }

    func pasteFromPasteboard() {
        guard let data = NSPasteboard.general.data(forType: NSPasteboard.PasteboardType(PRClipboard.typeIdentifier)) else {
            status = "В буфере нет нот My DAW"
            changed()
            return
        }
        do { pasteClipboard(try PRClipboard.decode(data)) }
        catch { fail(error) }
    }
}
