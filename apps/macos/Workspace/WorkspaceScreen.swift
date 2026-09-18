import Foundation

/// Presentation destinations, not new audio/session models. Raw values are
/// stable UserDefaults values and are deliberately separate from workspace.mode.
enum WorkspaceScreen: Int, CaseIterable, Codable {
    case arrange = 0
    case pianoRoll = 1
    case mixer = 2
    case devices = 3
    case browser = 4
    case recording = 5

    var title: String {
        switch self {
        case .arrange: return "Проект"
        case .pianoRoll: return "MIDI"
        case .mixer: return "Микшер"
        case .devices: return "Эффекты"
        case .browser: return "Браузер"
        case .recording: return "Запись"
        }
    }
    var accessibilityTitle: String {
        switch self {
        case .arrange: return "Аранжировка"
        case .pianoRoll: return "Piano Roll — MIDI-редактор"
        case .mixer: return "Микшер"
        case .devices: return "Цепочка эффектов выбранного канала"
        case .browser: return "Библиотека аудио и установленных плагинов"
        case .recording: return "Запись вокала и сборка дублей"
        }
    }
    var dockTab: WorkspaceDockTab? {
        switch self {
        case .pianoRoll: return .midi
        case .mixer: return .mixer
        case .devices: return .devices
        case .arrange, .browser, .recording: return nil
        }
    }
    init(dockTab: WorkspaceDockTab) {
        switch dockTab {
        case .devices: self = .devices
        case .midi: self = .pianoRoll
        case .mixer: self = .mixer
        }
    }

    /// Focus never writes back synthetic zero widths or full-window dock heights
    /// to the normal layout. Even corrupt or not-yet-laid-out sizes are bounded.
    func geometry(preference: WorkspaceLayout, width: Double, height: Double,
                  divider: Double = 1) -> WorkspaceLayout.Geometry {
        if self == .arrange {
            var adapted = preference
            if adapted.dockTab == .mixer {
                adapted.dockHeight = adapted.dockHeight.isFinite ? max(adapted.dockHeight, 400) : 400
            }
            return adapted.geometry(width: width, height: height, divider: divider)
        }
        let width = width.isFinite ? max(0, width) : 0
        let height = height.isFinite ? max(0, height) : 0
        if self == .browser {
            return .init(library: width, center: 0, inspector: 0,
                         arrangement: 0, dock: 0, gap: 0)
        }
        return .init(library: 0, center: width, inspector: 0,
                     arrangement: 0, dock: height, gap: 0)
    }

    static let defaultsKey = "workspace.activeScreen.v1"
    static func load(from defaults: UserDefaults) -> Self {
        guard let raw = defaults.object(forKey: defaultsKey) as? Int,
              let screen = Self(rawValue: raw) else { return .arrange }
        return screen
    }
    func save(to defaults: UserDefaults) { defaults.set(rawValue, forKey: Self.defaultsKey) }
}
