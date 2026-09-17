import AppKit

extension DraftApp {
    func refreshBrowserCatalog() {
        var targets: [UUID: BrowserPluginTarget] = [:]
        var items: [InspectorBrowserItem] = []
        for plugin in auCatalog {
            let instrument = plugin.type == 0x61756D75 // kAudioUnitType_MusicDevice
            let item = InspectorBrowserItem(title: plugin.name,
                detail: instrument ? "Audio Unit · инструмент" : "Audio Unit · эффект",
                available: true, category: instrument ? .instruments : .effects,
                pluginResourceKey: LibraryResourceKey.audioUnit(type: plugin.type, subtype: plugin.subtype, manufacturer: plugin.manufacturer),
                pluginFormat: .au)
            targets[item.id] = .audioUnit(type: plugin.type, subtype: plugin.subtype, manufacturer: plugin.manufacturer)
            items.append(item)
        }
        for plugin in vst3Catalog {
            // This reads the scanner's cached descriptor, not an instantiated
            // processor. Indices/names cannot identify an insert after a rescan.
            var descriptor = daw_vst3_component()
            descriptor.struct_size = UInt32(MemoryLayout<daw_vst3_component>.size)
            let read = daw_get_installed_vst3(session, plugin.index, &descriptor) == 0
            var pathBuffer = [CChar](repeating: 0, count: Int(DAW_MACOS_VST3_PATH_CAPACITY))
            let copied = read && pathBuffer.withUnsafeMutableBufferPointer { buffer in
                daw_macos_copy_vst3_path(&descriptor, buffer.baseAddress, buffer.count) == 0
            }
            let path = pathBuffer.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
            let classID = withUnsafeBytes(of: descriptor.class_id) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
            let item = InspectorBrowserItem(title: plugin.name,
                detail: "VST3" + (plugin.vendor.isEmpty ? "" : " · " + plugin.vendor),
                available: plugin.available && read, category: plugin.instrument ? .instruments : .effects,
                pluginResourceKey: copied ? LibraryResourceKey.vst3(path: path, classID: classID) : nil,
                pluginFormat: .vst3)
            if read { targets[item.id] = .vst3(index: plugin.index) }
            items.append(item)
        }
        // Publish dispatch targets before selection callbacks from reload.
        browserPluginTargets = targets
        libraryBrowser.pluginItems = items.sorted { $0.title.localizedStandardCompare($1.title) == .orderedAscending }
    }
}
