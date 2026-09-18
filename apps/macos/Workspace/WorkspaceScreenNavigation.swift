import AppKit

/// Presentation actions are kept separate from transport, recording and project
/// commands. All destinations route to the production session and controls.
@MainActor
extension DraftApp {
    func configureWorkspaceScreenPicker() {
        workspaceMode.segmentCount = WorkspaceScreen.allCases.count
        for screen in WorkspaceScreen.allCases {
            workspaceMode.setLabel(screen.title, forSegment: screen.rawValue)
            workspaceMode.setToolTip("\(screen.accessibilityTitle) · ⌃⌘\(screen.rawValue + 1)", forSegment: screen.rawValue)
        }
        workspaceMode.selectedSegment = WorkspaceScreen.load(from: .standard).rawValue
        workspaceMode.target = self
        workspaceMode.action = #selector(changeWorkspaceScreen(_:))
        workspaceMode.setAccessibilityLabel("Рабочий экран")
        workspaceMode.setAccessibilityHelp("Проект, MIDI, микшер, эффекты, браузер или запись. Переключение не запускает запись и не изменяет проект.")
    }

    func wireWorkspaceScreenNavigation() {
        workspace?.mayNavigate = { [weak self] in
            guard let self else { return false }
            guard consoleGesture == nil, automationGesture == nil,
                  pluginParameterGesture == nil, !mixerWorkspace.linkedLevels.isEditing, !recordingWorkspace.isGesturing else {
                status.stringValue = "Заверши текущее изменение параметра перед переключением экрана."
                return false
            }
            guard window.makeFirstResponder(nil) else {
                status.stringValue = "Исправь или отмени ввод в текущем поле."
                return false
            }
            return true
        }
        workspace?.onFocusRequested = { [weak self] screen in
            guard let self else { return }
            switch screen {
            case .recording: _ = window.makeFirstResponder(recordingWorkspace.tracks)
            case .browser: _ = window.makeFirstResponder(libraryBrowser.search)
            case .mixer: _ = window.makeFirstResponder(mixerWorkspace.search)
            case .pianoRoll: _ = window.makeFirstResponder(inspectorBrowser.midiEditor)
            case .devices: _ = window.makeFirstResponder(channelRack)
            case .arrange:
                if let id = selectedMixerID, let wave = laneViews[id] {
                    _ = window.makeFirstResponder(wave)
                }
            }
        }
        workspaceDock?.onToggleFocus = { [weak self] in self?.toggleWorkspaceEditorFocus() }
        installWorkspaceScreenMenu()
    }

    private func installWorkspaceScreenMenu() {
        guard let main = NSApp.mainMenu else { return }
        let root = main.items.first(where: { $0.identifier?.rawValue == "workspace.screens" })
            ?? NSMenuItem(title: "Экраны", action: nil, keyEquivalent: "")
        root.identifier = NSUserInterfaceItemIdentifier("workspace.screens")
        let menu = NSMenu(title: "Экраны")
        // The normal Cocoa action validation stays enabled; selectors recheck the
        // navigation guard, including programmatic and stale menu invocations.
        for screen in WorkspaceScreen.allCases {
            let item = NSMenuItem(title: screen.accessibilityTitle,
                                  action: #selector(pickWorkspaceScreen(_:)),
                                  keyEquivalent: String(screen.rawValue + 1))
            item.tag = screen.rawValue; item.target = self
            item.keyEquivalentModifierMask = [.control, .command]
            menu.addItem(item)
        }
        menu.addItem(.separator())
        let expand = NSMenuItem(title: "Развернуть редактор / вернуться к проекту",
                                action: #selector(toggleWorkspaceEditorFocus), keyEquivalent: "0")
        expand.target = self; expand.keyEquivalentModifierMask = [.control, .command]
        menu.addItem(expand)
        root.submenu = menu
        if root.menu == nil { main.addItem(root) }
    }

    @objc func changeWorkspaceScreen(_ sender: NSSegmentedControl) {
        guard let screen = WorkspaceScreen(rawValue: sender.selectedSegment) else {
            updateWorkspaceChrome(); return
        }
        _ = workspace?.present(screen)
    }
    @objc func pickWorkspaceScreen(_ sender: NSMenuItem) {
        guard let screen = WorkspaceScreen(rawValue: sender.tag) else { return }
        _ = workspace?.present(screen)
    }
    @objc func toggleWorkspaceEditorFocus() {
        guard let workspace else { return }
        workspace.present(workspace.screen == .arrange ? WorkspaceScreen(dockTab: workspace.activeDockTab) : .arrange)
    }

    func updateWorkspaceScreenChrome() {
        guard let workspace else { return }
        workspaceMode.selectedSegment = workspace.screen.rawValue
        workspaceDock?.setExpanded(workspace.dockFocused)
        mixerWorkspace.setWorkspaceFocus(workspace.screen == .mixer)
        let geometry = workspace.geometry
        workspaceToggleButtons[.library]?.state = geometry.library > 0 ? .on : .off
        workspaceToggleButtons[.inspector]?.state = geometry.inspector > 0 ? .on : .off
        workspaceToggleButtons[.dock]?.state = geometry.dock > 0 ? .on : .off
        if let menu = NSApp.mainMenu?.items.first(where: { $0.identifier?.rawValue == "workspace.screens" })?.submenu {
            for item in menu.items where item.action == #selector(pickWorkspaceScreen(_:)) {
                item.state = item.tag == workspace.screen.rawValue ? .on : .off
            }
        }
    }
}
