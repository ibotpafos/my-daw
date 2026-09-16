import AppKit

@MainActor final class MixerConsoleState {
    enum Filter:String,CaseIterable { case all="All",tracks="Audio",buses="Bus" }
    enum Density:String,CaseIterable { case compact,regular,wide; var width:CGFloat { switch self{case .compact:return 72;case .regular:return 92;case .wide:return 120} } }
    var filter:Filter = .all; var density:Density = .regular; var search=""; var selectedID:UInt64?; var sendsOnFadersBusID:UInt64?
    func visible(_ strips:[MixerStripModel])->[MixerStripModel] {
        let q=search.trimmingCharacters(in:.whitespacesAndNewlines).lowercased()
        return strips.filter { s in
            let kind = filter == .all ? s.kind != .master : (filter == .tracks ? s.kind == .track : s.kind == .bus)
            return kind && (q.isEmpty || s.title.lowercased().contains(q) || s.outputName.lowercased().contains(q))
        }
    }
    func exitSendsOnFaders(){sendsOnFadersBusID=nil}
}
