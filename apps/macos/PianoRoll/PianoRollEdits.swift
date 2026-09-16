import Foundation

enum PREdits {
    static func validate(_ notes: [PianoRollNote], clipLength: UInt64) throws {
        guard clipLength > 0, clipLength <= PRLimits.timelineFrames else { throw PREditError.invalidClip }
        guard notes.count <= PRLimits.noteCount else { throw PREditError.tooManyNotes }
        for note in notes {
            guard note.pitch <= 127, note.channel <= 15, (1...127).contains(note.velocity), note.lengthFrames > 0 else { throw PREditError.invalidNote }
            guard note.lengthFrames <= PRLimits.noteFrames else { throw PREditError.noteTooLong }
            guard note.startFrames < clipLength, note.lengthFrames <= clipLength - note.startFrames else { throw PREditError.outsideClip }
        }
    }
    static func checked(_ entities: [PRNoteEntity], map: PRTimeMap) throws -> [PRNoteEntity] { try validate(entities.map(\.note), clipLength: map.clipLength); return entities }
    static func move(_ entities: [PRNoteEntity], selected: Set<UInt64>, beats: Double, semitones: Int, map: PRTimeMap) throws -> [PRNoteEntity] {
        let items = entities.filter { selected.contains($0.id) }; guard !items.isEmpty else { return entities }; guard beats.isFinite else { throw PREditError.invalidNote }
        let earliest = items.map { map.start($0.note) }.min()!, latest = items.map { map.end($0.note) }.max()!
        let shift = max(-earliest, min(map.durationBeats - latest, beats))
        let low = Int(items.map(\.note.pitch).min()!), high = Int(items.map(\.note.pitch).max()!), transpose = max(-low, min(127 - high, semitones))
        let result = try entities.map { entity -> PRNoteEntity in
            guard selected.contains(entity.id) else { return entity }; var changed = entity
            if abs(shift) > 1e-12 { changed.note = try map.note(start: map.start(entity.note)+shift, end: map.end(entity.note)+shift, pitch: entity.note.pitch, channel: entity.note.channel, velocity: entity.note.velocity) }
            changed.note.pitch = UInt8(Int(entity.note.pitch)+transpose); return changed
        }; return try checked(result, map: map)
    }
    enum Edge { case start, end }
    static func resize(_ entities: [PRNoteEntity], selected: Set<UInt64>, delta: Double, edge: Edge, map: PRTimeMap) throws -> [PRNoteEntity] {
        guard delta.isFinite else { throw PREditError.invalidNote }; let items = entities.filter { selected.contains($0.id) }; guard !items.isEmpty else { return entities }
        var lower = -Double.infinity, upper = Double.infinity
        for item in items { let n=item.note, start=map.start(n), end=map.end(n); switch edge {
            case .start: let minFrame=(n.startFrames+n.lengthFrames)>PRLimits.noteFrames ? n.startFrames+n.lengthFrames-PRLimits.noteFrames:0; lower=max(lower,map.beat(at:minFrame)-start); upper=min(upper,map.beat(at:n.startFrames+n.lengthFrames-1)-start)
            case .end: lower=max(lower,map.beat(at:n.startFrames+1)-end); let maxFrame=n.startFrames+min(PRLimits.noteFrames,map.clipLength-n.startFrames); upper=min(upper,map.beat(at:maxFrame)-end) } }
        let amount=max(lower,min(upper,delta)); return try checked(try entities.map { entity in guard selected.contains(entity.id), abs(amount)>1e-12 else { return entity }; var changed=entity; changed.note=try map.note(start:map.start(entity.note)+(edge == .start ? amount:0), end:map.end(entity.note)+(edge == .end ? amount:0), pitch:entity.note.pitch, channel:entity.note.channel, velocity:entity.note.velocity); return changed }, map:map)
    }
    static func velocity(_ entities:[PRNoteEntity], selected:Set<UInt64>, delta:Int)->[PRNoteEntity]{ let values=entities.filter{selected.contains($0.id)}.map{Int($0.note.velocity)}; guard let low=values.min(),let high=values.max() else{return entities}; let amount=max(1-low,min(127-high,delta)); return entities.map{e in guard selected.contains(e.id) else{return e}; var c=e;c.note.velocity=UInt8(Int(e.note.velocity)+amount);return c} }
    static func setVelocity(_ entities:[PRNoteEntity],selected:Set<UInt64>,value:Int)->[PRNoteEntity]{entities.map{e in guard selected.contains(e.id) else{return e};var c=e;c.note.velocity=UInt8(min(127,max(1,value)));return c}}
    static func quantize(_ entities:[PRNoteEntity],selected:Set<UInt64>,grid:PRGrid,pointsPerBeat:Double,strength:Double,map:PRTimeMap)throws->[PRNoteEntity]{guard strength.isFinite else{throw PREditError.invalidNote};var g=grid;g.enabled=true;return try checked(try entities.map{e in guard selected.contains(e.id) else{return e};let s=map.start(e.note),l=map.length(e.note),t=g.snap(s,origin:map.originBeat,pointsPerBeat:pointsPerBeat),n=max(0,min(map.durationBeats-l,s+(t-s)*min(1,max(0,strength))));guard abs(n-s)>1e-12 else{return e};var c=e;c.note=try map.note(start:n,end:n+l,pitch:e.note.pitch,channel:e.note.channel,velocity:e.note.velocity);return c},map:map)}
    static func snapPitch(_ entities:[PRNoteEntity],selected:Set<UInt64>,scale:PRScale)->[PRNoteEntity]{entities.map{e in guard selected.contains(e.id) else{return e};var c=e;c.note.pitch=scale.nearest(Int(e.note.pitch));return c}}
    static func split(_ entities:[PRNoteEntity],selected:Set<UInt64>,at frame:UInt64,nextID:UInt64,map:PRTimeMap)throws->[PRNoteEntity]{var output:[PRNoteEntity]=[],id=nextID;for entity in entities{let note=entity.note,end=note.startFrames+note.lengthFrames;guard selected.contains(entity.id),frame>note.startFrames,frame<end else{output.append(entity);continue};var left=entity;left.note.lengthFrames=frame-note.startFrames;output.append(left);var right=note;right.startFrames=frame;right.lengthFrames=end-frame;output.append(PRNoteEntity(id:id,note:right));id+=1};return try checked(output,map:map)}
    static func legato(_ entities:[PRNoteEntity],selected:Set<UInt64>,map:PRTimeMap)throws->[PRNoteEntity]{var following:[UInt64:UInt64]=[:],lanes:[Int:[PRNoteEntity]]=[:];for e in entities{lanes[Int(e.note.channel)*128+Int(e.note.pitch),default:[]].append(e)};for lane in lanes.values{let ordered=lane.sorted{$0.note.startFrames<$1.note.startFrames};var nextStart:UInt64?,previousStart:UInt64?,distinctNext:UInt64?;for e in ordered.reversed(){if previousStart != e.note.startFrames{distinctNext=nextStart};following[e.id]=distinctNext ?? map.clipLength;nextStart=e.note.startFrames;previousStart=e.note.startFrames}};return try checked(entities.map{e in guard selected.contains(e.id) else{return e};var c=e;let start=e.note.startFrames,desired=max(e.note.lengthFrames,(following[e.id] ?? map.clipLength)-start);c.note.lengthFrames=min(desired,min(PRLimits.noteFrames,map.clipLength-start));return c},map:map)}
    static func humanize(_ entities:[PRNoteEntity],selected:Set<UInt64>,timeBeats:Double,velocityAmount:Int,seed:UInt64,map:PRTimeMap)throws->[PRNoteEntity]{guard timeBeats.isFinite,timeBeats>=0 else{throw PREditError.invalidNote};var generator=PRRandom(seed:seed);return try checked(try entities.map{e in guard selected.contains(e.id) else{return e};let n=e.note,l=map.length(n),j=generator.signedUnit()*timeBeats,s=min(map.durationBeats-l,max(0,map.start(n)+j)),v=Int(n.velocity)+Int((generator.signedUnit()*Double(max(0,min(126,velocityAmount)))).rounded());var c=e;if abs(s-map.start(n))>1e-12{c.note=try map.note(start:s,end:s+l,pitch:n.pitch,channel:n.channel,velocity:UInt8(max(1,min(127,v))))}else{c.note.velocity=UInt8(max(1,min(127,v)))};return c},map:map)}
}

struct PRRandom {
    private var state: UInt64
    init(seed: UInt64) { state = seed }
    mutating func signedUnit() -> Double {
        state &+= 0x9e3779b97f4a7c15
        var z = state
        z = (z ^ (z >> 30)) &* 0xbf58476d1ce4e5b9
        z = (z ^ (z >> 27)) &* 0x94d049bb133111eb
        z ^= z >> 31
        return Double(z >> 11) / Double(UInt64(1) << 53) * 2 - 1
    }
}
