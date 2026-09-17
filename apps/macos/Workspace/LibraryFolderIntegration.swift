import AppKit

extension DraftApp {
    func wireLibraryFolder() {
        let folder = libraryBrowser.folder
        folder.onPublish = { [weak self] urls in
            guard let self else { return }
            self.stopBrowserAudioPreview()
            let items = urls.map { url in
                InspectorBrowserItem(title: url.deletingPathExtension().lastPathComponent,
                    detail: url.deletingLastPathComponent().lastPathComponent, sourceURL: url)
            }
            // Install the matching targets before reload/selection callbacks.
            self.browserAudioURLs = Dictionary(uniqueKeysWithValues: items.compactMap { item in
                item.sourceURL.map { (item.id, $0) }
            })
            self.libraryBrowser.audioItems = items
        }
#if !DAW_WORKSPACE_TESTS
        folder.restore()
#endif
    }
}
