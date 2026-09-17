import AppKit

enum InspectorBrowserKind: Int { case audio = 0, plugins = 1 }

struct InspectorChannelModel: Equatable {
    var title = ""
    var kind = "TRACK"
    var renameable = true
    var volumeDb: Double = 0
    var pan: Double = 0
    var muted = false
    var solo = false
    var inserts: [String] = []
    var sends: [String] = []
    var accent: NSColor = .systemBlue
}

struct InspectorClipModel: Equatable {
    var title = ""
    var startFrames: UInt64 = 0
    var sourceOffsetFrames: UInt64 = 0
    var lengthFrames: UInt64 = 0
    var fadeInFrames: UInt64 = 0
    var fadeOutFrames: UInt64 = 0
}

struct InspectorMidiModel: Equatable {
    var clips: [PianoRollClipModel] = []
    var selectedClip: Int?
    var notes: [PianoRollNote] = []
    var editable = false
    var context: PRClipContext?
}

/// One live CoreMIDI source the app can arm a keyboard take from. id 0 is the
/// bridge's "no input" sentinel and never appears here; the view adds "Нет".
struct InspectorMidiInputOption: Equatable {
    var id: UInt32 = 0
    var title = ""
}

/// MIDI capture row of the track inspector: which source feeds a keyboard take
/// and whether a take is armed. The app owns every value; the view only draws
/// them and reports the two user choices.
struct InspectorMidiCaptureModel: Equatable {
    var inputs: [InspectorMidiInputOption] = []
    var selectedID: UInt32 = 0
    var armed = false
    var statusLine = ""
}

struct InspectorBrowserItem: Equatable, Identifiable {
    var id = UUID()
    var title = ""
    var detail = ""
    var available = true
    var category: LibraryCategory = .audio
    var sourceURL: URL?
    var pluginResourceKey: String?
    var pluginFormat: LibraryFormat = .unknown
    var resourceKey: String? { sourceURL.flatMap(LibraryResourceKey.audio) ?? pluginResourceKey }
    var format: LibraryFormat { category == .audio ? LibraryFormat.audio(sourceURL) : pluginFormat }
}

/// Selection inspector only. The original MIDI editor instance lives in the
/// bottom dock; library content has its own permanent surface on the left.
@MainActor
final class InspectorBrowserView: NSView {
    var channel: InspectorChannelModel? { didSet { if channel != oldValue { reloadInspector() } } }
    var clip: InspectorClipModel? { didSet { if clip != oldValue { clearValidation() }; reloadInspector() } }
    var midi: InspectorMidiModel? { didSet { applyMidi() } }
    var midiCapture: InspectorMidiCaptureModel? { didSet { applyMidiCapture() } }
    var onChannelChange: ((Double, Double) -> Void)?
    var onChannelRename: ((String) -> Void)?
    var onChannelMute: ((Bool) -> Void)?
    var onChannelSolo: ((Bool) -> Void)?
    var onClipChange: ((InspectorClipModel) -> Void)?
    var onMidiClipSelect: ((Int) -> Void)?
    var onMidiAddClip: (() -> Void)?
    var onMidiRemoveClip: ((Int) -> Void)?
    var onMidiInputSelect: ((UInt32) -> Void)?
    var onMidiRecordToggle: (() -> Void)?
    var onMidiNotesChange: (([PianoRollNote]) -> Void)?
    var onMidiCommitRequest: ((PRCommitRequest) -> Void)?
    var onMidiAddNote: (() -> Void)?
    var onMidiRemoveNote: ((Int) -> Void)?
    var onShowDevices: (() -> Void)?
    let midiEditor = PianoRollEditorView()
    let volume = NSSlider(value: 0, minValue: -120, maxValue: 24, target: nil, action: nil)
    let pan = NSSlider(value: 0, minValue: -1, maxValue: 1, target: nil, action: nil)
    private let form = NSStackView()
    private let titleLabel = NSTextField(labelWithString: "Канал")
    private let channelName = NSTextField(string: "")
    private let selection = NSTextField(wrappingLabelWithString: "Выберите дорожку, шину или мастер.")
    private let volumeCaption = NSTextField(labelWithString: "ГРОМКОСТЬ")
    private let panCaption = NSTextField(labelWithString: "ПАНОРАМА")
    private let mute = NSButton(title: "Mute", target: nil, action: nil)
    private let solo = NSButton(title: "Solo", target: nil, action: nil)
    let signalChain = InspectorSignalChainView(frame: .zero)
    private let channelIcon = NSImageView()
    private let channelBadge = WorkspaceSurface()
    private let volumeValue = NSTextField(labelWithString: "")
    private let panValue = NSTextField(labelWithString: "")
    private let clipForm = NSStackView()
    private var fields: [NSTextField] = []
    private let validation = NSTextField(wrappingLabelWithString: "")
    var validationMessage: String? { validation.isHidden ? nil : validation.stringValue }
    private func clearValidation() { validation.isHidden = true; validation.stringValue = "" }
    private let midiCaptureForm = NSStackView()
    private let midiInputPopup = NSPopUpButton()
    private let midiRecordButton = NSButton(title: "Запись с клавиатуры", target: nil, action: nil)
    private let midiCaptureStatus = NSTextField(wrappingLabelWithString: "")
    private var midiInputIDs: [UInt32] = []
    private var midiInputTitles: [String] = []
    var editingEnabled = true { didSet { if editingEnabled != oldValue { signalChain.setEditingEnabled(editingEnabled); reloadInspector(); applyMidi() } } }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true; layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        titleLabel.font = .systemFont(ofSize: 12, weight: .semibold)
        titleLabel.stringValue = "Инспектор"
        titleLabel.textColor = DAWDesignTokens.Color.text
        titleLabel.translatesAutoresizingMaskIntoConstraints = false; addSubview(titleLabel)
        let scroll = NSScrollView(); scroll.drawsBackground = false; scroll.hasVerticalScroller = true
        scroll.translatesAutoresizingMaskIntoConstraints = false; addSubview(scroll)
        let document = DraftCanvas(); document.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = document
        form.orientation = .vertical; form.alignment = .leading; form.spacing = 10
        form.translatesAutoresizingMaskIntoConstraints = false; document.addSubview(form)
        NSLayoutConstraint.activate([
            titleLabel.topAnchor.constraint(equalTo: topAnchor, constant: 14),
            titleLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14),
            titleLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            scroll.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 14),
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor), scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            document.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor),
            form.leadingAnchor.constraint(equalTo: document.leadingAnchor, constant: 14),
            form.trailingAnchor.constraint(equalTo: document.trailingAnchor, constant: -14),
            form.topAnchor.constraint(equalTo: document.topAnchor), form.bottomAnchor.constraint(equalTo: document.bottomAnchor, constant: -14)
        ])
        channelName.font = .systemFont(ofSize: 13, weight: .semibold)
        channelName.isBordered = false; channelName.drawsBackground = false
        channelName.focusRingType = .exterior
        channelName.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        channelName.setAccessibilityHelp("Введите имя и нажмите Return, чтобы переименовать канал")
        channelName.target = self; channelName.action = #selector(rename)
        channelName.setAccessibilityLabel("Имя выбранного канала")
        selection.font = .systemFont(ofSize: 11); selection.textColor = DAWDesignTokens.Color.secondaryText
        for (slider, action) in [(volume, #selector(changeChannel)), (pan, #selector(changeChannel))] {
            slider.target = self; slider.action = action; slider.isContinuous = false; slider.controlSize = .mini
        }
        volume.setAccessibilityLabel("Громкость выбранного канала, dB")
        pan.setAccessibilityLabel("Панорама выбранного канала")
        for caption in [volumeCaption, panCaption] { caption.font = .monospacedDigitSystemFont(ofSize: 11, weight: .medium); caption.textColor = DAWDesignTokens.Color.secondaryText }
        for button in [mute, solo, midiRecordButton] { button.bezelStyle = .texturedRounded; button.font = .systemFont(ofSize: 11, weight: .medium); button.target = self }
        mute.setButtonType(.toggle); solo.setButtonType(.toggle)
        mute.action = #selector(changeMute); solo.action = #selector(changeSolo)
        let flags = NSStackView(views: [mute, solo]); flags.spacing = 8
        let devices = NSButton(title: "Открыть устройства канала ↗", target: self, action: #selector(showDevices))
        devices.bezelStyle = .inline; devices.font = .systemFont(ofSize: 11, weight: .medium)
        clipForm.orientation = .vertical; clipForm.alignment = .leading; clipForm.spacing = 8
        for (index, name) in ["Начало · с", "Смещение исходника · с", "Длительность · с", "Fade in · с", "Fade out · с"].enumerated() {
            let field = NSTextField(string: "0.000"); field.tag = index; field.target = self; field.action = #selector(changeClip)
            field.setAccessibilityLabel(name); fields.append(field)
            let row = NSStackView(views: [caption(name), field]); row.alignment = .centerY; row.spacing = 8
            field.widthAnchor.constraint(equalToConstant: 88).isActive = true
            clipForm.addArrangedSubview(row)
        }
        validation.font = .systemFont(ofSize: 11); validation.textColor = DAWDesignTokens.Color.coral; validation.isHidden = true
        midiInputPopup.target = self; midiInputPopup.action = #selector(selectMidiInput)
        midiInputPopup.setAccessibilityLabel("MIDI-вход")
        midiRecordButton.action = #selector(toggleMidiRecording)
        midiCaptureStatus.font = .systemFont(ofSize: 10); midiCaptureStatus.textColor = DAWDesignTokens.Color.secondaryText
        midiCaptureForm.orientation = .vertical; midiCaptureForm.alignment = .leading; midiCaptureForm.spacing = 6
        [caption("MIDI-ВХОД"), midiInputPopup, midiRecordButton, midiCaptureStatus].forEach { midiCaptureForm.addArrangedSubview($0) }
        channelBadge.widthAnchor.constraint(equalToConstant: 44).isActive = true
        channelBadge.heightAnchor.constraint(equalToConstant: 48).isActive = true
        channelIcon.translatesAutoresizingMaskIntoConstraints = false; channelBadge.addSubview(channelIcon)
        NSLayoutConstraint.activate([
            channelIcon.centerXAnchor.constraint(equalTo: channelBadge.centerXAnchor),
            channelIcon.centerYAnchor.constraint(equalTo: channelBadge.centerYAnchor),
            channelIcon.widthAnchor.constraint(equalToConstant: 27), channelIcon.heightAnchor.constraint(equalToConstant: 30)
        ])
        let identity = NSStackView(views: [channelName, selection]); identity.orientation = .vertical
        identity.alignment = .leading; identity.spacing = 4
        channelName.widthAnchor.constraint(equalTo: identity.widthAnchor).isActive = true
        let channelHeader = NSStackView(views: [channelBadge, identity]); channelHeader.spacing = 10
        func controlRow(_ caption: NSTextField, _ slider: NSSlider, _ value: NSTextField) -> NSView {
            caption.font = .systemFont(ofSize: 10, weight: .medium)
            caption.widthAnchor.constraint(equalToConstant: 58).isActive = true
            value.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular)
            value.textColor = DAWDesignTokens.Color.text; value.alignment = .right
            value.widthAnchor.constraint(equalToConstant: 57).isActive = true
            let row = NSStackView(views: [caption, slider, value]); row.spacing = 5
            row.heightAnchor.constraint(equalToConstant: 26).isActive = true
            return row
        }
        let children: [NSView] = [channelHeader, controlRow(volumeCaption, volume, volumeValue),
                                   controlRow(panCaption, pan, panValue), flags, signalChain, devices,
                                   midiCaptureForm, clipForm, validation]
        for child in children { form.addArrangedSubview(child); child.widthAnchor.constraint(equalTo: form.widthAnchor).isActive = true }
        midiEditor.onClipSelect = { [weak self] in self?.onMidiClipSelect?($0) }
        midiEditor.onNotesChange = { [weak self] in self?.onMidiNotesChange?($0) }
        midiEditor.onCommitRequest = { [weak self] in self?.onMidiCommitRequest?($0) }
        midiEditor.onAddNote = { [weak self] in self?.onMidiAddNote?() }
        midiEditor.onAddClip = { [weak self] in self?.onMidiAddClip?() }
        midiEditor.onRemoveClip = { [weak self] in self?.onMidiRemoveClip?($0) }
        midiEditor.onRemoveNote = { [weak self] in self?.onMidiRemoveNote?($0) }
        reloadInspector(); applyMidi()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    private func caption(_ text: String) -> NSTextField {
        let label = NSTextField(labelWithString: text); label.font = .systemFont(ofSize: 10, weight: .semibold)
        label.textColor = DAWDesignTokens.Color.secondaryText; return label
    }
    private func reloadInspector() {
        let model = channel ?? InspectorChannelModel()
        if channelName.currentEditor() == nil { channelName.stringValue = model.title }
        selection.stringValue = channel == nil ? "Выберите дорожку, шину или мастер." : model.kind
        channelIcon.image = NSImage(systemSymbolName: model.kind == "MASTER" ? "speaker.wave.2.fill" :
            (model.kind == "BUS" ? "arrow.triangle.branch" : (midi?.clips.isEmpty == false ? "pianokeys" : "waveform")),
            accessibilityDescription: model.kind)
        channelIcon.contentTintColor = model.accent
        channelBadge.layer?.backgroundColor = model.accent.withAlphaComponent(0.13).cgColor
        channelBadge.layer?.borderColor = model.accent.withAlphaComponent(0.25).cgColor
        volume.doubleValue = model.volumeDb; pan.doubleValue = model.pan
        volumeCaption.stringValue = "Громкость"
        volumeValue.stringValue = String(format: "%+.1f dB", model.volumeDb)
        panCaption.stringValue = "Панорама"
        panValue.stringValue = abs(model.pan) < 0.001 ? "C" : String(format: "%.0f %@", abs(model.pan * 100), model.pan < 0 ? "L" : "R")
        mute.state = model.muted ? .on : .off; solo.state = model.solo ? .on : .off
        volume.isEnabled = channel != nil && editingEnabled
        pan.isEnabled = channel != nil && model.kind != "MASTER" && editingEnabled
        channelName.isEnabled = channel != nil && model.renameable && editingEnabled
        mute.isEnabled = channel != nil && model.kind != "MASTER" && editingEnabled
        solo.isEnabled = channel != nil && model.kind.hasPrefix("TRACK") && editingEnabled
        clipForm.isHidden = clip == nil
        if let clip {
            for (index, value) in [clip.startFrames, clip.sourceOffsetFrames, clip.lengthFrames, clip.fadeInFrames, clip.fadeOutFrames].enumerated() {
                if fields[index].currentEditor() == nil { fields[index].stringValue = String(format: "%.3f", Double(value) / 48000) }
                fields[index].isEnabled = editingEnabled
            }
        }
    }
    private func applyMidi() {
        var model = midi
        model?.editable = editingEnabled && midi?.editable == true
        midiEditor.apply(model: model)
        applyMidiCapture()
        reloadInspector()
    }
    private func applyMidiCapture() {
        guard let capture = midiCapture, let midi, !midi.clips.isEmpty else { midiCaptureForm.isHidden = true; return }
        midiCaptureForm.isHidden = false
        let ids = [UInt32(0)] + capture.inputs.map(\.id)
        let titles = ["Нет"] + capture.inputs.map(\.title)
        if ids != midiInputIDs || titles != midiInputTitles {
            midiInputIDs = ids; midiInputTitles = titles
            midiInputPopup.removeAllItems(); midiInputPopup.addItems(withTitles: titles)
        }
        midiInputPopup.selectItem(at: ids.firstIndex(of: capture.selectedID) ?? 0)
        midiRecordButton.title = capture.armed ? "■ Закончить MIDI-запись" : "● Запись с клавиатуры"
        midiCaptureStatus.stringValue = capture.statusLine
    }
    @objc private func rename() { guard channelName.isEnabled else { return }; onChannelRename?(channelName.stringValue) }
    @objc func changeChannel() { guard editingEnabled, channel != nil else { return }; onChannelChange?(volume.doubleValue, pan.doubleValue) }
    @objc private func changeMute() { guard mute.isEnabled, editingEnabled else { return }; onChannelMute?(mute.state == .on) }
    @objc private func changeSolo() { guard solo.isEnabled, editingEnabled else { return }; onChannelSolo?(solo.state == .on) }
    @objc private func showDevices() { onShowDevices?() }
    @objc private func selectMidiInput() {
        guard midiInputIDs.indices.contains(midiInputPopup.indexOfSelectedItem) else { return }
        onMidiInputSelect?(midiInputIDs[midiInputPopup.indexOfSelectedItem])
    }
    @objc private func toggleMidiRecording() { onMidiRecordToggle?() }
    @objc private func changeClip() { _ = commitClipValues(fields.map(\.stringValue)) }
    @discardableResult func commitClipValues(_ text: [String]) -> Bool {
        guard editingEnabled, var clip, text.count == 5 else { return false }
        let values = text.compactMap { Double($0.replacingOccurrences(of: ",", with: ".")) }
        guard values.count == 5, values.allSatisfy({ $0.isFinite && $0 >= 0 && $0 < Double(1 << 40) / 48000 }), values[2] > 0 else {
            validation.stringValue = "Нужны конечные неотрицательные значения и ненулевая длительность."
            validation.isHidden = false; reloadInspector(); return false
        }
        let frames = values.map { UInt64(($0 * 48000).rounded()) }
        guard frames[2] > 0 else { return false }
        clip.startFrames = frames[0]; clip.sourceOffsetFrames = frames[1]; clip.lengthFrames = frames[2]
        clip.fadeInFrames = min(frames[3], frames[2]); clip.fadeOutFrames = min(frames[4], frames[2] - clip.fadeInFrames)
        validation.isHidden = true; onClipChange?(clip); return true
    }
}
