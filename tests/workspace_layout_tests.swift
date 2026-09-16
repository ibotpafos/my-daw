import Foundation

@main
struct WorkspaceLayoutTests {
    static func main() throws {
        var checks = 0
        func expect(_ condition: Bool, _ name: String) {
            checks += 1
            precondition(condition, name)
        }
        var layout = WorkspaceLayout()
        for width in stride(from: 0.0, through: 3000, by: 37) {
            for height in stride(from: 0.0, through: 1600, by: 43) {
                let g = layout.geometry(width: width, height: height)
                let total = g.library + g.center + g.inspector + (g.library > 0 ? g.gap : 0) + (g.inspector > 0 ? g.gap : 0)
                expect(abs(total - width) < 0.001, "Columns conserve width")
                expect(abs(g.arrangement + g.dock + (g.dock > 0 ? g.gap : 0) - height) < 0.001, "Dock conserves height")
                expect(g.center >= min(520, width), "Center never disappears behind sidebars")
                expect(g.dock == 0 || (g.dock >= 250 && g.arrangement >= 220), "Dock and arrangement minima")
            }
        }
        let regular = layout.geometry(width: 1536, height: 920)
        expect(regular.library == 236 && regular.inspector == 268 && regular.dock == 286, "Reference defaults")
        layout.libraryWidth = .nan; layout.inspectorWidth = .infinity; layout.dockHeight = -.infinity
        expect(layout.geometry(width: 1536, height: 920) == regular, "Invalid preferences sanitized")
        expect(layout.geometry(width: .nan, height: .infinity).center == 0, "Invalid container sanitized")
        layout = WorkspaceLayout()
        layout.toggle(.library); expect(layout.geometry(width: 1536, height: 920).library == 0, "Hide library")
        layout.toggle(.library); expect(layout.geometry(width: 1536, height: 920) == regular, "Restore library")
        layout.dockTab = .midi
        let name = "mydaw.workspace-test.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        layout.save(to: defaults); expect(WorkspaceLayout.load(from: defaults) == layout, "Preferences round trip")
        defaults.set(Data("not json".utf8), forKey: WorkspaceLayout.defaultsKey)
        expect(WorkspaceLayout.load(from: defaults) == WorkspaceLayout(), "Corrupt preference fallback")
        print("PASS: \(checks) bounded workspace layout assertions, persistence and corruption guards")
    }
}
