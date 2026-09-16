import AppKit
import AVFoundation
import UniformTypeIdentifiers

let automationTrackVolume = Int32(DAW_AUTOMATION_TRACK_VOLUME)
let automationTrackPan = Int32(DAW_AUTOMATION_TRACK_PAN)
let automationBusGain = Int32(DAW_AUTOMATION_BUS_GAIN)
let automationMasterGain = Int32(DAW_AUTOMATION_MASTER_GAIN)
let automationTouch = Int32(DAW_AUTOMATION_TOUCH)
let automationLatch = Int32(DAW_AUTOMATION_LATCH)

enum BrowserPluginTarget {
    case audioUnit(type: UInt32, subtype: UInt32, manufacturer: UInt32)
    case vst3(index: UInt32)
}

/// UI-owned intent only. The C import job owns the decoded immutable audio and
/// never retains this session or an AppKit object.
enum BackgroundImportIntent {
    case track(path: URL, name: String)
    case take(path: URL, name: String, trackID: UInt64, startFrame: UInt64)
}

// MARK: - Темпо-карта проекта: Swift-зеркало доменного пересчёта beats↔frames
//
// Домен (engine/domain/session.cpp: State::beatsAtFrame/frameAtBeats) остаётся
// единственным источником истины; приложение только читает карту через C-ABI
// (daw_get_tempo_count/daw_get_tempo_point, daw_get_time_signature_count/
// daw_get_time_signature_point) и пишет её одной revision-aware командой
// daw_set_tempo. Формулы ниже — сверенное зеркало, а не молча скопированный код:
// на Apple arm64 `long double` — это 64-битный double, поэтому накопление
// сегментов в Double даёт побитово тот же результат, что и домен.
// Один бит = 60/bpm секунд при проектных 48 кГц, то есть 2 880 000 / bpm
// кадров; 120 BPM = 24 000 кадров на бит, 60 BPM = 48 000 кадров на бит.

/// Точка темпа из карты проекта (POD daw_tempo_point).
struct ProjectTempoPoint {
    var frame: UInt64
    var bpm: Double
}

/// Точка размера из карты проекта (POD daw_time_signature_point).
struct ProjectSignaturePoint {
    var frame: UInt64
    var numerator: UInt8
    var denominator: UInt8
}

/// Начало такта: кадр, позиция в битах и порядковый номер для линейки.
struct ProjectBarStart {
    var frame: UInt64
    var beats: Double
    var number: Int
}

struct BeatFrameMap {
    /// 48 000 кадров/с × 60 с — кадр/бит = framesPerMinute / bpm.
    static let framesPerMinute = 2_880_000.0
    static let timelineLimitFrame: UInt64 = 1 << 40
    static let ticksPerBeat = 480
    var tempo: [ProjectTempoPoint] = [ProjectTempoPoint(frame: 0, bpm: 120)]
    var signatures: [ProjectSignaturePoint] = [ProjectSignaturePoint(frame: 0, numerator: 4, denominator: 8)]

    /// Темп на позиции: последняя точка не позже кадра (карта действует last-holds).
    func tempoPoint(atFrame frame: UInt64) -> ProjectTempoPoint {
        var chosen = tempo[0]
        for candidate in tempo where candidate.frame <= frame { chosen = candidate }
        return chosen
    }
    func bpm(atFrame frame: UInt64) -> Double { tempoPoint(atFrame: frame).bpm }
    func framesPerBeat(atFrame frame: UInt64) -> Double { Self.framesPerMinute / bpm(atFrame: frame) }
    func signature(atFrame frame: UInt64) -> ProjectSignaturePoint {
        var chosen = signatures[0]
        for candidate in signatures where candidate.frame <= frame { chosen = candidate }
        return chosen
    }

    /// Зеркало State::beatsAtFrame: сумма целых сегментов, одно округление в конце.
    func beats(atFrame frame: UInt64) -> Double {
        let lane = tempo.sorted { $0.frame < $1.frame }
        var beats = 0.0
        var index = 0
        while index + 1 < lane.count, lane[index + 1].frame <= frame {
            beats += Double(lane[index + 1].frame - lane[index].frame) * lane[index].bpm / Self.framesPerMinute
            index += 1
        }
        beats += (frame > lane[index].frame ? Double(frame - lane[index].frame) : 0) * lane[index].bpm / Self.framesPerMinute
        return beats
    }

    /// Зеркало State::frameAtBeats: обратный ход по тем же сегментам.
    func frame(atBeats beats: Double) -> UInt64 {
        let lane = tempo.sorted { $0.frame < $1.frame }
        guard beats.isFinite, beats >= 0 else { return lane[0].frame }
        var remaining = beats
        var index = 0
        while true {
            let perBeat = Self.framesPerMinute / lane[index].bpm
            if index + 1 < lane.count {
                let segment = Double(lane[index + 1].frame - lane[index].frame) / perBeat
                if remaining == segment { return lane[index + 1].frame }
                if remaining > segment { remaining -= segment; index += 1; continue }
            }
            let value = Double(lane[index].frame) + remaining * perBeat
            guard value >= 0 else { return 0 }
            guard value < Double(Self.timelineLimitFrame) else { return Self.timelineLimitFrame }
            return UInt64(value.rounded())
        }
    }

    /// Такты: длина такта = numerator × 4 / denominator четвертных бит (бит домена — всегда
    /// четвертная): 4/8 → 2 бита, 4/4 → 4, 3/4 → 3. Размер читается из карты на кадре
    /// текущей границы, а сама точка смены размера всегда становится началом такта —
    /// при смене размера сетка тактов двигается.
    func barStarts(upToFrame limit: UInt64) -> [ProjectBarStart] {
        var marks: [ProjectBarStart] = []
        let changes = signatures.sorted { $0.frame < $1.frame }
        var beats = 0.0
        var frame: UInt64 = 0
        var number = 1
        while marks.count < 4096 {
            if frame > limit { break }
            marks.append(ProjectBarStart(frame: frame, beats: beats, number: number))
            let signature = self.signature(atFrame: frame)
            var nextBeats = beats + Double(max(1, Int(signature.numerator))) * 4 / Double(max(1, Int(signature.denominator)))
            var nextFrame = self.frame(atBeats: nextBeats)
            if let change = changes.first(where: { $0.frame > frame && $0.frame <= nextFrame }) {
                nextBeats = self.beats(atFrame: change.frame)
                nextFrame = change.frame
            }
            guard nextFrame > frame else { break }
            beats = nextBeats
            frame = nextFrame
            number += 1
        }
        return marks
    }

    /// 'такт.бит.тики' по позиции; тики — доли бита (480 на бит).
    func barBeatTick(atFrame frame: UInt64, bars: [ProjectBarStart]) -> String {
        let beats = max(0, self.beats(atFrame: frame))
        var anchor = ProjectBarStart(frame: 0, beats: 0, number: 1)
        for mark in bars where mark.beats <= beats + 1e-9 { anchor = mark }
        let offset = max(0, beats - anchor.beats)
        let whole = floor(offset)
        let tick = min(Self.ticksPerBeat - 1, Int((offset - whole) * Double(Self.ticksPerBeat)))
        return String(format: "%d.%d.%03d", anchor.number, Int(whole) + 1, max(0, tick))
    }
}

@MainActor
final class DraftCanvas: NSView { override var isFlipped: Bool { true } }

@MainActor
final class AutomationSlider: NSSlider {
    var automationBegin: (() -> Void)?
    var automationEnd: (() -> Void)?
    override func mouseDown(with event: NSEvent) {
        automationBegin?()
        super.mouseDown(with: event)
        automationEnd?()
    }
}

@MainActor
/// Маркер-локатор проекта (POD daw_marker): позиция на общей шкале 48 кГц и имя.
struct ProjectMarker {
    let frame: UInt64
    let name: String
}

final class TimelineRulerView: NSView {
    var projectFrames: UInt64 = 48000 * 12 { didSet { needsDisplay = true } }
    var playhead: UInt64 = 0 { didSet { needsDisplay = true } }
    /// Тактовые метки из темпо-карты проекта; пустой массив — прежняя секундная шкала без второго слоя.
    var barMarks: [ProjectBarStart] = [] { didSet { needsDisplay = true } }
    /// Маркеры-локаторы: отдельная верхняя полоса флажков (домен v18, мост v1).
    var markers: [ProjectMarker] = [] { didSet { needsDisplay = true } }
    var onMarkerSeek: ((UInt64) -> Void)?
    var onMarkerAdd: ((UInt64) -> Void)?
    var onMarkerMenu: ((ProjectMarker) -> NSMenu?)?
    /// Высота верхней полосы под номера тактов; секундная шкала остаётся ниже.
    static let barBand: CGFloat = 14
    /// Полоса флажков маркеров над тактовой полосой.
    static let markerBand: CGFloat = 16
    override var isFlipped: Bool { true }
    override func draw(_ dirtyRect: NSRect) {
        NSColor(white: 0.07, alpha: 1).setFill();bounds.fill();let frames=max(UInt64(1),projectFrames);let seconds=Double(frames)/48000;let raw=max(1,seconds/Double(max(1,Int(bounds.width/110))));let step:Double=raw <= 1 ? 1:(raw <= 2 ? 2:(raw <= 5 ? 5:10));let attributes:[NSAttributedString.Key:Any]=[.font:NSFont.monospacedDigitSystemFont(ofSize:10,weight:.regular),.foregroundColor:NSColor.secondaryLabelColor]
        // Второй слой: номера тактов из темпо-карты в верхней полосе. Горизонтальный
        // маппинг кадр↔пиксель тот же, что у секундной шкалы; меняется только то,
        // что метки приходят из карты, а не из локального темпа.
        // Верхняя полоса: флажки маркеров с именами и вертикальная линия на всю линейку.
        if !markers.isEmpty {
            NSColor(white:1,alpha:0.05).setFill();NSBezierPath(rect:NSRect(x:0,y:0,width:bounds.width,height:Self.markerBand)).fill()
            let markerAttributes:[NSAttributedString.Key:Any]=[.font:NSFont.systemFont(ofSize:9,weight:.medium),.foregroundColor:NSColor.systemYellow.withAlphaComponent(0.92)]
            for marker in markers {
                if marker.frame > frames { continue }
                let x=CGFloat(Double(marker.frame)/Double(frames))*bounds.width
                NSColor.systemYellow.withAlphaComponent(0.45).setStroke();let drop=NSBezierPath();drop.move(to:NSPoint(x:x,y:7));drop.line(to:NSPoint(x:x,y:bounds.height));drop.lineWidth=1;drop.stroke()
                NSColor.systemYellow.setFill();let flag=NSBezierPath();flag.move(to:NSPoint(x:x-4,y:0));flag.line(to:NSPoint(x:x+4,y:0));flag.line(to:NSPoint(x:x,y:7));flag.close();flag.fill()
                marker.name.draw(at:NSPoint(x:x+6,y:6),withAttributes:markerAttributes)
            }
        }
        if !barMarks.isEmpty {
            let barAttributes:[NSAttributedString.Key:Any]=[.font:NSFont.monospacedDigitSystemFont(ofSize:9,weight:.medium),.foregroundColor:NSColor.systemMint.withAlphaComponent(0.86)]
            NSColor(white:1,alpha:0.08).setFill();NSBezierPath(rect:NSRect(x:0,y:Self.markerBand,width:bounds.width,height:Self.barBand)).fill()
            NSColor(white:1,alpha:0.14).setStroke();let divider=NSBezierPath();divider.move(to:NSPoint(x:0,y:Self.markerBand+Self.barBand));divider.line(to:NSPoint(x:bounds.width,y:Self.markerBand+Self.barBand));divider.stroke()
            var lastLabelX:CGFloat = -60
            for mark in barMarks {
                if mark.frame > frames { break }
                let x=CGFloat(Double(mark.frame)/Double(frames))*bounds.width
                NSColor(white:1,alpha:0.26).setStroke();let tick=NSBezierPath();tick.move(to:NSPoint(x:x,y:Self.markerBand+Self.barBand-9));tick.line(to:NSPoint(x:x,y:Self.markerBand+Self.barBand));tick.stroke()
                guard x-lastLabelX >= 20 else { continue }
                lastLabelX=x
                String(mark.number).draw(at:NSPoint(x:x+3,y:Self.markerBand+1),withAttributes:barAttributes)
            }
        }
        let baseline=Self.markerBand+Self.barBand
        var second:Double=0;while second<=seconds {let x=CGFloat(second/seconds)*bounds.width;NSColor(white:1,alpha:0.16).setStroke();let line=NSBezierPath();line.move(to:NSPoint(x:x,y:bounds.height-8));line.line(to:NSPoint(x:x,y:bounds.height));line.stroke();String(format:"%.0f",second).draw(at:NSPoint(x:x+3,y:baseline+2),withAttributes:attributes);second+=step};let cursor=CGFloat(Double(min(playhead,frames))/Double(frames))*bounds.width;NSColor.systemMint.setStroke();let cursorLine=NSBezierPath();cursorLine.move(to:NSPoint(x:cursor,y:0));cursorLine.line(to:NSPoint(x:cursor,y:bounds.height));cursorLine.stroke()
    }
}

@MainActor
final class DraftApp: NSObject, NSApplicationDelegate, NSWindowDelegate, NSTextFieldDelegate, NSSplitViewDelegate {
    var session: OpaquePointer!
    var window: DAWWindow!
    var revision: UInt64 = 0
    var savedRevision: UInt64 = 0
    var currentURL: URL?
    var closeApproved = false
    var leavePromptActive = false
    var saveJob: OpaquePointer?
    var saveURL: URL?
    var saveCompletion: (() -> Void)?
    var saveStarted = Date()
    var saveError: String?
    var exportJob: OpaquePointer?
    var dawprojectJob: OpaquePointer?
    var exportURL: URL?
    var exportStarted = Date()
    var exportMessage: String?
    var exportMessageUntil = Date.distantPast
    var importJob: OpaquePointer?
    var importIntent: BackgroundImportIntent?
    var importSession: OpaquePointer?
    var importBaseRevision: UInt64 = 0
    var importStatus: daw_import_status?
    var importExistingTrackIDs = Set<UInt64>()
    var importMessage: String?
    var importMessageUntil = Date.distantPast
    var projectMessage: String?
    var projectMessageUntil = Date.distantPast
    let cancelImportButton = NSButton(title: "Отменить импорт", target: nil, action: nil)
    let resolveImportButton = NSButton(title: "", target: nil, action: nil)
    var auScanJob: OpaquePointer?
    var auScanTimer: Timer?
    var vst3ScanJob: OpaquePointer?
    var vst3ScanTimer: Timer?
    var recoveryJob: OpaquePointer?
    var recoveryJobURL: URL?
    var recoveryRoot: URL?
    var recordingRoot: URL?
    var auCacheURL: URL?
    var activeRecordingURL: URL?
    var recoveryURL: URL?
    var recoveredFrom: URL?
    var retiredRecovery = Set<URL>()
    var recoveredRevision: UInt64?
    var nextRecovery = Date()
    var recoveryError: String?
    let rows = NSStackView()
    let trackHeaderRows = NSStackView()
    let consoleRows = NSStackView()
    let mixerWorkspace = MixerWorkspaceView(frame: .zero)
    let mixerSummary = NSTextField(labelWithString: "0 CH · 0 BUS · MASTER")
    let timelineRuler = TimelineRulerView(frame: .zero)
    var timelineDocument: DraftCanvas!
    var timelineScroll: NSScrollView!
    var trackHeaderScroll: NSScrollView!
    var synchronizingArrangementScroll = false
    var timelineWidthConstraint: NSLayoutConstraint?
    var timelineZoom: CGFloat = 1
    weak var trackTimelineSplit: NSSplitView?
    weak var arrangementInspectorSplit: NSSplitView?
    weak var arrangementConsoleSplit: NSSplitView?
    weak var consoleView: NSView?
    weak var consoleDetailsScroll: NSScrollView?
    var consoleDetailsVisible = false
    var restoringWorkspaceLayout = false
    var workspaceLayoutReady = false
    let inspectorBrowser = InspectorBrowserView(frame: .zero)
    var inspectorTrackID: UInt64?
    var inspectorClipIndex: Int?
    var midiClipIndex: Int?
    var midiNotesCache: [PianoRollNote] = []
    var browserAudioURLs: [UUID: URL] = [:]
    var browserPluginTargets: [UUID: BrowserPluginTarget] = [:]
    let audioPreview = AudioPreviewController()
    var mixerKinds: [UInt64: MixerStripKind] = [:]
    var selectedMixerID: UInt64?
    var meterHolds: [UInt64: (left: Float, right: Float)] = [:]
    let status = NSTextField(labelWithString: "")
    let summary = NSTextField(labelWithString: "")
    let transportLabel = NSTextField(labelWithString: "Импортируй WAV/AIFF, чтобы услышать проект")
    let workspaceMode = NSSegmentedControl(labels: ["Создание", "Запись", "Сведение", "Мастеринг"], trackingMode: .selectOne, target: nil, action: nil)
    let playButton = NSButton(title: "▶ Играть", target: nil, action: nil)
    let stopButton = NSButton(title: "■ Стоп", target: nil, action: nil)
    let recordButton = NSButton(title: "● Запись", target: nil, action: nil)
    let exportButton = NSButton(title: "Экспорт WAV…", target: nil, action: nil)
    let dawprojectButton = NSButton(title: "Экспорт DAWproject…", target: nil, action: nil)
    let cancelExportButton = NSButton(title: "Отменить экспорт", target: nil, action: nil)
    let loopButton = NSButton(title: "↻ Цикл", target: nil, action: nil)
    let rangeLabel = NSTextField(labelWithString: "Диапазон: весь проект")
    let tempoLabel = NSTextField(labelWithString: "120 BPM")
    let tempoStepper = NSStepper()
    let tempoField = NSTextField(string: "120")
    let gridPopup = NSPopUpButton()
    let gridLabel = NSTextField(labelWithString: "Сетка: 1/8 @ 120")
    let positionLabel = NSTextField(labelWithString: "1.1.000")
    let masterSlider = AutomationSlider(value: 0, minValue: -120, maxValue: 24, target: nil, action: nil)
    let masterLabel = NSTextField(labelWithString: "+0.0 dB")
    let masterAutomationButton = NSButton(title:"AUTO",target:nil,action:nil)
    let masterAUPopup = NSPopUpButton()
    let scanAUButton = NSButton(title:"Сканировать AU",target:nil,action:nil)
    let masterVST3Popup = NSPopUpButton()
    let scanVST3Button = NSButton(title:"Сканировать VST3",target:nil,action:nil)
    let automationModePopup = NSPopUpButton()
    let automationArmPopup = NSPopUpButton()
    var automationMode: Int32 = 0 // 0 Read, 1 Touch, 2 Latch
    var automationArm: (target: Int32, id: UInt64)?
    var consoleGesture: (automation: Bool, revision: UInt64)?
    var automationGesture: (target: Int32, id: UInt64)?
    var automationTargets: [(target: Int32, id: UInt64, title: String)] = []
    var rangeStart: UInt64?
    var rangeEnd: UInt64?
    var loopEnabled = false
    // Темпо-карта проекта — единственный источник истины о темпе и размере; UI её
    // только читает (reloadTempoMap) и пишет одной командой daw_set_tempo.
    var tempoMap = BeatFrameMap()
    var tempoBars: [ProjectBarStart] = []
    var playheadFrame: UInt64 = 0
    /// Темп под позицией воспроизведения (последняя точка карты ≤ playhead).
    var tempo: Double { tempoMap.bpm(atFrame: playheadFrame) }
    /// Деления сетки в бит: выкл, 1/1, 1/2, 1/4, 1/8, 1/16, 1/32.
    let gridDivisions: [Double] = [0, 4, 2, 1, 0.5, 0.25, 0.125]
    var transportTimer: Timer?
    var hasAudio = false
    var hasMidiContent = false
    var waveforms: [WaveformView] = []
    var isPlaying = false
    var isRecording = false
    var recordingNumber = 1
    let undoButton = NSButton(title: "Отменить", target: nil, action: nil)
    let redoButton = NSButton(title: "Повторить", target: nil, action: nil)
    var trackIDs: [Int: UInt64] = [:]
    var trackNames: [UInt64: String] = [:]
    /// Собственный буфер обмена клипами: (дорожка-источник, индекс клипа, MIDI ли).
    var clipClipboard: (trackID: UInt64, index: Int, isMidi: Bool)?
    var recordPrerollFrames: UInt64 = 0
    func applyStoredPreroll(_ session:OpaquePointer?){
        let seconds=min(30,max(0,UserDefaults.standard.double(forKey:"transport.prerollSeconds.v1")))
        recordPrerollFrames=UInt64((seconds*48000).rounded())
        if let session { daw_set_record_preroll(session,recordPrerollFrames) }
    }
    var laneViews: [UInt64: WaveformView] = [:]
    var clipSelection: [UInt64: [Int]] = [:]
    var orderedBuses: [(id: UInt64, name: String)] = []
    var outputTargets: [ObjectIdentifier: (id: UInt64, isBus: Bool)] = [:]
    var busControlTargets: [ObjectIdentifier: UInt64] = [:]
    var busAutomationTargets: [ObjectIdentifier: UInt64] = [:]
    var busNameTargets: [ObjectIdentifier: UInt64] = [:]
    var newSendTargets: [ObjectIdentifier: UInt64] = [:]
    var sendControlTargets: [ObjectIdentifier: (track: UInt64, bus: UInt64, gain: Double, pre: Bool)] = [:]
    var auCatalog: [(type: UInt32, subtype: UInt32, manufacturer: UInt32, name: String)] = []
    var vst3Catalog: [(index: UInt32, name: String, vendor: String, available: Bool, instrument: Bool)] = []
    var pluginControlTargets: [ObjectIdentifier: (id: UInt64, bypassed: Bool, index: UInt32)] = [:]
    var pluginEditorTargets: [ObjectIdentifier: (id: UInt64, isolatedVST3: Bool)] = [:]
    var pluginParameterTargets: [ObjectIdentifier: (plugin: UInt64, parameter: UInt32)] = [:]
    var expandedInsertOwners = Set<String>()
    var insertDisclosureTargets: [ObjectIdentifier: (owner: Int32, ownerID: UInt64, title: String)] = [:]
    var insertControlTargets: [ObjectIdentifier: (owner: Int32, ownerID: UInt64, id: UInt64, bypassed: Bool, index: UInt32)] = [:]
    var insertEditorTargets: [ObjectIdentifier: (owner: Int32, ownerID: UInt64, id: UInt64, isolatedVST3: Bool)] = [:]
    var insertHostingTargets: [ObjectIdentifier: (owner: Int32, ownerID: UInt64, id: UInt64)] = [:]
    var insertRuntimeBadges: [(label: NSTextField, policy: NSTextField, selectedMode: UInt32, owner: Int32, ownerID: UInt64, id: UInt64)] = []
    var insertParameterTargets: [ObjectIdentifier: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32)] = [:]
    var parameterAutomationTargets: [ObjectIdentifier: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String, normalized: Double)] = [:]
    var armedPluginParameter: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String)?
    var pluginParameterGesture: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32)?
    var pluginParameterGestureTargets: [ObjectIdentifier: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String, minimum: Double, maximum: Double)] = [:]
    var pluginParameterArmTargets: [ObjectIdentifier: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String)] = [:]
    var selectedClips: [UInt64: Int] = [:]
    var selectedTakes:[UInt64:Int]=[:]
    var takePopups:[Int:NSPopUpButton]=[:]
    var armedTrackID:UInt64?
    // Метроном: состояние держит мост (daw_set_metronome), кнопка в панели —
    // только его отражение; syncMetronomeState() перечитывает мост после
    // любой команды, способной пересоздать граф.
    var metronomeOn = false
    let metronomeButton = NSButton(title: "", target: nil, action: nil)
    // Мониторинг входа: прямой моно-сигнал микрофона в оба канала во время
    // луп-записи; флаг живёт в мосте как метроном, в проект не пишется.
    var recordMonitorOn = false
    let recordMonitorButton = NSButton(title: "MON", target: nil, action: nil)
    // Автоматический мониторинг при вооружении дорожки: по умолчанию включён.
    var autoMonitorOnArm = true
    let autoMonitorButton = NSButton(title: "AUTO", target: nil, action: nil)
    // MIDI-захват: открыт ли вход и идёт ли тейк — вопросы моста, здесь кэш
    // показаний daw_midi_input_active/daw_midi_record_status для инспектора.
    var midiInputID: UInt32 = 0
    var midiTakeArmed = false
    var midiTakeTrackID: UInt64?
    var midiInputCache: [InspectorMidiInputOption] = []
    var dirty: Bool { revision != savedRevision }
    var exportBusy: Bool { exportJob != nil || dawprojectJob != nil }
    var importBusy: Bool { importJob != nil }
    let draftType = UTType(exportedAs: "dev.mydaw.draft", conformingTo: .data)
    let dawprojectType = UTType(filenameExtension: "dawproject") ?? .data

    func label(_ text: String, size: CGFloat, color: NSColor = .labelColor) -> NSTextField {
        let field = NSTextField(labelWithString: text)
        field.font = size <= 11 ? DAWDesignTokens.Typography.caption : (size >= 17 ? DAWDesignTokens.Typography.title : DAWDesignTokens.Typography.body)
        field.textColor = color
        return field
    }
    func button(_ title: String, _ action: Selector) -> NSButton {
        let b = NSButton(title: title, target: self, action: action)
        b.bezelStyle = .texturedRounded
        b.font = DAWDesignTokens.Typography.label
        b.contentTintColor = DAWDesignTokens.Color.text
        return b
    }
    func iconButton(_ icon: DAWIcon, _ action: Selector) -> NSButton {
        let b = button("", action)
        b.image = icon.symbol
        b.imagePosition = .imageOnly
        b.toolTip = icon.accessibilityLabel
        b.setAccessibilityLabel(icon.accessibilityLabel)
        b.widthAnchor.constraint(equalToConstant: 30).isActive = true
        return b
    }
    func styleIconButton(_ button: NSButton, icon: DAWIcon) {
        button.image = icon.symbol
        button.imagePosition = .imageOnly
        button.title = ""
        button.toolTip = icon.accessibilityLabel
        button.setAccessibilityLabel(icon.accessibilityLabel)
        if !button.constraints.contains(where: { $0.firstAttribute == .width }) {
            button.widthAnchor.constraint(equalToConstant: 32).isActive = true
        }
    }
    func updateRecordButton(_ recording: Bool) {
        styleIconButton(recordButton, icon: recording ? .stop : .record)
        recordButton.contentTintColor = DAWDesignTokens.Color.coral
        let label = recording ? "Закончить запись" : DAWIcon.record.accessibilityLabel
        recordButton.toolTip = label
        recordButton.setAccessibilityLabel(label)
    }
    func flexibleSpace() -> NSView {
        let view=NSView();view.setContentHuggingPriority(.defaultLow,for:.horizontal);view.setContentCompressionResistancePriority(.defaultLow,for:.horizontal);return view
    }
    func trackAccent(_ index:Int) -> NSColor { [.systemBlue,.systemTeal,.systemOrange,.systemPurple,.systemRed][index % 5] }
    /// Durable v19 color wins; the positional palette remains the fallback.
    func durableTrackColor(_ hex:UInt64,_ index:Int) -> NSColor { hex != 0 ? dawColorFromHex(UInt32(hex)) : trackAccent(index) }
    func durableTrackColor(_ hex:UInt32,_ index:Int) -> NSColor { hex != 0 ? dawColorFromHex(hex) : trackAccent(index) }
    func setTimelineZoom(_ zoom:CGFloat){timelineZoom=min(8,max(1,zoom));timelineWidthConstraint?.constant=1400*timelineZoom;UserDefaults.standard.set(Double(timelineZoom),forKey:"timelineZoom");timelineDocument?.needsLayout=true;timelineRuler.needsDisplay=true}
    @objc func zoomIn(){setTimelineZoom(timelineZoom*2)}
    @objc func zoomOut(){setTimelineZoom(timelineZoom/2)}
    @objc func resetZoom(){setTimelineZoom(1)}
    @objc func changeWorkspaceMode() {
        let titles = ["Создание", "Запись", "Сведение", "Мастеринг"]
        guard workspaceMode.selectedSegment >= 0, workspaceMode.selectedSegment < titles.count else { return }
        UserDefaults.standard.set(workspaceMode.selectedSegment, forKey: "workspace.mode")
        applyWorkspaceMode(workspaceMode.selectedSegment)
        status.stringValue = "Режим: \(titles[workspaceMode.selectedSegment])"
    }
    func fitTrackHeaderWidth() {
        guard let trackTimelineSplit else { return }
        let stored = CGFloat(UserDefaults.standard.double(forKey: "workspace.trackHeaderRatio.v3"))
        let ratio: CGFloat = stored > 0.05 && stored < 0.5 ? stored : 0.16
        trackTimelineSplit.setPosition(min(260, max(195, trackTimelineSplit.bounds.width * ratio)), ofDividerAt: 0)
    }
    @objc func toggleConsoleDetails(_ sender: NSButton) {
        consoleDetailsVisible.toggle()
        consoleDetailsScroll?.isHidden = !consoleDetailsVisible
        sender.title = consoleDetailsVisible ? "Скрыть детали" : "Детали канала"
        sender.setAccessibilityLabel(consoleDetailsVisible ? "Скрыть детали выбранного канала" : "Показать детали выбранного канала")
        arrangementConsoleSplit?.adjustSubviews()
        window.contentView?.layoutSubtreeIfNeeded()
    }
    func applyWorkspaceMode(_ mode: Int) {
        mixerWorkspace.resetFocus()
        arrangementInspectorSplit?.isHidden = false
        guard let arrangementInspectorSplit, let arrangementConsoleSplit else { return }
        restoringWorkspaceLayout = true
        defer { restoringWorkspaceLayout = false }
        let arrangementFirst = mode < 2
        inspectorBrowser.isHidden = arrangementFirst
        consoleView?.isHidden = arrangementFirst
        arrangementInspectorSplit.adjustSubviews()
        arrangementConsoleSplit.adjustSubviews()
        window.contentView?.layoutSubtreeIfNeeded()
        guard !arrangementFirst else { fitTrackHeaderWidth(); return }
        let height = arrangementConsoleSplit.bounds.height
        let modeKey = mode == 3 ? "mastering" : "mixing"
        let savedHeightRatio = CGFloat(UserDefaults.standard.double(forKey: "workspace.\(modeKey).arrangementHeightRatio"))
        let heightRatio: CGFloat = savedHeightRatio > 0.25 && savedHeightRatio < 0.75 ? savedHeightRatio : 0.50
        let arrangementHeight = min(max(220, height - 250), max(220, height * heightRatio))
        arrangementConsoleSplit.setPosition(arrangementHeight, ofDividerAt: 0)
        let savedArrangementRatio = CGFloat(UserDefaults.standard.double(forKey: "workspace.\(modeKey).arrangementRatio"))
        let arrangementRatio: CGFloat = savedArrangementRatio > 0.5 && savedArrangementRatio < 0.9 ? savedArrangementRatio : (mode == 3 ? 0.70 : 0.73)
        arrangementInspectorSplit.setPosition(max(420, arrangementInspectorSplit.bounds.width * arrangementRatio), ofDividerAt: 0)
        fitTrackHeaderWidth()
        if mode == 2, selectedMixerID == nil || selectedMixerID == 0 {
            if let firstTrackID = trackIDs[0] {
                selectedMixerID = firstTrackID; inspectorTrackID = firstTrackID; inspectorClipIndex = nil
                refresh(); updateMixerInspector(firstTrackID)
            } else {
                selectedMixerID = nil; inspectorTrackID = nil; inspectorClipIndex = nil
                inspectorBrowser.channel = nil; inspectorBrowser.clip = nil
            }
        }
        if mode == 3 {
            selectedMixerID = 0; inspectorTrackID = nil; inspectorClipIndex = nil
            refresh(); updateMixerInspector(0)
        }
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        DAWLog.lifecycle.info("Запуск My DAW \(DAWLog.buildStamp, privacy: .public)")
        guard let core = daw_create() else {
            DAWLog.lifecycle.critical("daw_create вернул nullptr: сессия не создана, приложение закрывается")
            NSApp.terminate(nil); return
        }
        applyStoredPreroll(core)
        session = core
        NSApp.appearance = NSAppearance(named: .darkAqua)
        installMenu()
        window = DAWWindow(contentRect: NSRect(x: 0, y: 0, width: 1240, height: 800),
            styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.minSize = NSSize(width: 1060, height: 620)
        window.delegate = self
        window.isReleasedWhenClosed = false
        window.backgroundColor = DAWDesignTokens.Color.canvas
        window.onPlayStop = { [weak self] in guard let self else{return};self.isPlaying ? self.stopAudio():self.playAudio() }
        window.onRewind = { [weak self] in self?.rewindAudio() }
        window.onDeleteSelectedClip = { [weak self] in self?.deleteCurrentSelectedClip() }
        window.onDeleteSelectedTrack = { [weak self] in self?.deleteCurrentSelectedTrack() }
        window.onZoomIn = { [weak self] in self?.zoomIn() };window.onZoomOut = { [weak self] in self?.zoomOut() };window.onZoomReset = { [weak self] in self?.resetZoom() }
        let root = NSView(); window.contentView = root
        let content = NSStackView(); content.orientation = .vertical; content.alignment = .leading; content.spacing = DAWDesignTokens.Space.sm
        content.translatesAutoresizingMaskIntoConstraints = false; root.addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: DAWDesignTokens.Space.md),
            content.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -DAWDesignTokens.Space.md),
            content.topAnchor.constraint(equalTo: root.topAnchor, constant: DAWDesignTokens.Space.sm),
            content.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -DAWDesignTokens.Space.sm)
        ])
        summary.font = .monospacedSystemFont(ofSize: 10, weight: .medium); summary.textColor = DAWDesignTokens.Color.secondaryText
        workspaceMode.selectedSegment = max(0, min(3, UserDefaults.standard.integer(forKey: "workspace.mode"))); workspaceMode.target = self; workspaceMode.action = #selector(changeWorkspaceMode); workspaceMode.controlSize = .small
        undoButton.target = self; undoButton.action = #selector(undo)
        redoButton.target = self; redoButton.action = #selector(redo)
        // Значение степпера и поля — из темпо-карты под позицией воспроизведения; локального
        // «настроенного темпа» больше нет, любое изменение уходит в daw_set_tempo.
        tempoStepper.minValue = 21; tempoStepper.maxValue = 999; tempoStepper.increment = 1; tempoStepper.integerValue = Int(tempo.rounded())
        tempoStepper.target = self; tempoStepper.action = #selector(changeTempo(_:));tempoStepper.setAccessibilityLabel("Темп темпо-карты под позицией воспроизведения, BPM");tempoStepper.setAccessibilityHelp("Добавляет или заменяет точку темпа на позиции воспроизведения. Темпо-карта допускает значения свыше 20 и не выше 999 BPM.")
        tempoField.alignment = .right; tempoField.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular); tempoField.placeholderString = "BPM"
        tempoField.delegate = self; tempoField.target = self; tempoField.action = #selector(commitTempoField(_:)); tempoField.tag = -1
        tempoField.setAccessibilityLabel("Поле темпа темпо-карты, BPM"); tempoField.setAccessibilityHelp("Введи значение от 20 до 999 BPM; Enter пишет точку темпа на позицию воспроизведения")
        let tempoFieldWidth=tempoField.widthAnchor.constraint(equalToConstant:54);tempoFieldWidth.priority = .defaultHigh;tempoFieldWidth.isActive=true
        gridPopup.addItems(withTitles: ["Сетка выкл.", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32"]); gridPopup.selectItem(at: 4)
        gridPopup.target = self; gridPopup.action = #selector(changeGrid(_:));gridPopup.setAccessibilityLabel("Деление сетки таймлайна в битах темпо-карты")
        gridLabel.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular); gridLabel.textColor = .secondaryLabelColor
        timelineRuler.setAccessibilityLabel("Линейка проекта: флажки маркеров сверху, затем такты из темпо-карты и секунды"); timelineRuler.setAccessibilityHelp("Клик по флажку — переход к маркеру. Двойной клик по полосе маркеров — добавить маркер. Правый клик по флажку — переименовать или удалить")
        timelineRuler.onMarkerSeek = { [weak self] frame in self?.seekAudio(frame) }
        timelineRuler.onMarkerAdd = { [weak self] frame in self?.promptAddMarker(frame) }
        timelineRuler.onMarkerMenu = { [weak self] marker in self?.markerMenu(marker) }
        gridLabel.setAccessibilityLabel("Сетка таймлайна: деление и темп под позицией воспроизведения")
        rangeLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular); rangeLabel.textColor = .secondaryLabelColor
        tempoLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular); tempoLabel.textColor = .secondaryLabelColor
        let importButton=button("Импорт…",#selector(importWav));let addTrackButton=button("＋ Track",#selector(addTrack));let addMidiTrackButton=button("＋ MIDI",#selector(addMidiTrack));let addBusButton=button("＋ Bus",#selector(addBus));let workflowButton=button("Workflow…",#selector(runVocalWorkflow))
        loopButton.target=self;loopButton.action=#selector(toggleLoop)
        let rangeStartButton=button("In",#selector(setRangeStart));let rangeEndButton=button("Out",#selector(setRangeEnd));let clearRangeButton=button("Очистить",#selector(clearRange))
        masterSlider.target=self;masterSlider.action=#selector(changeMasterGain(_:));masterSlider.isContinuous=true;masterSlider.widthAnchor.constraint(equalToConstant:120).isActive=true;masterSlider.setAccessibilityLabel("Уровень мастера, децибелы")
        masterSlider.automationBegin = { [weak self] in self?.beginAutomation(target: automationMasterGain, id: 0, value: self?.masterSlider.doubleValue ?? 0) }
        masterSlider.automationEnd = { [weak self] in self?.endAutomationGesture() }
        masterAutomationButton.target=self;masterAutomationButton.action=#selector(editMasterAutomation)
        masterLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular);masterLabel.textColor = .secondaryLabelColor;masterLabel.widthAnchor.constraint(equalToConstant:62).isActive=true
        masterAUPopup.addItem(withTitle:"＋ Master AU…");masterAUPopup.target=self;masterAUPopup.action=#selector(addMasterAU(_:));masterAUPopup.widthAnchor.constraint(equalToConstant:190).isActive=true
        scanAUButton.target=self;scanAUButton.action=#selector(scanInstalledAudioUnits)
        masterVST3Popup.addItem(withTitle:"＋ Master VST3…");masterVST3Popup.target=self;masterVST3Popup.action=#selector(addMasterVST3(_:));masterVST3Popup.widthAnchor.constraint(equalToConstant:210).isActive=true
        scanVST3Button.target=self;scanVST3Button.action=#selector(scanInstalledVST3)
        automationModePopup.addItems(withTitles:["AUTO Read","AUTO Touch","AUTO Latch"]);automationModePopup.selectItem(at:0);automationModePopup.target=self;automationModePopup.action=#selector(changeAutomationMode(_:));automationModePopup.widthAnchor.constraint(equalToConstant:105).isActive=true
        automationArmPopup.addItem(withTitle:"ARM: none");automationArmPopup.target=self;automationArmPopup.action=#selector(changeAutomationArm(_:));automationArmPopup.widthAnchor.constraint(equalToConstant:170).isActive=true
        exportButton.title="WAV…";exportButton.toolTip="Экспортировать микс WAV";dawprojectButton.title="DAWproject…";cancelExportButton.title="Отмена";cancelExportButton.toolTip="Отменить экспорт"
        cancelImportButton.title = "Отменить импорт"; cancelImportButton.toolTip = "Отменить фоновый импорт WAV"; cancelImportButton.setAccessibilityLabel("Отменить импорт WAV"); cancelImportButton.setAccessibilityHelp("Отменяет текущую фоновую загрузку WAV, не меняя проект")
        resolveImportButton.toolTip = "Продолжить готовый импорт WAV"; resolveImportButton.setAccessibilityLabel("Продолжить готовый импорт WAV"); resolveImportButton.setAccessibilityHelp("Повторно запускает готовый импорт для текущей ревизии проекта")
        updateRecordButton(false); styleIconButton(playButton, icon: .play); styleIconButton(stopButton, icon: .stop); styleIconButton(loopButton, icon: .loop); styleIconButton(undoButton, icon: .undo); styleIconButton(redoButton, icon: .redo)
        styleIconButton(metronomeButton, icon: .metronome)
        recordMonitorButton.setButtonType(.toggle)
        recordMonitorButton.font = .systemFont(ofSize: 10, weight: .semibold)
        recordMonitorButton.target = self; recordMonitorButton.action = #selector(toggleRecordMonitor(_:))
        recordMonitorButton.toolTip = "Мониторинг входа: прямой сигнал микрофона в наушники во время записи"
        recordMonitorButton.setAccessibilityLabel("Кнопка мониторинга входа")
        recordMonitorButton.setAccessibilityHelp("Включает слышимость входа только во время луп-записи: моно-сигнал подаётся в оба канала с unity-уровнем до эффектов и без затухания. Одиночный входной захват его игнорирует.")
        autoMonitorButton.setButtonType(.toggle)
        autoMonitorButton.font = .systemFont(ofSize: 10, weight: .semibold)
        autoMonitorButton.target = self; autoMonitorButton.action = #selector(toggleAutoMonitorOnArm(_:))
        autoMonitorButton.toolTip = "Авто-мониторинг: при вооружении дорожки вход автоматически слышен"
        autoMonitorButton.setAccessibilityLabel("Кнопка авто-мониторинга")
        autoMonitorButton.setAccessibilityHelp("Когда включено, вооружение дорожки автоматически включает мониторинг входа (MON). Позволяет сразу слышать микрофон при начале записи.")
        syncAutoMonitorButton()
        metronomeButton.target = self; metronomeButton.action = #selector(toggleMetronome(_:)); metronomeButton.setButtonType(.toggle)
        metronomeButton.toolTip = "Метроном: клик только в мониторинге, в экспорт не попадает"; metronomeButton.setAccessibilityHelp("Переключает клик метронома в живом звуке. Флаг принадлежит сессии, поэтому следующий play подхватит его без повтора. В проект не сохраняется.")
        styleIconButton(importButton, icon: .importAudio);styleIconButton(addTrackButton, icon: .addTrack);styleIconButton(addBusButton, icon: .addBus);styleIconButton(workflowButton, icon: .workflow)
        styleIconButton(rangeStartButton, icon: .rangeStart);styleIconButton(rangeEndButton, icon: .rangeEnd);styleIconButton(clearRangeButton, icon: .clearRange)
        let openButton=button("Открыть…",#selector(openDraft));styleIconButton(openButton,icon:.openProject)
        let saveButton=button("Сохранить",#selector(saveDraft));styleIconButton(saveButton,icon:.saveProject)
        styleIconButton(exportButton,icon:.exportAudio);styleIconButton(dawprojectButton,icon:.exportProject);styleIconButton(cancelExportButton,icon:.cancel)
        let toolbar=NSStackView(views:[workspaceMode,importButton,addTrackButton,addMidiTrackButton,addBusButton,workflowButton,undoButton,redoButton,label("RANGE",size:9,color:.tertiaryLabelColor),rangeStartButton,rangeEndButton,clearRangeButton,flexibleSpace(),tempoLabel,tempoStepper,tempoField,gridPopup,openButton,saveButton,exportButton,dawprojectButton,cancelExportButton,resolveImportButton,cancelImportButton]);toolbar.alignment = .centerY;toolbar.spacing=5;toolbar.edgeInsets=NSEdgeInsets(top:6,left:8,bottom:6,right:8);toolbar.wantsLayer=true;toolbar.layer?.backgroundColor=DAWDesignTokens.Color.surface.cgColor;toolbar.layer?.cornerRadius=DAWDesignTokens.Radius.card
        rangeLabel.setContentCompressionResistancePriority(.defaultLow,for:.horizontal)
        content.addArrangedSubview(toolbar);toolbar.widthAnchor.constraint(equalTo:content.widthAnchor).isActive=true
        playButton.target = self; playButton.action = #selector(playAudio)
        stopButton.target = self; stopButton.action = #selector(stopAudio); stopButton.isEnabled = false
        recordButton.target = self; recordButton.action = #selector(toggleRecording)
        recordButton.contentTintColor = DAWDesignTokens.Color.coral
        exportButton.target = self; exportButton.action = #selector(exportMix)
        dawprojectButton.target = self; dawprojectButton.action = #selector(exportDawproject)
        cancelExportButton.target = self; cancelExportButton.action = #selector(cancelExport); cancelExportButton.isEnabled = false; cancelExportButton.isHidden = true
        cancelImportButton.target = self; cancelImportButton.action = #selector(cancelImport); cancelImportButton.isEnabled = false; cancelImportButton.isHidden = true
        resolveImportButton.target = self; resolveImportButton.action = #selector(resolveReadyImport); resolveImportButton.isEnabled = false; resolveImportButton.isHidden = true
        transportLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular)
        transportLabel.textColor = DAWDesignTokens.Color.mint
        let scroll = NSScrollView();timelineScroll=scroll;scroll.hasVerticalScroller = true;scroll.hasHorizontalScroller=true; scroll.drawsBackground = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        rows.orientation = .vertical; rows.alignment = .leading; rows.spacing = 8
        rows.translatesAutoresizingMaskIntoConstraints = false
        let document = DraftCanvas(); timelineDocument=document;document.translatesAutoresizingMaskIntoConstraints = false
        document.addSubview(timelineRuler);document.addSubview(rows); scroll.documentView = document
        timelineZoom=CGFloat(UserDefaults.standard.double(forKey:"timelineZoom"));if timelineZoom < 1{timelineZoom=1}
        timelineWidthConstraint=document.widthAnchor.constraint(equalToConstant:1400*timelineZoom);timelineWidthConstraint?.isActive=true
        timelineRuler.translatesAutoresizingMaskIntoConstraints=false;timelineRuler.heightAnchor.constraint(equalToConstant:TimelineRulerView.markerBand+TimelineRulerView.barBand+28).isActive=true
        NSLayoutConstraint.activate([
            document.widthAnchor.constraint(greaterThanOrEqualTo: scroll.contentView.widthAnchor),timelineRuler.leadingAnchor.constraint(equalTo:document.leadingAnchor),timelineRuler.trailingAnchor.constraint(equalTo:document.trailingAnchor),timelineRuler.topAnchor.constraint(equalTo:document.topAnchor),
            rows.leadingAnchor.constraint(equalTo: document.leadingAnchor), rows.trailingAnchor.constraint(equalTo: document.trailingAnchor),
            rows.topAnchor.constraint(equalTo: timelineRuler.bottomAnchor), rows.bottomAnchor.constraint(equalTo: document.bottomAnchor)
        ])
        let headerScroll=NSScrollView();trackHeaderScroll=headerScroll;headerScroll.hasVerticalScroller=false;headerScroll.hasHorizontalScroller=false;headerScroll.drawsBackground=false
        trackHeaderRows.orientation = .vertical;trackHeaderRows.alignment = .leading;trackHeaderRows.spacing=8;trackHeaderRows.translatesAutoresizingMaskIntoConstraints=false
        let headerDocument=DraftCanvas();headerDocument.translatesAutoresizingMaskIntoConstraints=false;let tracksHeading=label("TRACKS",size:10,color:.tertiaryLabelColor);tracksHeading.font = .systemFont(ofSize:10,weight:.semibold);tracksHeading.translatesAutoresizingMaskIntoConstraints=false;headerDocument.addSubview(tracksHeading);headerDocument.addSubview(trackHeaderRows);headerScroll.documentView=headerDocument
        NSLayoutConstraint.activate([headerDocument.widthAnchor.constraint(equalTo:headerScroll.contentView.widthAnchor),tracksHeading.leadingAnchor.constraint(equalTo:headerDocument.leadingAnchor,constant:10),tracksHeading.trailingAnchor.constraint(lessThanOrEqualTo:headerDocument.trailingAnchor,constant:-8),tracksHeading.topAnchor.constraint(equalTo:headerDocument.topAnchor),tracksHeading.heightAnchor.constraint(equalToConstant:TimelineRulerView.markerBand+TimelineRulerView.barBand+28),trackHeaderRows.leadingAnchor.constraint(equalTo:headerDocument.leadingAnchor),trackHeaderRows.trailingAnchor.constraint(equalTo:headerDocument.trailingAnchor),trackHeaderRows.topAnchor.constraint(equalTo:tracksHeading.bottomAnchor),trackHeaderRows.bottomAnchor.constraint(equalTo:headerDocument.bottomAnchor)])
        scroll.contentView.postsBoundsChangedNotifications=true;headerScroll.contentView.postsBoundsChangedNotifications=true
        NotificationCenter.default.addObserver(self,selector:#selector(syncArrangementScroll(_:)),name:NSView.boundsDidChangeNotification,object:scroll.contentView)
        NotificationCenter.default.addObserver(self,selector:#selector(syncArrangementScroll(_:)),name:NSView.boundsDidChangeNotification,object:headerScroll.contentView)
        let trackTimelineSplit=NSSplitView();self.trackTimelineSplit=trackTimelineSplit;trackTimelineSplit.delegate=self;trackTimelineSplit.isVertical=true;trackTimelineSplit.dividerStyle = .thin;trackTimelineSplit.addArrangedSubview(headerScroll);trackTimelineSplit.addArrangedSubview(scroll)
        consoleRows.orientation = .vertical;consoleRows.alignment = .leading;consoleRows.spacing=6;consoleRows.translatesAutoresizingMaskIntoConstraints=false
        let consoleDocument=DraftCanvas();consoleDocument.translatesAutoresizingMaskIntoConstraints=false;consoleDocument.addSubview(consoleRows)
        let consoleScroll=NSScrollView();self.consoleDetailsScroll=consoleScroll;consoleScroll.hasVerticalScroller=true;consoleScroll.drawsBackground=false;consoleScroll.documentView=consoleDocument;consoleScroll.isHidden=true
        NSLayoutConstraint.activate([consoleDocument.widthAnchor.constraint(equalTo:consoleScroll.contentView.widthAnchor),consoleRows.leadingAnchor.constraint(equalTo:consoleDocument.leadingAnchor),consoleRows.trailingAnchor.constraint(equalTo:consoleDocument.trailingAnchor),consoleRows.topAnchor.constraint(equalTo:consoleDocument.topAnchor),consoleRows.bottomAnchor.constraint(equalTo:consoleDocument.bottomAnchor)])
        let console=NSStackView();self.consoleView=console;console.orientation = .vertical;console.alignment = .leading;console.distribution = .fill;console.spacing=4;console.wantsLayer=true;console.layer?.backgroundColor=DAWDesignTokens.Color.canvas.withAlphaComponent(0.72).cgColor;console.layer?.cornerRadius=DAWDesignTokens.Radius.card
        let mixerHeading=label("MIXER CONSOLE",size:10,color:.tertiaryLabelColor);mixerHeading.font = .systemFont(ofSize:10,weight:.semibold);mixerHeading.setAccessibilityLabel("Консоль микшера")
        mixerSummary.font = .monospacedSystemFont(ofSize:10,weight:.medium);mixerSummary.textColor = DAWDesignTokens.Color.secondaryText;mixerSummary.setAccessibilityLabel("Состав консоли микшера")
        let consoleDetailsButton=button("Детали канала",#selector(toggleConsoleDetails(_:)));consoleDetailsButton.setAccessibilityLabel("Показать детали выбранного канала");consoleDetailsButton.setAccessibilityHelp("Показывает routing, sends и inserts выбранного канала под консолью.")
        let consoleHeader=NSStackView(views:[mixerHeading,mixerSummary,flexibleSpace(),consoleDetailsButton]);consoleHeader.spacing=10;consoleHeader.alignment = .centerY;consoleHeader.edgeInsets=NSEdgeInsets(top:5,left:9,bottom:4,right:8);consoleHeader.wantsLayer=true;consoleHeader.layer?.backgroundColor=DAWDesignTokens.Color.surface.withAlphaComponent(0.90).cgColor
        console.addArrangedSubview(consoleHeader);console.addArrangedSubview(mixerWorkspace);console.addArrangedSubview(consoleScroll)
        mixerWorkspace.widthAnchor.constraint(equalTo:console.widthAnchor).isActive=true;consoleScroll.widthAnchor.constraint(equalTo:console.widthAnchor).isActive=true
        let mixerMinimumHeight=mixerWorkspace.heightAnchor.constraint(greaterThanOrEqualToConstant:300);mixerMinimumHeight.priority = .defaultHigh;mixerMinimumHeight.isActive=true;consoleScroll.heightAnchor.constraint(equalToConstant:180).isActive=true
        mixerWorkspace.setContentHuggingPriority(.defaultLow,for:.vertical);mixerWorkspace.setContentCompressionResistancePriority(.defaultLow,for:.vertical)
        mixerWorkspace.toolTip="Горизонтальная консоль: inserts, sends, routing, pan, meter и fader. Выбери канал для Inspector; используй горизонтальную прокрутку для остальных полос."
        mixerWorkspace.setAccessibilityLabel("Консоль микшера: горизонтальные полосы каналов")
        mixerWorkspace.setAccessibilityHelp("Каждая полоса содержит inserts, sends, выход, панораму, meter и fader. Track, bus и master визуально разделены.")
        inspectorBrowser.translatesAutoresizingMaskIntoConstraints=false
        inspectorBrowser.widthAnchor.constraint(greaterThanOrEqualToConstant:240).isActive=true
        inspectorBrowser.widthAnchor.constraint(lessThanOrEqualToConstant:380).isActive=true
        let arrangementSplit=NSSplitView();self.arrangementInspectorSplit=arrangementSplit;arrangementSplit.delegate=self;arrangementSplit.isVertical=true;arrangementSplit.dividerStyle = .thin;arrangementSplit.addArrangedSubview(trackTimelineSplit);arrangementSplit.addArrangedSubview(inspectorBrowser)
        let split=NSSplitView();self.arrangementConsoleSplit=split;split.delegate=self;split.isVertical=false;split.dividerStyle = .thin;split.addArrangedSubview(arrangementSplit);split.addArrangedSubview(console);content.addArrangedSubview(split);split.widthAnchor.constraint(equalTo:content.widthAnchor).isActive=true;split.heightAnchor.constraint(greaterThanOrEqualToConstant:430).isActive=true
        status.font = .systemFont(ofSize: 11); status.textColor = .secondaryLabelColor;status.lineBreakMode = .byTruncatingTail
        let transportControls=NSStackView(views:[recordButton,iconButton(.rewind,#selector(rewindAudio)),stopButton,playButton,loopButton,metronomeButton,recordMonitorButton,autoMonitorButton]);transportControls.spacing=4;transportControls.alignment = .centerY
        let statusBar=NSStackView(views:[summary,status,gridLabel,flexibleSpace(),transportControls,transportLabel,positionLabel,flexibleSpace(),rangeLabel]);statusBar.spacing=8;statusBar.alignment = .centerY;statusBar.edgeInsets=NSEdgeInsets(top:4,left:6,bottom:4,right:6);statusBar.wantsLayer=true;statusBar.layer?.backgroundColor=DAWDesignTokens.Color.surface.cgColor;statusBar.layer?.cornerRadius=DAWDesignTokens.Radius.control;content.addArrangedSubview(statusBar);statusBar.widthAnchor.constraint(equalTo:content.widthAnchor).isActive=true
        summary.setContentCompressionResistancePriority(.defaultLow,for:.horizontal);status.setContentCompressionResistancePriority(.defaultLow,for:.horizontal);transportLabel.setContentCompressionResistancePriority(.defaultHigh,for:.horizontal);rangeLabel.setContentCompressionResistancePriority(.defaultLow,for:.horizontal)
        gridLabel.setContentCompressionResistancePriority(.defaultLow,for:.horizontal)
        positionLabel.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular); positionLabel.textColor = DAWDesignTokens.Color.secondaryText
        positionLabel.setAccessibilityLabel("Позиция воспроизведения: такт.бит.тики"); positionLabel.setAccessibilityHelp("Формат такт.бит.тики по темпо-карте проекта, 480 тиков на бит")
        positionLabel.setContentCompressionResistancePriority(.defaultHigh,for:.horizontal)
        mixerWorkspace.onSelect = { [weak self] id in guard let self else{return};self.selectedMixerID=id;self.refresh();self.updateMixerInspector(id) }
        mixerWorkspace.onArm = { [weak self] id,armed in guard let self, self.mixerKinds[id] == .track else{return};self.armedTrackID=armed ? id:nil;self.refresh() }
        mixerWorkspace.onMute = { [weak self] id,muted in self?.mixerSetMute(id,muted) }
        mixerWorkspace.onSolo = { [weak self] id,solo in self?.mixerSetSolo(id,solo) }
        mixerWorkspace.onVolumeGestureBegin = { [weak self] id in self?.mixerBeginVolume(id) }
        mixerWorkspace.onVolume = { [weak self] id,value in self?.mixerSetVolume(id,value) }
        mixerWorkspace.onVolumeGestureEnd = { [weak self] _,_ in self?.mixerEndVolume() }
        mixerWorkspace.onPan = { [weak self] id,value in self?.mixerSetPan(id,value) }
        mixerWorkspace.onDeleteBus = { [weak self] id in self?.deleteBusWithConfirmation(id) }
        configureMixerConsole()
        wireInspectorBrowser()
        setupRecovery()
        loadSupportedAudioUnits()
        loadInstalledVST3()
        refreshBrowserCatalog()
        transportTimer = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in
            // Дрен MIDI-ring живёт в этом же цикле: транспорт, метры и тейк
            // опрашиваются одной 10 Гц-проверкой, отдельного таймера нет.
            MainActor.assumeIsolated { self?.pollTransport(); self?.pollMeters(); self?.pollMidiCapture(); self?.pollStorage() }
        }
        if let timer = transportTimer { RunLoop.main.add(timer, forMode: .common) }
        refresh(); window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        DispatchQueue.main.async { [weak self] in self?.restoreWorkspaceLayout() }
        DispatchQueue.main.async { [weak self] in self?.offerRecovery() }
    }

    func restoreWorkspaceLayout() {
        guard let trackTimelineSplit,let arrangementInspectorSplit,let arrangementConsoleSplit else{return}
        window.contentView?.layoutSubtreeIfNeeded();restoringWorkspaceLayout=true;defer{restoringWorkspaceLayout=false}
        func ratio(_ key:String,_ fallback:CGFloat)->CGFloat { let value=CGFloat(UserDefaults.standard.double(forKey:key));return value > 0.05 && value < 0.95 ? value:fallback }
        let headerWidth=min(260,max(195,trackTimelineSplit.bounds.width*ratio("workspace.trackHeaderRatio.v3",0.16)))
        trackTimelineSplit.setPosition(headerWidth,ofDividerAt:0)
        let inspectorWidth=min(360,max(260,arrangementInspectorSplit.bounds.width*(1-ratio("workspace.arrangementRatio",0.75))))
        arrangementInspectorSplit.setPosition(max(420,arrangementInspectorSplit.bounds.width-inspectorWidth),ofDividerAt:0)
        let arrangementHeight=min(max(220,arrangementConsoleSplit.bounds.height-250),max(220,arrangementConsoleSplit.bounds.height*ratio("workspace.arrangementHeightRatio",0.50)))
        arrangementConsoleSplit.setPosition(arrangementHeight,ofDividerAt:0)
        applyWorkspaceMode(workspaceMode.selectedSegment)
        workspaceLayoutReady = true
        layoutAudit("restore")
    }

    func splitViewDidResizeSubviews(_ notification: Notification) {
        guard workspaceLayoutReady,!restoringWorkspaceLayout,let split=notification.object as? NSSplitView,split.arrangedSubviews.count>1 else{return}
        let total=split.isVertical ? split.bounds.width:split.bounds.height;guard total>1 else{return}
        let first=split.arrangedSubviews[0].frame
        let ratio=(split.isVertical ? first.maxX:first.maxY)/total
        if split === trackTimelineSplit { UserDefaults.standard.set(Double(ratio),forKey:"workspace.trackHeaderRatio.v3") }
        else if split === arrangementInspectorSplit, workspaceMode.selectedSegment >= 2 {
            let modeKey = workspaceMode.selectedSegment == 3 ? "mastering" : "mixing"
            UserDefaults.standard.set(Double(ratio),forKey:"workspace.\(modeKey).arrangementRatio")
        }
        else if split === arrangementConsoleSplit, workspaceMode.selectedSegment >= 2 {
            let modeKey = workspaceMode.selectedSegment == 3 ? "mastering" : "mixing"
            UserDefaults.standard.set(Double(ratio),forKey:"workspace.\(modeKey).arrangementHeightRatio")
        }
        layoutAudit("divider")
    }

    func windowDidResize(_ notification: Notification) { layoutAudit("resize") }

    func splitView(_ splitView: NSSplitView, constrainMinCoordinate proposedMinimumPosition: CGFloat, ofSubviewAt dividerIndex: Int) -> CGFloat {
        if splitView === trackTimelineSplit{return 195}
        if splitView === arrangementInspectorSplit{return 420}
        if splitView === arrangementConsoleSplit{return 220}
        return proposedMinimumPosition
    }

    func splitView(_ splitView: NSSplitView, constrainMaxCoordinate proposedMaximumPosition: CGFloat, ofSubviewAt dividerIndex: Int) -> CGFloat {
        if splitView === trackTimelineSplit{return min(260,splitView.bounds.width-420)}
        if splitView === arrangementInspectorSplit{return splitView.bounds.width-240}
        if splitView === arrangementConsoleSplit{return splitView.bounds.height-250}
        return proposedMaximumPosition
    }
    func installMenu() {
        let main = NSMenu()
        func menu(_ title: String, _ items: [(String, Selector, String, Bool)]) {
            let root = NSMenuItem(); root.title = title
            let sub = NSMenu(title: title)
            for (title, action, key, shift) in items {
                let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
                item.target = self; item.keyEquivalentModifierMask = shift ? [.command, .shift] : [.command]
                sub.addItem(item)
            }
            root.submenu = sub; main.addItem(root)
        }
        menu("My DAW", [("Завершить My DAW", #selector(quit), "q", false)])
        menu("Файл", [("Новый черновик", #selector(newDraft), "n", false), ("Открыть…", #selector(openDraft), "o", false), ("Сохранить", #selector(saveDraft), "s", false), ("Сохранить как…", #selector(saveAs), "s", true), ("Упаковать проект…", #selector(packageProject), "", false), ("Открыть проект из архива…", #selector(openPackage), "", false), ("Экспорт WAV…", #selector(exportMix), "e", true), ("Экспортировать стемы…", #selector(exportStems), "", false), ("Экспорт DAWproject…", #selector(exportDawproject), "d", true), ("Восстановить черновик…", #selector(restoreDraft), "r", true)])
        menu("Проект", [("Начать или закончить запись", #selector(toggleRecording), "r", false), ("Отменить изменение проекта", #selector(undo), "z", false), ("Повторить изменение проекта", #selector(redo), "z", true), ("Добавить дорожку", #selector(addTrack), "t", false), ("Добавить MIDI-дорожку", #selector(addMidiTrack), "", false), ("Переместить выбранную дорожку выше", #selector(moveSelectedTrackUp), "", false), ("Переместить выбранную дорожку ниже", #selector(moveSelectedTrackDown), "", false), ("Удалить выбранную дорожку", #selector(deleteCurrentSelectedTrack), "\u{7f}", false), ("Добавить bus", #selector(addBus), "b", true), ("Импорт WAV…", #selector(importWav), "i", false), ("Цикл выбранного диапазона", #selector(toggleLoop), "l", false), ("Воспроизвести с позиции", #selector(playAudio), "p", false), ("Остановить", #selector(stopAudio), ".", false), ("Разделить выбранный клип (S в фокусе волны)", #selector(menuClipSplit), "", false), ("Дублировать выбранный клип (D)", #selector(menuClipDuplicate), "", false), ("Удалить выбранный клип (Delete)", #selector(menuClipDelete), "", false), ("Дублировать дорожку", #selector(menuTrackDuplicate), "t", true), ("Добавить маркер в позицию курсора", #selector(menuAddMarkerAtPlayhead), "m", true), ("Копировать выбранный клип (C в фокусе волны)", #selector(menuCopyClip), "", false), ("Вставить клип в курсор (V)", #selector(menuPasteClip), "", false)])
        if let projectMenu = main.items.last?.submenu {
            let prerollRoot=NSMenuItem(title:"Преролл записи (луп-режим)",action:nil,keyEquivalent:""); let prerollMenu=NSMenu(title:"Преролл записи"); prerollMenu.autoenablesItems=false
            for (label,seconds) in [("Выключен",0.0),("1 секунда",1.0),("2 секунды",2.0),("4 секунды",4.0),("8 секунд",8.0)] {
                let item=NSMenuItem(title:label,action:#selector(pickRecordPreroll(_:)),keyEquivalent:""); item.target=self; item.representedObject=seconds
                item.state = UInt64((seconds*48000).rounded())==recordPrerollFrames ? .on:.off
                prerollMenu.addItem(item)
            }
            prerollRoot.submenu=prerollMenu; prerollRoot.toolTip="Транспорт прокручивается перед punch-in с кликом; преролл не попадает в дубль"; projectMenu.addItem(prerollRoot)
            // Профессиональные клавиши транспорта и дорожек. Соло/мьют/арм
            // едут на ⌥S/⌥M/⌥A: без модификатора эти буквы уже живут в слое
            // волны (S = разрезать клип, M = mute клипа), а эквивалент главного
            // меню перехватывает клавишу раньше view.keyDown.
            projectMenu.addItem(.separator())
            let playStop = NSMenuItem(title: "Воспроизведение / Стоп", action: #selector(togglePlayStop), keyEquivalent: " ")
            playStop.target = self; playStop.keyEquivalentModifierMask = []
            projectMenu.addItem(playStop)
            let soloKey = NSMenuItem(title: "Solo выбранной дорожки", action: #selector(soloSelectedTrack), keyEquivalent: "s")
            soloKey.target = self; soloKey.keyEquivalentModifierMask = [.option]
            projectMenu.addItem(soloKey)
            let muteKey = NSMenuItem(title: "Mute выбранной дорожки", action: #selector(muteSelectedTrack), keyEquivalent: "m")
            muteKey.target = self; muteKey.keyEquivalentModifierMask = [.option]
            projectMenu.addItem(muteKey)
            let armKey = NSMenuItem(title: "Arm выбранной дорожки", action: #selector(armSelectedTrack), keyEquivalent: "a")
            armKey.target = self; armKey.keyEquivalentModifierMask = [.option]
            projectMenu.addItem(armKey)
            let up = NSMenuItem(title: "Переместить выбранную дорожку выше", action: #selector(moveSelectedTrackUp), keyEquivalent: "\u{F700}")
            up.target = self; up.keyEquivalentModifierMask = [.command, .option]
            let down = NSMenuItem(title: "Переместить выбранную дорожку ниже", action: #selector(moveSelectedTrackDown), keyEquivalent: "\u{F701}")
            down.target = self; down.keyEquivalentModifierMask = [.command, .option]
            // The non-shortcut commands above remain visible and discoverable;
            // these items provide the standard Option-Command arrow workflow.
            projectMenu.removeItem(at: 5); projectMenu.removeItem(at: 4)
            projectMenu.insertItem(up, at: 4); projectMenu.insertItem(down, at: 5)
        }
        menu("Вид", [("Увеличить timeline", #selector(zoomIn), "+", false), ("Уменьшить timeline", #selector(zoomOut), "-", false), ("Timeline 1×", #selector(resetZoom), "0", false)])
        let edit = NSMenuItem(); edit.title = "Текст"; let submenu = NSMenu(title: "Текст")
        for (title, selector, key) in [("Вырезать", "cut:", "x"), ("Копировать", "copy:", "c"), ("Вставить", "paste:", "v"), ("Выбрать всё", "selectAll:", "a")] {
            submenu.addItem(NSMenuItem(title: title, action: Selector(selector), keyEquivalent: key))
        }
        edit.submenu = submenu; main.addItem(edit); NSApp.mainMenu = main
    }
    /// true, пока фокус в текстовом поле: клавиши меню приходят раньше
    /// field editor, и без этого guard набор имени дорожки или маркера
    /// ронял бы транспорт и solo/mute.
    var isEditingText: Bool {
        guard let responder = window?.firstResponder else { return false }
        if responder is NSTextView { return true }
        if let control = responder as? NSControl, control.currentEditor() != nil { return true }
        return false
    }
    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        if menuItem.action == #selector(togglePlayStop) || menuItem.action == #selector(soloSelectedTrack)
            || menuItem.action == #selector(muteSelectedTrack) || menuItem.action == #selector(armSelectedTrack) {
            if isEditingText { return false }
            if menuItem.action == #selector(togglePlayStop) { return true }
            let selected = inspectorTrackID ?? selectedMixerID
            return selected.map { mixerKinds[$0] == .track } == true
        }
        if menuItem.action == #selector(deleteCurrentSelectedTrack) || menuItem.action == #selector(moveSelectedTrackUp) || menuItem.action == #selector(moveSelectedTrackDown) || menuItem.action == #selector(menuTrackDuplicate) {
            let selected = inspectorTrackID ?? selectedMixerID
            return !isRecording && automationGesture == nil && pluginParameterGesture == nil && selected.map { mixerKinds[$0] == .track } == true
        }
        if menuItem.action == #selector(menuClipSplit) || menuItem.action == #selector(menuClipDuplicate) || menuItem.action == #selector(menuClipDelete) || menuItem.action == #selector(menuCopyClip) || menuItem.action == #selector(menuPasteClip) {
            let selected = inspectorTrackID ?? selectedMixerID
            return !isRecording && selected.map { mixerKinds[$0] == .track && $0 != 0 } == true && trackIDs.values.contains(selected ?? 0)
        }
        return true
    }
    @objc func syncArrangementScroll(_ notification:Notification) {
        guard !synchronizingArrangementScroll,let source=notification.object as? NSClipView,let timelineScroll,let trackHeaderScroll else{return}
        let targetScroll=source === timelineScroll.contentView ? trackHeaderScroll:timelineScroll
        let target=targetScroll.contentView;guard abs(target.bounds.origin.y-source.bounds.origin.y)>0.5 else{return}
        synchronizingArrangementScroll=true;target.scroll(to:NSPoint(x:target.bounds.origin.x,y:source.bounds.origin.y));targetScroll.reflectScrolledClipView(target);synchronizingArrangementScroll=false
    }
    /// Отказ команды движка. Кроме модального окна пишет строку в unified log:
    /// только так «что-то не сработало» остаётся проверяемым фактом, когда alert
    /// уже закрыт, а экран недоступен. Вызов не меняется: координаты снимаются
    /// аргументами по умолчанию в точке вызова.
    func check(_ result: Int32, site: String = #function, line: UInt = #line) -> Bool {
        guard result != 0 else { return true }
        var bytes = [CChar](repeating: 0, count: 512); daw_error(session, &bytes, bytes.count)
        let reason = String(decoding: bytes.prefix(while: { $0 != 0 }).map { UInt8(bitPattern: $0) }, as: UTF8.self)
        DAWLog.bridge.error("Команда отклонена в \(site, privacy: .public):\(line, privacy: .public): \(reason, privacy: .public)")
        let alert = NSAlert(); alert.messageText = "Изменение не выполнено"; alert.informativeText = reason
        alert.runModal(); return false
    }
    func routingPopup(selected: UInt64, excluding: UInt64? = nil) -> NSPopUpButton {
        let popup=NSPopUpButton();popup.addItem(withTitle:"Master");popup.lastItem?.representedObject=NSNumber(value:UInt64(0))
        for bus in orderedBuses where bus.id != excluding {popup.addItem(withTitle:bus.name);popup.lastItem?.representedObject=NSNumber(value:bus.id)}
        if let item=popup.itemArray.first(where:{($0.representedObject as? NSNumber)?.uint64Value == selected}){popup.select(item)}else{popup.selectItem(at:0)}
        popup.widthAnchor.constraint(equalToConstant:130).isActive=true
        return popup
    }
    func mixerInsertSummaries(owner: Int32, ownerID: UInt64) -> [MixerInsertSummary] {
        var count: UInt32 = 0
        guard daw_get_insert_count(session,owner,ownerID,&count) == 0 else { return [] }
        return (0..<count).compactMap { index in
            var plugin=daw_plugin();plugin.struct_size=UInt32(MemoryLayout<daw_plugin>.size)
            guard daw_get_insert(session,owner,ownerID,index,&plugin) == 0 else { return nil }
            let name=withUnsafeBytes(of:plugin.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)}
            return MixerInsertSummary(name:name,bypassed:plugin.bypassed != 0,id:plugin.id,available:plugin.available != 0,latencyFrames:plugin.latency_frames)
        }
    }
    func updateMixerInspector(_ id: UInt64) {
        guard let strip=mixerWorkspace.strips.first(where:{$0.id == id}) else{return}
        let kind=strip.kind == .master ? "MASTER":(strip.kind == .bus ? "BUS":"TRACK")
        inspectorTrackID=strip.kind == .track ? id:nil;inspectorClipIndex=nil
        inspectorBrowser.clip=nil
        inspectorBrowser.channel=InspectorChannelModel(title:strip.title,kind:kind,renameable:strip.kind != .master,volumeDb:strip.volumeDb,pan:strip.pan,muted:strip.isMuted,solo:strip.isSolo,inserts:strip.inserts.map{$0.bypassed ? "⊘ \($0.name)":$0.name},sends:strip.sends.map{"→ \($0.destination)  \(String(format:"%+.1f dB",$0.gainDb)) \($0.preFader ? "PRE":"POST")"},accent:strip.color ?? .systemBlue)
        if strip.kind == .track { loadMidiInspector(id) } else { inspectorBrowser.midi = nil }
    }
    func loadMidiInspector(_ trackID: UInt64) {
        var clipCount: UInt32 = 0
        guard daw_get_midi_clip_count(session, trackID, &clipCount) == 0, clipCount > 0 else { midiClipIndex = nil; midiNotesCache = []; inspectorBrowser.midi = nil; return }
        var clips: [PianoRollClipModel] = []
        for index in 0..<Int(clipCount) {
            var meta = daw_midi_clip(); meta.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
            guard daw_get_midi_clip(session, trackID, UInt32(index), &meta, 0, nil, 0, nil) == 0 else { continue }
            clips.append(PianoRollClipModel(index: index, startFrames: meta.start, lengthFrames: meta.length, noteCount: meta.note_count))
        }
        guard let first = clips.first else { midiClipIndex = nil; midiNotesCache = []; inspectorBrowser.midi = nil; return }
        let selected = clips.contains(where: { $0.index == midiClipIndex }) ? midiClipIndex : first.index
        var notes: [PianoRollNote] = []
        if let selected {
            var meta = daw_midi_clip(); meta.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
            if daw_get_midi_clip(session, trackID, UInt32(selected), &meta, 0, nil, 0, nil) == 0 {
                var offset: UInt32 = 0
                while offset < meta.note_count {
                    let pageSize = Int(min(meta.note_count - offset, UInt32(DAW_MIDI_NOTES_PER_CALL)))
                    var page = [daw_midi_note](repeating: daw_midi_note(), count: pageSize)
                    var written: UInt32 = 0
                    let result = page.withUnsafeMutableBufferPointer { buffer in daw_get_midi_clip(session, trackID, UInt32(selected), &meta, offset, buffer.baseAddress, UInt32(pageSize), &written) }
                    guard result == 0, written > 0 else { break }
                    for note in page.prefix(Int(written)) { notes.append(PianoRollNote(startFrames: note.start, lengthFrames: note.length, pitch: note.pitch, channel: note.channel, velocity: note.velocity)) }
                    offset += written
                }
            }
        }
        midiClipIndex = selected
        midiNotesCache = notes
        inspectorBrowser.midi = InspectorMidiModel(clips: clips, selectedClip: selected, notes: notes, editable: !isRecording)
        // Ряд захвата относится к той же MIDI-дорожке: пересобираем список
        // источников именно здесь (выбор дорожки — естественная точка обновления).
        refreshMidiCapture(rescan: true)
    }
    func commitMidiNotes(track trackID: UInt64, clip clipIndex: Int, notes: [PianoRollNote]) {
        guard !isRecording else { return }
        var marshaled = notes.map { note -> daw_midi_note in
            var value = daw_midi_note()
            value.struct_size = UInt32(MemoryLayout<daw_midi_note>.size); value.version = UInt32(DAW_MIDI_NOTE_VERSION)
            value.start = note.startFrames; value.length = note.lengthFrames
            value.pitch = note.pitch; value.channel = note.channel; value.velocity = note.velocity
            return value
        }
        let noteCount = UInt32(marshaled.count)
        let result = marshaled.withUnsafeMutableBufferPointer { buffer in daw_set_midi_notes(session, trackID, UInt32(clipIndex), buffer.baseAddress, noteCount, revision) }
        if check(result) { refresh(); updateMixerInspector(trackID) } else { loadMidiInspector(trackID) }
    }
    // MARK: - Живой MIDI-вход, запись с клавиатуры и метроном
    //
    // Всё состояние принадлежит мосту: приложение только перечисляет источники,
    // выбирает один из них, вооружает/останавливает тейк и показывает счётчики,
    // которые мост и считает. Единственный открытый вход — лимит прототипа.
    func midiInputOptions() -> [InspectorMidiInputOption] {
        var count: UInt32 = 0
        guard daw_get_midi_input_device_count(session, &count) == 0 else { return [] }
        var options: [InspectorMidiInputOption] = []
        for index in 0..<count {
            var device = daw_midi_device(); device.struct_size = UInt32(MemoryLayout<daw_midi_device>.size); device.version = UInt32(DAW_MIDI_DEVICE_VERSION)
            guard daw_get_midi_input_device(session, index, &device) == 0 else { continue }
            let name = withUnsafeBytes(of: device.name) { bytes in String(decoding: bytes.prefix(while: { $0 != 0 }), as: UTF8.self) }
            options.append(InspectorMidiInputOption(id: device.uniqueID, title: name.isEmpty ? "Источник \(device.uniqueID)" : name))
        }
        return options
    }
    /// Перечитывает мост и отдаёт инспектору строку состояния. Вызывается при
    /// выборе дорожки, при смене входа и каждый тик таймера, пока тейк вооружён;
    /// список источников пересобирается только по просьбе (rescan), чтобы под
    /// открытым попапом не менять пункты.
    func refreshMidiCapture(rescan: Bool = false) {
        if rescan { midiInputCache = midiInputOptions() }
        var active: UInt32 = 0
        midiInputID = daw_midi_input_active(session, &active) == 0 ? active : 0
        var counters = daw_midi_record_status_t(); counters.struct_size = UInt32(MemoryLayout<daw_midi_record_status_t>.size); counters.version = UInt32(DAW_MIDI_RECORD_STATUS_VERSION)
        guard daw_midi_record_status(session, &counters) == 0 else { inspectorBrowser.midiCapture = nil; midiTakeArmed = false; return }
        midiTakeArmed = counters.armed != 0
        let lost = counters.dropped + counters.unmatched
        let model: InspectorMidiCaptureModel
        if midiTakeArmed {
            model = InspectorMidiCaptureModel(inputs: midiInputCache, selectedID: midiInputID, armed: true,
                statusLine: "● запись · держат \(counters.open_notes) · записано \(counters.recorded), отброшено \(lost)")
        } else {
            model = InspectorMidiCaptureModel(inputs: midiInputCache, selectedID: midiInputID, armed: false,
                statusLine: midiInputID == 0 ? "MIDI-вход не выбран · «Нет»" : "Готов · записано \(counters.recorded), отброшено \(lost)")
        }
        if inspectorBrowser.midiCapture != model { inspectorBrowser.midiCapture = model }
    }
    func selectMidiInput(_ uniqueID: UInt32) {
        guard !midiTakeArmed else { storageMessage("Сначала заверши запись с клавиатуры."); refreshMidiCapture(); return }
        guard check(daw_set_midi_input(session, uniqueID)) else { refreshMidiCapture(); return }
        refreshMidiCapture(rescan: true)
        status.stringValue = midiInputID == 0 ? "MIDI-вход закрыт · запись с клавиатуры недоступна" : "MIDI-вход: \(midiInputCache.first { $0.id == midiInputID }?.title ?? "источник \(midiInputID)")"
    }
    /// Арм тейка в выбранный клип. Кадр нот считается от позиции транспорта,
    /// поэтому если он не играет — запускаем: без хода транспорта тейк лёг бы в
    /// одну точку (см. v0-оценку кадра в daw.h).
    @objc func toggleMidiRecording() {
        if midiTakeArmed { stopMidiTake(); return }
        guard !isRecording else { storageMessage("Сначала заверши аудиозапись."); return }
        guard midiInputID != 0 else { storageMessage("Выбери MIDI-вход в инспекторе дорожки."); return }
        guard let track = inspectorTrackID, let clip = midiClipIndex else { storageMessage("Выбери MIDI-дорожку с клипом — в него лягут ноты."); return }
        guard check(daw_midi_record_arm(session, track, UInt32(clip))) else { return }
        midiTakeTrackID = track
        if !isPlaying { playAudio() }
        refreshMidiCapture()
        status.stringValue = "Идёт запись с клавиатуры · играй; «Закончить запись» закроет тейк и запишет ноты в клип"
    }
    /// Порядок остановки: один принудительный drain, затем stop+append одним
    /// guard'ом моста. Второй drain «отложив на кадр» не делаем — он уехал бы и
    /// stop-кадр вперёд; ноты, долетевшие уже после клика, теряются, а клавиши,
    /// зажатые на остановке, закрываются самим MidiRecorder на stop-кадре. Это
    /// документированный v0-допуск.
    func stopMidiTake() {
        guard midiTakeArmed else { return }
        _ = daw_midi_record_poll(session)
        let before = revision
        var counters = daw_midi_record_status_t(); counters.struct_size = UInt32(MemoryLayout<daw_midi_record_status_t>.size); counters.version = UInt32(DAW_MIDI_RECORD_STATUS_VERSION)
        _ = daw_midi_record_status(session, &counters)   // счётчики до стопа: после они обнулены
        let recorded = counters.recorded + counters.open_notes, lost = counters.dropped + counters.unmatched
        guard check(daw_midi_record_stop(session)) else { midiTakeTrackID = nil; refreshMidiCapture(); return }
        syncRevision()
        if revision != before {
            refresh()
            if let track = midiTakeTrackID { updateMixerInspector(track) }
            status.stringValue = "Тейк записан в клип · нот \(recorded), отброшено \(lost)"
        } else {
            loadMidiInspector(midiTakeTrackID ?? inspectorTrackID ?? 0)
            status.stringValue = "Пустой тейк · нот 0, отброшено \(lost) · проект не изменён"
        }
        status.stringValue = revision != before ? "Тейк записано в клип · нот \(counters.recorded), отброшено \(counters.dropped + counters.unmatched)" : "Пустой тейк · проект не изменён"
        setProjectMessage("Запись с клавиатуры v0: кадр ноты считается в момент дрена ring (±интервал таймера 100 мс), без привязки к доле.")
        midiTakeTrackID = nil
        refreshMidiCapture(); pollTransport()
    }
    /// Дренирует ring в тейк из уже существующего UI-таймера транспорта.
    func pollMidiCapture() {
        guard midiTakeArmed else { return }
        guard daw_midi_record_poll(session) == 0 else {
            _ = daw_midi_record_stop(session)
            midiTakeTrackID = nil; refreshMidiCapture()
            setProjectMessage("Запись с клавиатуры остановлена мостом; тейк не записан.")
            return
        }
        refreshMidiCapture()
    }
    @objc func toggleRecordMonitor(_ sender: NSButton) {
        let wanted: Int32 = recordMonitorOn ? 0 : 1
        guard check(daw_set_record_monitor(session, wanted)) else { syncRecordMonitorButton(); return }
        recordMonitorOn = wanted != 0
        syncRecordMonitorButton()
        status.stringValue = recordMonitorOn ? "Мониторинг входа включён · слышен во время луп-записи" : "Мониторинг входа выключен"
    }
    func syncRecordMonitorButton() { recordMonitorButton.state = recordMonitorOn ? .on : .off }
    @objc func toggleAutoMonitorOnArm(_ sender: NSButton) {
        let wanted: Int32 = autoMonitorOnArm ? 0 : 1
        guard check(daw_set_auto_monitor_on_arm(session, wanted)) else { syncAutoMonitorButton(); return }
        autoMonitorOnArm = wanted != 0
        syncAutoMonitorButton()
        status.stringValue = autoMonitorOnArm ? "Авто-мониторинг включён" : "Авто-мониторинг выключен"
    }
    func syncAutoMonitorButton() { autoMonitorButton.state = autoMonitorOnArm ? .on : .off }
    @objc func toggleMetronome(_ sender: NSButton) {
        let wanted: Int32 = metronomeOn ? 0 : 1
        guard check(daw_set_metronome(session, wanted)) else { syncMetronomeButton(); return }
        metronomeOn = wanted != 0
        syncMetronomeButton()
        status.stringValue = metronomeOn ? "Метроном включён · слышен в мониторинге, в экспорт не попадает" : "Метроном выключен"
    }
    /// Состояние хранит сессия-контроллер моста; кнопка лишь отражает его, а
    /// после пересборки графа (stop→play, смена устройства) флаг тот же.
    func syncMetronomeButton() {
        var value: Int32 = 0
        if daw_get_metronome(session, &value) == 0 { metronomeOn = value != 0 }
        metronomeButton.state = metronomeOn ? .on : .off
        metronomeButton.contentTintColor = metronomeOn ? DAWDesignTokens.Color.coral : DAWDesignTokens.Color.text
        metronomeButton.setAccessibilityValue(metronomeOn ? "включён" : "выключен")
    }
    func wireInspectorBrowser() {
        audioPreview.onChange = { [weak self] state in
            self?.updateBrowserAudioPreview(state)
        }
        inspectorBrowser.onChannelChange = { [weak self] volume,pan in
            guard let self,let id=self.selectedMixerID ?? self.inspectorTrackID,let current=self.inspectorBrowser.channel else{return}
            if abs(volume-current.volumeDb) >= 0.01 { self.mixerSetVolume(id,volume) }
            if abs(pan-current.pan) >= 0.001 { self.mixerSetPan(id,pan) }
            self.updateMixerInspector(id)
        }
        inspectorBrowser.onChannelRename = { [weak self] name in
            guard let self,let id=self.selectedMixerID ?? self.inspectorTrackID,!name.trimmingCharacters(in:.whitespacesAndNewlines).isEmpty,let kind=self.mixerKinds[id] else{return}
            let result:Int32
            switch kind { case .track:result=daw_rename_track(self.session,id,name,self.revision);case .bus:result=daw_rename_bus(self.session,id,name,self.revision);case .master:return }
            if self.check(result){self.refresh();self.selectedMixerID=id;self.updateMixerInspector(id)}
        }
        inspectorBrowser.onChannelMute = { [weak self] muted in guard let self,let id=self.selectedMixerID ?? self.inspectorTrackID else{return};self.mixerSetMute(id,muted);self.updateMixerInspector(id) }
        inspectorBrowser.onChannelSolo = { [weak self] solo in guard let self,let id=self.selectedMixerID ?? self.inspectorTrackID else{return};self.mixerSetSolo(id,solo);self.updateMixerInspector(id) }
        inspectorBrowser.onClipChange = { [weak self] model in
            guard let self,let track=self.inspectorTrackID,let clip=self.inspectorClipIndex,!self.isRecording else{return}
            self.finishEditing();_=daw_stop(self.session)
            if self.check(daw_edit_clip_full(self.session,track,UInt32(clip),model.startFrames,model.sourceOffsetFrames,model.lengthFrames,model.fadeInFrames,model.fadeOutFrames,self.revision)){self.refresh();self.pollTransport();self.updateClipInspector(track,clip)}
        }
        inspectorBrowser.onAddFolder = { [weak self] in self?.addBrowserFolder() }
        inspectorBrowser.onImport = { [weak self] in self?.importWav() }
        inspectorBrowser.onAdd = { [weak self] kind,item in self?.addBrowserItem(kind,item) }
        inspectorBrowser.onScanAU = { [weak self] in self?.scanInstalledAudioUnits() }
        inspectorBrowser.onScanVST3 = { [weak self] in self?.scanInstalledVST3() }
        inspectorBrowser.onBrowserSelect = { [weak self] kind, item in
            guard let self else { return }
            guard kind == .audio, let item, let url = self.browserAudioURLs[item.id] else {
                self.stopBrowserAudioPreview()
                return
            }
            self.audioPreview.select(url)
        }
        inspectorBrowser.onPreview = { [weak self] item in
            guard let self, let item, let url = self.browserAudioURLs[item.id] else { return }
            if self.audioPreview.state.selectedURL != url { self.audioPreview.select(url) }
            self.audioPreview.play()
        }
        inspectorBrowser.onStopPreview = { [weak self] in self?.stopBrowserAudioPreview() }
        inspectorBrowser.onMidiClipSelect = { [weak self] index in guard let self, let track = self.inspectorTrackID else { return }; self.midiClipIndex = index; self.loadMidiInspector(track) }
        inspectorBrowser.onMidiAddClip = { [weak self] in
            guard let self, let track = self.inspectorTrackID, !self.isRecording else { return }
            var transport = daw_transport(); transport.struct_size = UInt32(MemoryLayout<daw_transport>.size)
            _ = daw_get_transport(self.session, &transport)
            var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size); clip.version = UInt32(DAW_MIDI_CLIP_VERSION)
            clip.start = transport.frame / 48000 * 48000; clip.length = 192000; clip.lane = 0; clip.color = 0
            guard self.check(daw_add_midi_clip(self.session, track, &clip, nil, 0, self.revision)) else { return }
            var count: UInt32 = 0; _ = daw_get_midi_clip_count(self.session, track, &count)
            self.midiClipIndex = count > 0 ? Int(count - 1) : nil
            self.refresh(); self.loadMidiInspector(track)
        }
        inspectorBrowser.onMidiRemoveClip = { [weak self] index in
            guard let self, let track = self.inspectorTrackID, !self.isRecording else { return }
            guard self.check(daw_remove_midi_clip(self.session, track, UInt32(index), self.revision)) else { return }
            self.midiClipIndex = nil
            self.refresh(); self.loadMidiInspector(track)
        }
        inspectorBrowser.onMidiInputSelect = { [weak self] id in self?.selectMidiInput(id) }
        inspectorBrowser.onMidiRecordToggle = { [weak self] in self?.toggleMidiRecording() }
        inspectorBrowser.onMidiNotesChange = { [weak self] notes in guard let self, let track = self.inspectorTrackID, let clip = self.midiClipIndex else { return }; self.commitMidiNotes(track: track, clip: clip, notes: notes) }
        inspectorBrowser.onMidiAddNote = { [weak self] in
            guard let self, let track = self.inspectorTrackID, let clip = self.midiClipIndex, !self.isRecording else { return }
            guard let model = self.inspectorBrowser.midi, let meta = model.clips.first(where: { $0.index == clip }), meta.lengthFrames >= 480 else { return }
            var transport = daw_transport(); transport.struct_size = UInt32(MemoryLayout<daw_transport>.size)
            _ = daw_get_transport(self.session, &transport)
            let relative = transport.frame > meta.startFrames ? transport.frame - meta.startFrames : 0
            var notes = self.midiNotesCache
            notes.append(PianoRollNote(startFrames: min(relative, meta.lengthFrames - 480), lengthFrames: 480, pitch: 60, channel: 0, velocity: 100))
            self.commitMidiNotes(track: track, clip: clip, notes: notes)
        }
        inspectorBrowser.onMidiRemoveNote = { [weak self] row in
            guard let self, let track = self.inspectorTrackID, let clip = self.midiClipIndex else { return }
            var notes = self.midiNotesCache
            guard row >= 0, row < notes.count else { return }
            notes.remove(at: row)
            self.commitMidiNotes(track: track, clip: clip, notes: notes)
        }
    }

    func updateBrowserAudioPreview(_ state: AudioPreviewController.State) {
        let selectedID = state.selectedURL.flatMap { url in
            browserAudioURLs.first { $0.value.standardizedFileURL == url.standardizedFileURL }?.key
        }
        inspectorBrowser.updateAudioPreview(isPlaying: state.isPlaying, selectedID: selectedID, error: state.errorMessage)
    }

    func stopBrowserAudioPreview() {
        audioPreview.stop()
        if audioPreview.state.selectedURL != nil { audioPreview.select(nil) }
    }
    func refreshBrowserCatalog() {
        browserPluginTargets.removeAll();var items:[InspectorBrowserItem]=[]
        for plugin in auCatalog {let isInstrument = plugin.subtype == 0x61616D69 || plugin.subtype == 0x61756961 /* aami, auia */;let item=InspectorBrowserItem(title:plugin.name,detail:isInstrument ? "Audio Unit · инструмент · experimental":"Audio Unit",available:true);browserPluginTargets[item.id] = .audioUnit(type:plugin.type,subtype:plugin.subtype,manufacturer:plugin.manufacturer);items.append(item)}
        for plugin in vst3Catalog {let kind=plugin.instrument ? "VST3 · инструмент":"VST3";let item=InspectorBrowserItem(title:plugin.name,detail:plugin.vendor.isEmpty ? kind:"\(kind) · \(plugin.vendor)",available:plugin.available);browserPluginTargets[item.id] = .vst3(index:plugin.index);items.append(item)}
        inspectorBrowser.pluginItems=items.sorted{$0.title.localizedCaseInsensitiveCompare($1.title) == .orderedAscending}
    }
    func addBrowserFolder() {
        let panel=NSOpenPanel();panel.canChooseDirectories=true;panel.canChooseFiles=false;panel.allowsMultipleSelection=false;panel.prompt="Добавить";panel.message="Выбери папку с WAV. My DAW читает только эту явно выбранную папку и не запрашивает общий доступ к Документам."
        guard panel.runModal() == .OK,let root=panel.url else{return}
        let keys:[URLResourceKey]=[.isRegularFileKey,.isHiddenKey]
        guard let enumerator=FileManager.default.enumerator(at:root,includingPropertiesForKeys:keys,options:[.skipsHiddenFiles,.skipsPackageDescendants]) else{return}
        var urls:[URL]=[]
        for case let url as URL in enumerator where url.pathExtension.lowercased() == "wav" {urls.append(url);if urls.count>=1000{break}}
        urls.sort{$0.lastPathComponent.localizedCaseInsensitiveCompare($1.lastPathComponent) == .orderedAscending}
        stopBrowserAudioPreview();browserAudioURLs.removeAll();let items=urls.map{url -> InspectorBrowserItem in let item=InspectorBrowserItem(title:url.deletingPathExtension().lastPathComponent,detail:url.deletingLastPathComponent().lastPathComponent,available:true);browserAudioURLs[item.id]=url;return item};inspectorBrowser.audioItems=items
    }
    func addBrowserItem(_ kind:InspectorBrowserKind,_ item:InspectorBrowserItem?) {
        guard !isRecording,let item else{return}
        if kind == .audio { guard let url = browserAudioURLs[item.id] else { return }; beginTrackImport(url); return }
        stopBrowserAudioPreview()
        guard item.available,let target=browserPluginTargets[item.id] else{return}
        let destination=selectedMixerID ?? 0;let owner:Int32
        switch mixerKinds[destination] ?? .master {case .track:owner=Int32(DAW_INSERT_OWNER_TRACK);case .bus:owner=Int32(DAW_INSERT_OWNER_BUS);case .master:owner=Int32(DAW_INSERT_OWNER_MASTER)}
        _=daw_stop(session);let result:Int32
        switch target {case let .audioUnit(type,subtype,manufacturer):result=daw_add_insert_au(session,owner,destination,type,subtype,manufacturer,revision);case let .vst3(index):result=daw_add_insert_vst3(session,owner,destination,index,revision)}
        if check(result){expandedInsertOwners.insert(insertOwnerKey(owner,destination));refresh();selectedMixerID=destination;updateMixerInspector(destination);pollTransport()}
    }
    func pollMeters() {
        var snapshots: [UInt64: MixerMeterSnapshot] = [:]
        var masterLoud=daw_master_loudness();masterLoud.struct_size=UInt32(MemoryLayout<daw_master_loudness>.size)
        let liveLoudness: (Float,Float)? = daw_get_master_loudness(session,&masterLoud)==0 && masterLoud.live==1 ? (masterLoud.momentary_lufs,masterLoud.short_term_lufs):nil
        let activeIDs=Set(mixerWorkspace.strips.map(\.id));meterHolds=meterHolds.filter{activeIDs.contains($0.key)}
        for strip in mixerWorkspace.strips {
            let owner:Int32=strip.kind == .master ? Int32(DAW_INSERT_OWNER_MASTER):(strip.kind == .bus ? Int32(DAW_INSERT_OWNER_BUS):Int32(DAW_INSERT_OWNER_TRACK))
            var meter=daw_channel_meter();meter.struct_size=UInt32(MemoryLayout<daw_channel_meter>.size)
            guard daw_get_channel_meter(session,owner,strip.id,&meter) == 0 else{continue}
            let old=meterHolds[strip.id] ?? (left:Float(0),right:Float(0));let hold=(left:max(meter.left_peak,old.left*0.92),right:max(meter.right_peak,old.right*0.92));meterHolds[strip.id]=hold
            var snapshot=MixerMeterSnapshot(leftPeak:meter.left_peak,rightPeak:meter.right_peak,leftHold:hold.0,rightHold:hold.1)
            if strip.kind == .master { snapshot.momentaryLufs=liveLoudness?.0; snapshot.shortTermLufs=liveLoudness?.1 }
            snapshots[strip.id]=snapshot
        }
        mixerWorkspace.updateMeters(snapshots)
    }
    func mixerSendSummaries(trackID: UInt64, count: UInt32) -> [MixerSendSummary] {
        (0..<count).compactMap { index in
            var send=daw_send();send.struct_size=UInt32(MemoryLayout<daw_send>.size)
            guard daw_get_send(session,trackID,index,&send) == 0 else { return nil }
            let destination=orderedBuses.first(where:{$0.id == send.bus_id})?.name ?? "Bus \(send.bus_id)"
            return MixerSendSummary(destination:destination,gainDb:send.gain_db,preFader:send.pre_fader != 0,busID:send.bus_id)
        }
    }
    func loadSupportedAudioUnits(){
        var count:UInt32=0;var quarantined:UInt32=0
        let helper=Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/daw_au_scan_helper").path;var invalidated:UInt32=0
        if let cache=auCacheURL,daw_load_installed_au_scan_cache(session,helper,cache.path,&count,&quarantined,&invalidated)==0,count>0{DAWLog.plugins.info("Кэш AU применён: доступно \(count, privacy: .public), карантин \(quarantined, privacy: .public), устаревших \(invalidated, privacy: .public)");reloadAudioUnitPopup(count);let stale=invalidated>0 ? ", обновить \(invalidated)":"";scanAUButton.title=quarantined==0 ? "AU: \(count)\(stale)":"AU: \(count), карантин \(quarantined)\(stale)";return}
        guard check(daw_scan_supported_au(session,&count))else{return};auCatalog.removeAll();while masterAUPopup.numberOfItems>1{masterAUPopup.removeItem(at:1)}
        for index in 0..<count{var item=daw_au_component();item.struct_size=UInt32(MemoryLayout<daw_au_component>.size);guard check(daw_get_supported_au(session,index,&item))else{return};let name=withUnsafeBytes(of:item.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};auCatalog.append((item.type,item.subtype,item.manufacturer,name));masterAUPopup.addItem(withTitle:name)}
        masterAUPopup.isEnabled = !auCatalog.isEmpty
    }
    func reloadAudioUnitPopup(_ count:UInt32){auCatalog.removeAll();while masterAUPopup.numberOfItems>1{masterAUPopup.removeItem(at:1)};for index in 0..<count{var item=daw_au_component();item.struct_size=UInt32(MemoryLayout<daw_au_component>.size);guard check(daw_get_supported_au(session,index,&item))else{return};let name=withUnsafeBytes(of:item.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};auCatalog.append((item.type,item.subtype,item.manufacturer,name));masterAUPopup.addItem(withTitle:name)};masterAUPopup.isEnabled = !auCatalog.isEmpty;refreshBrowserCatalog()}
    var vst3CacheURL: URL? { try? FileManager.default.url(for:.applicationSupportDirectory,in:.userDomainMask,appropriateFor:nil,create:true).appendingPathComponent("My DAW/vst3-scan-cache-v1.txt") }
    func loadInstalledVST3(){let helper=Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/daw_vst3_scan_helper").path;var available:UInt32=0;var quarantined:UInt32=0;var invalidated:UInt32=0;if let cache=vst3CacheURL,daw_load_installed_vst3_scan_cache(session,helper,cache.path,&available,&quarantined,&invalidated)==0{reloadVST3Popup();let stale=invalidated>0 ? ", обновить \(invalidated)":"";scanVST3Button.title=quarantined==0 ? "VST3: \(available)\(stale)":"VST3: \(available), карантин \(quarantined)\(stale)"}}
    func reloadVST3Popup(){vst3Catalog.removeAll();while masterVST3Popup.numberOfItems>1{masterVST3Popup.removeItem(at:1)};var count:UInt32=0;guard daw_get_installed_vst3_count(session,&count)==0 else{return};for index in 0..<count{var item=daw_vst3_component();item.struct_size=UInt32(MemoryLayout<daw_vst3_component>.size);guard daw_get_installed_vst3(session,index,&item)==0 else{return};let name=withUnsafeBytes(of:item.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};let vendor=withUnsafeBytes(of:item.vendor){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};let instrument=item.available != 0 && (item.flags & UInt32(DAW_VST3_FLAG_INSTRUMENT)) != 0;/* flags осмысленны только при available != 0 — контракт моста */vst3Catalog.append((index,name,vendor,item.available != 0,instrument));if item.available != 0{masterVST3Popup.addItem(withTitle:(instrument ? "🎹 " : "") + (vendor.isEmpty ? name:"\(name) — \(vendor)"));masterVST3Popup.lastItem?.representedObject=NSNumber(value:index)}};masterVST3Popup.isEnabled = masterVST3Popup.numberOfItems>1;refreshBrowserCatalog()}
    func refresh() {
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        guard check(daw_get_snapshot(session, &snapshot)) else { return }
        revision = snapshot.revision
        reloadTempoMap()
        masterSlider.doubleValue=snapshot.master_gain_db;masterLabel.stringValue=String(format:"%+.1f dB",snapshot.master_gain_db)
        var masterAutomationCount:UInt32=0;guard check(daw_get_master_gain_automation_count(session,&masterAutomationCount))else{return};masterAutomationButton.title=masterAutomationCount==0 ? "AUTO":"AUTO \(masterAutomationCount)";masterAutomationButton.contentTintColor=masterAutomationCount==0 ? .secondaryLabelColor:.systemCyan
        undoButton.isEnabled = snapshot.can_undo != 0; redoButton.isEnabled = snapshot.can_redo != 0
        syncMetronomeButton()   // флаг держит мост: после открытия проекта сверяем кнопку
        window.title = (currentURL?.deletingPathExtension().lastPathComponent ?? "Новый черновик") + " — My DAW"
        window.isDocumentEdited = dirty
        summary.stringValue = "ДОРОЖКИ  \(snapshot.track_count) / 256      BUS  \(snapshot.bus_count) / 16      AU  \(snapshot.master_insert_count) / 4      РЕВИЗИЯ  \(revision)"
        mixerSummary.stringValue = "\(snapshot.track_count) CH · \(snapshot.bus_count) BUS · MASTER"
        mixerSummary.setAccessibilityLabel("Состав консоли: \(snapshot.track_count) дорожек, \(snapshot.bus_count) шин и master канал")
        status.stringValue = dirty ? "Есть несохранённые изменения" : (currentURL == nil ? "Создай первую дорожку, чтобы начать." : "Черновик сохранён на этом Mac")
        for view in rows.arrangedSubviews { rows.removeArrangedSubview(view); view.removeFromSuperview() }
        for view in trackHeaderRows.arrangedSubviews { trackHeaderRows.removeArrangedSubview(view); view.removeFromSuperview() }
        for view in consoleRows.arrangedSubviews { consoleRows.removeArrangedSubview(view); view.removeFromSuperview() }
        mixerKinds.removeAll()
        trackNames.removeAll()
        laneViews.removeAll()
        var mixerStrips:[MixerStripModel]=[]
        trackIDs.removeAll();takePopups.removeAll();orderedBuses.removeAll();outputTargets.removeAll();busControlTargets.removeAll();busAutomationTargets.removeAll();busNameTargets.removeAll();newSendTargets.removeAll();sendControlTargets.removeAll();pluginControlTargets.removeAll();pluginEditorTargets.removeAll();pluginParameterTargets.removeAll();insertRuntimeBadges.removeAll();automationTargets=[(automationMasterGain,0,"Master · Volume")];hasAudio = false; hasMidiContent = false; waveforms.removeAll()
        for busIndex in 0..<snapshot.bus_count {var bus=daw_bus();bus.struct_size=UInt32(MemoryLayout<daw_bus>.size);guard check(daw_get_bus(session,busIndex,&bus))else{return};let name=withUnsafeBytes(of:bus.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};orderedBuses.append((bus.id,name))}
        var transport = daw_transport(); transport.struct_size = UInt32(MemoryLayout<daw_transport>.size)
        guard check(daw_get_transport(session, &transport)) else { return }
        playheadFrame = transport.frame
        loopEnabled=transport.loop_enabled != 0
        if let end=rangeEnd,end>transport.duration { rangeStart=nil;rangeEnd=nil }
        if snapshot.track_count == 0 {
            let empty = label("Пока тихо.\n\nДобавь вокал, дубль или инструмент — названия и уровни сохранятся в проекте.", size: 16, color: .secondaryLabelColor)
            empty.maximumNumberOfLines = 0; rows.addArrangedSubview(empty)
            empty.heightAnchor.constraint(equalToConstant: 180).isActive = true
            let headerEmpty=label("Нет дорожек",size:11,color:.tertiaryLabelColor);trackHeaderRows.addArrangedSubview(headerEmpty);headerEmpty.widthAnchor.constraint(equalTo:trackHeaderRows.widthAnchor).isActive=true;headerEmpty.heightAnchor.constraint(equalToConstant:180).isActive=true
        }
        for index in 0..<snapshot.track_count {
            var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
            guard check(daw_get_track(session, index, &track)) else { return }
            let name = withUnsafeBytes(of: track.name) { bytes in String(decoding: bytes.prefix(while: { $0 != 0 }), as: UTF8.self) }
            trackIDs[Int(index)] = track.id
            trackNames[track.id] = name
            if track.audio_frames > 0 { hasAudio = true }
            var midiClipCount: UInt32 = 0; if track.audio_frames == 0 { _ = daw_get_midi_clip_count(session, track.id, &midiClipCount); if midiClipCount > 0 { hasMidiContent = true } }
            let accent=durableTrackColor(track.color,Int(index));let number = label(String(format: "%02d", index + 1), size: 12, color: accent)
            number.widthAnchor.constraint(equalToConstant: 26).isActive = true
            let field = NSTextField(string: name); field.tag = Int(index); field.delegate = self
            field.font = .systemFont(ofSize: 15, weight: .medium); field.isBordered = false; field.drawsBackground = false
            field.setAccessibilityLabel("Название дорожки \(index + 1)")
            field.widthAnchor.constraint(greaterThanOrEqualToConstant: 120).isActive = true
            let slider = AutomationSlider(value: track.gain_db, minValue: -120, maxValue: 24, target: self, action: #selector(changeGain(_:)))
            slider.tag = Int(index); slider.isContinuous = true; slider.setAccessibilityLabel("Уровень \(name), децибелы")
            slider.automationBegin = { [weak self] in self?.beginAutomation(target: automationTrackVolume, id: track.id, value: slider.doubleValue) }
            slider.automationEnd = { [weak self] in self?.endAutomationGesture() }
            slider.widthAnchor.constraint(equalToConstant: 105).isActive = true
            let gain = label(String(format: "%+.1f dB", track.gain_db), size: 12, color: .secondaryLabelColor)
            gain.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular); gain.widthAnchor.constraint(equalToConstant: 62).isActive = true
            let panSlider=AutomationSlider(value:track.pan,minValue:-1,maxValue:1,target:self,action:#selector(changePan(_:)));panSlider.tag=Int(index);panSlider.isContinuous=true;panSlider.widthAnchor.constraint(equalToConstant:105).isActive=true;panSlider.setAccessibilityLabel("Панорама \(name)")
            panSlider.automationBegin = { [weak self] in self?.beginAutomation(target: automationTrackPan, id: track.id, value: panSlider.doubleValue) }
            panSlider.automationEnd = { [weak self] in self?.endAutomationGesture() }
            let pan=label(String(format:"%+.2f",track.pan),size:12,color:.secondaryLabelColor);pan.font = .monospacedDigitSystemFont(ofSize:12,weight:.regular);pan.widthAnchor.constraint(equalToConstant:48).isActive=true
            let mute=button("M",#selector(toggleMute(_:)));mute.setButtonType(.toggle);mute.tag=Int(index);mute.state=track.muted != 0 ? .on:.off;mute.contentTintColor=track.muted != 0 ? .systemOrange:.secondaryLabelColor;mute.setAccessibilityLabel("Mute \(name)");mute.widthAnchor.constraint(equalToConstant:30).isActive=true
            let solo=button("S",#selector(toggleSolo(_:)));solo.setButtonType(.toggle);solo.tag=Int(index);solo.state=track.solo != 0 ? .on:.off;solo.contentTintColor=track.solo != 0 ? .systemYellow:.secondaryLabelColor;solo.setAccessibilityLabel("Solo \(name)");solo.widthAnchor.constraint(equalToConstant:30).isActive=true
            let importTake=button("＋ Дубль",#selector(importTake(_:)));importTake.tag=Int(index);importTake.isEnabled=track.audio_frames>0
            let takePopup=NSPopUpButton();takePopup.tag=Int(index);takePopup.target=self;takePopup.action=#selector(selectTake(_:));takePopup.widthAnchor.constraint(equalToConstant:120).isActive=true
            if track.take_count>0 {for takeIndex in 0..<track.take_count{var take=daw_take();take.struct_size=UInt32(MemoryLayout<daw_take>.size);guard check(daw_get_take(session,track.id,takeIndex,&take))else{return};let takeName=withUnsafeBytes(of:take.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};takePopup.addItem(withTitle:takeIndex==0 ? "Основной":takeName)}}
            let selectedTake=min(selectedTakes[track.id] ?? 0,max(0,Int(track.take_count)-1));selectedTakes[track.id]=selectedTake;takePopup.selectItem(at:selectedTake);takePopups[Int(index)]=takePopup
            let applyComp=button("В comp",#selector(applyComp(_:)));applyComp.tag=Int(index);applyComp.isEnabled=track.take_count>1
            var clips: [ClipGeometry] = []
            if track.audio_frames > 0 { for clipIndex in 0..<track.clip_count { var clip=daw_clip(); clip.struct_size=UInt32(MemoryLayout<daw_clip>.size); guard check(daw_get_clip(session,track.id,clipIndex,&clip)) else { return };var take=daw_take();take.struct_size=UInt32(MemoryLayout<daw_take>.size);var sourcePeaks=[Float](repeating:0,count:512);guard check(daw_get_take(session,track.id,clip.take_index,&take)),check(daw_get_take_waveform(session,track.id,clip.take_index,&sourcePeaks,512))else{return};clips.append(ClipGeometry(start:clip.start,sourceOffset:clip.source_offset,length:clip.length,fadeIn:clip.fade_in,fadeOut:clip.fade_out,takeIndex:clip.take_index,sourceFramesForTake:take.frames,sourcePeaks:sourcePeaks,color:clip.color,gainDb:clip.gain_db,muted:clip.muted != 0,looped:clip.looped != 0,pan:clip.pan)) } }
            let totalFrames=clips.reduce(UInt64(0)){$0+$1.length}
            let duration = label(track.audio_frames > 0 ? String(format: "%d клип. · %.1f с", track.clip_count, Double(totalFrames) / 48000) : (midiClipCount > 0 ? "MIDI · (midiClipCount) клип." : "Без аудио"), size: 11, color: .secondaryLabelColor)
            duration.widthAnchor.constraint(equalToConstant: 105).isActive = true
            let edit = button("Клип…", #selector(editClipPanel(_:))); edit.tag = Int(index); edit.isEnabled = track.audio_frames > 0
            let arm=button("R",#selector(toggleArm(_:)));arm.tag=Int(index);arm.state=armedTrackID==track.id ? .on:.off;arm.contentTintColor=armedTrackID==track.id ? .systemRed:.secondaryLabelColor;arm.isEnabled=track.audio_frames>0;arm.setAccessibilityLabel("Записывать новые дубли в \(name)");arm.widthAnchor.constraint(equalToConstant:30).isActive=true
            let split = button("Split", #selector(splitClipAtCursor(_:))); split.tag=Int(index); split.isEnabled=track.audio_frames>0
            let copy = button("Копия", #selector(duplicateSelectedClip(_:))); copy.tag=Int(index); copy.isEnabled=track.audio_frames>0
            let remove = button("Удалить", #selector(deleteSelectedClip(_:))); remove.tag=Int(index); remove.isEnabled=track.clip_count>1
            let crossfade = button("XFade", #selector(toggleSelectedCrossfade(_:))); crossfade.tag=Int(index); crossfade.isEnabled=track.clip_count>1
            var automationCount:UInt32=0;guard check(daw_get_track_volume_automation_count(session,track.id,&automationCount))else{return};var automationPoints:[(frame:UInt64,gain:Double)]=[];for pointIndex in 0..<automationCount{var point=daw_automation_point();point.struct_size=UInt32(MemoryLayout<daw_automation_point>.size);guard check(daw_get_track_volume_automation_point(session,track.id,pointIndex,&point))else{return};automationPoints.append((point.frame,point.gain_db))};let automation=button(automationCount==0 ? "V AUTO":"V \(automationCount)",#selector(editTrackAutomation(_:)));automation.tag=Int(index);automation.contentTintColor=automationCount==0 ? .secondaryLabelColor:.systemCyan
            var panAutomationCount:UInt32=0;guard check(daw_get_track_pan_automation_count(session,track.id,&panAutomationCount))else{return};var panAutomationPoints:[(frame:UInt64,value:Double)]=[];for pointIndex in 0..<panAutomationCount{var point=daw_automation_point();point.struct_size=UInt32(MemoryLayout<daw_automation_point>.size);guard check(daw_get_track_pan_automation_point(session,track.id,pointIndex,&point))else{return};panAutomationPoints.append((point.frame,point.gain_db))};let panAutomation=button(panAutomationCount==0 ? "P AUTO":"P \(panAutomationCount)",#selector(editTrackPanAutomation(_:)));panAutomation.tag=Int(index);panAutomation.contentTintColor=panAutomationCount==0 ? .secondaryLabelColor:.systemPurple
            automationTargets.append((automationTrackVolume,track.id,"\(name) · Volume"));automationTargets.append((automationTrackPan,track.id,"\(name) · Pan"))
            let trackOutputName = orderedBuses.first(where: { $0.id == track.output_bus_id })?.name ?? "Main"
            mixerKinds[track.id] = .track;mixerStrips.append(MixerStripModel(id:track.id,kind:.track,title:name,color:accent,volumeDb:track.gain_db,pan:track.pan,outputName:trackOutputName,inserts:mixerInsertSummaries(owner:Int32(DAW_INSERT_OWNER_TRACK),ownerID:track.id),sends:mixerSendSummaries(trackID:track.id,count:track.send_count),isSelected:selectedMixerID == track.id,isArmed:armedTrackID == track.id,isMuted:track.muted != 0,isSolo:track.solo != 0,isAutomationRead:automationMode == 0,outputID:track.output_bus_id,automationLabel:consoleAutomationLabel(.track,id:track.id)))
            remove.contentTintColor = .systemRed
            let output=routingPopup(selected:track.output_bus_id);output.target=self;output.action=#selector(changeOutput(_:));output.setAccessibilityLabel("Выход \(name)");outputTargets[ObjectIdentifier(output)]=(track.id,false)
            let addSend=NSPopUpButton();addSend.addItem(withTitle:"＋ Send…");addSend.lastItem?.representedObject=NSNumber(value:UInt64(0));for bus in orderedBuses{addSend.addItem(withTitle:bus.name);addSend.lastItem?.representedObject=NSNumber(value:bus.id)};addSend.target=self;addSend.action=#selector(addSend(_:));addSend.isEnabled = !orderedBuses.isEmpty;addSend.widthAnchor.constraint(equalToConstant:130).isActive=true;newSendTargets[ObjectIdentifier(addSend)]=track.id
            let detailGroup=NSStackView();detailGroup.orientation = .vertical;detailGroup.alignment = .leading;detailGroup.spacing=2
            let routingRow=NSStackView(views:[label(String(format:"TRACK %02d",index+1),size:10,color:accent),label(name,size:11,color:.labelColor),takePopup,importTake,applyComp,label("OUT",size:10,color:.tertiaryLabelColor),output,addSend,flexibleSpace()]);routingRow.spacing=7;routingRow.edgeInsets=NSEdgeInsets(top:5,left:10,bottom:6,right:10);routingRow.wantsLayer=true;routingRow.layer?.backgroundColor=NSColor(white:1,alpha:0.02).cgColor
            detailGroup.addArrangedSubview(routingRow);routingRow.widthAnchor.constraint(equalTo:detailGroup.widthAnchor).isActive=true
            let midiBadge=label("♫ MIDI",size:10,color:.systemPurple);midiBadge.isHidden = !(track.audio_frames == 0 && midiClipCount > 0);routingRow.insertArrangedSubview(midiBadge, at: 2);midiBadge.setAccessibilityLabel("Инструментальная MIDI-дорожка")
            let inserts=insertPanel(owner:Int32(DAW_INSERT_OWNER_TRACK),ownerID:track.id,title:name);detailGroup.addArrangedSubview(inserts);inserts.widthAnchor.constraint(equalTo:detailGroup.widthAnchor).isActive=true
            for sendIndex in 0..<track.send_count {
                var send=daw_send();send.struct_size=UInt32(MemoryLayout<daw_send>.size);guard check(daw_get_send(session,track.id,sendIndex,&send))else{return}
                let busName=orderedBuses.first(where:{$0.id==send.bus_id})?.name ?? "Bus \(send.bus_id)"
                let sendSlider=NSSlider(value:send.gain_db,minValue:-120,maxValue:24,target:self,action:#selector(changeSendGain(_:)));sendSlider.isContinuous=false;sendSlider.widthAnchor.constraint(equalToConstant:130).isActive=true
                let sendValue=label(String(format:"%+.1f dB",send.gain_db),size:11,color:.secondaryLabelColor);sendValue.font = .monospacedDigitSystemFont(ofSize:11,weight:.regular);sendValue.widthAnchor.constraint(equalToConstant:62).isActive=true
                let pre=button(send.pre_fader != 0 ? "PRE":"POST",#selector(toggleSendPre(_:)));pre.setButtonType(.momentaryPushIn);pre.widthAnchor.constraint(equalToConstant:54).isActive=true
                let delete=button("×",#selector(removeSend(_:)));delete.contentTintColor = .systemRed;delete.widthAnchor.constraint(equalToConstant:30).isActive=true
                let route=(track:track.id,bus:send.bus_id,gain:send.gain_db,pre:send.pre_fader != 0);sendControlTargets[ObjectIdentifier(sendSlider)]=route;sendControlTargets[ObjectIdentifier(pre)]=route;sendControlTargets[ObjectIdentifier(delete)]=route
                let sendRow=NSStackView(views:[label("SEND",size:10,color:.tertiaryLabelColor),label(busName,size:11,color:.labelColor),sendSlider,sendValue,pre,delete,flexibleSpace()]);sendRow.spacing=7;sendRow.edgeInsets=NSEdgeInsets(top:5,left:46,bottom:6,right:10);sendRow.wantsLayer=true;sendRow.layer?.backgroundColor=NSColor(white:1,alpha:0.02).cgColor
                detailGroup.addArrangedSubview(sendRow);sendRow.widthAnchor.constraint(equalTo:detailGroup.widthAnchor).isActive=true
            }
            consoleRows.addArrangedSubview(detailGroup);detailGroup.widthAnchor.constraint(equalTo:consoleRows.widthAnchor).isActive=true
            let timelineGroup=NSStackView();timelineGroup.orientation = .vertical;timelineGroup.alignment = .leading;timelineGroup.spacing=2
            if track.audio_frames > 0 {
                var peaks = [Float](repeating: 0, count: 512)
                guard check(daw_get_waveform(session, track.id, &peaks, 512)) else { return }
                let wave = WaveformView(frame: .zero)
                wave.trackAccent=accent
                wave.peaks = peaks; wave.clips = clips; wave.sourceFrames = track.audio_frames
                wave.automationPoints=automationPoints
                wave.panAutomationPoints=panAutomationPoints
                wave.snapFrames = gridFrames; wave.snapGrid = { [weak self] frame in self?.gridSnap(atFrame: frame) ?? (anchor:0,quantum:0) }; wave.rangeStart = rangeStart; wave.rangeEnd = rangeEnd;wave.loopEnabled=loopEnabled
                wave.selectedIndex=min(selectedClips[track.id] ?? 0,max(0,clips.count-1)); selectedClips[track.id]=wave.selectedIndex
                var group=(clipSelection[track.id] ?? []).filter{$0<clips.count}; if group.isEmpty{group=[wave.selectedIndex]}; group=Array(Set(group)).sorted(); clipSelection[track.id]=group; wave.selectedIndices=group
                wave.projectFrames = min(48000 * 600, max(48000 * 12, transport.duration + 48000 * 2))
                wave.playableFrames = transport.duration; wave.playhead = transport.frame
                wave.onEditBegin = { [weak self] in self?.stopAudio() }
                let trackID = track.id
                wave.onEdit = { [weak self] clipIndex,start,offset,length in self?.applyClipEdit(trackID,clipIndex,start,offset,length) }
                wave.onFadeEdit = { [weak self] clipIndex,fadeIn,fadeOut in self?.applyClipFades(trackID,clipIndex,fadeIn,fadeOut) }
                wave.onSelect = { [weak self] clipIndex,additive in self?.clipTapped(trackID,clipIndex,additive:additive) }
                wave.onNudge = { [weak self] direction in self?.nudgeClipGroup(trackID,direction) }
                laneViews[trackID] = wave
                wave.onClipMenu = { [weak self] clipIndex in self?.clipContextMenu(trackID,clipIndex) }
                wave.onDropFile = { [weak self] url, frame in self?.beginDropImport(url,trackID,frame) ?? false }
                wave.onClipHotkey = { [weak self] key in self?.clipHotkey(trackID,key) }
                wave.setAccessibilityLabel("Позиция на аудиоволне: \(name)")
                wave.onSeek = { [weak self] frame in self?.seekAudio(frame) }
                wave.onToggle = { [weak self] in guard let self else { return }; if self.isPlaying { self.stopAudio() } else { self.playAudio() } }
                timelineGroup.addArrangedSubview(wave)
                wave.heightAnchor.constraint(equalToConstant: 92).isActive = true
                wave.widthAnchor.constraint(equalTo: timelineGroup.widthAnchor).isActive = true
                waveforms.append(wave)
            } else {let empty=EmptyTimelineLaneView(message:"Import WAV or start recording",accent:accent);timelineGroup.addArrangedSubview(empty);empty.heightAnchor.constraint(equalToConstant:92).isActive=true;empty.widthAnchor.constraint(equalTo:timelineGroup.widthAnchor).isActive=true}
            if track.take_count>1 {for takeIndex in 0..<track.take_count{var take=daw_take();take.struct_size=UInt32(MemoryLayout<daw_take>.size);var takePeaks=[Float](repeating:0,count:512);guard check(daw_get_take(session,track.id,takeIndex,&take)),check(daw_get_take_waveform(session,track.id,takeIndex,&takePeaks,512))else{return};let takeName=withUnsafeBytes(of:take.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};let lane=TakeLaneView(frame:.zero);lane.title=takeIndex==0 ? "Основной":takeName;lane.peaks=takePeaks;lane.takeStart=take.start;lane.takeFrames=take.frames;lane.projectFrames=min(48000*600,max(48000*12,transport.duration+48000*2));lane.selected=selectedTake==Int(takeIndex);lane.setAccessibilityLabel("Дубль \(lane.title)");let laneIndex=Int(takeIndex);lane.onSelect={[weak self]in self?.selectedTakes[track.id]=laneIndex;self?.refresh()};timelineGroup.addArrangedSubview(lane);lane.heightAnchor.constraint(equalToConstant:46).isActive=true;lane.widthAnchor.constraint(equalTo:timelineGroup.widthAnchor).isActive=true}}
            let laneCount=track.take_count>1 ? Int(track.take_count):0;let groupHeight=CGFloat(92+laneCount*48)
            rows.addArrangedSubview(timelineGroup);timelineGroup.widthAnchor.constraint(equalTo:rows.widthAnchor).isActive=true;timelineGroup.heightAnchor.constraint(equalToConstant:groupHeight).isActive=true
            let header=PinnedTrackHeaderView(model:PinnedTrackHeaderModel(id:track.id,index:Int(index),name:name,accent:accent,gainDb:track.gain_db,pan:track.pan,armed:armedTrackID==track.id,muted:track.muted != 0,solo:track.solo != 0,takeCount:Int(track.take_count),hasAudio:track.audio_frames>0,selected:selectedMixerID==track.id || inspectorTrackID==track.id))
            header.onSelect={[weak self] id in self?.selectedMixerID=id;self?.inspectorTrackID=id;self?.inspectorClipIndex=nil;self?.refresh()};header.onRename={[weak self] id,name in guard let self else{return};if self.check(daw_rename_track(self.session,id,name,self.revision)){self.refresh()}};header.onArm={[weak self] id,armed in self?.armedTrackID=armed ? id:nil;if armed{var auto:Int32=0;if daw_get_auto_monitor_on_arm(self?.session,&auto)==0,auto==1{var mon:Int32=0;if daw_get_record_monitor(self?.session,&mon)==0,mon==0{_=daw_set_record_monitor(self?.session,1)}}};self?.refresh()};header.onMute={[weak self] id,value in self?.mixerSetMute(id,value)};header.onSolo={[weak self] id,value in self?.mixerSetSolo(id,value)};header.onGain={[weak self] id,value in self?.mixerSetVolume(id,value)};header.onPan={[weak self] id,value in self?.mixerSetPan(id,value)}
            header.onImportTake={[weak self] id in self?.performTrackAction(id,#selector(DraftApp.importTake(_:)))};header.onComp={[weak self] id in self?.performTrackAction(id,#selector(DraftApp.applyComp(_:)))};header.onSplit={[weak self] id in self?.performTrackAction(id,#selector(DraftApp.splitClipAtCursor(_:)))};header.onDuplicate={[weak self] id in self?.performTrackAction(id,#selector(DraftApp.duplicateSelectedClip(_:)))};header.onDelete={[weak self] id in self?.performTrackAction(id,#selector(DraftApp.deleteSelectedClip(_:)))};header.onCrossfade={[weak self] id in self?.performTrackAction(id,#selector(DraftApp.toggleSelectedCrossfade(_:)))};header.onDeleteTrack={[weak self] id in self?.deleteTrack(id)};header.onMoveToIndex={[weak self] id,insertionIndex in self?.moveTrack(id, toInsertionIndex: insertionIndex)};header.onDuplicateTrack={[weak self] id in self?.duplicateTrackNow(id)};header.onGroupMenu={[weak self] id in self?.showTrackGroupMenu(id)};header.onTrackColor={[weak self] id in self?.showTrackPalette(id)};header.onMidiTranspose={[weak self] id in self?.showMidiTranspose(id)};header.onMidiQuantize={[weak self] id in self?.showMidiQuantize(id)};header.onMidiColor={[weak self] id in self?.showMidiClipColor(id)};header.onMidiMove={[weak self] id in self?.showMidiMovePalette(id)};header.onMidiCopy={[weak self] id in self?.copyMidiClipNow(id)};header.onExportTrackWav={[weak self] id in self?.exportTrackAsWav(id)}
            trackHeaderRows.addArrangedSubview(header);header.widthAnchor.constraint(equalTo:trackHeaderRows.widthAnchor).isActive=true;header.heightAnchor.constraint(equalToConstant:groupHeight).isActive=true
        }
        let masterHeading=label("MASTER / PLUG-INS",size:10,color:.tertiaryLabelColor);masterHeading.font = .systemFont(ofSize:10,weight:.semibold)
        let masterControls=NSStackView(views:[masterHeading,masterSlider,masterLabel,masterAutomationButton,automationModePopup,automationArmPopup,flexibleSpace()]);masterControls.spacing=7;masterControls.alignment = .centerY;consoleRows.addArrangedSubview(masterControls);masterControls.widthAnchor.constraint(equalTo:consoleRows.widthAnchor).isActive=true
        let pluginTools=NSStackView(views:[masterAUPopup,scanAUButton,masterVST3Popup,scanVST3Button,flexibleSpace()]);pluginTools.spacing=8;pluginTools.alignment = .centerY;consoleRows.addArrangedSubview(pluginTools);pluginTools.widthAnchor.constraint(equalTo:consoleRows.widthAnchor).isActive=true
        for pluginIndex in 0..<snapshot.master_insert_count{
            var plugin=daw_plugin();plugin.struct_size=UInt32(MemoryLayout<daw_plugin>.size);guard check(daw_get_master_insert(session,pluginIndex,&plugin))else{return};let name=withUnsafeBytes(of:plugin.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)}
            var hostingStatus=daw_insert_hosting_status();hostingStatus.struct_size=UInt32(MemoryLayout<daw_insert_hosting_status>.size);let isolated=daw_get_insert_hosting_status(session,Int32(DAW_INSERT_OWNER_MASTER),0,plugin.id,&hostingStatus) == 0 && hostingStatus.selected_mode == UInt32(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS)
            let isVST3=plugin.type == 0 && plugin.subtype == 0 && plugin.manufacturer == 0;let badge=label(isVST3 ? "VST3":"AU",size:10,color:isVST3 ? .systemPurple:.systemBlue);badge.font = .systemFont(ofSize:10,weight:.semibold);badge.widthAnchor.constraint(equalToConstant:34).isActive=true
            let stateTitle=plugin.available == 0 ? "MISSING":(plugin.bypassed != 0 ? "BYPASSED":"ACTIVE")
            let bypass=button(stateTitle,#selector(toggleMasterInsert(_:)));bypass.setButtonType(.momentaryPushIn);bypass.isEnabled=plugin.available != 0;bypass.contentTintColor=plugin.available == 0 ? .systemRed:(plugin.bypassed != 0 ? .systemOrange:.systemGreen);bypass.widthAnchor.constraint(equalToConstant:84).isActive=true
            let remoteVST3Editor=isolated && isVST3
            let isolatedAU=isolated && !isVST3
            let edit=button(remoteVST3Editor ? "Параметры VST3…":"Параметры…",#selector(editMasterInsert(_:)));edit.isEnabled=plugin.available != 0 && !isolatedAU;edit.toolTip=isolatedAU ? "Этот isolated AU пока не предоставляет remote parameter editor. Переключи insert в режим «В процессе», чтобы изменить параметры.":(remoteVST3Editor ? "VST3 parameters открываются через отдельный helper; изменение выполнит controlled reprepare.":nil);edit.setAccessibilityLabel(isolatedAU ? "Параметры мастера недоступны для isolated AU" : (remoteVST3Editor ? "Параметры VST3 мастера в отдельном helper" : "Параметры мастера \(name)"));edit.setAccessibilityHelp(edit.toolTip ?? "Открывает параметры plug-in мастера.");pluginEditorTargets[ObjectIdentifier(edit)]=(plugin.id,remoteVST3Editor)
            let up=button("↑",#selector(moveMasterInsertUp(_:)));up.isEnabled=pluginIndex>0;let down=button("↓",#selector(moveMasterInsertDown(_:)));down.isEnabled=pluginIndex+1<snapshot.master_insert_count
            let remove=button("Удалить",#selector(removeMasterInsert(_:)));remove.contentTintColor = .systemRed
            let pluginTarget=(plugin.id,plugin.bypassed != 0,pluginIndex);for control in [bypass,up,down,remove]{pluginControlTargets[ObjectIdentifier(control)]=pluginTarget}
            let latency=label(String(format:"LATENCY  %.2f ms",Double(plugin.latency_frames)/48.0),size:10,color:.secondaryLabelColor);latency.font = .monospacedDigitSystemFont(ofSize:10,weight:.regular)
            let row=NSStackView(views:[badge,label(name,size:13,color:.labelColor),latency,flexibleSpace(),edit,up,down,bypass,remove]);row.spacing=8;row.edgeInsets=NSEdgeInsets(top:7,left:10,bottom:7,right:10);row.wantsLayer=true;row.layer?.backgroundColor=NSColor(calibratedRed:0.05,green:0.12,blue:0.18,alpha:0.4).cgColor;row.layer?.cornerRadius=2;consoleRows.addArrangedSubview(row);row.widthAnchor.constraint(equalTo:consoleRows.widthAnchor).isActive=true
            let hosting=insertHostingRow(owner:Int32(DAW_INSERT_OWNER_MASTER),ownerID:0,plugin:plugin);consoleRows.addArrangedSubview(hosting);hosting.widthAnchor.constraint(equalTo:consoleRows.widthAnchor).isActive=true
        }
        if !orderedBuses.isEmpty {
            let heading=label("BUS CONSOLE",size:10,color:.tertiaryLabelColor);heading.font = .systemFont(ofSize:10,weight:.semibold);consoleRows.addArrangedSubview(heading)
        }
        for busIndex in 0..<snapshot.bus_count {
            var bus=daw_bus();bus.struct_size=UInt32(MemoryLayout<daw_bus>.size);guard check(daw_get_bus(session,busIndex,&bus))else{return}
            let name=withUnsafeBytes(of:bus.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)}
            let badge=label("BUS",size:10,color:.systemPurple);badge.font = .systemFont(ofSize:10,weight:.semibold);badge.widthAnchor.constraint(equalToConstant:34).isActive=true
            let mute=button("M",#selector(toggleBusMute(_:)));mute.setButtonType(.toggle);mute.state=bus.muted != 0 ? .on:.off;mute.contentTintColor=bus.muted != 0 ? .systemOrange:.secondaryLabelColor;mute.widthAnchor.constraint(equalToConstant:30).isActive=true
            let field=NSTextField(string:name);field.delegate=self;field.font = .systemFont(ofSize:14,weight:.medium);field.isBordered=false;field.drawsBackground=false;field.widthAnchor.constraint(greaterThanOrEqualToConstant:130).isActive=true;busNameTargets[ObjectIdentifier(field)]=bus.id
            let gainSlider=AutomationSlider(value:bus.gain_db,minValue:-120,maxValue:24,target:self,action:#selector(changeBusGain(_:)));gainSlider.isContinuous=true;gainSlider.widthAnchor.constraint(equalToConstant:120).isActive=true
            gainSlider.automationBegin = { [weak self] in self?.beginAutomation(target: automationBusGain, id: bus.id, value: gainSlider.doubleValue) }
            gainSlider.automationEnd = { [weak self] in self?.endAutomationGesture() }
            let gainValue=label(String(format:"%+.1f dB",bus.gain_db),size:11,color:.secondaryLabelColor);gainValue.font = .monospacedDigitSystemFont(ofSize:11,weight:.regular);gainValue.widthAnchor.constraint(equalToConstant:62).isActive=true
            var busAutomationCount:UInt32=0;guard check(daw_get_bus_gain_automation_count(session,bus.id,&busAutomationCount))else{return};let busAutomation=button(busAutomationCount==0 ? "AUTO":"AUTO \(busAutomationCount)",#selector(editBusAutomation(_:)));busAutomation.contentTintColor=busAutomationCount==0 ? .secondaryLabelColor:.systemCyan;busAutomationTargets[ObjectIdentifier(busAutomation)]=bus.id
            let panSlider=NSSlider(value:bus.pan,minValue:-1,maxValue:1,target:self,action:#selector(changeBusPan(_:)));panSlider.isContinuous=false;panSlider.widthAnchor.constraint(equalToConstant:105).isActive=true
            let panValue=label(String(format:"%+.2f",bus.pan),size:11,color:.secondaryLabelColor);panValue.font = .monospacedDigitSystemFont(ofSize:11,weight:.regular);panValue.widthAnchor.constraint(equalToConstant:48).isActive=true
            let output=routingPopup(selected:bus.output_bus_id,excluding:bus.id);output.target=self;output.action=#selector(changeOutput(_:));outputTargets[ObjectIdentifier(output)]=(bus.id,true)
            busControlTargets[ObjectIdentifier(mute)]=bus.id;busControlTargets[ObjectIdentifier(gainSlider)]=bus.id;busControlTargets[ObjectIdentifier(panSlider)]=bus.id
            automationTargets.append((automationBusGain,bus.id,"\(name) · Volume"))
            let busOutputName = orderedBuses.first(where: { $0.id == bus.output_bus_id })?.name ?? "Main"
            mixerKinds[bus.id] = .bus;mixerStrips.append(MixerStripModel(id:bus.id,kind:.bus,title:name,color:.systemPurple,volumeDb:bus.gain_db,pan:bus.pan,outputName:busOutputName,inserts:mixerInsertSummaries(owner:Int32(DAW_INSERT_OWNER_BUS),ownerID:bus.id),isSelected:selectedMixerID == bus.id,isMuted:bus.muted != 0,isAutomationRead:automationMode == 0,outputID:bus.output_bus_id,automationLabel:consoleAutomationLabel(.bus,id:bus.id)))
            let row=NSStackView(views:[badge,mute,field,flexibleSpace(),label("VOL",size:10,color:.tertiaryLabelColor),gainSlider,gainValue,busAutomation,label("PAN",size:10,color:.tertiaryLabelColor),panSlider,panValue,label("OUT",size:10,color:.tertiaryLabelColor),output]);row.spacing=7;row.edgeInsets=NSEdgeInsets(top:7,left:10,bottom:7,right:10);row.wantsLayer=true;row.layer?.backgroundColor=NSColor(calibratedRed:0.16,green:0.10,blue:0.22,alpha:0.18).cgColor;row.layer?.cornerRadius=2
            let group=NSStackView();group.orientation = .vertical;group.alignment = .leading;group.spacing = 2;group.addArrangedSubview(row);row.widthAnchor.constraint(equalTo:group.widthAnchor).isActive=true
            let inserts=insertPanel(owner:Int32(DAW_INSERT_OWNER_BUS),ownerID:bus.id,title:name);group.addArrangedSubview(inserts);inserts.widthAnchor.constraint(equalTo:group.widthAnchor).isActive=true
            consoleRows.addArrangedSubview(group);group.widthAnchor.constraint(equalTo:consoleRows.widthAnchor).isActive=true
        }
        mixerKinds[0] = .master;mixerStrips.append(MixerStripModel(id:0,kind:.master,title:"MASTER",color:.systemOrange,volumeDb:snapshot.master_gain_db,outputName:"Output 1–2",inserts:mixerInsertSummaries(owner:Int32(DAW_INSERT_OWNER_MASTER),ownerID:0),isSelected:selectedMixerID == 0,isAutomationRead:automationMode == 0,automationLabel:consoleAutomationLabel(.master,id:0)));mixerWorkspace.editingEnabled = !isRecording;mixerWorkspace.strips=mixerStrips;timelineRuler.projectFrames=min(48000*600,max(48000*12,transport.duration+48000*2));timelineRuler.playhead=transport.frame
        reloadAutomationArmPopup()
        exportButton.isEnabled = hasAudio && !exportBusy && !isRecording
        dawprojectButton.isEnabled = !exportBusy && !isRecording
        cancelExportButton.isEnabled = exportBusy
        cancelExportButton.isHidden = !exportBusy
        updateTimelineTools()
        if let track=inspectorTrackID,let clip=inspectorClipIndex {updateClipInspector(track,clip)}
        else if let selected=selectedMixerID,mixerKinds[selected] != nil {updateMixerInspector(selected)}
    }
    func insertOwnerKey(_ owner: Int32, _ ownerID: UInt64) -> String { "\(owner):\(ownerID)" }
    func hostingModeTitle(_ mode: UInt32) -> String {
        switch mode { case UInt32(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS): return "Изолированно"; default: return "В процессе" }
    }
    func insertRuntimeStatus(owner: Int32, ownerID: UInt64, pluginID: UInt64) -> daw_insert_runtime_status? {
        var runtime=daw_insert_runtime_status();runtime.struct_size=UInt32(MemoryLayout<daw_insert_runtime_status>.size)
        return daw_get_insert_runtime_status(session,owner,ownerID,pluginID,&runtime) == 0 ? runtime:nil
    }
    func runtimeFaultHint(_ code: UInt32) -> String {
        switch code {
        case UInt32(DAW_INSERT_RUNTIME_FAULT_PREPARE_FAILED): return " Подготовка plug-in не удалась."
        case UInt32(DAW_INSERT_RUNTIME_FAULT_RESTART_REQUIRED): return " Нажми Play, чтобы подготовить граф."
        case UInt32(DAW_INSERT_RUNTIME_FAULT_DEADLINE_MISSED): return " Изолированный helper не уложился в аудиосрок."
        case UInt32(DAW_INSERT_RUNTIME_FAULT_PROTOCOL_ERROR): return " Ошибка протокола изолированного helper."
        case UInt32(DAW_INSERT_RUNTIME_FAULT_HELPER_EXITED): return " Изолированный helper завершился. Нажми Play для новой подготовки."
        default: return ""
        }
    }
    func runtimeBadge(_ runtime: daw_insert_runtime_status?) -> (title: String, color: NSColor, hint: String) {
        guard let runtime else { return ("UNPREPARED",.secondaryLabelColor,"Статус подготовленного графа недоступен.") }
        let latency = runtime.extra_pipeline_latency_frames == 0 ? "" : String(format:" Доп. pipeline latency: %.2f ms.",Double(runtime.extra_pipeline_latency_frames)/48.0)
        let fault=runtimeFaultHint(runtime.fault_code)
        switch runtime.state {
        case UInt32(DAW_INSERT_RUNTIME_ACTIVE_IN_PROCESS): return ("IN PROCESS",.systemGreen,"Insert подготовлен и исполняется в процессе." + latency + fault)
        case UInt32(DAW_INSERT_RUNTIME_ACTIVE_ISOLATED): return ("ISOLATED",.systemCyan,"Insert подготовлен в изолированном процессе." + latency + fault)
        case UInt32(DAW_INSERT_RUNTIME_DRY_FALLBACK): return ("DRY",.systemOrange,"Граф использует dry fallback." + latency + fault)
        case UInt32(DAW_INSERT_RUNTIME_FAILED): return ("FAILED",.systemRed,"Подготовка графа не удалась." + fault)
        default:
            return ("UNPREPARED",.secondaryLabelColor,"Граф ещё не подготовлен." + fault)
        }
    }
    func updateInsertRuntimeBadges() {
        for target in insertRuntimeBadges {
            let raw=insertRuntimeStatus(owner:target.owner,ownerID:target.ownerID,pluginID:target.id)
            let runtime=runtimeBadge(raw)
            target.label.stringValue=runtime.title;target.label.textColor=runtime.color;target.label.toolTip=runtime.hint
            target.label.setAccessibilityLabel("Runtime insert: \(runtime.title)");target.label.setAccessibilityHelp(runtime.hint)
            let pending=raw?.state == UInt32(DAW_INSERT_RUNTIME_UNPREPARED) ? " · ждёт Play":""
            target.policy.stringValue="Policy: \(hostingModeTitle(target.selectedMode))\(pending)"
        }
    }
    func hostingUnavailableReason(_ format: UInt32) -> String {
        switch format {
        case UInt32(DAW_INSERT_HOSTING_FORMAT_VST3): return "VST3 пока не поддерживает отдельный процесс"
        case UInt32(DAW_INSERT_HOSTING_FORMAT_AUV2): return "Этот AUv2 не предоставляет отдельный процесс"
        case UInt32(DAW_INSERT_HOSTING_FORMAT_AUV3): return "Этот AUv3 не предоставляет отдельный процесс"
        default: return "Формат этого плагина не предоставляет отдельный процесс"
        }
    }
    func insertHostingRow(owner: Int32, ownerID: UInt64, plugin: daw_plugin) -> NSStackView {
        var hosting=daw_insert_hosting_status();hosting.struct_size=UInt32(MemoryLayout<daw_insert_hosting_status>.size)
        let row=NSStackView();row.spacing=7;row.edgeInsets=NSEdgeInsets(top:2,left:88,bottom:4,right:10)
        let heading=label("ХОСТИНГ",size:10,color:.secondaryLabelColor);heading.font = .systemFont(ofSize:10,weight:.semibold);heading.widthAnchor.constraint(equalToConstant:58).isActive=true
        row.addArrangedSubview(heading)
        guard daw_get_insert_hosting_status(session,owner,ownerID,plugin.id,&hosting) == 0 else {
            row.addArrangedSubview(label("Статус хостинга недоступен",size:10,color:.secondaryLabelColor));return row
        }
        let mode=NSPopUpButton();mode.addItem(withTitle:"В процессе");mode.lastItem?.representedObject=NSNumber(value:UInt32(DAW_INSERT_HOSTING_MODE_IN_PROCESS))
        mode.addItem(withTitle:"Изолированно");mode.lastItem?.representedObject=NSNumber(value:UInt32(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS))
        if let item=mode.itemArray.first(where: { ($0.representedObject as? NSNumber)?.uint32Value == hosting.selected_mode }) { mode.select(item) }
        let outOfProcessAvailable=(hosting.supported_modes & UInt32(DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS)) != 0
        if !outOfProcessAvailable { mode.item(at:1)?.isEnabled=false }
        mode.isEnabled=plugin.available != 0 && !isRecording
        mode.target=self;mode.action=#selector(changeInsertHostingMode(_:));mode.widthAnchor.constraint(equalToConstant:126).isActive=true;mode.setAccessibilityLabel("Режим хостинга insert \(plugin.id)");mode.setAccessibilityHelp("В процессе исполняет plug-in внутри приложения. Изолированно доступен только для поддерживаемого plug-in.");insertHostingTargets[ObjectIdentifier(mode)]=(owner,ownerID,plugin.id)
        let runtimeStatus=insertRuntimeStatus(owner:owner,ownerID:ownerID,pluginID:plugin.id)
        let pending=runtimeStatus?.state == UInt32(DAW_INSERT_RUNTIME_UNPREPARED) ? " · ждёт Play":""
        let policy=label("Policy: \(hostingModeTitle(hosting.selected_mode))\(pending)",size:10,color:.secondaryLabelColor);policy.font = .monospacedDigitSystemFont(ofSize:10,weight:.regular)
        let runtime=runtimeBadge(runtimeStatus)
        let badge=label(runtime.title,size:10,color:runtime.color);badge.font = .monospacedDigitSystemFont(ofSize:10,weight:.semibold);badge.toolTip=runtime.hint;badge.setAccessibilityLabel("Runtime insert: \(runtime.title)");badge.setAccessibilityHelp(runtime.hint)
        insertRuntimeBadges.append((badge,policy,hosting.selected_mode,owner,ownerID,plugin.id))
        row.addArrangedSubview(mode);row.addArrangedSubview(policy);row.addArrangedSubview(badge)
        if !outOfProcessAvailable {
            row.addArrangedSubview(label(hostingUnavailableReason(hosting.format),size:10,color:.systemOrange))
        }
        return row
    }
    func insertPanel(owner: Int32, ownerID: UInt64, title: String) -> NSStackView {
        var count: UInt32 = 0
        guard check(daw_get_insert_count(session,owner,ownerID,&count)) else { return NSStackView() }
        let key=insertOwnerKey(owner,ownerID);let expanded=expandedInsertOwners.contains(key)
        let disclosure=button(expanded ? "⌄ INSERTS \(count)":"› INSERTS \(count)",#selector(toggleInsertDisclosure(_:)));disclosure.contentTintColor=count == 0 ? .secondaryLabelColor:.systemMint;insertDisclosureTargets[ObjectIdentifier(disclosure)]=(owner,ownerID,title)
        let add=button("＋ Insert…",#selector(addOwnerInsert(_:)));add.contentTintColor = .systemMint;insertDisclosureTargets[ObjectIdentifier(add)]=(owner,ownerID,title)
        let panel=NSStackView();panel.orientation = .vertical;panel.alignment = .leading;panel.spacing = 4
        let tools=NSStackView(views:[disclosure,add,flexibleSpace()]);tools.spacing=7;tools.edgeInsets=NSEdgeInsets(top:5,left:46,bottom:5,right:10);tools.wantsLayer=true;tools.layer?.backgroundColor=NSColor(white:1,alpha:0.018).cgColor;panel.addArrangedSubview(tools);tools.widthAnchor.constraint(equalTo:panel.widthAnchor).isActive=true
        guard expanded else{return panel}
        for index in 0..<count {
            var plugin=daw_plugin();plugin.struct_size=UInt32(MemoryLayout<daw_plugin>.size);guard check(daw_get_insert(session,owner,ownerID,index,&plugin))else{return panel}
            let name=withUnsafeBytes(of:plugin.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};let vst3=plugin.type == 0 && plugin.subtype == 0 && plugin.manufacturer == 0
            var hostingStatus=daw_insert_hosting_status();hostingStatus.struct_size=UInt32(MemoryLayout<daw_insert_hosting_status>.size);let isolated=daw_get_insert_hosting_status(session,owner,ownerID,plugin.id,&hostingStatus) == 0 && hostingStatus.selected_mode == UInt32(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS)
            let badge=label(vst3 ? "VST3":"AU",size:10,color:vst3 ? .systemPurple:.systemOrange);badge.font = .systemFont(ofSize:10,weight:.semibold);badge.widthAnchor.constraint(equalToConstant:34).isActive=true
            let state=plugin.available == 0 ? "MISSING":(plugin.bypassed != 0 ? "BYPASSED":"ACTIVE");let bypass=button(state,#selector(toggleOwnerInsert(_:)));bypass.isEnabled=plugin.available != 0;bypass.contentTintColor=plugin.available == 0 ? .systemRed:(plugin.bypassed != 0 ? .systemOrange:.systemGreen);bypass.widthAnchor.constraint(equalToConstant:84).isActive=true
            let remoteVST3Editor=isolated && vst3
            let isolatedAU=isolated && !vst3
            let edit=button(remoteVST3Editor ? "Параметры VST3…":"Параметры…",#selector(editOwnerInsert(_:)));edit.isEnabled=plugin.available != 0 && !isolatedAU;edit.toolTip=isolatedAU ? "Этот isolated AU пока не предоставляет remote parameter editor. Переключи insert в режим «В процессе», чтобы изменить параметры.":(remoteVST3Editor ? "VST3 parameters открываются через отдельный helper; изменение выполнит controlled reprepare.":nil);edit.setAccessibilityLabel(isolatedAU ? "Параметры недоступны для isolated AU" : (remoteVST3Editor ? "Параметры VST3 в отдельном helper" : "Параметры \(name)"));edit.setAccessibilityHelp(edit.toolTip ?? "Открывает параметры plug-in.");insertEditorTargets[ObjectIdentifier(edit)]=(owner,ownerID,plugin.id,remoteVST3Editor)
            let up=button("↑",#selector(moveOwnerInsertUp(_:)));up.isEnabled=index>0;let down=button("↓",#selector(moveOwnerInsertDown(_:)));down.isEnabled=index+1<count;let remove=button("Удалить",#selector(removeOwnerInsert(_:)));remove.contentTintColor = .systemRed
            let target=(owner,ownerID,plugin.id,plugin.bypassed != 0,index);for control in [bypass,up,down,remove]{insertControlTargets[ObjectIdentifier(control)]=target}
            let latency=label(String(format:"%.2f ms",Double(plugin.latency_frames)/48.0),size:10,color:.secondaryLabelColor);latency.font = .monospacedDigitSystemFont(ofSize:10,weight:.regular)
            let row=NSStackView(views:[badge,label(name,size:12,color:.labelColor),latency,flexibleSpace(),edit,up,down,bypass,remove]);row.spacing=7;row.edgeInsets=NSEdgeInsets(top:5,left:54,bottom:5,right:10);row.wantsLayer=true;row.layer?.backgroundColor=NSColor(white:1,alpha:0.02).cgColor;row.layer?.cornerRadius=2;panel.addArrangedSubview(row);row.widthAnchor.constraint(equalTo:panel.widthAnchor).isActive=true
            let hosting=insertHostingRow(owner:owner,ownerID:ownerID,plugin:plugin);panel.addArrangedSubview(hosting);hosting.widthAnchor.constraint(equalTo:panel.widthAnchor).isActive=true
        }
        return panel
    }
    @objc func toggleInsertDisclosure(_ sender: NSButton) { guard let target=insertDisclosureTargets[ObjectIdentifier(sender)] else{return};let key=insertOwnerKey(target.owner,target.ownerID);if expandedInsertOwners.contains(key){expandedInsertOwners.remove(key)}else{expandedInsertOwners.insert(key)};refresh() }
    @objc func addOwnerInsert(_ sender: NSButton) {
        guard !isRecording,let target=insertDisclosureTargets[ObjectIdentifier(sender)] else{return}
        addInsertOnChannel(owner:target.owner,ownerID:target.ownerID,title:target.title)
    }
    func addInsertOnChannel(owner:Int32,ownerID:UInt64,title:String) {
        guard !isRecording else { return }
        let target=(owner:owner,ownerID:ownerID,title:title)
        let choices=auCatalog.map{("AU · \($0.name)",false,$0.type,$0.subtype,$0.manufacturer,UInt32(0))}+vst3Catalog.filter{$0.available}.map{("\($0.instrument ? "🎹 ":"")VST3 · \($0.name)\($0.vendor.isEmpty ? "":" — \($0.vendor)")",true,UInt32(0),UInt32(0),UInt32(0),$0.index)}
        guard !choices.isEmpty else{storageMessage("Сначала отсканируй AU или VST3 плагины.");return};let popup=NSPopUpButton();for choice in choices{popup.addItem(withTitle:choice.0)};popup.widthAnchor.constraint(equalToConstant:440).isActive=true;let alert=NSAlert();alert.messageText="Добавить insert: \(target.title)";alert.informativeText="Плагин создаётся на выбранной полосе.";alert.accessoryView=popup;alert.addButton(withTitle:"Добавить");alert.addButton(withTitle:"Отмена");guard alert.runModal() == .alertFirstButtonReturn else{return};let choice=choices[popup.indexOfSelectedItem];_ = daw_stop(session);let result=choice.1 ? daw_add_insert_vst3(session,target.owner,target.ownerID,choice.5,revision):daw_add_insert_au(session,target.owner,target.ownerID,choice.2,choice.3,choice.4,revision);if check(result){expandedInsertOwners.insert(insertOwnerKey(target.owner,target.ownerID));refresh();pollTransport()}
    }
    @objc func toggleOwnerInsert(_ sender:NSButton){guard !isRecording,let target=insertControlTargets[ObjectIdentifier(sender)]else{return};_=daw_stop(session);if check(daw_set_insert_bypass(session,target.owner,target.ownerID,target.id,target.bypassed ? 0:1,revision)){refresh();pollTransport()}}
    @objc func moveOwnerInsertUp(_ sender:NSButton){guard let target=insertControlTargets[ObjectIdentifier(sender)],target.index>0 else{return};_=daw_stop(session);if check(daw_move_insert(session,target.owner,target.ownerID,target.id,target.index-1,revision)){refresh();pollTransport()}}
    @objc func moveOwnerInsertDown(_ sender:NSButton){guard let target=insertControlTargets[ObjectIdentifier(sender)]else{return};_=daw_stop(session);if check(daw_move_insert(session,target.owner,target.ownerID,target.id,target.index+1,revision)){refresh();pollTransport()}}
    @objc func removeOwnerInsert(_ sender:NSButton){guard !isRecording,let target=insertControlTargets[ObjectIdentifier(sender)]else{return};_=daw_stop(session);if check(daw_remove_insert(session,target.owner,target.ownerID,target.id,revision)){refresh();pollTransport()}}
    @objc func changeInsertHostingMode(_ sender:NSPopUpButton){guard !isRecording,let target=insertHostingTargets[ObjectIdentifier(sender)],let mode=(sender.selectedItem?.representedObject as? NSNumber)?.uint32Value else{return};_=daw_stop(session);if check(daw_set_insert_hosting_mode(session,target.owner,target.ownerID,target.id,mode,revision)){refresh();pollTransport()}}
    @objc func editOwnerInsert(_ sender:NSButton){
        guard !isRecording,let target=insertEditorTargets[ObjectIdentifier(sender)]else{return}
        editInsertOnChannel(owner:target.owner,ownerID:target.ownerID,id:target.id,isolatedVST3:target.isolatedVST3)
    }
    func editInsertOnChannel(owner:Int32,ownerID:UInt64,id:UInt64,isolatedVST3:Bool) {
        guard !isRecording else { return }
        let target=(owner:owner,ownerID:ownerID,id:id,isolatedVST3:isolatedVST3)
        var hosting=daw_insert_hosting_status();hosting.struct_size=UInt32(MemoryLayout<daw_insert_hosting_status>.size)
        guard daw_get_insert_hosting_status(session,target.owner,target.ownerID,target.id,&hosting) == 0 else{return}
        let isolated=hosting.selected_mode == UInt32(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS)
        guard !isolated || target.isolatedVST3 else { storageMessage("Этот isolated AU пока не предоставляет remote parameter editor. Переключи insert в режим «В процессе», чтобы изменить параметры."); return }
        if target.isolatedVST3 { _=daw_stop(session);pollTransport() }
        var count:UInt32=0;guard check(daw_get_insert_parameter_count(session,target.owner,target.ownerID,target.id,&count))else{return};let content=NSStackView();content.orientation = .vertical;content.spacing = 8;content.alignment = .leading;insertParameterTargets.removeAll()
        for index in 0..<count{var parameter=daw_au_parameter();parameter.struct_size=UInt32(MemoryLayout<daw_au_parameter>.size);guard check(daw_get_insert_parameter(session,target.owner,target.ownerID,target.id,index,&parameter))else{return};let name=withUnsafeBytes(of:parameter.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};let slider=AutomationSlider(value:Double(parameter.value),minValue:Double(parameter.minimum),maxValue:Double(parameter.maximum),target:self,action:#selector(changeOwnerInsertParameter(_:)));slider.isContinuous = !target.isolatedVST3;slider.isEnabled=parameter.writable != 0;slider.widthAnchor.constraint(equalToConstant:220).isActive=true;slider.setAccessibilityLabel("Параметр \(name)");slider.setAccessibilityHelp(target.isolatedVST3 ? "VST3 исполняется в отдельном helper. Значение применяется одним controlled reprepare после отпускания slider." : "Изменяет параметр plug-in.");let gestureTarget=(target.owner,target.ownerID,target.id,parameter.id,name);slider.automationBegin={[weak self]in let range=slider.maxValue-slider.minValue;self?.beginPluginParameterAutomation(gestureTarget,normalized:range == 0 ? 0:(slider.doubleValue-slider.minValue)/range)};slider.automationEnd={[weak self]in self?.endPluginParameterAutomation()};insertParameterTargets[ObjectIdentifier(slider)]=(target.owner,target.ownerID,target.id,parameter.id);pluginParameterGestureTargets[ObjectIdentifier(slider)]=(target.owner,target.ownerID,target.id,parameter.id,name,slider.minValue,slider.maxValue);let value=label(String(format:"%.3f",parameter.value),size:11,color:.secondaryLabelColor);value.font = .monospacedDigitSystemFont(ofSize:11,weight:.regular);value.widthAnchor.constraint(equalToConstant:66).isActive=true;var views:[NSView]=[label(name,size:12,color:.labelColor),flexibleSpace(),slider,value];if let automation=parameterAutomationButton(owner:target.owner,ownerID:target.ownerID,plugin:target.id,parameter:parameter,name:name){views.insert(automation,at:1)};views.insert(parameterArmButton(gestureTarget),at:1);let row=NSStackView(views:views);row.spacing=8;row.widthAnchor.constraint(equalToConstant:560).isActive=true;content.addArrangedSubview(row)}
        if count==0{content.addArrangedSubview(label("Этот плагин не публикует параметры.",size:13,color:.secondaryLabelColor))};let scroll=NSScrollView();scroll.documentView=content;scroll.hasVerticalScroller=true;scroll.drawsBackground=false;scroll.widthAnchor.constraint(equalToConstant:560).isActive=true;scroll.heightAnchor.constraint(equalToConstant:min(420,max(90,CGFloat(count)*34))).isActive=true;let alert=NSAlert();alert.messageText="Параметры плагина";alert.informativeText=target.isolatedVST3 ? "VST3 работает в отдельном helper. Изменение параметра выполнит controlled reprepare и сохранится в проекте.":"Изменения сохраняются в проекте и применяются к playback и export.";alert.accessoryView=scroll;alert.addButton(withTitle:"Готово");alert.runModal();insertParameterTargets.removeAll();refresh();pollTransport()
    }
    @objc func changeOwnerInsertParameter(_ sender:NSSlider){guard let target=insertParameterTargets[ObjectIdentifier(sender)],let gesture=pluginParameterGestureTargets[ObjectIdentifier(sender)]else{return};let armed=(gesture.owner,gesture.ownerID,gesture.plugin,gesture.parameter,gesture.name);if pluginParameterWrites(armed){let range=gesture.maximum-gesture.minimum;_ = writePluginParameterAutomation(armed,normalized:range == 0 ? 0:(sender.doubleValue-gesture.minimum)/range);return};_=daw_stop(session);if check(daw_set_insert_parameter(session,target.owner,target.ownerID,target.plugin,target.parameter,Float(sender.doubleValue),revision)){var snapshot=daw_snapshot();snapshot.struct_size=UInt32(MemoryLayout<daw_snapshot>.size);if daw_get_snapshot(session,&snapshot)==0{revision=snapshot.revision;window.isDocumentEdited=dirty}}}
    func parameterAutomationButton(owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: daw_au_parameter, name: String) -> NSButton? {
        var count: UInt32 = 0
        guard check(daw_get_insert_parameter_automation_count(session,owner,ownerID,plugin,parameter.id,&count)) else { return nil }
        let button=button(count == 0 ? "AUTO":"AUTO \(count)",#selector(editInsertParameterAutomation(_:)));button.contentTintColor=count == 0 ? .secondaryLabelColor:.systemCyan;button.widthAnchor.constraint(equalToConstant:64).isActive=true;parameterAutomationTargets[ObjectIdentifier(button)]=(owner,ownerID,plugin,parameter.id,name,Double(parameter.normalized_value));return button
    }
    func parameterArmButton(_ target: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String)) -> NSButton {
        let armed=pluginParameterArmed(target);let button=button(armed ? "ARMED":"ARM",#selector(togglePluginParameterArm(_:)));button.contentTintColor=armed ? .systemRed:.secondaryLabelColor;button.widthAnchor.constraint(equalToConstant:58).isActive=true;pluginParameterArmTargets[ObjectIdentifier(button)]=target;return button
    }
    @objc func editInsertParameterAutomation(_ sender: NSButton) {
        guard !isRecording,let target=parameterAutomationTargets[ObjectIdentifier(sender)] else{return};var count:UInt32=0;guard check(daw_get_insert_parameter_automation_count(session,target.owner,target.ownerID,target.plugin,target.parameter,&count))else{return};var points:[daw_plugin_parameter_automation_point]=[]
        for index in 0..<count{var point=daw_plugin_parameter_automation_point();point.struct_size=UInt32(MemoryLayout<daw_plugin_parameter_automation_point>.size);guard check(daw_get_insert_parameter_automation_point(session,target.owner,target.ownerID,target.plugin,target.parameter,index,&point))else{return};points.append(point)}
        let popup=NSPopUpButton();for point in points{popup.addItem(withTitle:String(format:"%7.3f с     %.3f",Double(point.frame)/48000,point.normalized_value))};popup.isEnabled = !points.isEmpty;popup.widthAnchor.constraint(equalToConstant:280).isActive=true;let alert=NSAlert();alert.messageText="Автоматизация: \(target.name)";alert.informativeText="Добавь текущее normalized значение параметра в позиции курсора или удали выбранную точку.";alert.accessoryView=popup;alert.addButton(withTitle:"Точка в позиции");alert.addButton(withTitle:"Удалить выбранную");alert.addButton(withTitle:"Закрыть");let response=alert.runModal()
        if response == .alertFirstButtonReturn,let frame=currentTransportFrame(){_=daw_stop(session);if check(daw_upsert_insert_parameter_automation_point(session,target.owner,target.ownerID,target.plugin,target.parameter,target.name,frame,target.normalized,revision)){refresh();pollTransport()}}
        else if response == .alertSecondButtonReturn,!points.isEmpty{let frame=points[popup.indexOfSelectedItem].frame;_=daw_stop(session);if check(daw_remove_insert_parameter_automation_point(session,target.owner,target.ownerID,target.plugin,target.parameter,frame,revision)){refresh();pollTransport()}}
    }
    func reloadAutomationArmPopup() {
        let previous = automationArm
        automationArmPopup.removeAllItems(); automationArmPopup.addItem(withTitle:"ARM: none"); automationArmPopup.lastItem?.representedObject=NSNumber(value:0)
        for (index,target) in automationTargets.enumerated() { automationArmPopup.addItem(withTitle:target.title); automationArmPopup.lastItem?.representedObject=NSNumber(value:index + 1) }
        if let previous,let index=automationTargets.firstIndex(where:{$0.target==previous.target && $0.id==previous.id}) { automationArmPopup.selectItem(at:index + 1) } else { automationArm=nil; automationArmPopup.selectItem(at:0) }
    }
    @objc func changeAutomationMode(_ sender:NSPopUpButton) {
        automationMode = sender.indexOfSelectedItem == 1 ? automationTouch : (sender.indexOfSelectedItem == 2 ? automationLatch : 0)
        if automationGesture != nil { cancelAutomationGesture() };if pluginParameterGesture != nil { cancelPluginParameterAutomation() }
    }
    @objc func changeAutomationArm(_ sender:NSPopUpButton) {
        if sender.indexOfSelectedItem <= 0 { automationArm=nil; if automationGesture != nil { cancelAutomationGesture() }; if pluginParameterGesture != nil { cancelPluginParameterAutomation() }; return }
        let selection=automationTargets[sender.indexOfSelectedItem - 1]; automationArm=(selection.target,selection.id)
        if automationGesture != nil { cancelAutomationGesture() };if pluginParameterGesture != nil { cancelPluginParameterAutomation() }
    }
    func beginAutomation(target: Int32, id: UInt64, value: Double) {
        guard !isRecording,automationMode != 0,automationArm?.target == target,automationArm?.id == id,automationGesture == nil,let frame=currentTransportFrame() else{return}
        if check(daw_begin_automation_gesture(session,target,id,automationMode,revision)) {
            automationGesture=(target,id)
            _=check(daw_write_automation_gesture(session,frame,value))
        }
    }
    func writeAutomation(target: Int32, id: UInt64, value: Double) -> Bool {
        guard automationGesture?.target == target,automationGesture?.id == id,let frame=currentTransportFrame() else{return false}
        return check(daw_write_automation_gesture(session,frame,value))
    }
    func automationWrites(target: Int32, id: UInt64) -> Bool { automationMode != 0 && automationArm?.target == target && automationArm?.id == id }
    func endAutomationGesture() {
        guard automationGesture != nil else{return}; defer { automationGesture=nil }
        guard let frame=currentTransportFrame() else { daw_cancel_automation_gesture(session); return }
        if check(daw_end_automation_gesture(session,frame,revision)) { refresh(); pollTransport() } else { daw_cancel_automation_gesture(session) }
    }
    func cancelAutomationGesture() { if automationGesture != nil { daw_cancel_automation_gesture(session);automationGesture=nil } }
    func pluginParameterArmed(_ target: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String)) -> Bool { armedPluginParameter?.owner == target.owner && armedPluginParameter?.ownerID == target.ownerID && armedPluginParameter?.plugin == target.plugin && armedPluginParameter?.parameter == target.parameter }
    func pluginParameterWrites(_ target: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String)) -> Bool { automationMode != 0 && pluginParameterArmed(target) }
    func beginPluginParameterAutomation(_ target: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String), normalized: Double) {
        guard !isRecording,automationMode != 0,pluginParameterArmed(target),pluginParameterGesture == nil,let frame=currentTransportFrame() else{return}
        if check(daw_begin_insert_parameter_automation_gesture(session,target.owner,target.ownerID,target.plugin,target.parameter,target.name,automationMode,revision)){pluginParameterGesture=(target.owner,target.ownerID,target.plugin,target.parameter);_ = check(daw_write_insert_parameter_automation_gesture(session,frame,normalized))}
    }
    func writePluginParameterAutomation(_ target: (owner: Int32, ownerID: UInt64, plugin: UInt64, parameter: UInt32, name: String), normalized: Double) -> Bool {
        guard pluginParameterGesture?.owner == target.owner,pluginParameterGesture?.ownerID == target.ownerID,pluginParameterGesture?.plugin == target.plugin,pluginParameterGesture?.parameter == target.parameter,let frame=currentTransportFrame() else{return false};return check(daw_write_insert_parameter_automation_gesture(session,frame,normalized))
    }
    func endPluginParameterAutomation() {
        guard pluginParameterGesture != nil else{return};defer{pluginParameterGesture=nil};guard let frame=currentTransportFrame()else{daw_cancel_insert_parameter_automation_gesture(session);return};if check(daw_end_insert_parameter_automation_gesture(session,frame,revision)){refresh();pollTransport()}else{daw_cancel_insert_parameter_automation_gesture(session)}
    }
    func cancelPluginParameterAutomation(){if pluginParameterGesture != nil{daw_cancel_insert_parameter_automation_gesture(session);pluginParameterGesture=nil}}
    @objc func togglePluginParameterArm(_ sender:NSButton){guard let target=pluginParameterArmTargets[ObjectIdentifier(sender)]else{return};if pluginParameterArmed(target){armedPluginParameter=nil;sender.title="ARM";sender.contentTintColor = .secondaryLabelColor}else{armedPluginParameter=target;sender.title="ARMED";sender.contentTintColor = .systemRed};if pluginParameterGesture != nil{cancelPluginParameterAutomation()}}
    func syncRevision(){var snapshot=daw_snapshot();snapshot.struct_size=UInt32(MemoryLayout<daw_snapshot>.size);if daw_get_snapshot(session,&snapshot)==0{revision=snapshot.revision;window.isDocumentEdited=dirty}}
    func mixerBeginVolume(_ id:UInt64){guard let kind=mixerKinds[id]else{return};let value=mixerWorkspace.strips.first(where:{$0.id == id})?.volumeDb ?? 0;switch kind{case .track:beginAutomation(target:automationTrackVolume,id:id,value:value);case .bus:beginAutomation(target:automationBusGain,id:id,value:value);case .master:beginAutomation(target:automationMasterGain,id:0,value:value)}}
    func mixerSetVolume(_ id:UInt64,_ value:Double){guard !isRecording,let kind=mixerKinds[id]else{return};let target:Int32;let targetID:UInt64;switch kind{case .track:target=automationTrackVolume;targetID=id;case .bus:target=automationBusGain;targetID=id;case .master:target=automationMasterGain;targetID=0};if automationWrites(target:target,id:targetID){_ = writeAutomation(target:target,id:targetID,value:value);return};let result:Int32;switch kind{case .track:result=daw_set_gain(session,id,value,revision);case .bus:result=daw_set_bus_gain(session,id,value,revision);case .master:result=daw_set_master_gain(session,value,revision)};if check(result){syncRevision()}}
    func mixerEndVolume(){if automationGesture != nil{endAutomationGesture()}else{refresh()}}
    func mixerSetPan(_ id:UInt64,_ value:Double){guard !isRecording,let kind=mixerKinds[id]else{return};switch kind{case .track:if check(daw_set_pan(session,id,value,revision)){syncRevision()};case .bus:if check(daw_set_bus_pan(session,id,value,revision)){syncRevision()};case .master:return}}
    func mixerSetMute(_ id:UInt64,_ muted:Bool){guard let kind=mixerKinds[id]else{return};switch kind{case .track:if check(daw_set_mute(session,id,muted ? 1:0,revision)){refresh()};case .bus:if check(daw_set_bus_mute(session,id,muted ? 1:0,revision)){refresh()};case .master:return}}
    func mixerSetSolo(_ id:UInt64,_ solo:Bool) {
        guard !isRecording,mixerKinds[id] == .track else { return }
        let result = NSEvent.modifierFlags.contains(.option)
            ? daw_set_solo_exclusive(session,id,solo ? 1:0,revision)
            : daw_set_solo(session,id,solo ? 1:0,revision)
        if check(result) { refresh() }
    }
    func setProjectControlsEnabled(_ enabled: Bool) {
        mixerWorkspace.editingEnabled = enabled
        func visit(_ view: NSView) {
            // Метроном — мониторинг, а не правка проекта: он нужен и во время записи.
            if let button = view as? NSButton, button !== recordButton, button !== metronomeButton { button.isEnabled = enabled }
            if let popup = view as? NSPopUpButton { popup.isEnabled = enabled }
            if let slider = view as? NSSlider { slider.isEnabled = enabled }
            if let field = view as? NSTextField, field.isEditable { field.isEnabled = enabled }
            for child in view.subviews { visit(child) }
        }
        if let contentView = window.contentView { visit(contentView) }
        recordButton.isEnabled = true
    }
    func recordingAlert(_ message: String) {
        DAWLog.audio.error("Запись недоступна: \(message, privacy: .public)")
        let alert=NSAlert(); alert.messageText="Запись недоступна"; alert.informativeText=message; alert.runModal()
    }
    @objc func toggleRecording() {
        if isRecording { finishRecording(); return }
        guard !midiTakeArmed else { storageMessage("Сначала заверши запись MIDI с клавиатуры."); return }
        guard !exportBusy else { storageMessage("Дождись завершения экспорта или отмени его перед записью."); return }
        finishEditing()
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: beginRecording()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { [weak self] granted in
                Task { @MainActor in
                    guard let self else { return }
                    if granted { self.beginRecording() }
                    else { self.recordingAlert("Доступ к микрофону отклонён. Его можно включить в Системных настройках → Конфиденциальность и безопасность → Микрофон.") }
                }
            }
        default:
            recordingAlert("Разреши My DAW доступ к микрофону в Системных настройках → Конфиденциальность и безопасность → Микрофон.")
        }
    }
    /// Деление сетки в битах (0 = сетка выключена).
    var gridDivisionBeats: Double {
        gridDivisions[max(0, min(gridPopup.indexOfSelectedItem, gridDivisions.count - 1))]
    }
    /// Квант сетки в кадрах: кадр/бит берётся из темпо-карты под позицией, а не из локального темпа.
    var gridFrames: UInt64 {
        let beats = gridDivisionBeats
        return beats == 0 ? 0 : max(1, UInt64((BeatFrameMap.framesPerMinute / tempoMap.bpm(atFrame: playheadFrame) * beats).rounded()))
    }
    /// Beat-grid для произвольной позиции: опора — темпо-точка, которой принадлежит кадр,
    /// квант — деление в битах на bpm этой точки (120 BPM ⇒ 24 000 кадров на бит).
    func gridSnap(atFrame frame: Int64) -> (anchor: UInt64, quantum: UInt64) {
        let beats = gridDivisionBeats
        guard beats > 0 else { return (0, 0) }
        let point = tempoMap.tempoPoint(atFrame: UInt64(max(0, frame)))
        let quantum = max(1, UInt64((BeatFrameMap.framesPerMinute / point.bpm * beats).rounded()))
        return (point.frame, quantum)
    }
    /// Формат темпа как в соседних строках панели: целое без дробного нуля, иначе — %.2f.
    func formattedBpm(_ value: Double) -> String {
        abs(value - value.rounded()) < 0.005 ? String(format: "%.0f", value.rounded()) : String(format: "%.2f", value)
    }
    func updateTimelineTools() {
        let bpmAtPlayhead = tempoMap.bpm(atFrame: playheadFrame)
        let bpmText = formattedBpm(bpmAtPlayhead)
        tempoLabel.stringValue = "\(bpmText) BPM"
        tempoLabel.toolTip = "Темп из темпо-карты на позиции воспроизведения"
        if window?.firstResponder !== tempoField, tempoField.currentEditor() == nil { tempoField.stringValue = bpmText }
        tempoStepper.integerValue = Int(bpmAtPlayhead.rounded())
        let gridTitle = gridPopup.titleOfSelectedItem ?? "выкл."
        gridLabel.stringValue = gridDivisionBeats == 0 ? "Сетка: выкл." : "Сетка: \(gridTitle) @ \(bpmText)"
        gridLabel.toolTip = "Деления 1/1…1/32 считаются в битах от темпо-точки под позицией"
        gridPopup.toolTip = gridLabel.stringValue
        if let start=rangeStart, let end=rangeEnd {
            rangeLabel.stringValue=String(format:"%.2f–%.2f с · %.2f с",Double(start)/48000,Double(end)/48000,Double(end-start)/48000)
        } else if let start=rangeStart { rangeLabel.stringValue=String(format:"начало %.2f с · выбери конец",Double(start)/48000) }
        else { rangeLabel.stringValue="Диапазон: весь проект" }
        exportButton.title = rangeEnd != nil ? "Экспорт диапазона WAV…" : "Экспорт WAV…"
        loopButton.title=loopEnabled ? "↻ Цикл вкл." : "↻ Цикл"
        loopButton.state=loopEnabled ? .on:.off
        loopButton.isEnabled=rangeStart != nil && rangeEnd != nil && !isRecording
        for wave in waveforms { wave.snapFrames=gridFrames; wave.snapGrid={[weak self] frame in self?.gridSnap(atFrame: frame) ?? (anchor:0,quantum:0)}; wave.rangeStart=rangeStart; wave.rangeEnd=rangeEnd;wave.loopEnabled=loopEnabled }
        refreshBarMarks()
        positionLabel.stringValue = tempoMap.barBeatTick(atFrame: playheadFrame, bars: tempoBars)
    }
    /// Читает темпо-карту и карту размеров из сессии (единственный источник истины).
    func reloadTempoMap() {
        guard session != nil else { return }
        var tempoCount: UInt32 = 0
        if daw_get_tempo_count(session, &tempoCount) == 0, tempoCount > 0 {
            var points: [ProjectTempoPoint] = []
            for index in 0..<tempoCount {
                var point = daw_tempo_point(); point.struct_size = UInt32(MemoryLayout<daw_tempo_point>.size)
                guard daw_get_tempo_point(session, index, &point) == 0 else { continue }
                points.append(ProjectTempoPoint(frame: point.frame, bpm: point.bpm))
            }
            if !points.isEmpty { tempoMap.tempo = points }
        }
        var signatureCount: UInt32 = 0
        if daw_get_time_signature_count(session, &signatureCount) == 0, signatureCount > 0 {
            var points: [ProjectSignaturePoint] = []
            for index in 0..<signatureCount {
                var point = daw_time_signature_point(); point.struct_size = UInt32(MemoryLayout<daw_time_signature_point>.size)
                guard daw_get_time_signature_point(session, index, &point) == 0 else { continue }
                points.append(ProjectSignaturePoint(frame: point.frame, numerator: point.numerator, denominator: point.denominator))
            }
            if !points.isEmpty { tempoMap.signatures = points }
        }
    }
    /// Позиция воспроизведения: обновляет playhead-состояния UI без перестройки проекта.
    func syncPlayhead(_ frame: UInt64) {
        let crossing = tempoMap.tempoPoint(atFrame: frame).frame != tempoMap.tempoPoint(atFrame: playheadFrame).frame
        playheadFrame = frame
        timelineRuler.playhead = frame
        if crossing { updateTimelineTools() }
        else { positionLabel.stringValue = tempoMap.barBeatTick(atFrame: frame, bars: tempoBars) }
    }
    /// Одна revision-aware команда: точка темпа пишется ровно на позицию воспроизведения.
    /// Отказ откатывает UI к карте; сообщение об ошибке остаётся в существующем статусе.
    func commitTempo(_ bpm: Double) {
        guard !isRecording else { setProjectMessage("Во время записи темп не меняется."); updateTimelineTools(); return }
        let frame = currentTransportFrame() ?? playheadFrame
        if bpm <= 20 || bpm > 999 || !bpm.isFinite {
            setProjectMessage("Темпо-карта допускает темп свыше 20 и не выше 999 BPM; изменение не выполнено.")
            updateTimelineTools()
            return
        }
        if check(daw_set_tempo(session, frame, bpm, revision)) {
            playheadFrame = frame
            reloadTempoMap(); syncRevision(); updateTimelineTools()
            setProjectMessage("Темп \(formattedBpm(tempoMap.bpm(atFrame: frame))) BPM записан в темпо-карту на \(String(format: "%.2f", Double(frame)/48000)) с")
        } else {
            updateTimelineTools()
            setProjectMessage("Изменение темпа отклонено; значение восстановлено из темпо-карты.")
        }
    }
    func currentTransportFrame() -> UInt64? {
        var value=daw_transport();value.struct_size=UInt32(MemoryLayout<daw_transport>.size)
        return check(daw_get_transport(session,&value)) ? value.frame : nil
    }
    @objc func setRangeStart() {
        disableLoop()
        guard let frame=currentTransportFrame() else{return};rangeStart=frame
        if let end=rangeEnd,end<=frame { rangeEnd=nil };updateTimelineTools()
    }
    @objc func setRangeEnd() {
        disableLoop()
        guard let frame=currentTransportFrame() else{return};let start=rangeStart ?? 0
        guard frame>start else{storageMessage("Конец диапазона должен быть позже начала.");return}
        rangeStart=start;rangeEnd=frame;updateTimelineTools()
    }
    @objc func clearRange() { disableLoop();rangeStart=nil;rangeEnd=nil;updateTimelineTools() }
    func disableLoop(){guard loopEnabled else{return};if check(daw_set_loop(session,0,0,0)){loopEnabled=false}}
    @objc func toggleLoop(){
        guard !isRecording else{return}
        if loopEnabled { disableLoop();updateTimelineTools();pollTransport();return }
        guard let start=rangeStart,let end=rangeEnd,end>start else{storageMessage("Сначала задай начало и конец диапазона.");return}
        if check(daw_set_loop(session,1,start,end)){loopEnabled=true;updateTimelineTools();pollTransport()}
    }
    /// Тактовые метки линейки: пересчитываются из карты при каждом изменении темпа,
    /// размера, позиции или длины проекта.
    func refreshBarMarks() {
        let limit = min(max(timelineRuler.projectFrames, playheadFrame + 48000 * 30), BeatFrameMap.timelineLimitFrame)
        tempoBars = tempoMap.barStarts(upToFrame: limit)
        timelineRuler.barMarks = tempoBars
        refreshMarkers()
    }
    /// Полная перечитка дорожки маркеров: домен остаётся единственным носителем,
    /// линейка лишь отображает срез после каждой команды/Undo/Redo/Load.
    func refreshMarkers() {
        var count: UInt32 = 0
        guard daw_get_marker_count(session, &count) == 0 else { timelineRuler.markers = []; return }
        var list: [ProjectMarker] = []
        for index in 0..<Int(count) {
            var marker = daw_marker(); marker.struct_size = UInt32(MemoryLayout<daw_marker>.size)
            guard daw_get_marker(session, UInt32(index), &marker) == 0 else { continue }
            let name = withUnsafeBytes(of: marker.name) { bytes in String(decoding: bytes.prefix(while: { $0 != 0 }), as: UTF8.self) }
            list.append(ProjectMarker(frame: marker.frame, name: name))
        }
        timelineRuler.markers = list
    }
    func promptAddMarker(_ frame: UInt64) {
        guard !isRecording else { return }
        finishEditing()
        let snapped = gridSnap(atFrame: Int64(frame)).anchor
        let alert = NSAlert(); alert.messageText = "Новый маркер"; alert.informativeText = "Имя 1–120 символов; позиция " + String(format: "%.2f", Double(snapped)/48000) + " с от начала проекта."
        let field = NSTextField(string: "Маркер \((timelineRuler.markers.count + 1))"); field.widthAnchor.constraint(equalToConstant: 220).isActive = true; field.setAccessibilityLabel("Имя маркера")
        let form = NSStackView(); form.orientation = .vertical; form.alignment = .leading; form.spacing = 8; form.addArrangedSubview(field); form.frame = NSRect(x:0,y:0,width:260,height:34); alert.accessoryView = form
        alert.addButton(withTitle: "Добавить"); alert.addButton(withTitle: "Отмена")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        if check(daw_add_marker(session, snapped, field.stringValue, revision)) { refresh(); pollTransport() }
    }
    func markerMenu(_ marker: ProjectMarker) -> NSMenu {
        let menu = NSMenu(); menu.autoenablesItems = false
        for (title, selector) in [("Перейти к маркеру", #selector(markerSeekAction(_:))), ("Цикл отсюда до следующего", #selector(markerLoopAction(_:))), ("Переименовать…", #selector(markerRenameAction(_:))), ("Удалить маркер", #selector(markerDeleteAction(_:)))] {
            let item = NSMenuItem(title: title, action: selector, keyEquivalent: ""); item.target = self; item.representedObject = marker; menu.addItem(item)
        }
        return menu
    }
    @objc func markerSeekAction(_ sender: NSMenuItem) { guard let marker = sender.representedObject as? ProjectMarker else { return }; seekAudio(marker.frame) }
    @objc func markerRenameAction(_ sender: NSMenuItem) {
        guard let marker = sender.representedObject as? ProjectMarker, !isRecording else { return }
        finishEditing()
        let alert = NSAlert(); alert.messageText = "Переименовать маркер"; alert.informativeText = "Позиция \(String(format: "%.2f", Double(marker.frame)/48000)) с остаётся прежней."
        let field = NSTextField(string: marker.name); field.widthAnchor.constraint(equalToConstant: 220).isActive = true; field.setAccessibilityLabel("Новое имя маркера")
        let form = NSStackView(); form.orientation = .vertical; form.alignment = .leading; form.spacing = 8; form.addArrangedSubview(field); form.frame = NSRect(x:0,y:0,width:260,height:34); alert.accessoryView = form
        alert.addButton(withTitle: "Сохранить"); alert.addButton(withTitle: "Отмена")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        if check(daw_rename_marker(session, marker.frame, field.stringValue, revision)) { refresh(); pollTransport() }
    }
    @objc func markerDeleteAction(_ sender: NSMenuItem) {
        guard let marker = sender.representedObject as? ProjectMarker, !isRecording else { return }
        finishEditing()
        if check(daw_remove_marker(session, marker.frame, revision)) { refresh(); pollTransport() }
    }
    @objc func markerLoopAction(_ sender: NSMenuItem) {
        guard let marker = sender.representedObject as? ProjectMarker, !isRecording else { return }
        finishEditing()
        guard let next = timelineRuler.markers.map({ $0.frame }).filter({ $0 > marker.frame }).min() else {
            storageMessage("Нет маркера правее — добавь его, чтобы задать конец цикла."); return
        }
        if check(daw_set_loop(session, 1, marker.frame, next)) { rangeStart = marker.frame; rangeEnd = next; loopEnabled = true; updateTimelineTools(); pollTransport() }
    }
    @objc func menuAddMarkerAtPlayhead() { promptAddMarker(playheadFrame) }
    @objc func changeTempo(_ sender:NSStepper) { commitTempo(Double(sender.integerValue)) }
    @objc func commitTempoField(_ sender:NSTextField) {
        let raw = sender.stringValue.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: ",", with: ".")
        guard let value = Double(raw), value.isFinite else {
            setProjectMessage("Темп должен быть числом BPM, например 120 или 96.5.")
            updateTimelineTools()
            return
        }
        commitTempo(value)
    }
    @objc func changeGrid(_ sender:NSPopUpButton) { updateTimelineTools() }
    func beginRecording() {
        guard !isRecording else { return }
        guard let recordingRoot else { recordingAlert("Не удалось подготовить папку восстановления записи."); return }
        var transport=daw_transport(); transport.struct_size=UInt32(MemoryLayout<daw_transport>.size)
        guard check(daw_get_transport(session,&transport)) else { return }
        let recovery=recordingRoot.appendingPathComponent("\(ProcessInfo.processInfo.processIdentifier)-\(UUID().uuidString).mydawtake")
        let start=rangeStart ?? transport.frame
        let started=armedTrackID.map{daw_record_start_take(session,$0,start,recovery.path)} ?? daw_record_start(session,start,recovery.path)
        guard check(started) else { return }
        activeRecordingURL=recovery
        isRecording=true; updateRecordButton(true); setProjectControlsEnabled(false); pollTransport()
    }
    func finishRecording() {
        guard isRecording else { return }
        let name="Запись \(recordingNumber)"
        let target=armedTrackID
        if check(daw_record_stop(session,name,revision)) {
            if let target { selectedTakes[target]=Int.max }
            recordingNumber += 1; activeRecordingURL=nil; isRecording=false; updateRecordButton(false); setProjectControlsEnabled(true); refresh(); pollTransport()
        } else {
            _=daw_record_cancel(session); activeRecordingURL=nil; isRecording=false; updateRecordButton(false); setProjectControlsEnabled(true); pollTransport()
        }
    }
    func pollTransport() {
        guard session != nil else { return }
        updateInsertRuntimeBadges()
        var recording=daw_recording(); recording.struct_size=UInt32(MemoryLayout<daw_recording>.size)
        guard check(daw_get_recording(session,&recording)) else {
            _=daw_record_cancel(session); isRecording=false; updateRecordButton(false); setProjectControlsEnabled(true); return
        }
        if recording.recording != 0 {
            isRecording=true; updateRecordButton(true)
            if recording.loop_recording != 0 {if let start=rangeStart,let end=rangeEnd,end>start{let frame=start+recording.frames%(end-start);for wave in waveforms{wave.playhead=frame}};transportLabel.stringValue=String(format:"● Loop recording  %.1f с · дублей %d · playback + mono input",Double(recording.frames)/48000,recording.pass_count)}
            else{transportLabel.stringValue=String(format:recording.target_track_id != 0 ? "● Новый дубль  %.1f с · mono / 48 кГц":"● Запись  %.1f с · mono / 48 кГц",Double(recording.frames)/48000)}
            if recording.overflowed != 0 { finishRecording() }
            return
        }
        var t = daw_transport(); t.struct_size = UInt32(MemoryLayout<daw_transport>.size)
        guard check(daw_get_transport(session, &t)) else {
            var output=daw_output_status();output.struct_size=UInt32(MemoryLayout<daw_output_status>.size);_ = daw_get_output_status(session,&output)
            switch output.state { case 3: transportLabel.stringValue="Аудиовыход изменён или отключён · нажми Play";case 4:transportLabel.stringValue="Аудиовыход завис · нажми Play";case 5:transportLabel.stringValue="Ошибка аудиобуфера · нажми Play";case 7:transportLabel.stringValue="Не удалось подготовить плагины · проверь insert";default:transportLabel.stringValue="Вывод остановлен из-за ошибки устройства" }
            playButton.isEnabled = hasAudio || hasMidiContent; stopButton.isEnabled = false; return
        }
        var output=daw_output_status();output.struct_size=UInt32(MemoryLayout<daw_output_status>.size);_ = daw_get_output_status(session,&output)
        if output.state == Int32(DAW_OUTPUT_PREPARING) {
            isPlaying=false;for wave in waveforms { wave.playhead=t.frame }
            playButton.isEnabled=false;stopButton.isEnabled=true
            transportLabel.stringValue="Подготовка render graph и плагинов…"
            return
        }
        if output.state == Int32(DAW_OUTPUT_PREPARATION_FAILED) {
            isPlaying=false;playButton.isEnabled=hasAudio || hasMidiContent;stopButton.isEnabled=false
            transportLabel.stringValue="Не удалось подготовить плагины · нажми Play после исправления insert"
            return
        }
        isPlaying = t.playing != 0
        for wave in waveforms { wave.playhead = t.frame }
        syncPlayhead(t.frame)
        playButton.isEnabled = (hasAudio || hasMidiContent) && t.playing == 0; stopButton.isEnabled = t.playing != 0
        if t.duration > 0 {
            let state = t.playing != 0 ? "Играет" : "Остановлено"
            let warning = t.plugin_errors > 0 ? " · Plug-in error: dry fallback" : (t.clipped_frames > 0 ? " · Перегрузка: уменьши уровни" : "")
            let latency=t.output_latency_frames>0 ? String(format:" · latency %.2f ms",Double(t.output_latency_frames)/48.0):""
            transportLabel.stringValue = String(format: "%@  %.1f / %.1f с%@%@", state, Double(t.frame) / 48000, Double(t.duration) / 48000, latency,warning)
        } else { transportLabel.stringValue = (hasAudio || hasMidiContent) ? "Готово к воспроизведению · системный аудиовыход" : "Импортируй WAV/AIFF или добавь MIDI-дорожку с инструментом" }
    }
    func beginBackgroundImport(_ intent: BackgroundImportIntent) {
        guard !isRecording else { return }
        guard importJob == nil else { storageMessage("Импорт WAV/AIFF уже выполняется. Его можно отменить в верхней панели."); return }
        stopBrowserAudioPreview()
        finishEditing()
        let job: OpaquePointer?
        switch intent {
        case let .track(path, name):
            job = (path.pathExtension.lowercased() == "aif" || path.pathExtension.lowercased() == "aiff")
                ? daw_begin_import_aiff(session, path.path, name, revision)
                : daw_begin_import_wav(session, path.path, name, revision)
        case let .take(path, name, trackID, startFrame):
            job = (path.pathExtension.lowercased() == "aif" || path.pathExtension.lowercased() == "aiff")
                ? daw_begin_import_take_aiff(session, trackID, path.path, name, startFrame, revision)
                : daw_begin_import_take_wav(session, trackID, path.path, name, startFrame, revision)
        }
        guard let job else { _ = check(1); return }
        var startingStatus = daw_import_status(); startingStatus.struct_size = UInt32(MemoryLayout<daw_import_status>.size); startingStatus.version = UInt32(DAW_IMPORT_STATUS_VERSION); startingStatus.status = Int32(DAW_IMPORT_RUNNING); startingStatus.phase = Int32(DAW_IMPORT_PHASE_READING); startingStatus.base_revision = revision
        importJob = job; importIntent = intent; importSession = session; importBaseRevision = revision; importStatus = startingStatus; importExistingTrackIDs = Set(trackIDs.values); importMessage = nil; importMessageUntil = .distantPast
        cancelImportButton.isHidden = false; cancelImportButton.isEnabled = true
        resolveImportButton.isHidden = true; resolveImportButton.isEnabled = false
        inspectorBrowser.isImportBusy = true
        updateStorageStatus()
    }
    func beginTrackImport(_ url: URL) {
        let name = String(url.deletingPathExtension().lastPathComponent.unicodeScalars.prefix(120))
        beginBackgroundImport(.track(path: url, name: name))
    }
    func beginDropImport(_ url: URL,_ trackID: UInt64,_ frame: UInt64) -> Bool {
        guard !isRecording else { return false }
        guard importJob == nil else { storageMessage("Импорт уже выполняется — дождись или отмени его."); return true }
        let name = String(url.deletingPathExtension().lastPathComponent.unicodeScalars.prefix(120))
        beginBackgroundImport(.take(path: url, name: name, trackID: trackID, startFrame: gridSnap(atFrame: Int64(frame)).anchor))
        return true
    }
    @objc func importWav() {
        guard !isRecording else { return }
        let aif = UTType(filenameExtension: "aif") ?? .aiff
        let panel = NSOpenPanel(); panel.allowedContentTypes = [.wav, .aiff, aif]; panel.allowsMultipleSelection = false; panel.canChooseDirectories = false
        panel.message = "PCM WAV/AIFF mono/stereo: 44,1 / 48 / 88,2 / 96 / 192 кГц. Импорт идёт в фоне и автоматически конвертируется в 48 кГц; до 60 секунд."
        guard panel.runModal() == .OK, let url = panel.url else { return }
        beginTrackImport(url)
    }
    func performTrackAction(_ id:UInt64,_ action:Selector) {guard let index=trackIDs.first(where:{$0.value==id})?.key else{return};let sender=NSButton();sender.tag=index;_ = NSApp.sendAction(action,to:self,from:sender)}
    // MARK: - Редактор клипов и дорожек (v19)
    // Общие палитры цветов клипа, дорожки и MIDI-клипа; значения — доменные
    // uint32 0xRRGGBB, 0 сбрасывает durable-цвет обратно к акценту по позиции.
    static let colorPalette: [(String,UInt32)] = [("Красный",0xE57373),("Оранжевый",0xFFB74D),("Жёлтый",0xFFF176),("Зелёный",0x81C784),("Бирюзовый",0x4DB6AC),("Синий",0x64B5F6),("Фиолетовый",0xBA68C8),("Розовый",0xF06292)]
    final class ClipActionPayload { let trackID:UInt64; let clipIndex:Int; let color:UInt32; var targetTrack:UInt64 = 0; var panValue:Double = 0; init(_ trackID:UInt64,_ clipIndex:Int,_ color:UInt32=0){self.trackID=trackID;self.clipIndex=clipIndex;self.color=color} }
    func clipContextMenu(_ trackID:UInt64,_ clipIndex:Int)->NSMenu {
        let menu=NSMenu(); menu.autoenablesItems=false
        if let group=clipSelection[trackID],group.count>1 {
            let del=NSMenuItem(title:"Удалить группу (\(group.count))",action:#selector(deleteClipGroupFromMenu(_:)),keyEquivalent:""); del.target=self; del.representedObject=ClipActionPayload(trackID,0); menu.addItem(del)
            let nudgeLeft=NSMenuItem(title:"Сдвинуть группу назад (Option ←)",action:#selector(nudgeClipGroupLeft(_:)),keyEquivalent:""); nudgeLeft.target=self; nudgeLeft.representedObject=ClipActionPayload(trackID,0); menu.addItem(nudgeLeft)
            let nudgeRight=NSMenuItem(title:"Сдвинуть группу вперёд (Option →)",action:#selector(nudgeClipGroupRight(_:)),keyEquivalent:""); nudgeRight.target=self; nudgeRight.representedObject=ClipActionPayload(trackID,0); menu.addItem(nudgeRight)
            menu.addItem(.separator())
        }
        let colorRoot=NSMenuItem(title:"Цвет клипа",action:nil,keyEquivalent:""); let colors=NSMenu(title:"Цвет клипа"); colors.autoenablesItems=false
        for (name,hex) in Self.colorPalette { let item=NSMenuItem(title:name,action:#selector(pickClipColor(_:)),keyEquivalent:""); item.target=self; item.representedObject=ClipActionPayload(trackID,clipIndex,hex); colors.addItem(item) }
        let reset=NSMenuItem(title:"Без цвета",action:#selector(pickClipColor(_:)),keyEquivalent:""); reset.target=self; reset.representedObject=ClipActionPayload(trackID,clipIndex,0)
        colors.addItem(.separator()); colors.addItem(reset); colorRoot.submenu=colors; menu.addItem(colorRoot)
        let gain=NSMenuItem(title:"Громкость клипа…",action:#selector(editClipGain(_:)),keyEquivalent:""); gain.target=self; gain.representedObject=ClipActionPayload(trackID,clipIndex); menu.addItem(gain)
        menu.addItem(.separator())
        let copy=NSMenuItem(title:"Копировать клип (C)",action:#selector(copyClipFromMenu(_:)),keyEquivalent:""); copy.target=self; copy.representedObject=ClipActionPayload(trackID,clipIndex); menu.addItem(copy)
        var state=daw_clip(); state.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        if check(daw_get_clip(session,trackID,UInt32(clipIndex),&state)) {
            let mute=NSMenuItem(title:"Мьют клипа (M)",action:#selector(toggleClipMuteFromMenu(_:)),keyEquivalent:""); mute.target=self; mute.representedObject=ClipActionPayload(trackID,clipIndex); mute.state=state.muted != 0 ? .on:.off; menu.addItem(mute)
            let loop=NSMenuItem(title:"Луп клипа (L)",action:#selector(toggleClipLoopFromMenu(_:)),keyEquivalent:""); loop.target=self; loop.representedObject=ClipActionPayload(trackID,clipIndex); loop.state=state.looped != 0 ? .on:.off; menu.addItem(loop)
            let panRoot=NSMenuItem(title:"Панорама клипа",action:nil,keyEquivalent:""); let pans=NSMenu(title:"Панорама клипа"); pans.autoenablesItems=false
            for (name,value) in [("Левый канал полностью",-1.0),("Левая половина",-0.5),("По центру",0.0),("Правая половина",0.5),("Правый канал полностью",1.0)] {
                let item=NSMenuItem(title:name,action:#selector(pickClipPan(_:)),keyEquivalent:""); item.target=self; let payload=ClipActionPayload(trackID,clipIndex); payload.panValue=value; item.representedObject=payload
                item.state = abs(state.pan-value)<1e-9 ? .on:.off; pans.addItem(item)
            }
            panRoot.submenu=pans; menu.addItem(panRoot)
        }
        menu.addItem(trackSubmenu(title:"Копировать на дорожку",action:#selector(pasteClipToFromMenu(_:)),source:trackID,clipIndex:clipIndex))
        menu.addItem(trackSubmenu(title:"Перенести на дорожку",action:#selector(moveClipToFromMenu(_:)),source:trackID,clipIndex:clipIndex))
        menu.addItem(.separator())
        for (title,selector) in [("Разделить в позиции курсора",#selector(splitClipFromMenu(_:))),("Дублировать клип",#selector(duplicateClipFromMenu(_:))),("Удалить клип",#selector(deleteClipFromMenu(_:)))] {
            let item=NSMenuItem(title:title,action:selector,keyEquivalent:""); item.target=self; item.representedObject=ClipActionPayload(trackID,clipIndex); menu.addItem(item)
        }
        return menu
    }
    /// Подменю «…на дорожку»: кандидаты — все дорожки, кроме исходной; имена
    /// берутся из последнего refresh, правила совместности источников держит домен.
    func trackSubmenu(title:String, action:Selector, source:UInt64, clipIndex:Int) -> NSMenuItem {
        let root=NSMenuItem(title:title,action:nil,keyEquivalent:"")
        let submenu=NSMenu(title:title); submenu.autoenablesItems=false
        for (_,id) in trackIDs.sorted(by:{$0.key<$1.key}) where id != source {
            let item=NSMenuItem(title:trackNames[id] ?? "Дорожка \(id)",action:action,keyEquivalent:""); item.target=self
            let payload=ClipActionPayload(source,clipIndex); payload.targetTrack=id; item.representedObject=payload
            submenu.addItem(item)
        }
        if submenu.items.isEmpty { root.isEnabled=false }
        root.submenu=submenu
        return root
    }
    @objc func copyClipFromMenu(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; clipClipboard=(p.trackID,p.clipIndex,false); storageMessage("Клип в буфере обмена — V в фокусе волны или «Вставить клип».") }
    func toggleClipState(_ trackID:UInt64,_ clipIndex:Int,looped:Bool){
        guard !isRecording else{return}
        var state=daw_clip(); state.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        guard check(daw_get_clip(session,trackID,UInt32(clipIndex),&state)) else{return}
        let next:UInt32 = looped ? (state.looped == 0 ? 1:0) : (state.muted == 0 ? 1:0)
        let ok = looped ? check(daw_set_clip_looped(session,trackID,UInt32(clipIndex),next,revision)) : check(daw_set_clip_muted(session,trackID,UInt32(clipIndex),next,revision))
        if ok {refresh(); pollTransport()}
    }
    @objc func toggleClipMuteFromMenu(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; finishEditing(); stopAudio(); toggleClipState(p.trackID,p.clipIndex,looped:false) }
    @objc func toggleClipLoopFromMenu(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; finishEditing(); stopAudio(); toggleClipState(p.trackID,p.clipIndex,looped:true) }
    @objc func pasteClipToFromMenu(_ sender:NSMenuItem){
        guard let p=sender.representedObject as? ClipActionPayload,!isRecording else{return}
        var clip=daw_clip(); clip.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        guard check(daw_get_clip(session,p.trackID,UInt32(p.clipIndex),&clip)) else{return}
        finishEditing(); stopAudio()
        if check(daw_copy_clip_to_track(session,p.trackID,UInt32(p.clipIndex),p.targetTrack,clip.start,revision)){refresh(); pollTransport()}
    }
    @objc func moveClipToFromMenu(_ sender:NSMenuItem){
        guard let p=sender.representedObject as? ClipActionPayload,!isRecording else{return}
        var clip=daw_clip(); clip.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        guard check(daw_get_clip(session,p.trackID,UInt32(p.clipIndex),&clip)) else{return}
        finishEditing(); stopAudio()
        if check(daw_move_clip_to_track(session,p.trackID,UInt32(p.clipIndex),p.targetTrack,clip.start,revision)){selectedClips[p.targetTrack]=0;refresh(); pollTransport()}
    }
    func pasteClipboardTo(_ trackID:UInt64,_ startFrame:UInt64){
        guard !isRecording,let board=clipClipboard else{storageMessage("Буфер обмена клипов пуст — скопируй клип (C).");return}
        finishEditing(); stopAudio()
        let snapped=gridSnap(atFrame:Int64(startFrame)).anchor
        let ok = board.isMidi ? check(daw_copy_midi_clip_to_track(session,board.trackID,UInt32(board.index),trackID,snapped,revision)) : check(daw_copy_clip_to_track(session,board.trackID,UInt32(board.index),trackID,snapped,revision))
        if ok {selectedClips[trackID]=0;refresh(); pollTransport()}
    }
    @objc func menuPasteClip(){ guard let id=inspectorTrackID ?? selectedMixerID,mixerKinds[id] == .track else{return}; pasteClipboardTo(id,playheadFrame) }
    @objc func menuCopyClip(){ guard let id=inspectorTrackID ?? selectedMixerID,mixerKinds[id] == .track,let index=selectedClips[id] else{return}; clipClipboard=(id,index,false); storageMessage("Клип в буфере обмена.") }
    @objc func pickRecordPreroll(_ sender:NSMenuItem){
        guard let seconds=sender.representedObject as? Double else{return}
        let frames=UInt64((seconds*48000).rounded())
        guard check(daw_set_record_preroll(session,frames)) else{return}
        recordPrerollFrames=frames; UserDefaults.standard.set(seconds,forKey:"transport.prerollSeconds.v1")
        sender.menu?.items.forEach{ $0.state = ($0.representedObject as? Double).map{ UInt64(($0*48000).rounded())==frames } == true ? .on:.off }
        storageMessage(frames==0 ? "Преролл выключен." : "Преролл \(seconds) с — транспорт и клик начнутся раньше punch-in (запись в луп).")
    }
    @objc func deleteClipGroupFromMenu(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; performTrackAction(p.trackID,#selector(deleteSelectedClip(_:))) }
    @objc func pickClipPan(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload,!isRecording else{return}; finishEditing(); stopAudio(); if check(daw_set_clip_pan(session,p.trackID,UInt32(p.clipIndex),p.panValue,revision)){selectedClips[p.trackID]=p.clipIndex;refresh(); pollTransport()} }
    @objc func nudgeClipGroupLeft(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; nudgeClipGroup(p.trackID,-1) }
    @objc func nudgeClipGroupRight(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; nudgeClipGroup(p.trackID,1) }
    // MARK: мультиселект: Ctrl-клик тоггит группу, Option+стрелки сдвигают её на шаг сетки
    func clipTapped(_ trackID:UInt64,_ index:Int,additive:Bool){
        guard mixerKinds[trackID] != nil else{return}
        var group=clipSelection[trackID] ?? selectedClips[trackID].map{[$0]} ?? []
        if additive {
            if let position=group.firstIndex(of:index) { if group.count>1 {group.remove(at:position)} } else { group.append(index) }
        } else { group=[index] }
        group=Array(Set(group)).sorted()
        clipSelection[trackID]=group
        let primary=group.contains(index) ? index:(group.first ?? 0)
        selectedClips[trackID]=primary
        laneViews[trackID]?.selectedIndices=group; laneViews[trackID]?.selectedIndex=primary
        updateClipInspector(trackID,primary)
    }
    func groupIndices(_ trackID:UInt64)->[UInt32]{
        let group=clipSelection[trackID] ?? selectedClips[trackID].map{[$0]} ?? []
        return group.sorted().map{UInt32($0)}
    }
    func nudgeClipGroup(_ trackID:UInt64,_ direction:Int){
        guard !isRecording else{return}
        var indices=groupIndices(trackID); guard !indices.isEmpty else{return}
        var meta=daw_clip(); meta.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        guard check(daw_get_clip(session,trackID,indices[0],&meta)) else{return}
        let quantum=max(1,Int(gridSnap(atFrame:Int64(meta.start)).quantum))
        let delta=Int64(direction)*Int64(quantum)
        stopAudio()
        if check(daw_nudge_clips(session,trackID,&indices,UInt32(indices.count),delta,revision)) { refresh(); pollTransport(); storageMessage(direction>0 ? "Группа клипов сдвинута вперёд на шаг сетки." : "Группа клипов сдвинута назад на шаг сетки.") }
    }
    func showMidiMovePalette(_ id:UInt64){
        guard !isRecording,let clip=selectedMidiClip(id) else{return}
        let menu=NSMenu(); menu.autoenablesItems=false
        for (_,target) in trackIDs.sorted(by:{$0.key<$1.key}) where target != id {
            let item=NSMenuItem(title:"→ \(trackNames[target] ?? "Дорожка \(target)")",action:#selector(pickMidiMove(_:)),keyEquivalent:""); item.target=self
            let payload=ClipActionPayload(id,Int(clip)); payload.targetTrack=target; item.representedObject=payload
            menu.addItem(item)
        }
        if menu.items.isEmpty { storageMessage("Нет другой дорожки для переноса."); return }
        menu.popUp(positioning:nil,at:NSEvent.mouseLocation,in:nil)
    }
    @objc func pickMidiMove(_ sender:NSMenuItem){
        guard let p=sender.representedObject as? ClipActionPayload,!isRecording else{return}
        var meta=daw_midi_clip(); meta.struct_size=UInt32(MemoryLayout<daw_midi_clip>.size)
        guard check(daw_get_midi_clip(session,p.trackID,UInt32(p.clipIndex),&meta,0,nil,0,nil)) else{return}
        finishEditing(); stopAudio()
        if check(daw_move_midi_clip_to_track(session,p.trackID,UInt32(p.clipIndex),p.targetTrack,meta.start,revision)){midiClipIndex=0;refresh(); updateMixerInspector(p.targetTrack)}
    }
    @objc func copyMidiClipNow(_ id:UInt64){ guard !isRecording,let clip=selectedMidiClip(id) else{return}; clipClipboard=(id,Int(clip),true); storageMessage("MIDI-клип в буфере обмена — V на дорожке или «Вставить клип».") }
    @objc func pickClipColor(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload,!isRecording else{return}; finishEditing(); stopAudio(); if check(daw_set_clip_color(session,p.trackID,UInt32(p.clipIndex),p.color,revision)){selectedClips[p.trackID]=p.clipIndex;refresh(); pollTransport()} }
    @objc func editClipGain(_ sender:NSMenuItem){
        guard let p=sender.representedObject as? ClipActionPayload,!isRecording else{return}
        var clip=daw_clip(); clip.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        guard check(daw_get_clip(session,p.trackID,UInt32(p.clipIndex),&clip)) else{return}
        finishEditing(); stopAudio()
        let alert=NSAlert(); alert.messageText="Громкость клипа"; alert.informativeText="Децибелы от −60 до +12; применяются к исходному сигналу до фейдера дорожки."
        let field=NSTextField(string:String(format:"%+.1f",clip.gain_db)); field.widthAnchor.constraint(equalToConstant:120).isActive=true; field.setAccessibilityLabel("Громкость клипа, децибелы")
        let form = NSStackView(); form.orientation = .vertical; form.alignment = .leading; form.spacing = 8; form.addArrangedSubview(label("Громкость, dB",size:12)); form.addArrangedSubview(field); form.frame = NSRect(x:0,y:0,width:300,height:64); alert.accessoryView = form
        alert.addButton(withTitle:"Применить"); alert.addButton(withTitle:"Отмена")
        guard alert.runModal() == .alertFirstButtonReturn,let value=Double(field.stringValue.replacingOccurrences(of:",",with:".")) else{return}
        if check(daw_set_clip_gain(session,p.trackID,UInt32(p.clipIndex),value,revision)){refresh(); pollTransport()}
    }
    @objc func splitClipFromMenu(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; selectedClips[p.trackID]=p.clipIndex; performTrackAction(p.trackID,#selector(splitClipAtCursor(_:))) }
    @objc func duplicateClipFromMenu(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; selectedClips[p.trackID]=p.clipIndex; performTrackAction(p.trackID,#selector(duplicateSelectedClip(_:))) }
    @objc func deleteClipFromMenu(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; selectedClips[p.trackID]=p.clipIndex; performTrackAction(p.trackID,#selector(deleteSelectedClip(_:))) }
    func clipHotkey(_ trackID:UInt64,_ key:String){ switch key { case "s": performTrackAction(trackID,#selector(splitClipAtCursor(_:))); case "d": performTrackAction(trackID,#selector(duplicateSelectedClip(_:))); case "c": if let index=selectedClips[trackID]{clipClipboard=(trackID,index,false);storageMessage("Клип в буфере обмена — V на дорожке или «Вставить клип».")}; case "v": pasteClipboardTo(trackID,playheadFrame); case "m": if let index=selectedClips[trackID]{stopAudio();toggleClipState(trackID,index,looped:false)}; case "l": if let index=selectedClips[trackID]{stopAudio();toggleClipState(trackID,index,looped:true)}; default: performTrackAction(trackID,#selector(deleteSelectedClip(_:))) } }
    func commitTrackColor(_ id:UInt64,_ color:UInt32){ guard !isRecording else{return}; finishEditing(); stopAudio(); if check(daw_set_track_color(session,id,color,revision)){refresh(); pollTransport()} }
    @objc func pickTrackColor(_ sender:NSMenuItem){ guard let p=sender.representedObject as? ClipActionPayload else{return}; commitTrackColor(p.trackID,p.color) }
    func showTrackPalette(_ id:UInt64){
        guard !isRecording,automationGesture==nil,pluginParameterGesture==nil else{return}
        let menu=NSMenu(); menu.autoenablesItems=false
        for (name,hex) in Self.colorPalette { let item=NSMenuItem(title:name,action:#selector(pickTrackColor(_:)),keyEquivalent:""); item.target=self; item.representedObject=ClipActionPayload(id,0,hex); menu.addItem(item) }
        menu.addItem(.separator()); let reset=NSMenuItem(title:"Без цвета (акцент по позиции)",action:#selector(pickTrackColor(_:)),keyEquivalent:""); reset.target=self; reset.representedObject=ClipActionPayload(id,0,0); menu.addItem(reset)
        menu.popUp(positioning:nil,at:NSEvent.mouseLocation,in:nil)
    }
    func duplicateTrackNow(_ id:UInt64){ guard !isRecording else{return}; finishEditing(); stopAudio(); var newID:UInt64=0; if check(daw_duplicate_track(session,id,&newID,revision)){refresh(); pollTransport()} }
    /// Удаление шины с подтверждением: дорожки и sends перенаправляются на мастер.
    func deleteBusWithConfirmation(_ busID:UInt64){
        guard !isRecording else { return }
        // Get bus name for confirmation message
        var bus = daw_bus(); bus.struct_size = UInt32(MemoryLayout<daw_bus>.size)
        var found = false
        var busName = "Шина"
        var snap = daw_snapshot(); snap.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        if check(daw_get_snapshot(session, &snap)) {
            for i in 0..<Int(snap.bus_count) {
                if check(daw_get_bus(session, UInt32(i), &bus)), bus.id == busID {
                    busName = withUnsafeBytes(of: bus.name) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
                    found = true; break
                }
            }
        }
        guard found else { storageMessage("Шина не найдена."); return }
        let alert = NSAlert()
        alert.messageText = "Удалить шину «\(busName)»?"
        alert.informativeText = "Дорожки, направляемые в эту шину, будут перенаправлены на мастер. Sends на эту шину будут удалены."
        alert.addButton(withTitle: "Удалить")
        alert.addButton(withTitle: "Отмена")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        finishEditing(); stopAudio()
        if check(daw_delete_bus(session, busID, bridgeRevision())) {
            refresh(); pollTransport()
            status.stringValue = "Шина «\(busName)» удалена, дорожки перенаправлены на мастер"
        }
    }
    /// Экспорт одной дорожки в WAV: тот же фоновый stems-джоб, что и у
    /// «Экспорт стемов», но на одну дорожку. UI не блокируется — завершение
    /// обрабатывает общий pump pollStorage().
    func exportTrackAsWav(_ trackID:UInt64){
        guard !exportBusy else { storageMessage("Экспорт уже выполняется."); return }
        guard !isRecording else { return }
        finishEditing()
        let choice = NSAlert()
        choice.messageText = "Экспорт дорожки в WAV"
        choice.informativeText = "Один WAV слышимой дорожки: мастер-гейн и мастер-цепочка не применяются. Замьюченная или пустая дорожка не экспортируется."
        choice.addButton(withTitle: "WAV 24-bit"); choice.addButton(withTitle: "WAV float32"); choice.addButton(withTitle: "Отмена")
        let response = choice.runModal()
        guard response != .alertThirdButtonReturn else { return }
        let format: Int32 = response == .alertSecondButtonReturn ? 2 : 1
        var options = daw_export_options()
        options.struct_size = UInt32(MemoryLayout<daw_export_options>.size)
        options.version = UInt32(DAW_EXPORT_OPTIONS_VERSION)
        let defaults = UserDefaults.standard
        let storedMode = UInt32(max(0, defaults.integer(forKey: "export.tail.mode.v1")))
        let storedLimit = UInt32(max(0, defaults.integer(forKey: "export.tail.limitSeconds.v1")))
        options.tail_mode = storedMode >= UInt32(DAW_EXPORT_TAIL_AUTOMATIC) && storedMode <= UInt32(DAW_EXPORT_TAIL_MANUAL_LIMIT) ? storedMode : UInt32(DAW_EXPORT_TAIL_AUTOMATIC)
        options.manual_tail_frames = [2, 5, 15, 30].contains(Int(storedLimit)) ? storedLimit * 48_000 : 30 * 48_000
        let panel = NSOpenPanel(); panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
        if let currentURL { panel.directoryURL = currentURL.deletingLastPathComponent() }
        panel.message = "Папка, куда записать WAV этой дорожки"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let ids: [UInt64] = [trackID]
        let job = ids.withUnsafeBufferPointer { daw_begin_stem_export_tracks(session, url.path, format, &options, $0.baseAddress, UInt32(ids.count)) }
        guard let job else { _ = check(1); return }
        exportJob = job; exportURL = url; exportStarted = Date(); exportMessage = nil; exportMessageUntil = .distantPast
        exportButton.isEnabled = false; cancelExportButton.isEnabled = true; recordButton.isEnabled = false
        updateStorageStatus()
    }
    /// Группировка дорожек: шина и есть папка. Подменю предлагает создать
    /// «Группу N», маршрутизацию в существующие шины и возврат на мастер.
    func bridgeRevision()->UInt64 { var snap=daw_snapshot();snap.struct_size=UInt32(MemoryLayout<daw_snapshot>.size);return check(daw_get_snapshot(session,&snap)) ? snap.revision : revision }
    func currentTrackOutputBus(_ id:UInt64)->UInt64 {
        guard let index = trackIDs.first(where: { $0.value == id })?.key else { return 0 }
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        guard check(daw_get_track(session, UInt32(index), &track)) else { return 0 }
        return track.output_bus_id
    }
    func showTrackGroupMenu(_ id:UInt64){
        guard !isRecording,automationGesture==nil,pluginParameterGesture==nil else{return}
        var snap = daw_snapshot(); snap.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        guard check(daw_get_snapshot(session, &snap)) else { return }
        let currentOutput = currentTrackOutputBus(id)
        let menu = NSMenu(); menu.autoenablesItems=false
        let create = NSMenuItem(title: "Новая шина «Группа \(snap.bus_count + 1)»", action: #selector(groupTrackNewBus(_:)), keyEquivalent: ""); create.target = self; create.representedObject = ["track": id]; menu.addItem(create)
        if snap.bus_count > 0 { menu.addItem(.separator()) }
        for busIndex in 0..<Int(snap.bus_count) {
            var bus = daw_bus(); bus.struct_size = UInt32(MemoryLayout<daw_bus>.size)
            guard check(daw_get_bus(session, UInt32(busIndex), &bus)) else { return }
            let name = withUnsafeBytes(of: bus.name) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
            let item = NSMenuItem(title: "Шина: \(name)", action: #selector(groupTrackToBus(_:)), keyEquivalent: "")
            item.target = self; item.representedObject = ["track": id, "bus": bus.id]
            item.state = bus.id == currentOutput ? .on : .off
            menu.addItem(item)
        }
        if currentOutput != 0 || snap.bus_count > 0 {
            let direct = NSMenuItem(title: "Мастер (без шины)", action: #selector(groupTrackToBus(_:)), keyEquivalent: "")
            direct.target = self; direct.representedObject = ["track": id, "bus": UInt64(0)]; direct.state = currentOutput == 0 ? .on : .off
            menu.addItem(direct)
        }
        menu.popUp(positioning: nil, at: NSEvent.mouseLocation, in: nil)
    }
    @objc func groupTrackNewBus(_ sender:NSMenuItem){
        guard let track = (sender.representedObject as? [String:UInt64])?["track"], !isRecording else { return }
        finishEditing(); stopAudio()
        var count:UInt32=0; guard check(daw_get_bus_count(session,&count)) else{return}
        var newID:UInt64=0
        guard check(daw_create_bus(session,"Группа \(count+1)",&newID,bridgeRevision())) else{return}
        refresh()
        if check(daw_set_track_output(session,track,newID,bridgeRevision())){refresh();pollTransport();status.stringValue="Дорожка направлена в новую шину «Группа \(count+1)»"}
    }
    @objc func groupTrackToBus(_ sender:NSMenuItem){
        guard let dict = sender.representedObject as? [String:UInt64], let track = dict["track"], let bus = dict["bus"], !isRecording else { return }
        finishEditing(); stopAudio()
        if check(daw_set_track_output(session,track,bus,bridgeRevision())){refresh();pollTransport()}
    }
    func selectedMidiClip(_ trackID:UInt64)->UInt32? {
        var count:UInt32=0; guard daw_get_midi_clip_count(session,trackID,&count)==0,count>0 else{storageMessage("На дорожке нет MIDI-клипа.");return nil}
        let index=midiClipIndex != nil && UInt32(midiClipIndex!) < count ? UInt32(midiClipIndex!) : 0
        return index
    }
    func showMidiTranspose(_ id:UInt64){
        guard !isRecording,let clip=selectedMidiClip(id) else{return}
        finishEditing(); stopAudio()
        let alert=NSAlert(); alert.messageText="Транспонировать MIDI-клип"; alert.informativeText="Полутонов от −127 до 127; питчи клампятся в 0…127."
        let field=NSTextField(string:"+12"); field.widthAnchor.constraint(equalToConstant:120).isActive=true; field.setAccessibilityLabel("Полутонов")
        let form = NSStackView(); form.orientation = .vertical; form.alignment = .leading; form.spacing = 8; form.addArrangedSubview(label("Полутонов",size:12)); form.addArrangedSubview(field); form.frame = NSRect(x:0,y:0,width:300,height:64); alert.accessoryView = form
        alert.addButton(withTitle:"Применить"); alert.addButton(withTitle:"Отмена")
        guard alert.runModal() == .alertFirstButtonReturn,let value=Int(field.stringValue),value >= -127,value <= 127 else{return}
        if check(daw_transpose_midi_clip(session,id,clip,Int32(value),revision)){refresh(); updateMixerInspector(id)}
    }
    func showMidiQuantize(_ id:UInt64){
        guard !isRecording,let clip=selectedMidiClip(id) else{return}
        finishEditing(); stopAudio()
        let alert=NSAlert(); alert.messageText="Квантовать MIDI-клип"; alert.informativeText="Старты нот вытягиваются к ближайшей сетке по темпо-карте."
        let popup=NSPopUpButton(); popup.widthAnchor.constraint(equalToConstant:220).isActive=true
        let grids:[(String,Double)]=[("1/4 — такт",1.0),("1/8 — полтакта",0.5),("1/16 — четверть такта",0.25),("1/32",0.125),("1/4 триоль",2.0/3.0),("1/8 триоль",1.0/3.0)]
        for (title,_) in grids { popup.addItem(withTitle:title) }
        popup.setAccessibilityLabel("Сетка квантования")
        let form = NSStackView(); form.orientation = .vertical; form.alignment = .leading; form.spacing = 8; form.addArrangedSubview(label("Сетка",size:12)); form.addArrangedSubview(popup); form.frame = NSRect(x:0,y:0,width:280,height:64); alert.accessoryView = form
        alert.addButton(withTitle:"Квантовать"); alert.addButton(withTitle:"Отмена")
        guard alert.runModal() == .alertFirstButtonReturn else{return}
        if check(daw_quantize_midi_clip(session,id,clip,grids[popup.indexOfSelectedItem].1,revision)){refresh(); updateMixerInspector(id)}
    }
    func showMidiClipColor(_ id:UInt64){
        guard !isRecording,let clip=selectedMidiClip(id) else{return}
        let menu=NSMenu(); menu.autoenablesItems=false
        for (name,hex) in Self.colorPalette { let item=NSMenuItem(title:name,action:#selector(pickMidiColor(_:)),keyEquivalent:""); item.target=self; item.representedObject=ClipActionPayload(id,Int(clip),hex); menu.addItem(item) }
        menu.addItem(.separator()); let reset=NSMenuItem(title:"Без цвета",action:#selector(pickMidiColor(_:)),keyEquivalent:""); reset.target=self; reset.representedObject=ClipActionPayload(id,Int(clip),0); menu.addItem(reset)
        menu.popUp(positioning:nil,at:NSEvent.mouseLocation,in:nil)
    }
    @objc func pickMidiColor(_ sender:NSMenuItem){
        guard let p=sender.representedObject as? ClipActionPayload,!isRecording else{return}
        stopAudio()
        if check(daw_set_midi_clip_color(session,p.trackID,UInt32(p.clipIndex),p.color,revision)){refresh(); updateMixerInspector(p.trackID)}
    }
    @objc func menuClipSplit(){ guard let id=inspectorTrackID ?? selectedMixerID,mixerKinds[id] == .track else{return}; performTrackAction(id,#selector(splitClipAtCursor(_:))) }
    @objc func menuClipDuplicate(){ guard let id=inspectorTrackID ?? selectedMixerID,mixerKinds[id] == .track else{return}; performTrackAction(id,#selector(duplicateSelectedClip(_:))) }
    @objc func menuClipDelete(){ guard let id=inspectorTrackID ?? selectedMixerID,mixerKinds[id] == .track else{return}; performTrackAction(id,#selector(deleteSelectedClip(_:))) }
    @objc func menuTrackDuplicate(){ guard let id=inspectorTrackID ?? selectedMixerID,mixerKinds[id] == .track else{return}; duplicateTrackNow(id) }
    func deleteCurrentSelectedClip(){guard let id=inspectorTrackID ?? selectedMixerID,mixerKinds[id] == .track else{return};performTrackAction(id,#selector(deleteSelectedClip(_:)))}
    @objc func deleteCurrentSelectedTrack() {
        guard let id = inspectorTrackID ?? selectedMixerID, mixerKinds[id] == .track else { return }
        deleteTrack(id)
    }
    @objc func moveSelectedTrackUp() {
        guard let id = inspectorTrackID ?? selectedMixerID, mixerKinds[id] == .track,
              let index = trackIDs.first(where: { $0.value == id })?.key else { return }
        moveTrack(id, toInsertionIndex: index - 1)
    }
    @objc func moveSelectedTrackDown() {
        guard let id = inspectorTrackID ?? selectedMixerID, mixerKinds[id] == .track,
              let index = trackIDs.first(where: { $0.value == id })?.key else { return }
        moveTrack(id, toInsertionIndex: index + 2)
    }
    /// `insertionIndex` is measured before the source row is removed. This
    /// makes drag targets and keyboard movement use the same arrangement rule.
    func moveTrack(_ id: UInt64, toInsertionIndex insertionIndex: Int) {
        guard !isRecording else {
            setProjectMessage("Останови запись перед перемещением дорожки.")
            updateStorageStatus()
            return
        }
        guard automationGesture == nil, pluginParameterGesture == nil else {
            storageMessage("Заверши жест автоматизации перед перемещением дорожки.")
            return
        }
        let orderedTrackIDs = trackIDs.keys.sorted().compactMap { trackIDs[$0] }
        guard let sourceIndex = orderedTrackIDs.firstIndex(of: id) else { return }
        let rawDestination = min(max(0, insertionIndex), orderedTrackIDs.count)
        let destination = rawDestination > sourceIndex ? rawDestination - 1 : rawDestination
        guard destination != sourceIndex else {
            setProjectMessage("Порядок дорожек не изменился.")
            updateStorageStatus()
            return
        }
        finishEditing()
        stopBrowserAudioPreview()
        guard check(daw_move_track(session, id, UInt32(destination), revision)) else { return }
        selectedMixerID = id; inspectorTrackID = id; inspectorClipIndex = nil
        refresh(); updateMixerInspector(id)
        let direction = destination < sourceIndex ? "выше" : "ниже"
        setProjectMessage("Дорожка перемещена \(direction) · ⌘Z")
        updateStorageStatus()
        status.setAccessibilityLabel("Дорожка перемещена \(direction). Нажми Command-Z, чтобы вернуть порядок.")
        pollTransport()
    }
    func deleteTrack(_ id: UInt64) {
        guard !isRecording else {
            setProjectMessage("Останови запись перед удалением дорожки.")
            updateStorageStatus()
            return
        }
        guard automationGesture == nil, pluginParameterGesture == nil else {
            storageMessage("Заверши жест автоматизации перед удалением дорожки.")
            return
        }
        guard let index = trackIDs.first(where: { $0.value == id })?.key else { return }

        // A take import targets this exact channel and cannot be committed after
        // it disappears.  An independent new-track import remains useful.
        let canceledTakeImport: Bool
        if case let .some(.take(_, _, trackID, _)) = importIntent, trackID == id {
            releaseImportJob(cancel: true)
            importMessage = nil; importMessageUntil = .distantPast
            canceledTakeImport = true
        } else { canceledTakeImport = false }

        let orderedTrackIDs = trackIDs.keys.sorted().compactMap { trackIDs[$0] }
        let nextSelection = orderedTrackIDs.dropFirst(index + 1).first ?? orderedTrackIDs.prefix(index).last
        finishEditing()
        stopBrowserAudioPreview()
        guard check(daw_remove_track(session, id, revision)) else { return }

        armedTrackID = armedTrackID == id ? nil : armedTrackID
        selectedClips[id] = nil
        selectedTakes[id] = nil
        selectedMixerID = nextSelection
        inspectorTrackID = nextSelection
        inspectorClipIndex = nil
        midiClipIndex = nil; midiNotesCache = []
        refresh()
        if let nextSelection { updateMixerInspector(nextSelection) }
        else { inspectorBrowser.channel = nil; inspectorBrowser.clip = nil; inspectorBrowser.midi = nil }
        let deletionMessage = canceledTakeImport ? "Дорожка удалена · импорт дубля отменён · ⌘Z" : "Дорожка удалена · ⌘Z"
        setProjectMessage(deletionMessage)
        status.stringValue = deletionMessage
        status.setAccessibilityLabel("Дорожка удалена. Нажми Command-Z, чтобы восстановить её.")
        pollTransport()
    }
    @objc func toggleArm(_ sender:NSButton){guard !isRecording,let id=trackIDs[sender.tag]else{return};armedTrackID=armedTrackID==id ? nil:id;refresh()}
    @objc func selectTake(_ sender:NSPopUpButton){guard let id=trackIDs[sender.tag]else{return};selectedTakes[id]=sender.indexOfSelectedItem;refresh()}
    @objc func importTake(_ sender:NSButton){
        guard !isRecording,let id=trackIDs[sender.tag]else{return}
        let aif = UTType(filenameExtension: "aif") ?? .aiff
        let panel=NSOpenPanel();panel.allowedContentTypes=[.wav, .aiff, aif];panel.allowsMultipleSelection=false;panel.canChooseDirectories=false;panel.message="Выбери PCM WAV/AIFF-дубль mono/stereo: 44,1 / 48 / 88,2 / 96 / 192 кГц. Импорт идёт в фоне, будет конвертирован в 48 кГц, сохранится внутри дорожки и не изменит текущий comp; до 60 секунд."
        guard panel.runModal() == .OK,let url=panel.url else{return}
        let name=String(url.deletingPathExtension().lastPathComponent.unicodeScalars.prefix(120));let start=rangeStart ?? currentTransportFrame() ?? 0
        beginBackgroundImport(.take(path: url, name: name, trackID: id, startFrame: start))
    }
    @objc func applyComp(_ sender:NSButton){guard !isRecording,let id=trackIDs[sender.tag],let start=rangeStart,let end=rangeEnd,end>start else{storageMessage("Сначала задай начало и конец диапазона для comp.");return};finishEditing();stopAudio();let take=selectedTakes[id] ?? 0;if check(daw_comp_range(session,id,UInt32(take),start,end,revision)){selectedClips[id]=0;refresh();pollTransport()}}
    func applyClipEdit(_ id: UInt64, _ clipIndex: Int, _ start: UInt64, _ offset: UInt64, _ length: UInt64) {
        guard !isRecording else { return }
        _ = check(daw_edit_clip(session, id, UInt32(clipIndex), start, offset, length, revision)); refresh(); pollTransport()
    }
    func updateClipInspector(_ trackID:UInt64,_ clipIndex:Int){
        var clip=daw_clip();clip.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        guard daw_get_clip(session,trackID,UInt32(clipIndex),&clip)==0 else{return}
        inspectorTrackID=trackID;inspectorClipIndex=clipIndex;selectedMixerID=trackID
        if let strip=mixerWorkspace.strips.first(where:{$0.id == trackID}) {
            inspectorBrowser.channel=InspectorChannelModel(title:strip.title,kind:"TRACK · CLIP \(clipIndex + 1)",renameable:true,volumeDb:strip.volumeDb,pan:strip.pan,muted:strip.isMuted,solo:strip.isSolo,inserts:strip.inserts.map{$0.bypassed ? "⊘ \($0.name)":$0.name},sends:strip.sends.map{"→ \($0.destination)  \(String(format:"%+.1f dB",$0.gainDb))"},accent:strip.color ?? .systemBlue)
        }
        inspectorBrowser.clip=InspectorClipModel(title:"Clip \(clipIndex + 1)",startFrames:clip.start,sourceOffsetFrames:clip.source_offset,lengthFrames:clip.length,fadeInFrames:clip.fade_in,fadeOutFrames:clip.fade_out)
    }
    func applyClipFades(_ id: UInt64, _ clipIndex: Int, _ fadeIn: UInt64, _ fadeOut: UInt64) {
        guard !isRecording else{return};_ = daw_stop(session);if check(daw_set_clip_fades(session,id,UInt32(clipIndex),fadeIn,fadeOut,revision)){refresh();pollTransport()}
    }
    @objc func splitClipAtCursor(_ sender:NSButton) {
        guard !isRecording else { return }
        finishEditing(); stopAudio(); guard let id=trackIDs[sender.tag] else { return }
        let index=selectedClips[id] ?? 0
        var transport=daw_transport(); transport.struct_size=UInt32(MemoryLayout<daw_transport>.size)
        guard check(daw_get_transport(session,&transport)) else { return }
        if check(daw_split_clip(session,id,UInt32(index),transport.frame,revision)) { selectedClips[id]=index+1; refresh(); pollTransport() }
    }
    @objc func duplicateSelectedClip(_ sender:NSButton) { guard !isRecording else{return}; finishEditing(); stopAudio(); guard let id=trackIDs[sender.tag] else{return}; let index=selectedClips[id] ?? 0; var track=daw_track();track.struct_size=UInt32(MemoryLayout<daw_track>.size);guard check(daw_get_track(session,UInt32(sender.tag),&track)) else{return}; if check(daw_duplicate_clip(session,id,UInt32(index),revision)) { selectedClips[id]=Int(track.clip_count); refresh(); pollTransport() } }
    @objc func deleteSelectedClip(_ sender:NSButton) { guard !isRecording else{return}; finishEditing(); stopAudio(); guard let id=trackIDs[sender.tag] else{return}
        var group=groupIndices(id)
        if group.count>1 { if check(daw_delete_clips(session,id,&group,UInt32(group.count),revision)) { selectedClips[id]=0; clipSelection[id]=[0]; refresh(); pollTransport() } }
        else { let index=selectedClips[id] ?? 0; if check(daw_delete_clip(session,id,UInt32(index),revision)) { selectedClips[id]=max(0,index-1); clipSelection.removeValue(forKey:id); refresh(); pollTransport() } } }
    @objc func toggleSelectedCrossfade(_ sender:NSButton) {
        guard !isRecording else{return}; finishEditing(); stopAudio(); guard let id=trackIDs[sender.tag] else{return}
        let index=selectedClips[id] ?? 0
        var left=daw_clip();left.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        var right=daw_clip();right.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        guard check(daw_get_clip(session,id,UInt32(index),&left)) else{return}
        guard daw_get_clip(session,id,UInt32(index+1),&right)==0 else{storageMessage("Для кроссфейда выбери клип, справа от которого есть соседний клип.");return}
        let leftEnd=left.start+left.length
        let overlap=leftEnd>right.start ? leftEnd-right.start : 0
        var value:UInt64=0
        if overlap==0 {
            guard leftEnd==right.start else{storageMessage("Кроссфейд можно создать между соприкасающимися клипами.");return}
            let leftRoom=left.length>left.fade_in ? left.length-left.fade_in : 0
            value=min(480,leftRoom>0 ? leftRoom-1 : 0,right.length>0 ? right.length-1 : 0,right.source_offset)
            guard value>0 else{storageMessage("У клипов недостаточно исходного аудио для кроссфейда.");return}
        }
        if check(daw_set_crossfade(session,id,UInt32(index),value,revision)){refresh();pollTransport()}
    }
    @objc func editClipPanel(_ sender: NSButton) {
        guard !isRecording else { return }
        finishEditing()
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        guard check(daw_get_track(session, UInt32(sender.tag), &track)) else { return }
        let clipIndex=selectedClips[track.id] ?? 0
        var clip=daw_clip(); clip.struct_size=UInt32(MemoryLayout<daw_clip>.size)
        guard check(daw_get_clip(session,track.id,UInt32(clipIndex),&clip)) else { return }
        stopAudio()
        let alert = NSAlert(); alert.messageText = "Положение и границы клипа"
        alert.informativeText = "Время в секундах. Исходное аудио сохраняется целиком."
        let fields = [clip.start, clip.source_offset, clip.length,clip.fade_in,clip.fade_out].map { value -> NSTextField in
            let f = NSTextField(string: String(format: "%.6f", Double(value) / 48000)); f.widthAnchor.constraint(equalToConstant: 180).isActive = true; return f
        }
        let form = NSStackView(); form.orientation = .vertical; form.alignment = .leading; form.spacing = 8
        for (index, title) in ["Позиция на таймлайне", "Отступ в исходном аудио", "Длительность","Fade in","Fade out"].enumerated() {
            fields[index].setAccessibilityLabel(title)
            form.addArrangedSubview(label(title, size: 12)); form.addArrangedSubview(fields[index])
        }
        form.frame = NSRect(x: 0, y: 0, width: 300, height: 276); alert.accessoryView = form
        alert.addButton(withTitle: "Применить"); alert.addButton(withTitle: "Отмена")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        var frames: [UInt64] = []
        for field in fields {
            guard let seconds = Double(field.stringValue.replacingOccurrences(of: ",", with: ".")), seconds.isFinite, seconds >= 0, seconds <= 600 else {
                let error = NSAlert(); error.messageText = "Укажи время от 0 до 600 секунд"; error.runModal(); return
            }
            frames.append(UInt64((seconds * 48000).rounded()))
        }
        guard frames[3]+frames[4] <= frames[2] else { let error=NSAlert();error.messageText="Сумма fades не должна превышать длительность";error.runModal();return }
        _=check(daw_edit_clip_full(session,track.id,UInt32(clipIndex),frames[0],frames[1],frames[2],frames[3],frames[4],revision)); refresh(); pollTransport()
    }
    func seekAudio(_ frame: UInt64) { guard !isRecording else{return}; if check(daw_seek_frame(session, frame)) { pollTransport() } }
    @objc func rewindAudio() { finishEditing(); seekAudio(0) }
    @objc func playAudio() { guard !isRecording else{return}; finishEditing(); if check(daw_play(session)) { pollTransport() } }
    @objc func stopAudio() { _ = check(daw_stop(session)); pollTransport() }
    @objc func togglePlayStop() {
        // Пробел — честный тумблер. Во время записи он обязан завершить тейк:
        // голый daw_stop оставил бы активный рекордер «висячим».
        if isRecording { finishRecording(); return }
        if isPlaying { stopAudio() } else { playAudio() }
    }
    @objc func soloSelectedTrack() {
        guard let id = selectedMixerID ?? inspectorTrackID, let kind = mixerKinds[id], case .track = kind else { return }
        // Get current solo state
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        guard let index = trackIDs.first(where: { $0.value == id })?.key else { return }
        guard check(daw_get_track(session, UInt32(index), &track)) else { return }
        let newState: Int32 = track.solo == 0 ? 1 : 0
        _ = check(daw_set_solo(session, id, newState, revision))
        refresh()
    }
    @objc func muteSelectedTrack() {
        guard let id = selectedMixerID ?? inspectorTrackID, let kind = mixerKinds[id], case .track = kind else { return }
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        guard let index = trackIDs.first(where: { $0.value == id })?.key else { return }
        guard check(daw_get_track(session, UInt32(index), &track)) else { return }
        let newState: Int32 = track.muted == 0 ? 1 : 0
        _ = check(daw_set_mute(session, id, newState, revision))
        refresh()
    }
    @objc func armSelectedTrack() {
        guard let id = selectedMixerID ?? inspectorTrackID else { return }
        armedTrackID = armedTrackID == id ? nil : id
        refresh()
    }
    func finishEditing() { if window.firstResponder is NSTextView { window.makeFirstResponder(nil) } }
    @objc func addTrack() { guard !isRecording else{return}; finishEditing(); if check(daw_add_track(session, "Дорожка \(trackIDs.count + 1)", revision)) { refresh() } }
    @objc func addMidiTrack() {
        guard !isRecording else{return}; finishEditing()
        guard check(daw_add_track(session, "MIDI \(trackIDs.count + 1)", revision)) else { return }
        refresh()
        guard let newID = trackIDs.values.max() else { return }
        var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size); clip.version = UInt32(DAW_MIDI_CLIP_VERSION)
        clip.start = 0; clip.length = 480000; clip.lane = 0; clip.note_count = 0
        guard check(daw_add_midi_clip(session, newID, &clip, nil, 0, revision)) else { return }
        refresh()
        selectedMixerID = newID
        updateMixerInspector(newID)
        status.stringValue = "MIDI дорожка: добавьте ноты и инструмент · ⌘Z отменяет"
    }
    @objc func addBus() { guard !isRecording else{return};finishEditing();if check(daw_add_bus(session,"Bus \(orderedBuses.count + 1)",revision)){refresh()} }
    @objc func addMasterAU(_ sender:NSPopUpButton){guard !isRecording,sender.indexOfSelectedItem>0 else{return};let index=sender.indexOfSelectedItem-1;guard index<auCatalog.count else{return};let item=auCatalog[index];_=daw_stop(session);if check(daw_add_master_au(session,item.type,item.subtype,item.manufacturer,revision)){sender.selectItem(at:0);refresh();pollTransport()}else{sender.selectItem(at:0)}}
    @objc func scanInstalledAudioUnits(){guard auScanJob==nil else{return};let helper=Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/daw_au_scan_helper").path;guard let job=daw_begin_installed_au_scan(helper,3000)else{storageMessage("Не удалось запустить изолированный AU scanner.");return};auScanJob=job;scanAUButton.isEnabled=false;scanAUButton.title="Сканирование…";auScanTimer=Timer.scheduledTimer(withTimeInterval:0.2,repeats:true){[weak self]_ in Task{@MainActor in self?.pollInstalledAudioUnits()}}}
    func pollInstalledAudioUnits(){guard let job=auScanJob else{return};var scan=daw_au_scan_status();scan.struct_size=UInt32(MemoryLayout<daw_au_scan_status>.size);guard daw_poll_installed_au_scan(job,&scan)==0 else{return};guard scan.status != 0 else{return};auScanTimer?.invalidate();auScanTimer=nil;defer{daw_release_installed_au_scan(job);auScanJob=nil;scanAUButton.isEnabled=true};if scan.status==1{var available:UInt32=0;var quarantined:UInt32=0;if check(daw_apply_installed_au_scan(session,job,&available,&quarantined)){if let cache=auCacheURL{_=check(daw_save_installed_au_scan_cache(session,job,cache.path))};reloadAudioUnitPopup(available);scanAUButton.title=quarantined==0 ? "AU: \(available)":"AU: \(available), карантин \(quarantined)"}}else{let message=withUnsafeBytes(of:scan.error){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};scanAUButton.title="Сканировать AU";storageMessage(message)}}
    @objc func addMasterVST3(_ sender:NSPopUpButton){guard !isRecording,sender.indexOfSelectedItem>0,let index=(sender.selectedItem?.representedObject as? NSNumber)?.uint32Value else{return};_=daw_stop(session);if check(daw_add_master_vst3(session,index,revision)){sender.selectItem(at:0);refresh();pollTransport()}else{sender.selectItem(at:0)}}
    @objc func scanInstalledVST3(){guard vst3ScanJob==nil else{return};let helper=Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/daw_vst3_scan_helper").path;guard let job=daw_begin_installed_vst3_scan(helper,3000)else{storageMessage("Не удалось запустить изолированный VST3 scanner.");return};vst3ScanJob=job;scanVST3Button.isEnabled=false;scanVST3Button.title="Сканирование…";vst3ScanTimer=Timer.scheduledTimer(withTimeInterval:0.2,repeats:true){[weak self]_ in Task{@MainActor in self?.pollInstalledVST3()}}}
    func pollInstalledVST3(){guard let job=vst3ScanJob else{return};var scan=daw_vst3_scan_status();scan.struct_size=UInt32(MemoryLayout<daw_vst3_scan_status>.size);guard daw_poll_installed_vst3_scan(job,&scan)==0 else{return};guard scan.status != 0 else{return};vst3ScanTimer?.invalidate();vst3ScanTimer=nil;defer{daw_release_installed_vst3_scan(job);vst3ScanJob=nil;scanVST3Button.isEnabled=true};if scan.status==1{var available:UInt32=0;var quarantined:UInt32=0;if check(daw_apply_installed_vst3_scan(session,job,&available,&quarantined)){if let cache=vst3CacheURL{_ = daw_save_installed_vst3_scan_cache(job,cache.path)};reloadVST3Popup();scanVST3Button.title=quarantined==0 ? "VST3: \(available)":"VST3: \(available), карантин \(quarantined)"}}else{let message=withUnsafeBytes(of:scan.error){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)};scanVST3Button.title="Сканировать VST3";storageMessage(message)}}
    @objc func toggleMasterInsert(_ sender:NSButton){guard !isRecording,let plugin=pluginControlTargets[ObjectIdentifier(sender)]else{return};_=daw_stop(session);if check(daw_set_master_insert_bypass(session,plugin.id,plugin.bypassed ? 0:1,revision)){refresh();pollTransport()}}
    @objc func moveMasterInsertUp(_ sender:NSButton){guard let plugin=pluginControlTargets[ObjectIdentifier(sender)],plugin.index>0 else{return};_=daw_stop(session);if check(daw_move_master_insert(session,plugin.id,plugin.index-1,revision)){refresh();pollTransport()}}
    @objc func moveMasterInsertDown(_ sender:NSButton){guard let plugin=pluginControlTargets[ObjectIdentifier(sender)]else{return};_=daw_stop(session);if check(daw_move_master_insert(session,plugin.id,plugin.index+1,revision)){refresh();pollTransport()}}
    @objc func editMasterInsert(_ sender:NSButton){
        guard !isRecording,let target=pluginEditorTargets[ObjectIdentifier(sender)] else{return};let pluginID=target.id
        var hosting=daw_insert_hosting_status();hosting.struct_size=UInt32(MemoryLayout<daw_insert_hosting_status>.size)
        guard daw_get_insert_hosting_status(session,Int32(DAW_INSERT_OWNER_MASTER),0,pluginID,&hosting) == 0 else{return}
        let isolated=hosting.selected_mode == UInt32(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS)
        guard !isolated || target.isolatedVST3 else { storageMessage("Этот isolated AU пока не предоставляет remote parameter editor. Переключи insert в режим «В процессе», чтобы изменить параметры."); return }
        if target.isolatedVST3 { _=daw_stop(session);pollTransport() }
        var count:UInt32=0;guard check(daw_get_master_insert_parameter_count(session,pluginID,&count))else{return}
        let content=NSStackView();content.orientation = .vertical;content.spacing=8;content.alignment = .leading
        pluginParameterTargets.removeAll()
        for index in 0..<count{
            var parameter=daw_au_parameter();parameter.struct_size=UInt32(MemoryLayout<daw_au_parameter>.size);guard check(daw_get_master_insert_parameter(session,pluginID,index,&parameter))else{return}
            let name=withUnsafeBytes(of:parameter.name){String(decoding:$0.prefix(while:{$0 != 0}),as:UTF8.self)}
            let slider=AutomationSlider(value:Double(parameter.value),minValue:Double(parameter.minimum),maxValue:Double(parameter.maximum),target:self,action:#selector(changeMasterInsertParameter(_:)));slider.isContinuous = !target.isolatedVST3;slider.isEnabled=parameter.writable != 0;slider.widthAnchor.constraint(equalToConstant:220).isActive=true;slider.setAccessibilityLabel("Параметр \(name)");slider.setAccessibilityHelp(target.isolatedVST3 ? "VST3 исполняется в отдельном helper. Значение применяется одним controlled reprepare после отпускания slider." : "Изменяет параметр plug-in.");let gestureTarget=(Int32(DAW_INSERT_OWNER_MASTER),UInt64(0),pluginID,parameter.id,name);slider.automationBegin={[weak self]in let range=slider.maxValue-slider.minValue;self?.beginPluginParameterAutomation(gestureTarget,normalized:range == 0 ? 0:(slider.doubleValue-slider.minValue)/range)};slider.automationEnd={[weak self]in self?.endPluginParameterAutomation()};pluginParameterTargets[ObjectIdentifier(slider)]=(pluginID,parameter.id);pluginParameterGestureTargets[ObjectIdentifier(slider)]=(gestureTarget.0,gestureTarget.1,gestureTarget.2,gestureTarget.3,gestureTarget.4,slider.minValue,slider.maxValue)
            let value=label(String(format:"%.3f",parameter.value),size:11,color:.secondaryLabelColor);value.font = .monospacedDigitSystemFont(ofSize:11,weight:.regular);value.widthAnchor.constraint(equalToConstant:66).isActive=true
            var views:[NSView]=[label(name,size:12,color:.labelColor),flexibleSpace(),slider,value]
            if let automation=parameterAutomationButton(owner:Int32(DAW_INSERT_OWNER_MASTER),ownerID:0,plugin:pluginID,parameter:parameter,name:name){views.insert(automation,at:1)}
            views.insert(parameterArmButton((gestureTarget.0,gestureTarget.1,gestureTarget.2,gestureTarget.3,gestureTarget.4)),at:1)
            let row=NSStackView(views:views);row.spacing=8;row.widthAnchor.constraint(equalToConstant:520).isActive=true;content.addArrangedSubview(row)
        }
        if count==0{content.addArrangedSubview(label("Этот плагин не публикует параметры.",size:13,color:.secondaryLabelColor))}
        let scroll=NSScrollView();scroll.documentView=content;scroll.hasVerticalScroller=true;scroll.drawsBackground=false;scroll.widthAnchor.constraint(equalToConstant:560).isActive=true;scroll.heightAnchor.constraint(equalToConstant:min(420,max(90,CGFloat(count)*34))).isActive=true
        let alert=NSAlert();alert.messageText="Параметры плагина";alert.informativeText=target.isolatedVST3 ? "VST3 работает в отдельном helper. Изменение параметра выполнит controlled reprepare и сохранится в проекте.":"Изменения сохраняются в проекте и применяются к playback и export.";alert.accessoryView=scroll;alert.addButton(withTitle:"Готово");alert.runModal();pluginParameterTargets.removeAll();refresh();pollTransport()
    }
    @objc func changeMasterInsertParameter(_ sender:NSSlider){guard let target=pluginParameterTargets[ObjectIdentifier(sender)],let gesture=pluginParameterGestureTargets[ObjectIdentifier(sender)]else{return};let armed=(gesture.owner,gesture.ownerID,gesture.plugin,gesture.parameter,gesture.name);if pluginParameterWrites(armed){let range=gesture.maximum-gesture.minimum;_ = writePluginParameterAutomation(armed,normalized:range == 0 ? 0:(sender.doubleValue-gesture.minimum)/range);return};_=daw_stop(session);if check(daw_set_master_insert_parameter(session,target.plugin,target.parameter,Float(sender.doubleValue),revision)){var snapshot=daw_snapshot();snapshot.struct_size=UInt32(MemoryLayout<daw_snapshot>.size);if daw_get_snapshot(session,&snapshot)==0{revision=snapshot.revision;window.isDocumentEdited=dirty}}}
    @objc func removeMasterInsert(_ sender:NSButton){guard !isRecording,let plugin=pluginControlTargets[ObjectIdentifier(sender)]else{return};_=daw_stop(session);if check(daw_remove_master_insert(session,plugin.id,revision)){refresh();pollTransport()}}
    @objc func editTrackAutomation(_ sender:NSButton){guard !isRecording,let trackID=trackIDs[sender.tag]else{return};var track=daw_track();track.struct_size=UInt32(MemoryLayout<daw_track>.size);guard check(daw_get_track(session,UInt32(sender.tag),&track))else{return};var count:UInt32=0;guard check(daw_get_track_volume_automation_count(session,trackID,&count))else{return};let popup=NSPopUpButton();var frames:[UInt64]=[];for index in 0..<count{var point=daw_automation_point();point.struct_size=UInt32(MemoryLayout<daw_automation_point>.size);guard check(daw_get_track_volume_automation_point(session,trackID,index,&point))else{return};frames.append(point.frame);popup.addItem(withTitle:String(format:"%7.3f с     %+.1f dB",Double(point.frame)/48000,point.gain_db))};popup.isEnabled=count>0;popup.widthAnchor.constraint(equalToConstant:260).isActive=true;let alert=NSAlert();alert.messageText="Автоматизация громкости";alert.informativeText="Добавь значение текущего фейдера в позиции курсора или удали выбранную точку.";alert.accessoryView=popup;alert.addButton(withTitle:"Точка в позиции");alert.addButton(withTitle:"Удалить выбранную");alert.addButton(withTitle:"Закрыть");let response=alert.runModal();if response == .alertFirstButtonReturn,let frame=currentTransportFrame(){_=daw_stop(session);if check(daw_upsert_track_volume_automation_point(session,trackID,frame,track.gain_db,revision)){refresh();pollTransport()}}else if response == .alertSecondButtonReturn,count>0{let frame=frames[popup.indexOfSelectedItem];_=daw_stop(session);if check(daw_remove_track_volume_automation_point(session,trackID,frame,revision)){refresh();pollTransport()}}}
    func automationDialog(_ title:String,_ unit:String,_ points:[daw_automation_point])->(NSApplication.ModalResponse,UInt64?){let popup=NSPopUpButton();for point in points{popup.addItem(withTitle:String(format:"%7.3f с     %+.2f %@",Double(point.frame)/48000,point.gain_db,unit))};popup.isEnabled = !points.isEmpty;popup.widthAnchor.constraint(equalToConstant:280).isActive=true;let alert=NSAlert();alert.messageText=title;alert.informativeText="Добавь текущее значение в позиции курсора или удали выбранную точку.";alert.accessoryView=popup;alert.addButton(withTitle:"Точка в позиции");alert.addButton(withTitle:"Удалить выбранную");alert.addButton(withTitle:"Закрыть");let response=alert.runModal();let frame=points.isEmpty ? nil:points[popup.indexOfSelectedItem].frame;return(response,frame)}
    @objc func editTrackPanAutomation(_ sender:NSButton){guard !isRecording,let trackID=trackIDs[sender.tag]else{return};var track=daw_track();track.struct_size=UInt32(MemoryLayout<daw_track>.size);guard check(daw_get_track(session,UInt32(sender.tag),&track))else{return};var count:UInt32=0;guard check(daw_get_track_pan_automation_count(session,trackID,&count))else{return};var points:[daw_automation_point]=[];for index in 0..<count{var point=daw_automation_point();point.struct_size=UInt32(MemoryLayout<daw_automation_point>.size);guard check(daw_get_track_pan_automation_point(session,trackID,index,&point))else{return};points.append(point)};let choice=automationDialog("Автоматизация панорамы","",points);if choice.0 == .alertFirstButtonReturn,let frame=currentTransportFrame(){_=daw_stop(session);if check(daw_upsert_track_pan_automation_point(session,trackID,frame,track.pan,revision)){refresh();pollTransport()}}else if choice.0 == .alertSecondButtonReturn,let frame=choice.1{_=daw_stop(session);if check(daw_remove_track_pan_automation_point(session,trackID,frame,revision)){refresh();pollTransport()}}}
    @objc func editBusAutomation(_ sender:NSButton){guard !isRecording,let busID=busAutomationTargets[ObjectIdentifier(sender)],let index=orderedBuses.firstIndex(where:{$0.id==busID})else{return};var bus=daw_bus();bus.struct_size=UInt32(MemoryLayout<daw_bus>.size);guard check(daw_get_bus(session,UInt32(index),&bus))else{return};var count:UInt32=0;guard check(daw_get_bus_gain_automation_count(session,busID,&count))else{return};var points:[daw_automation_point]=[];for pointIndex in 0..<count{var point=daw_automation_point();point.struct_size=UInt32(MemoryLayout<daw_automation_point>.size);guard check(daw_get_bus_gain_automation_point(session,busID,pointIndex,&point))else{return};points.append(point)};let choice=automationDialog("Автоматизация громкости bus","dB",points);if choice.0 == .alertFirstButtonReturn,let frame=currentTransportFrame(){_=daw_stop(session);if check(daw_upsert_bus_gain_automation_point(session,busID,frame,bus.gain_db,revision)){refresh();pollTransport()}}else if choice.0 == .alertSecondButtonReturn,let frame=choice.1{_=daw_stop(session);if check(daw_remove_bus_gain_automation_point(session,busID,frame,revision)){refresh();pollTransport()}}}
    @objc func editMasterAutomation(){guard !isRecording else{return};var count:UInt32=0;guard check(daw_get_master_gain_automation_count(session,&count))else{return};var points:[daw_automation_point]=[];for index in 0..<count{var point=daw_automation_point();point.struct_size=UInt32(MemoryLayout<daw_automation_point>.size);guard check(daw_get_master_gain_automation_point(session,index,&point))else{return};points.append(point)};let choice=automationDialog("Автоматизация мастера","dB",points);if choice.0 == .alertFirstButtonReturn,let frame=currentTransportFrame(){_=daw_stop(session);if check(daw_upsert_master_gain_automation_point(session,frame,masterSlider.doubleValue,revision)){refresh();pollTransport()}}else if choice.0 == .alertSecondButtonReturn,let frame=choice.1{_=daw_stop(session);if check(daw_remove_master_gain_automation_point(session,frame,revision)){refresh();pollTransport()}}}
    @objc func changeOutput(_ sender:NSPopUpButton){
        guard !isRecording,let source=outputTargets[ObjectIdentifier(sender)],let destination=(sender.selectedItem?.representedObject as? NSNumber)?.uint64Value else{return}
        _=daw_stop(session);let result=source.isBus ? daw_set_bus_output(session,source.id,destination,revision):daw_set_track_output(session,source.id,destination,revision)
        if check(result){refresh();pollTransport()}
    }
    @objc func addSend(_ sender:NSPopUpButton){
        guard !isRecording,let track=newSendTargets[ObjectIdentifier(sender)],let bus=(sender.selectedItem?.representedObject as? NSNumber)?.uint64Value,bus != 0 else{return}
        _=daw_stop(session);if check(daw_upsert_send(session,track,bus,-12,0,revision)){refresh();pollTransport()}
    }
    @objc func changeSendGain(_ sender:NSSlider){guard !isRecording,let route=sendControlTargets[ObjectIdentifier(sender)]else{return};let gain=(sender.doubleValue*10).rounded()/10;if check(daw_upsert_send(session,route.track,route.bus,gain,route.pre ? 1:0,revision)){refresh()} }
    @objc func toggleSendPre(_ sender:NSButton){guard !isRecording,let route=sendControlTargets[ObjectIdentifier(sender)]else{return};if check(daw_upsert_send(session,route.track,route.bus,route.gain,route.pre ? 0:1,revision)){refresh()} }
    @objc func removeSend(_ sender:NSButton){guard !isRecording,let route=sendControlTargets[ObjectIdentifier(sender)]else{return};_=daw_stop(session);if check(daw_remove_send(session,route.track,route.bus,revision)){refresh();pollTransport()} }
    @objc func changeBusGain(_ sender:NSSlider){guard !isRecording,let id=busControlTargets[ObjectIdentifier(sender)]else{return};let value=(sender.doubleValue*10).rounded()/10;if automationWrites(target:automationBusGain,id:id){_ = writeAutomation(target:automationBusGain,id:id,value:value);return};_=check(daw_set_bus_gain(session,id,value,revision));refresh()}
    @objc func changeBusPan(_ sender:NSSlider){guard !isRecording,let id=busControlTargets[ObjectIdentifier(sender)]else{return};_=check(daw_set_bus_pan(session,id,(sender.doubleValue*100).rounded()/100,revision));refresh()}
    @objc func toggleBusMute(_ sender:NSButton){guard !isRecording,let id=busControlTargets[ObjectIdentifier(sender)]else{return};_=check(daw_set_bus_mute(session,id,sender.state == .on ? 1:0,revision));refresh()}
    func controlTextDidEndEditing(_ notification: Notification) {
        guard let sender = notification.object as? NSTextField else { return }
        if let id=busNameTargets[ObjectIdentifier(sender)]{_=check(daw_rename_bus(session,id,sender.stringValue,revision));refresh();return}
        renameTrack(sender)
    }
    func renameTrack(_ sender: NSTextField) {
        guard !isRecording else { return }
        guard let id = trackIDs[sender.tag] else { return }
        _ = check(daw_rename_track(session, id, sender.stringValue, revision)); refresh()
    }
    @objc func changeGain(_ sender: NSSlider) {
        guard !isRecording else { return }
        guard let id = trackIDs[sender.tag] else { return }
        let value=(sender.doubleValue * 10).rounded() / 10
        if automationWrites(target:automationTrackVolume,id:id){_ = writeAutomation(target:automationTrackVolume,id:id,value:value);return}
        _ = check(daw_set_gain(session, id, value, revision)); refresh()
    }
    @objc func changePan(_ sender:NSSlider){guard !isRecording,let id=trackIDs[sender.tag] else{return};let value=(sender.doubleValue*100).rounded()/100;if automationWrites(target:automationTrackPan,id:id){_ = writeAutomation(target:automationTrackPan,id:id,value:value);return};_=check(daw_set_pan(session,id,value,revision));refresh()}
    @objc func toggleMute(_ sender:NSButton){guard !isRecording,let id=trackIDs[sender.tag] else{return};_=check(daw_set_mute(session,id,sender.state == .on ? 1:0,revision));refresh()}
    @objc func toggleSolo(_ sender:NSButton){guard !isRecording,let id=trackIDs[sender.tag] else{return};_=check(daw_set_solo(session,id,sender.state == .on ? 1:0,revision));refresh()}
    @objc func changeMasterGain(_ sender:NSSlider){guard !isRecording else{return};let value=(sender.doubleValue*10).rounded()/10;if automationWrites(target:automationMasterGain,id:0){_ = writeAutomation(target:automationMasterGain,id:0,value:value);return};_=check(daw_set_master_gain(session,value,revision));refresh()}
    @objc func undo() { guard !isRecording else{return}; finishEditing(); if undoButton.isEnabled && check(daw_undo(session, revision)) { setProjectMessage("Изменение отменено"); refresh(); updateStorageStatus() } }
    @objc func redo() { guard !isRecording else{return}; finishEditing(); if redoButton.isEnabled && check(daw_redo(session, revision)) { setProjectMessage("Изменение повторено"); refresh(); updateStorageStatus() } }
    @objc func quit() { NSApp.terminate(nil) }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if closeApproved { return true }
        requestLeave { [weak self] in DispatchQueue.main.async { self?.closeApproved = true; NSApp.terminate(nil) } }; return false
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if closeApproved { return .terminateNow }
        requestLeave { [weak self] in DispatchQueue.main.async { self?.closeApproved = true; NSApp.terminate(nil) } }
        return .terminateCancel
    }
    func applicationWillTerminate(_ notification: Notification) {
        transportTimer?.invalidate();auScanTimer?.invalidate();vst3ScanTimer?.invalidate(); stopBrowserAudioPreview(); releaseImportJob(cancel: true); cancelAutomationGesture(); cancelPluginParameterAutomation(); if let vst3ScanJob { daw_release_installed_vst3_scan(vst3ScanJob) }; rotateRecovery()
        if let exportJob { daw_cancel_export(exportJob); daw_release_export(exportJob) }
        if let dawprojectJob { daw_cancel_dawproject_export(dawprojectJob); daw_release_dawproject_export(dawprojectJob) }
        daw_release_save(saveJob); daw_release_save(recoveryJob); daw_destroy(session); session = nil
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
let app = NSApplication.shared
let delegate = DraftApp()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
