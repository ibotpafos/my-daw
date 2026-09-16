import Foundation

/// Window preference only; never part of the audio session or its Undo history.
struct WorkspaceLayout: Codable, Equatable {
    var libraryVisible = true
    var inspectorVisible = true
    var dockVisible = true
    var libraryWidth: Double = 236
    var inspectorWidth: Double = 268
    var dockHeight: Double = 286
    var dockTab: WorkspaceDockTab = .devices

    enum Pane: String { case library, inspector, dock }
    struct Geometry: Equatable {
        var library: Double
        var center: Double
        var inspector: Double
        var arrangement: Double
        var dock: Double
        var gap: Double
    }

    mutating func toggle(_ pane: Pane) {
        switch pane {
        case .library: libraryVisible.toggle()
        case .inspector: inspectorVisible.toggle()
        case .dock: dockVisible.toggle()
        }
    }

    func geometry(width: Double, height: Double, divider: Double = 1) -> Geometry {
        let width = width.isFinite ? max(0, width) : 0
        let height = height.isFinite ? max(0, height) : 0
        let gap = divider.isFinite ? min(8, max(0, divider)) : 1
        func bounded(_ value: Double, _ minimum: Double, _ maximum: Double, _ fallback: Double) -> Double {
            min(maximum, max(minimum, value.isFinite ? value : fallback))
        }
        var left = libraryVisible ? bounded(libraryWidth, 200, 340, 236) : 0
        var right = inspectorVisible ? bounded(inspectorWidth, 240, 380, 268) : 0
        let minimumCenter = 520.0
        // Compress sidebars before temporarily hiding either. Auto-collapse does
        // not change the stored preference: enlarging the window brings it back.
        var available = max(0, width - minimumCenter - (left > 0 ? gap : 0) - (right > 0 ? gap : 0))
        if left + right > available { left = left > 0 ? 200 : 0; right = right > 0 ? 240 : 0 }
        if left + right > available { left = 0 }
        available = max(0, width - minimumCenter - (right > 0 ? gap : 0))
        if right > available { right = 0 }
        let center = max(0, width - left - right - (left > 0 ? gap : 0) - (right > 0 ? gap : 0))
        let bottom = dockVisible && height >= 470 + gap ? bounded(dockHeight, 250, height - 220 - gap, 286) : 0
        return Geometry(library: left, center: center, inspector: right,
                        arrangement: max(0, height - bottom - (bottom > 0 ? gap : 0)), dock: bottom, gap: gap)
    }

    static let defaultsKey = "workspace.referenceLayout.v1"
    static func load(from defaults: UserDefaults) -> Self {
        guard let data = defaults.data(forKey: defaultsKey),
              let value = try? JSONDecoder().decode(Self.self, from: data) else { return Self() }
        return value
    }
    func save(to defaults: UserDefaults) {
        guard let data = try? JSONEncoder().encode(self) else { return }
        defaults.set(data, forKey: Self.defaultsKey)
    }
}

enum WorkspaceDockTab: Int, Codable, CaseIterable {
    case devices, midi, mixer
    var title: String {
        switch self { case .devices: return "Устройства"; case .midi: return "MIDI-редактор"; case .mixer: return "Микшер" }
    }
}
