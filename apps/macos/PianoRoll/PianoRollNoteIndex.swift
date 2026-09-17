import Foundation

struct PRNoteIndex {
    private struct Row { var entries:[PRNoteEntity]; var maximumEnds:[UInt64] }
    private var rows:[UInt8:Row]=[:]
    init(_ entities:[PRNoteEntity]) { var buckets:[UInt8:[PRNoteEntity]]=[:];for e in entities{buckets[e.note.pitch,default:[]].append(e)};for(pitch,entries)in buckets{let sorted=entries.sorted{$0.note.startFrames==$1.note.startFrames ? $0.id<$1.id:$0.note.startFrames<$1.note.startFrames};var furthest:UInt64=0;let ends=sorted.map{e->UInt64 in let sum=e.note.startFrames.addingReportingOverflow(e.note.lengthFrames);furthest=max(furthest,sum.overflow ? UInt64.max:sum.partialValue);return furthest};rows[pitch]=Row(entries:sorted,maximumEnds:ends)} }
    func query(start:UInt64,end:UInt64,pitches:[Int])->[PRNoteEntity]{guard end>start else{return[]};var matches:[PRNoteEntity]=[];for pitch in pitches where (0...127).contains(pitch){guard let row=rows[UInt8(pitch)]else{continue};var low=0,high=row.entries.count;while low<high{let middle=(low+high)/2;if row.maximumEnds[middle]<=start{low=middle+1}else{high=middle}};var index=low;while index<row.entries.count{let e=row.entries[index],n=e.note;if n.startFrames>=end{break};let sum=n.startFrames.addingReportingOverflow(n.lengthFrames);if sum.overflow||sum.partialValue>start{matches.append(e)};index+=1}};return matches}
}
