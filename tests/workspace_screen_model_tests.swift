import Foundation

@main
struct WorkspaceScreenModelTests {
    static func main() throws {
        var checks = 0
        func expect(_ value: @autoclosure () -> Bool, _ message: String) {
            checks += 1
            precondition(value(), message)
        }
        let suite = "workspace-screen-model-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        var preference = WorkspaceLayout()
        preference.libraryWidth = 312; preference.inspectorWidth = 350
        preference.dockHeight = 310; preference.dockTab = .midi
        preference.save(to: defaults)
        let encodedLayout = defaults.data(forKey: WorkspaceLayout.defaultsKey)
        expect(WorkspaceScreen.load(from: defaults) == .arrange, "First launch opens arrangement")
        expect(WorkspaceScreen.allCases.map(\.rawValue) == [0, 1, 2, 3, 4, 5], "Stable screen IDs")
        expect(WorkspaceScreen.defaultsKey != WorkspaceLayout.defaultsKey, "Separate persistence")
        for screen in WorkspaceScreen.allCases {
            screen.save(to: defaults)
            expect(WorkspaceScreen.load(from: defaults) == screen, "Screen round trip")
            expect(defaults.data(forKey: WorkspaceLayout.defaultsKey) == encodedLayout, "Screen never overwrites pane sizes")
            expect(!screen.title.isEmpty && !screen.accessibilityTitle.isEmpty, "Named accessible destination")
            if let tab = screen.dockTab { expect(WorkspaceScreen(dockTab: tab) == screen, "Dock mapping is bijective") }
        }
        for invalid: Any in [-1, 6, 999, "unknown-screen", Data([0,1,2])] {
            defaults.set(invalid, forKey: WorkspaceScreen.defaultsKey)
            expect(WorkspaceScreen.load(from: defaults) == .arrange, "Invalid saved destination has a safe fallback")
        }
        let sizes: [Double] = [-100, 0, 1, 240, 469, 470, 520, 700, 1060, 1536, 1920, 4096, .nan, .infinity, -.infinity]
        for screen in WorkspaceScreen.allCases {
            for width in sizes {
                for height in sizes {
                    let g = screen.geometry(preference: preference, width: width, height: height)
                    let safeWidth = width.isFinite ? max(0, width) : 0
                    let safeHeight = height.isFinite ? max(0, height) : 0
                    let values = [g.library,g.center,g.inspector,g.arrangement,g.dock,g.gap]
                    expect(values.allSatisfy { $0.isFinite && $0 >= 0 }, "Finite nonnegative geometry")
                    let horizontalGaps = (g.library > 0 && g.center > 0 ? g.gap : 0) + (g.inspector > 0 ? g.gap : 0)
                    expect(abs(g.library + g.center + g.inspector + horizontalGaps - safeWidth) < 0.001, "Horizontal conservation")
                    if screen == .browser {
                        expect(g.library == safeWidth && g.center == 0 && g.dock == 0, "Dedicated browser fills its window")
                    } else {
                        let verticalGap = g.arrangement > 0 && g.dock > 0 ? g.gap : 0
                        expect(abs(g.arrangement + g.dock + verticalGap - safeHeight) < 0.001, "Vertical conservation")
                        if screen != .arrange {
                            expect(g.library == 0 && g.inspector == 0 && g.arrangement == 0 && g.dock == safeHeight, "Focused editor owns all space")
                        }
                    }
                }
            }
        }
        var mixer = preference; mixer.dockTab = .mixer
        let g = WorkspaceScreen.arrange.geometry(preference: mixer, width: 1536, height: 900)
        expect(g.dock >= 400 && g.arrangement >= 220, "Normal mixer/arrangement split remains usable")
        mixer.dockHeight = .nan
        expect(WorkspaceScreen.arrange.geometry(preference: mixer, width: 1536, height: 900).dock == 400, "Corrupt mixer dock height")
        expect(WorkspaceLayout.load(from: defaults) == preference, "Normal layout survives every screen operation")
        print("PASS: \(checks) workspace screen model assertions")
    }
}
