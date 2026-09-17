import Foundation

/// UI availability only. The existing renderer remains the authority on the
/// actual range, plug-in preparation, audio generation and export-tail length.
struct MixExportPolicy {
    enum Block: String, Equatable {
        case unavailable = "Состояние проекта недоступно. Повторите экспорт после обновления."
        case busy = "Дождитесь завершения текущего экспорта или импорта."
        case dialog = "Диалог экспорта уже открыт."
        case recording = "Сначала завершите аудиозапись. Экспорт не останавливает запись автоматически."
        case midiCapture = "Сначала завершите запись MIDI с клавиатуры."
        case gesture = "Сначала завершите или отмените текущий жест редактирования."
        case empty = "Добавьте аудиоклип или MIDI-клип с инструментом перед экспортом."
        case range = "Диапазон экспорта пуст или выходит за пределы проекта."
    }
    var available = true
    var hasAudio = false
    var hasMIDI = false
    var duration: UInt64 = 0
    var busy = false
    var dialogOpen = false
    var recording = false
    var midiCapture = false
    var gesture = false
    var start: UInt64?
    var end: UInt64?

    var block: Block? {
        if !available { return .unavailable }
        if busy { return .busy }
        if dialogOpen { return .dialog }
        if recording { return .recording }
        if midiCapture { return .midiCapture }
        if gesture { return .gesture }
        if !(hasAudio || hasMIDI) || duration == 0 { return .empty }
        // A lone start is an insertion cursor, not an export range.
        if let end, start == nil || start! >= end || end > duration { return .range }
        return nil
    }
    var canExport: Bool { block == nil }
    var midiOnly: Bool { hasMIDI && !hasAudio }
}
