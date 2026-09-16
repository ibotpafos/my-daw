import Foundation

@MainActor
extension PRProState {
    var hasTransformPreview: Bool {
        isGesturing && status.hasPrefix("Предпросмотр ·")
    }

    private func prepareTransformPreview() -> (Gesture, PRTimeMap)? {
        if isGesturing {
            guard hasTransformPreview, let gesture, let map = timeMap else { return nil }
            return (gesture, map)
        }
        guard beginGesture(), let gesture, let map = timeMap else { return nil }
        return (gesture, map)
    }

    private func previewBaseNextID(_ gesture: Gesture) throws -> UInt64 {
        let maximum = gesture.original.map(\.id).max() ?? 0
        let next = maximum.addingReportingOverflow(1)
        guard !next.overflow else { throw PREditError.invalidNote }
        return next.partialValue
    }

    func previewRatchet(count: Int, gate: Double) {
        guard let (gesture, map) = prepareTransformPreview() else {
            fail(PREditError.unavailable)
            return
        }
        do {
            let result = try PRTransforms.ratchet(gesture.original,
                                                  selected: gesture.originalSelection,
                                                  count: count,
                                                  gate: gate,
                                                  nextID: try previewBaseNextID(gesture),
                                                  map: map)
            selection = result.selection
            previewGesture(result.entities)
            status = "Предпросмотр · Ratchet ×\(count) · gate \(Int((gate * 100).rounded()))%"
            changed()
        } catch {
            cancelGesture()
            fail(error)
        }
    }

    func previewStrum(spreadBeats: Double, descending: Bool) {
        guard let (gesture, map) = prepareTransformPreview() else {
            fail(PREditError.unavailable)
            return
        }
        do {
            let result = try PRTransforms.strum(gesture.original,
                                                selected: gesture.originalSelection,
                                                spreadBeats: spreadBeats,
                                                descending: descending,
                                                map: map)
            selection = result.selection
            previewGesture(result.entities)
            status = "Предпросмотр · Strum \(descending ? "↓" : "↑") · \(String(format: "%.3f", spreadBeats)) beat"
            changed()
        } catch {
            cancelGesture()
            fail(error)
        }
    }

    func previewVelocityRamp(from: Int, to: Int) {
        guard let (gesture, map) = prepareTransformPreview() else {
            fail(PREditError.unavailable)
            return
        }
        do {
            let result = try PRTransforms.velocityRamp(gesture.original,
                                                       selected: gesture.originalSelection,
                                                       from: from,
                                                       to: to,
                                                       map: map)
            selection = result.selection
            previewGesture(result.entities)
            status = "Предпросмотр · Velocity \(from) → \(to)"
            changed()
        } catch {
            cancelGesture()
            fail(error)
        }
    }

    func applyTransformPreview() {
        guard hasTransformPreview else { return }
        finishGesture()
    }

    func cancelTransformPreview() {
        guard hasTransformPreview else { return }
        cancelGesture()
        status = "Предпросмотр преобразования отменён"
        changed()
    }
}
