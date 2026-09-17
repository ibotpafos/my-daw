import Foundation

/// Local preference only; never stored in a project, uploaded, or used as a
/// fallback path when bookmark resolution fails.
struct LibraryFolderBookmark: Codable, Sendable {
    var version = 1
    let data: Data
    let scoped: Bool
    let title: String
    static let key = "workspace.libraryFolder.v1"
    static let maximumBytes = 128 * 1024

    var encoded: Data? { try? JSONEncoder().encode(self) }
    static func decode(_ data: Data?) -> Self? {
        guard let data, data.count <= maximumBytes * 2,
              let value = try? JSONDecoder().decode(Self.self, from: data), value.version == 1,
              !value.data.isEmpty, value.data.count <= maximumBytes,
              value.title.utf8.count <= 4096, !value.title.contains("\0") else { return nil }
        return value
    }
}

/// Immutable shared lease. The scan task and the published catalog retain it;
/// its one successful scope acquisition is balanced exactly once on release.
/// No mutable state crosses actors; Foundation URL scope calls are thread-safe.
final class LibraryFolderAccess: Sendable {
    let url: URL
    let bookmark: LibraryFolderBookmark
    private let started: Bool

    init(picked url: URL) throws {
        guard url.isFileURL else { throw CocoaError(.fileReadUnsupportedScheme) }
        let started = url.startAccessingSecurityScopedResource()
        do {
            let scoped = try? url.bookmarkData(options: [.withSecurityScope, .securityScopeAllowOnlyReadAccess],
                includingResourceValuesForKeys: nil, relativeTo: nil)
            // The current development bundle is not sandboxed. An ordinary
            // bookmark locates a resource but is NOT a permanent access grant.
            let data = try scoped ?? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
            guard !data.isEmpty, data.count <= LibraryFolderBookmark.maximumBytes else { throw CocoaError(.fileReadTooLarge) }
            self.url = url; self.started = started
            bookmark = .init(data: data, scoped: scoped != nil, title: String(url.lastPathComponent.prefix(512)))
        } catch {
            if started { url.stopAccessingSecurityScopedResource() }
            throw error
        }
    }

    init(remembered bookmark: LibraryFolderBookmark) throws {
        guard LibraryFolderBookmark.decode(bookmark.encoded) != nil else { throw CocoaError(.fileReadCorruptFile) }
        var stale = false
        var options: URL.BookmarkResolutionOptions = [.withoutUI, .withoutMounting, .withoutImplicitStartAccessing]
        if bookmark.scoped { options.insert(.withSecurityScope) }
        let resolved = try URL(resolvingBookmarkData: bookmark.data, options: options,
            relativeTo: nil, bookmarkDataIsStale: &stale)
        guard resolved.isFileURL else { throw CocoaError(.fileReadUnsupportedScheme) }
        let started = resolved.startAccessingSecurityScopedResource()
        do {
            var renewed = bookmark
            if stale {
                let creation: URL.BookmarkCreationOptions = bookmark.scoped ? [.withSecurityScope, .securityScopeAllowOnlyReadAccess] : []
                let data = try resolved.bookmarkData(options: creation, includingResourceValuesForKeys: nil, relativeTo: nil)
                guard data.count <= LibraryFolderBookmark.maximumBytes else { throw CocoaError(.fileReadTooLarge) }
                renewed = .init(data: data, scoped: bookmark.scoped, title: String(resolved.lastPathComponent.prefix(512)))
            }
            self.url = resolved; self.started = started; self.bookmark = renewed
        } catch {
            if started { resolved.stopAccessingSecurityScopedResource() }
            throw error
        }
    }
    deinit { if started { url.stopAccessingSecurityScopedResource() } }
}
