# Professional Mixer Console — integrated implementation

Updated 17 September 2026. This console extends the existing AppKit/Session/C ABI/renderer.
It does not introduce a second routing graph or change the project file format.
See [mixer semantics](30-mixer.md), [routing](35-routing-buses-sends.md),
[automation/PDC](38-volume-automation-pdc.md), and [multi-target automation](39-multi-automation-workflow.md).

## Implemented path

`MixerWorkspaceView` owns search/type filters, three strip widths, selected-channel
inspector, horizontally scrolling channels, a pinned Master, focus mode and the
explicit Sends on Faders destination. `MixerStripView` exposes exact numeric dB
entry, native slider tracking, stereo balance, arm/mute/solo, output routing,
insert add/edit/bypass/reorder/remove, and send add/edit/PRE/POST/remove. Channel
menus keep processing accessible when the docked console is too short for racks.
Inserts and sends carry stable plug-in/bus IDs, never names or display indices.
The existing parameter editor and plug-in catalog remain the implementation.

A selected send replaces only the eligible track fader. Tracks without that send
are disabled; buses and Master retain their main faders. Main balance is disabled
in send mode because independent send pan is not supported. Search also matches
output names, so searching a bus exposes its feeding tracks. Meters update existing
strip instances without rebuilding controls; the selected inspector receives the
same meter and fader previews. Clip reset is UI-only. The meters show logarithmic
**sample peak**, not true peak; Master adds existing momentary/short-term LUFS,
not integrated LUFS. Native AppKit handles focus, menus, text entry and tracking.

## Gesture and realtime contract

`beginMixerGesture` copies the authoritative state once; subsequent writes change
only the private scalar preview. Reads/save still see committed state. End commits
at most one revision/Undo entry; no-op and cancel create none. Invalid/non-finite
values, stale end, conflicting revisioned edits and nested automation are rejected.
A failed end remains cancelable. Recording and transport preparation cannot start
in a static gesture. UI automation-armed faders use the existing automation gesture
instead; this does not implement new live automation audition or new write modes.
Option-Solo and Clear Solo now use one atomic Session command, not a stale-revision
loop of independent commands.

A gain-only change to an existing send preserves transport and PRE/POST routing.
The renderer has prepared stable-ID/atomic target slots and per-send smoothed gains;
only the control thread looks up domain objects. The audio callback has no new
allocation, locking, file I/O or mutable domain traversal. Structural route/tap
changes still rebuild transport. Bus deletion now invalidates the running graph.

## Verification

`mixer_gestures` covers all six static targets, preview isolation, failed/nested
commands, one-step Undo/Redo, no-op/cancel, C ABI save/open, exclusive solo and a
live prepared-renderer send edit with smoothing and mute gate. Existing routing,
Session/storage, automation, history and plug-in regression tests remain enabled.
Linux Clang Debug build succeeded; six focused tests passed under ASan/UBSan.
The earlier full Linux run had two import failures: the pre-existing non-macOS
resampler stub rejects 44.1 kHz conversion required by those fixtures. These tests
were not skipped or weakened; no full-green Linux claim is made.

`bash scripts/test-mixer-ui.sh` builds the actual AppKit views and exercises search,
filters, pinned Master, send mapping, disabled editing, retained view identities,
clip reset, multiple sizes and a 256-strip presentation. It writes
`build/mixer-ui.png` from AppKit offscreen rendering. This test and full macOS app
build must be run on macOS; local Swift syntax parsing is not typechecking.

## Native validation recorded — 16 September 2026

Verified source: `2616c534b377e5882daf154e93ed12cf9652cd18`.
The [native run](https://github.com/ibotpafos/my-daw/actions/runs/35114763374)
finished successfully in both `appkit` and `core-and-app` jobs:

- 32/32 macOS core tests passed with ASan/UBSan, including the new mixer gesture
  test, routing, automation, Session/storage, plug-ins and Undo/Redo.
- Actual AppKit controls compiled and their tests passed: insert/send click action
  IDs, search/type filtering, send fader mapping, clip latch/reset, retained views,
  three window sizes and 256-strip presentation. An offscreen PNG was produced
  and visually inspected. Its channels, insert names and levels are test fixtures,
  not a recording or evidence of real-device audio/plugin processing.
- The complete arm64 `My DAW.app` built and passed strict code-signature verification.
  Bundle version is 1.73.0, build commit `2616c53`, minimum macOS 14.0.
  The build used Apple Swift 6.1.2 / SDK 15.5 on macOS 15.7.9.
- The delivered candidate is **ad-hoc signed, not notarized**. This CI build did
  not bootstrap the optional VST3 SDK, so it contains the VST3 fallback, not the
  optional VST3 runtime helpers. The normal developer build still packages/signs
  those helpers when the pinned SDK is present.
- Native documentation links, script hygiene, version and SQL checks passed.
  Full JSON Schema validation was skipped there because `jsonschema` was absent;
  local full schema validation passed separately. The unrelated generic Ubuntu
  workflow still fails at build; the dedicated native success is not an all-platform
  green-CI claim.

These checks distinguish a built complete application, tested native components,
and tested core audio from an interactive full-app/device listening session.

## Using the candidate

Open the **Сведение** workspace and use **Focus** to expand the console. The Master
stays visible when the channel bank scrolls. Click a channel title for selection;
use its context menu for rename, processing menus, unity reset and bus deletion.
Choose **Faders: Main** or **Send → destination** in the toolbar. Missing-send tracks
become non-editable instead of silently changing their main level. Click an insert
for the existing parameter editor; its context menu exposes bypass/reorder/remove.
Click a send for exact dB entry and use its context menu for PRE/POST or removal.
The numeric fader field accepts decimal comma; double-clicking the fader resets
it to 0 dB. Structural routing/tap changes still stop/rebuild transport by design;
only existing-send gain changes use the non-stopping live-target path.

## Routing and large-session UI — 17 September 2026

The console also has a sample-peak overview, visibility controls, up to three
left/right pinned channels, and Full/Inserts/Sends/Faders section focus. These
are presentation settings, not audio groups or VCA controls. The scrolling bank
culls offscreen strips from compositing but retains their control instances.
The Undo/Redo buttons use the existing Session history, not a second mixer history.

Open **Микшер → Матрица маршрутизации…** (**Command-Option-R**). The matrix uses
native view-based `NSTableView` cells, a fixed channel-name table, synchronized
vertical scrolling and fixed destination headers. Destination columns resize;
their widths and the focused row/destination IDs survive bus renaming/reordering.
Search matches channel and output names. The two modes remain distinct:

- **Main outputs:** choose one main destination per track/bus. Re-selecting the
  existing route is a no-op. Self-routing, indirect feedback, and broken/cyclic
  destination chains are disabled with a reason. This is UI preflight only:
  Session remains the final authority on every mutation.
- **Sends:** clicking an empty cell adds a post-fader send at -12 dB. Clicking an
  existing send opens its exact level editor; it no longer deletes that send.
  The context menu exposes PRE/POST and explicit removal. The eight-send add
  limit does not prevent editing/removing an existing send.

With a routing table focused, arrow keys navigate; Return/Space activates the
focused cell, and Delete/Forward Delete removes only an existing send. Command-F
focuses search. Native cells expose source/destination, value, help and enabled
state to accessibility. A main connection is announced separately from a send
level even when both target the same bus. Full VoiceOver interaction remains a
manual acceptance task, not implied by metadata assertions.

The non-modal window refreshes UI snapshots at 4 Hz while visible and not
minimized. Identical snapshots do not rebuild tables. Each edit also refreshes
synchronously and checks recording/active-gesture locks before emitting a callback.
Actions from menus/cells opened against a changed snapshot are rejected rather
than replayed on stale routing data. Closing the window releases its timer and
providers; reopening an existing window preserves its geometry instead of
recentering it. The providers read UI-owned snapshots on the main thread, never
the audio callback. This iteration changes no engine, C ABI, storage format or
real-time processing code. Structural routing/tap changes retain the existing
stop/rebuild behavior.

`tests/mixer_routing_tests.swift` is compiled into the existing AppKit harness.
It exercises native cell clicks, synthesized key events, menu actions, stale
snapshots, dynamic read-only guards, direct/indirect feedback, missing destinations,
capacity limits, search/empty states, stable focus, column widths, accessibility
metadata and presenter close/reopen cleanup. A 256-track by 16-destination fixture
checks that the table does not instantiate all 4096 logical buttons at once.
This is a UI allocation/compositing check, not a DSP throughput benchmark.

Visual inspection caught an initially invisible channel-name column despite the
first action tests passing. Its document/cell geometry is now explicit and covered
by visible-frame assertions. The harness emits `build/mixer-routing-ui.png` and
`build/mixer-ui.png` from real AppKit offscreen rendering with synthetic fixtures.
The read-only native workflow retains both PNGs, logs and a tracked-source archive.

## Latest native validation — 17 September 2026

Verified code: `5ed6eacc6a1aaf744a3f1a4e9dbb2d9e9043a161`.
[Native run 35225797150](https://github.com/ibotpafos/my-daw/actions/runs/35225797150)
completed successfully in both jobs. A following documentation-only commit does
not change the source used for this candidate.

- **32/32 core tests PASS** with ASan/UBSan, total 62.88 seconds. This includes
  mixer gestures, routing, automation, project persistence and Undo/Redo.
- **AppKit harness PASS**, including the routing tests above and all pre-existing
  mixer checks. The 4096-cell fixture instantiated **240 buttons** at the tested
  viewport, not all 4096. Both offscreen PNGs were downloaded and visually checked.
  This validates component rendering and event dispatch, not physical playback.
- **Complete arm64 application PASS**, including the existing strict codesign
  verification. Bundle version is 1.73.0, `DAWBuildCommit=5ed6eac`, minimum macOS
  14.0. Manifest: Apple Swift 6.1.2, SDK 15.5, macOS runner 15.7.9.
- The candidate is **ad-hoc signed, not notarized**. The optional VST3 SDK was not
  bootstrapped: this artifact has the AU scan helper and VST3 fallback, not VST3
  runtime helpers. Normal developer packaging remains unchanged.
- Native documentation, SQL, version and script checks passed. Full JSON Schema
  validation was skipped on the runner because `jsonschema` was absent. Separately,
  local `python3 scripts/check_docs.py --require-schemas` passed all three schemas,
  three examples, ten negative schema cases and the remaining documentation checks.
- The generic Ubuntu workflow still fails on existing GCC misleading-indentation
  errors in `engine/domain/session.cpp`. No all-platform green-CI claim is made;
  tests and warning gates were not disabled.

Run artifacts are `mixer-appkit-5ed6eacc...` (PNGs, harness log, source archive)
and `mixer-core-app-5ed6eacc...` (application ZIP, CTest/build/docs logs, manifest),
with seven-day retention. SHA-256 of the inner `My-DAW-app.zip`:
`2a41faf8cd1c85662b1b49b43baed373f86e3643ca9fa6f0e7ac9bace6478632`.

## Remaining acceptance / explicit limitations

Physical device listening, third-party plug-in GUI compatibility, audible automation
writing/PDC acceptance, accessibility navigation and interactive visual acceptance
remain separate. A synthetic 256-strip view test is not a 256-track audio benchmark.
The engine still lacks bus sends/solo, VCA, snapshots and independent send pan;
these are not simulated by disabled or mislabeled UI. Track pan is the documented
stereo balance, not an equal-power mono panner. An integrated console is not a claim
of parity with every mixer feature in mature DAWs.

Workflow references consulted: [Logic Sends on Faders](https://support.apple.com/en-gb/101897)
and the [AppKit slider SDK](https://developer.apple.com/documentation/appkit/nsslider).
