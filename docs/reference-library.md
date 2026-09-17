# Reference workspace: library favorites and filters

## Scope

The left library keeps the existing import, audition and plug-in insertion paths.
This change adds All/Favorites, format filtering (WAV/AIFF or AU/VST3), an
availability filter, multi-word Unicode search and native row/context actions.
No new DSP, dependency, project schema, content pack or cloud service is added.

Favorites are bounded local `UserDefaults` preferences (256 resources, versioned
JSON, at most 1 MiB). Audio identity is the standardized file path; AU identity is
the component type/subtype/manufacturer; VST3 identity is the standardized module
path plus class FUID. Display names, versions and transient scanner row indices
are not identities. Same-named files/plug-ins must remain distinct.

Only resources present in the current catalog are shown. Missing/unscanned
favorites remain identifiers, not available dummy rows. A favorite grants no
filesystem permission, does not follow a moved file, and does not reopen a folder.
The existing explicit folder picker still chooses one catalog of at most 1,000
WAV/AIFF files, synchronously; multi-root/bookmark/background indexing is not
part of this change. Choosing that folder again restores matching favorites.

## Selection, commands and focus

Refresh rebinds selection by stable resource identity when scanner UUIDs change.
The host publishes the new UUID-to-dispatch map before the browser reloads.
Detached row/menu actions cannot act on a new row occupying the same index.
Current availability, recording/MIDI-capture and import-busy guards are checked
again on action, not only when a menu was created.

With focus in the resource table, Space starts/stops the existing audio audition,
Return adds the resource, and Home/End navigate the list. Key repeat cannot add
multiple tracks or toggle playback repeatedly. The window routes these before
global menu/transport keys. Delete and Command-Delete in the library cannot
remove arrangement clips or tracks. Search-field typing stays in AppKit's text
system. Project transport remains unchanged when focus is outside the library.

Filters/stars do not call the audio engine or consume a project revision/Undo.
Audition and add use existing controllers. Favorites can be organized during
recording, but new audition, insertion and scanning controls are disabled.

## Reuse and verification

AppKit owns table reuse, selection, scroll clamping, search input, SF Symbols,
context menus and keyboard events. Foundation owns Unicode folding and JSON
encoding. `LibraryCatalogModel` contains only resource identity/preference/search
rules; `LibraryCatalog` adapts the real scanner catalog; cell/menu code is separate
from the browser. No filesystem probing or plug-in instantiation occurs while
filtering/favoriting. VST3 identity reads the existing cached descriptor ABI.

Primary API references:
- [NSView contextual menus](https://developer.apple.com/documentation/appkit/nsview/menu(for:))
- [NSTableView reuse](https://developer.apple.com/documentation/appkit/nstableview/makeview(withidentifier:owner:))
- [UserDefaults](https://developer.apple.com/documentation/foundation/userdefaults)

`tests/library_catalog_tests.swift` covers stable IDs, duplicate names, corrupt
preferences, bounds and Unicode queries. `tests/workspace_library_tests.swift`
exercises native stars/menus, filters, selection replacement, capture guards,
keyboard routing, a real system-AU insertion/Undo and screenshots at two widths.
Audition callbacks in keyboard tests are spies, not a physical audio acceptance.
The native UI workflow runs both suites; existing Core/VST3 workflows remain.

The first native compile exposed the Swift importer's inability to expose the
4097-byte C `module_path` field. `apps/macos/DAWBridge.h` is a header-only copy
adapter using standard C `memchr`/`memcpy`, not a new exported engine ABI or hard-
coded layout offset. Production and UI tests import the same header; 12 C checks
cover empty/maximum/malformed paths, buffer bounds and descriptor size.
