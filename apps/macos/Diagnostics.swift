import Foundation
import OSLog

/// Наблюдаемость приложения одним каналом: unified log с фиксированным
/// subsystem = bundle id. Категории подобраны так, чтобы жалоба «не работает /
/// криво» превращалась в одну читаемую строку:
///
///     log show --last 30m --predicate 'subsystem == "dev.mydaw.prototype"' --style compact
///     ./scripts/run-macos.sh --telemetry
///
/// Правило приватности: в лог не пишутся пути проекта, имена файлов, образцы
/// аудио и содержимое полей. Только номер сборки, категория события, короткая
/// техническая причина от движка и счётчики. Функция вызывающей стороны
/// передаётся аргументом по умолчанию, поэтому ни одна точка вызова не требует
/// правки.
enum DAWLog {
    static let subsystem = Bundle.main.bundleIdentifier ?? "dev.mydaw.prototype"

    /// Версия из Info.plist и короткая ревизия из сборки: единственный способ
    /// удалённо понять, какую из трёх копий (app / Next / Candidate) видит пользователь.
    static let buildStamp: String = {
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let commit = info["DAWBuildCommit"] as? String ?? "unknown"
        return "\(version) (\(commit))"
    }()

    static let lifecycle = Logger(subsystem: subsystem, category: "Lifecycle")
    static let bridge = Logger(subsystem: subsystem, category: "Bridge")
    static let audio = Logger(subsystem: subsystem, category: "Audio")
    static let jobs = Logger(subsystem: subsystem, category: "Jobs")
    static let plugins = Logger(subsystem: subsystem, category: "Plugins")
    static let layout = Logger(subsystem: subsystem, category: "Layout")
}

/// Состояние троттла. Оба поля читаются и пишутся только с главной нити (все
/// точки вызова — AppKit-колбэки), поэтому `nonisolated(unsafe)` здесь честная
/// договорённость, а не обход проверки.
nonisolated(unsafe) private var lastLayoutAudit = Date.distantPast
nonisolated(unsafe) private var lastLayoutSummary = Date.distantPast

/// Самоаудит раскладки. Экран недоступен агенту сборки (ни Screen Recording,
/// ни Accessibility), а ровно эти дефекты уже случались: уехавший за границу
/// окна Inspector, arrangement, сжатый в узкую полосу, консоль, показывающая
/// середину слишком высокого document view. Поэтому геометрия проверяется на
/// месте и уходит в unified log: одна строка-базис при старте и строка
/// уровня error при нарушении инварианта. Дёргается на границах (восстановление
/// макета, смена режима, movement разделителя, resize окна) и троттлится,
/// чтобы не превращаться в след на каждое событие.
extension DraftApp {
    func layoutAudit(_ trigger: String) {
        guard let window, let content = window.contentView else { return }
        let now = Date()
        let auditDue = now.timeIntervalSince(lastLayoutAudit) > 0.4
        let summaryDue = now.timeIntervalSince(lastLayoutSummary) > 5
        guard auditDue || summaryDue else { return }
        if auditDue { lastLayoutAudit = now }

        var problems: [String] = []
        let size = content.bounds.size
        if size.width < 1 || size.height < 1 {
            problems.append("content size \(Int(size.width))x\(Int(size.height))")
        }
        let arrangementWidth = timelineScroll?.frame.width ?? -1
        if arrangementWidth < 320 { problems.append("arrangement width \(Int(arrangementWidth)) < 320") }
        let headersWidth = trackHeaderScroll?.frame.width ?? -1
        if headersWidth < 190 || headersWidth > 420 {
            problems.append("pinned headers width \(Int(headersWidth)) outside 190...420")
        }

        for (tag, split) in [("headers", trackTimelineSplit), ("inspector", arrangementInspectorSplit),
                             ("console", arrangementConsoleSplit)] {
            guard let split, split.subviews.count >= 2 else { continue }
            let span = split.isVertical ? split.bounds.width : split.bounds.height
            let firstPane = split.subviews[0].frame
            let divider = split.isVertical ? firstPane.maxX : firstPane.maxY
            if divider < 1 || divider > span - 1 {
                problems.append("\(tag) divider \(Int(divider)) at edge of \(Int(span))")
            }
            for (index, pane) in split.subviews.enumerated() where index < 4 {
                let paneSpan = split.isVertical ? pane.frame.width : pane.frame.height
                if paneSpan < 1 { problems.append("\(tag) pane \(index) collapsed") }
            }
        }

        // MixerWorkspaceView — сам NSScrollView (documentView = холст полос), так
        // что проверяется его собственный document/viewport, а не родительский скролл.
        if let document = mixerWorkspace.documentView {
            let docHeight = document.frame.height
            let viewport = mixerWorkspace.contentView.bounds
            if docHeight < 1 { problems.append("mixer document height \(Int(docHeight))") }
            if viewport.minY < -1 || viewport.maxY > docHeight + 1 {
                problems.append("mixer clip \(Int(viewport.minY))..\(Int(viewport.maxY)) outside document \(Int(docHeight))")
            }
            if mixerWorkspace.frame.width < 1 { problems.append("mixer strip width collapsed") }
        }

        if !problems.isEmpty {
            lastLayoutSummary = now
            DAWLog.layout.error("Раскладка \(trigger, privacy: .public): \(problems.prefix(8).joined(separator: "; "), privacy: .public)")
        } else if summaryDue {
            lastLayoutSummary = now
            let mixerDoc = Int(mixerWorkspace.documentView?.frame.height ?? -1)
            let mixerViewport = Int(mixerWorkspace.contentView.bounds.height)
            DAWLog.layout.info("Базис раскладки \(trigger, privacy: .public): content \(Int(size.width))x\(Int(size.height)), arrangement \(Int(arrangementWidth)), headers \(Int(headersWidth)), mixer doc \(mixerDoc)/viewport \(mixerViewport, privacy: .public)")
        }
    }
}
