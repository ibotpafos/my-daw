# Professional Mixer Console

Updated 17 September 2026. This console extends the existing AppKit/Session/C ABI/
renderer. There is no second audio graph, plug-in catalog or Undo history.
See [mixer semantics](30-mixer.md), [routing](35-routing-buses-sends.md),
[automation/PDC](38-volume-automation-pdc.md), [automation writing](39-multi-automation-workflow.md),
[send mute and independent balance](83-send-controls.md), and
[linked static track levels](84-linked-mixer-levels.md).

## Current feature boundary

`MixerWorkspaceView` presents the selected-channel inspector, a horizontal channel
bank and pinned Master, search by channel/output, type filters, three strip widths,
Focus mode, visibility controls, left/right pin zones, an overview/meter bridge,
and Full/Inserts/Sends/Faders section focus. Offscreen strips are culled from
compositing but retain their control instances. This is presentation optimization,
not evidence of real-time performance for 256 simultaneous audio tracks.

`MixerStripView` connects native faders, exact dB entry, stereo balance, arm/mute/solo,
output routing, insert add/edit/bypass/reorder/remove, and send add/edit/PRE/POST/
remove to existing commands. Actions carry stable track/bus/plug-in IDs, never
names or displayed positions. Channel menus retain processing access at short
console heights. The existing parameter editor and plug-in catalog remain in use.

Meters show logarithmic stereo **sample peak**, with a clip latch/reset. Master
also reports existing momentary/short-term LUFS. These are not live true-peak or
integrated-loudness meters. Polling updates retained meter views rather than
rebuilding the complete channel strip. The selected inspector mirrors previews.

### Sends on Faders

Choose **Faders: Main** or **Send → destination** in the toolbar. A selected send
replaces only eligible track controls. Tracks without the send cannot accidentally
edit their main gain/mute/balance; buses and Master retain main faders. **SM** mutes
only the selected send while retaining its stored level. **LINK / IND** selects
legacy linked or independent send balance. IND has its own native balance slider
and one Undo per gesture. Main solo/arm remain separate.

Independent POST follows the track fader, including automation, but taps before
track balance; independent PRE taps before both. These controls use unity-center
stereo balance, not equal-power mono pan. Their full signal/history/compatibility
contract is [documented separately](83-send-controls.md).

### Multiple selected track faders

Command-click toggles track selection. Shift-click selects a visible range;
Command-Shift adds a range. A normal header click selects one track. Selecting a
bus/Master clears the temporary track selection. With multiple tracks, the bar
shows **Link levels** and **Clear selection**. Filtering/hiding removes members;
scrolling offscreen does not.

Linked main faders move all selected static track levels by a common dB delta.
At either travel limit the entire group stops, preserving relative gains. Numeric
entry, keyboard and accessibility increments use the same production binding and
Session gesture. An invalid/automated group never silently falls back to editing
only the source. Buses, Master, sends, pan, mute, solo and arm are not grouped.

This is temporary static-level linking, **not VCA, durable grouping or group
automation**. Tracks with volume automation or armed volume writing are rejected
as a whole. Resulting ordinary gains are saved; selection membership is UI-only.
See the [linked-level contract and tests](84-linked-mixer-levels.md).

## Routing matrix

Open **Микшер → Матрица маршрутизации…** (**Command-Option-R**). Native view-based
NSTableView cells reuse views for large sessions. Channel names stay fixed at the
left; destination headers stay fixed at the top. Vertical scrolling is synchronized.
Search matches channel/output names; column widths and focused row/destination IDs
survive bus renaming/reordering.

**Main outputs:** select one destination per track/bus. Selecting the existing
route is a no-op. UI preflight blocks self-routing, indirect feedback and malformed
chains; Session is the final validator for every mutation.

**Sends:** an empty cell adds a post-fader send at -12 dB. An existing send opens
its exact level editor, not deletion. Context menus provide PRE/POST, mute,
independent balance and explicit removal. The eight-send cap limits additions,
not edits/removals of existing sends.

With a table focused, arrows navigate, Return/Space activates, Delete/Forward
Delete removes a send, and Command-F focuses search. Native cells expose source,
destination, value, help and enabled state to accessibility. Metadata/event tests
do not substitute for manual VoiceOver acceptance.

The non-modal window refreshes UI snapshots at 4 Hz while visible and not minimized.
Identical snapshots do not rebuild tables. Each edit refreshes synchronously and
checks recording/active-gesture guards. Stale cell/menu actions reject rather than
applying to changed settings. Closing releases the timer/providers; reopening an
existing window preserves geometry. Providers read UI-owned snapshots on the main
thread, not the audio callback.

## Gesture, audio and persistence contract

Static mixer gestures capture one private State. High-rate preview writes do not
create history snapshots. Save/export/committed reads keep seeing committed state.
A changed end produces one revision/Undo; cancel/no-op produces none and preserves
Redo. Non-finite/out-of-range values, missing targets, stale revisions and conflicting
or nested edits reject. A failed end remains cancelable. Group no-ops compare actual
resulting gains, so floating-point roundoff does not create empty Undo entries.

Option-Solo/Clear Solo use a single atomic Session command. Automation-armed single
faders reuse existing automation gestures; no new automation mode or live automation
audition implementation is implied by this mixer work.

Scalar main/send targets are published to an already prepared renderer using the
existing atomic/smoothing path. Gain-only send edits and send mute/balance/mode do
not restart transport. Group static levels reuse Renderer::updateMix; there is no
new allocation/lock/file I/O/domain traversal in the audio callback, nor a claim
that multiple independent atomic target writes are sample-atomic together.
Structural output/PRE-POST changes and adding/removing routes still stop/rebuild
transport. Bus deletion invalidates the running graph.

Send controls introduced draft **v22**; old drafts v1–21 open with linked, unmuted,
centered send defaults. **Pre-v22 builds cannot open drafts saved by this candidate;
test using project copies.** Linked levels add no further format change. Native
save/recovery/package retain send controls. DAWproject export warns about omitted
independent send balance and muted sends rather than silently changing their
meaning. Details: [send persistence and interchange](83-send-controls.md).

## Current native evidence

Code: `322342668384fe48a4167288a792eed442adf662`.
[Read-only run 35239753182](https://github.com/ibotpafos/my-daw/actions/runs/35239753182)
finished successfully in both jobs:

- **35/35 core tests PASS with ASan/UBSan**, 72.98 seconds, including mixer groups,
  send controls, routing, automation, save/open, offline export and Undo/Redo.
- **Native AppKit component tests PASS**, including linked selection, relative
  previews, guards, resize, routing and send interactions. Four real offscreen
  AppKit PNGs are retained; synthetic channels/levels are not hardware audio evidence.
- **Native controls → production MixerGroupBinding → real C ABI PASS** for group
  preview, one-step commit/Undo, keyboard events, cancellation, collective limits,
  automation rejection and edit-policy guards. The adapter is the one used by the app.
- **Full arm64 My DAW.app build and strict codesign verification PASS**. Version
  1.73.0, build `3223426`, minimum macOS 14.0; Swift 6.1.2 / SDK 15.5 / runner 15.7.9.
- Native docs/SQL/version/script checks passed. JSON Schema validation was skipped
  there because jsonschema was absent; full schema checks ran separately locally.

The 256-track console test checks presentation/culling. The 256×16 routing fixture
creates 240 buttons in the tested viewport, not 4096. The group core test resolves
and edits 256 tracks in one revision. None is a 256-track real-time DSP benchmark.

Artifacts are mixer-appkit-322342668... (PNGs, log, source) and
mixer-core-app-322342668... (app ZIP, logs, manifest), retained seven days. The full
[group verification report](84-linked-mixer-levels.md#verified-native-candidate--17-september-2026)
records the application ZIP checksum and local-test limits. The downloaded source
matches the local checked code; the temporary publishing workflow and staged patches
are removed. Ongoing native validation is read-only.

Historical evidence remains available in the [initial console run](https://github.com/ibotpafos/my-daw/actions/runs/35114763374)
(2616c53, 32 tests), [routing run](https://github.com/ibotpafos/my-daw/actions/runs/35225797150)
(5ed6eac, 32 tests), and [send-control run](https://github.com/ibotpafos/my-daw/actions/runs/35234854227)
(b08655a). Earlier reports saying independent send balance was unsupported describe
those earlier code revisions, not the current branch.

## Remaining acceptance

The delivered build is **ad-hoc signed, not notarized**. This CI candidate did not
bootstrap optional VST3 SDK: it contains the AU scan helper and VST3 fallback,
not the optional VST3 runtime helpers. Existing developer packaging still builds
and signs those helpers when the pinned SDK is present.

The generic Ubuntu pipeline remains red on the existing GCC misleading-indentation
build issue. A local Linux subset timed out in session_storage_bridge; no full-green
Linux claim is made. Tests and compiler warning gates were not disabled.

Physical device listening, full interactive app/device acceptance, third-party
plug-in GUIs, audible automation/PDC acceptance and manual VoiceOver testing remain
open. The engine still lacks bus sends/solo, VCA, durable mix snapshots and group
automation. Stereo balance is not advertised as an equal-power mono panner. A built
and tested mixer branch is not parity with every feature of mature DAWs, and is
not a claim that the PR has been merged into the evolving main branch.
