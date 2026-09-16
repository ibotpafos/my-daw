import AppKit

// Stable domain identities, not labels/positions, are carried through every menu.
enum MixerStripKind: Sendable { case track, bus, master }
struct MixerMeterSnapshot: Sendable, Equatable {
    var leftPeak: Float = 0, rightPeak: Float = 0, leftHold: Float = 0, rightHold: Float = 0
    var momentaryLufs: Float?
    var shortTermLufs: Float?
}
struct MixerInsertSummary: Sendable, Equatable {
    var name: String
    var bypassed = false
    var id: UInt64 = 0
    var available = true
    var latencyFrames: UInt32 = 0
}
struct MixerSendSummary: Sendable, Equatable {
    var destination: String
    var gainDb: Double = -12
    var preFader = false
    var busID: UInt64 = 0
}
struct MixerStripModel: Identifiable, Sendable, Equatable {
    var id: UInt64
    var kind: MixerStripKind
    var title: String
    var channelNumber = 0
    var color: NSColor? = nil
    var volumeDb: Double = 0, pan: Double = 0
    var meter = MixerMeterSnapshot()
    var outputName = "Main"
    var inserts: [MixerInsertSummary] = []
    var sends: [MixerSendSummary] = []
    var isSelected = false, isArmed = false, isMuted = false, isSolo = false, isAutomationRead = true
    var outputID: UInt64 = 0
    var hasMidi = false
    var automationLabel = "Read"

    var totalInsertLatencyFrames: UInt32 {
        inserts.reduce(UInt32(0)) { partial, insert in
            let (value, overflow) = partial.addingReportingOverflow(insert.latencyFrames)
            return overflow ? UInt32.max : value
        }
    }
    /// Project and renderer timing are fixed at 48 kHz in the current engine contract.
    var totalInsertLatencyMilliseconds: Double { Double(totalInsertLatencyFrames) / 48.0 }
    var hasUnavailableInsert: Bool { inserts.contains { !$0.available } }
}
enum MixerInsertAction { case add, edit(UInt64), bypass(UInt64, Bool), move(UInt64, Int), remove(UInt64) }
enum MixerSendAction { case add(UInt64), edit(UInt64), tap(UInt64, Bool), remove(UInt64) }
