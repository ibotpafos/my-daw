import Foundation

struct PRClipboard: Codable {
    static let typeIdentifier = "dev.mydaw.piano-roll-notes.v1"
    struct Note: Codable { var startBeat: Double; var lengthBeats: Double; var pitch: UInt8; var channel: UInt8; var velocity: UInt8 }
    var version = 1; var notes: [Note]
    var span: Double { notes.map { $0.startBeat + $0.lengthBeats }.max() ?? 0 }
    init(entities: [PRNoteEntity], selected: Set<UInt64>, map: PRTimeMap) { let selectedNotes=entities.filter{selected.contains($0.id)}.map(\.note);let origin=selectedNotes.map{map.start($0)}.min() ?? 0;notes=selectedNotes.map{Note(startBeat:map.start($0)-origin,lengthBeats:map.length($0),pitch:$0.pitch,channel:$0.channel,velocity:$0.velocity)} }
    func encoded() throws -> Data { try JSONEncoder().encode(self) }
    static func decode(_ data: Data) throws -> PRClipboard { guard !data.isEmpty,data.count<=PRLimits.clipboardBytes else{throw PREditError.invalidClipboard};guard let value=try? JSONDecoder().decode(PRClipboard.self,from:data),value.version==1,!value.notes.isEmpty,value.notes.count<=PRLimits.noteCount else{throw PREditError.invalidClipboard};for n in value.notes{guard n.startBeat.isFinite,n.startBeat>=0,n.lengthBeats.isFinite,n.lengthBeats>0,(n.startBeat+n.lengthBeats).isFinite,n.pitch<=127,n.channel<=15,(1...127).contains(n.velocity)else{throw PREditError.invalidClipboard}};return value }
    func inserted(into entities:[PRNoteEntity],at beat:Double,nextID:UInt64,map:PRTimeMap)throws->(entities:[PRNoteEntity],selection:Set<UInt64>){_ = try PREdits.checked(entities,map:map);try PREdits.validateFreshIDs(nextID,count:notes.count,in:entities);guard notes.count+entities.count<=PRLimits.noteCount else{throw PREditError.tooManyNotes};guard beat.isFinite,beat>=0,beat+span<=map.durationBeats+1e-8 else{throw PREditError.outsideClip};var output=entities,selection=Set<UInt64>(),id=nextID;for item in notes{let note=try map.note(start:beat+item.startBeat,end:beat+item.startBeat+item.lengthBeats,pitch:item.pitch,channel:item.channel,velocity:item.velocity);output.append(PRNoteEntity(id:id,note:note));selection.insert(id);id+=1};return(try PREdits.checked(output,map:map),selection)}
}
