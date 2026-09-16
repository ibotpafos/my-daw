import AppKit

/// Presentation state only: never owns project gain, routing or plugin data.
@MainActor
final class MixerConsoleState {
    enum Filter: Int { case all, tracks, buses }
    enum Density: Int { case compact, regular, wide
        var width: CGFloat { switch self { case .compact: 84; case .regular: 110; case .wide: 138 } }
    }
    var filter: Filter = .all
    var density: Density = .regular
    var search = ""
    func visible(_ strips: [MixerStripModel]) -> [MixerStripModel] {
        let query = search.trimmingCharacters(in: .whitespacesAndNewlines)
        return strips.filter {
            $0.kind != .master && (filter == .all || (filter == .tracks && $0.kind == .track) || (filter == .buses && $0.kind == .bus)) &&
            (query.isEmpty || $0.title.localizedCaseInsensitiveContains(query) || $0.outputName.localizedCaseInsensitiveContains(query))
        }
    }
}
