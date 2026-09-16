import AppKit

/// Presentation state only: never owns project gain, routing or plugin data.
@MainActor
final class MixerConsoleState {
    enum Filter: Int { case all, tracks, buses }
    enum Density: Int {
        case compact, regular, wide
        var width: CGFloat {
            switch self {
            case .compact: return 84
            case .regular: return 110
            case .wide: return 138
            }
        }
    }
    enum RackMode: Int { case full, inserts, sends, faders }
    enum Zone { case scrolling, left, right }

    static let maximumPinnedChannels = 3

    var filter: Filter = .all
    var density: Density = .regular
    var rackMode: RackMode = .full
    var search = ""
    private(set) var hiddenIDs = Set<UInt64>()
    private(set) var leftPinnedIDs: [UInt64] = []
    private(set) var rightPinnedIDs: [UInt64] = []

    private func passesType(_ strip: MixerStripModel) -> Bool {
        filter == .all || (filter == .tracks && strip.kind == .track) || (filter == .buses && strip.kind == .bus)
    }

    private func passesSearch(_ strip: MixerStripModel) -> Bool {
        let query = search.trimmingCharacters(in: .whitespacesAndNewlines)
        return query.isEmpty || strip.title.localizedCaseInsensitiveContains(query) || strip.outputName.localizedCaseInsensitiveContains(query)
    }

    func visible(_ strips: [MixerStripModel]) -> [MixerStripModel] {
        strips.filter {
            $0.kind != .master && !hiddenIDs.contains($0.id) && !isPinned($0.id) && passesType($0) && passesSearch($0)
        }
    }

    /// Fixed meter bridge keeps project context while text search narrows the scrolling bank.
    func overview(_ strips: [MixerStripModel]) -> [MixerStripModel] {
        strips.filter { $0.kind != .master && !hiddenIDs.contains($0.id) && passesType($0) }
    }

    func pinned(_ strips: [MixerStripModel], in zone: Zone) -> [MixerStripModel] {
        let ids = zone == .left ? leftPinnedIDs : rightPinnedIDs
        let byID = Dictionary(uniqueKeysWithValues: strips.filter { $0.kind != .master }.map { ($0.id, $0) })
        return ids.compactMap { id in hiddenIDs.contains(id) ? nil : byID[id] }
    }

    func isHidden(_ id: UInt64) -> Bool { hiddenIDs.contains(id) }
    func isPinned(_ id: UInt64) -> Bool { leftPinnedIDs.contains(id) || rightPinnedIDs.contains(id) }
    func zone(of id: UInt64) -> Zone {
        if leftPinnedIDs.contains(id) { return .left }
        if rightPinnedIDs.contains(id) { return .right }
        return .scrolling
    }

    func setHidden(_ hidden: Bool, id: UInt64) {
        if hidden {
            hiddenIDs.insert(id)
            leftPinnedIDs.removeAll { $0 == id }
            rightPinnedIDs.removeAll { $0 == id }
        } else {
            hiddenIDs.remove(id)
        }
    }

    func toggleHidden(_ id: UInt64) { setHidden(!hiddenIDs.contains(id), id: id) }
    func showAll() { hiddenIDs.removeAll() }

    func showOnly(_ ids: Set<UInt64>, from strips: [MixerStripModel]) {
        let candidates = strips.filter { $0.kind != .master }.map(\.id)
        hiddenIDs = Set(candidates.filter { !ids.contains($0) })
        leftPinnedIDs.removeAll { hiddenIDs.contains($0) }
        rightPinnedIDs.removeAll { hiddenIDs.contains($0) }
    }

    func pin(_ id: UInt64, to zone: Zone) {
        leftPinnedIDs.removeAll { $0 == id }
        rightPinnedIDs.removeAll { $0 == id }
        hiddenIDs.remove(id)
        guard zone != .scrolling else { return }
        while leftPinnedIDs.count + rightPinnedIDs.count >= Self.maximumPinnedChannels {
            if !leftPinnedIDs.isEmpty { leftPinnedIDs.removeFirst() }
            else if !rightPinnedIDs.isEmpty { rightPinnedIDs.removeFirst() }
            else { break }
        }
        if zone == .left { leftPinnedIDs.append(id) }
        else { rightPinnedIDs.append(id) }
    }

    func clearPins(_ zone: Zone? = nil) {
        if zone == nil || zone == .left { leftPinnedIDs.removeAll() }
        if zone == nil || zone == .right { rightPinnedIDs.removeAll() }
    }

    func cleanup(validIDs: Set<UInt64>) {
        hiddenIDs.formIntersection(validIDs)
        leftPinnedIDs.removeAll { !validIDs.contains($0) }
        rightPinnedIDs.removeAll { !validIDs.contains($0) }
    }
}
