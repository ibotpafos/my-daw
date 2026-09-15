import AppKit

/// Semantic icon API. SF Symbols are resolved at draw time and inherit accessibility labels.
enum DAWIcon: String, CaseIterable {
    // Transport
    case play = "play.fill", pause = "pause.fill", stop = "stop.fill", record = "record.circle.fill", rewind = "backward.end.fill", forward = "forward.end.fill", loop = "repeat"
    // Editing / workspace
    case select = "cursorarrow", scissors = "scissors", split = "rectangle.split.2x1", undo = "arrow.uturn.backward", redo = "arrow.uturn.forward", snap = "scope", grid = "grid", browser = "sidebar.left", inspector = "sidebar.right", mixer = "slider.vertical.3"
    // Tracks / effects
    case audioTrack = "waveform", instrumentTrack = "pianokeys", vocalTrack = "mic.fill", drumTrack = "music.note.list", guitarTrack = "guitars", bus = "arrow.triangle.branch", folder = "folder.fill", equalizer = "slider.horizontal.3", compressor = "waveform.path.ecg", reverb = "water.waves", effect = "sparkles"
    // AI / status
    case alignDoubles = "arrow.left.and.right.text.vertical", cleanBreath = "wind", matchLoudness = "speaker.wave.2", analyze = "wand.and.stars", saved = "checkmark.icloud", syncing = "arrow.triangle.2.circlepath.icloud", offline = "icloud.slash", warning = "exclamationmark.triangle.fill", error = "xmark.octagon.fill", info = "info.circle"

    var symbol: NSImage? { NSImage(systemSymbolName: rawValue, accessibilityDescription: accessibilityLabel) }
    var accessibilityLabel: String {
        switch self {
        case .play: return "Воспроизвести"; case .pause: return "Пауза"; case .stop: return "Стоп"; case .record: return "Запись"; case .rewind: return "В начало"; case .forward: return "Вперёд"; case .loop: return "Цикл"
        case .select: return "Выбор"; case .scissors: return "Ножницы"; case .split: return "Разделить"; case .undo: return "Отменить"; case .redo: return "Повторить"; case .snap: return "Привязка"; case .grid: return "Сетка"; case .browser: return "Браузер"; case .inspector: return "Инспектор"; case .mixer: return "Микшер"
        case .audioTrack: return "Аудиодорожка"; case .instrumentTrack: return "Инструментальная дорожка"; case .vocalTrack: return "Вокальная дорожка"; case .drumTrack: return "Ударные"; case .guitarTrack: return "Гитара"; case .bus: return "Шина"; case .folder: return "Папка"; case .equalizer: return "Эквалайзер"; case .compressor: return "Компрессор"; case .reverb: return "Реверберация"; case .effect: return "Эффект"
        case .alignDoubles: return "Выровнять дубли"; case .cleanBreath: return "Смягчить дыхание"; case .matchLoudness: return "Выровнять громкость"; case .analyze: return "Анализировать"; case .saved: return "Сохранено"; case .syncing: return "Синхронизация"; case .offline: return "Нет сети"; case .warning: return "Предупреждение"; case .error: return "Ошибка"; case .info: return "Информация"
        }
    }
}
