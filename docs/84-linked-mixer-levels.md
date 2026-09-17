# Linked static track levels

17 September 2026. Extends the existing Session mixer gesture; no new audio graph,
storage version, dependency, VCA object or separate Undo history is introduced.
For the preceding send controls and draft v22 compatibility, see
[send mute and independent balance](83-send-controls.md).

## Interaction

Command-click a track header to toggle selection. Shift-click selects a range in
visible console order; Command-Shift adds the range. A normal header click selects
one track. Selecting a bus or Master clears this temporary track selection.
The selection bar appears only for multiple tracks and exposes **Link levels**
and **Clear selection**. Link levels is enabled initially and can be turned off
without losing selection. A changed arranger/inspector selection replaces the
console selection. Filtering/hiding a track removes it from the temporary
selection; scrolling it offscreen does not. These choices prevent invisible
channels from changing through an old hidden selection.

With Link levels enabled, moving any selected track's main fader moves the selected
static levels by one common dB offset. Numeric entry, double-click unity, keyboard
and accessibility changes take the same path. At a travel boundary the entire group
stops, preserving relative levels instead of flattening the quietest/loudest member.
For example, -6 dB and +22 dB can rise together by only 2 dB. Requesting unity on the
first then produces -4 dB and +24 dB; the visible source fader reflects the actual
clamped level. Moving back uses the original gesture baseline, not cumulative
floating-point increments. The selected inspector mirrors the same previews.

The group membership and source are frozen for a gesture. Selection changes, link
toggling and switching Sends on Faders are refused until the level gesture ends.
Cancel restores the preview without an Undo entry. An unavailable/invalid group
never falls back to editing just the source track.

Sends on Faders stays **individual**: main-level selection cannot change another
track's sends. Buses, Master, pan, mute, solo and record arm are not linked. No group
membership is persisted: the resulting ordinary track gains are saved normally.
This is quick linking of static levels, **not VCA or automation grouping**.
Tracks with existing volume automation are rejected as a whole by Session; armed
volume-writing targets are rejected by the UI binding before beginning. Automation
is never erased, rewritten or silently overridden by a static group edit.

## Domain and C ABI

`Session::beginTrackGainGroup` resolves 2..256 unique existing track IDs, captures
one private State and original gains, and computes the common legal delta interval.
The original array is copied; bus/Master IDs, duplicates, missing tracks, wrong
revisions, non-finite writes and nested/conflicting edits are rejected. No partially
validated group becomes active. It uses the same mixer-gesture lock as static
single-fader gestures and is mutually exclusive with automation writes/recording.

The additive `daw_begin_track_gain_group` entrypoint retains all old ABI layouts.
After group begin, `daw_write_mixer_gesture` accepts an absolute delta from the start
of the gesture, not an incremental change and not an absolute track level. The old
end/cancel APIs finish the group. Preview is absent from committed snapshots and
save/export inputs; a changed end produces one revision and one Undo entry. A no-op
or cancel preserves both history and the redo branch. A failed end remains cancelable.

Only the main/control thread reads or writes the private preview. The existing
`Renderer::updateMix` publishes the resulting targets to an already prepared graph;
its existing audio-thread smoothers apply them without restarting the playhead.
There is no new work, allocation, lock or domain traversal in the audio callback.
This does not promise sample-atomic publication of every channel target: the existing
per-channel atomic target mechanism is unchanged.

## Tests

`mixer_groups` checks validation, IDs, stale/nested commands, preview/save isolation,
common bounds, reversing without drift, one-step Undo/Redo, preserved redo on no-op
and cancel, committed save/open, automation rejection, uninterrupted prepared-renderer
position/smoothing/amplitude, and a full 256-track selection.

`MixerLinkedLevelsTests` exercises native strip modifier events, range semantics,
plain header clicks, primary/secondary selection, previews during model refresh,
cancel/rejection with no single-track fallback, individual sends, pruning hidden
members, resize and a real offscreen AppKit PNG. `scripts/test-mixer-groups.sh`
recompiles the native harness with the **same MixerGroupBinding used by the app**
and the real C ABI. It checks the full control → binding → Session → UI-refresh
path, including keyboard edits, collective limits, cancellation and automation locks.
Run it after `scripts/build-macos.sh`; the lightweight `test-mixer-ui.sh` remains
independent of the C++ core.

Native build and test results must be tied to the published revision. A screenshot
fixture and a synthetic prepared renderer do not substitute for device listening,
third-party plug-in compatibility or manual accessibility acceptance.

Design reference: [Logic channel-strip groups](https://support.apple.com/en-euro/guide/logicpro/lgcp8e7ab0b8/10.7/mac/11.0)
distinguish linked parameter relationships from bus routing and VCA control. This
implementation intentionally covers only the static-level subset described above.


## Verified native candidate — 17 September 2026

Code revision: `322342668384fe48a4167288a792eed442adf662`.
[Read-only native run 35239753182](https://github.com/ibotpafos/my-daw/actions/runs/35239753182)
completed successfully in both jobs:

- **35/35 macOS core tests PASS with ASan/UBSan**, total 72.98 seconds. The new
  group test passed in 3.83 seconds. Existing send controls, routing, automation,
  save/open, export and history tests remain enabled and passed.
- **AppKit component harness PASS** for ordinary mixer, routing, sends and linked
  selection. The group fixture covers modifier/range selection, source/peer
  previews, model refresh, rejection/cancel, common travel limits and resizing.
- **Native controls + production MixerGroupBinding + real C ABI PASS**. This is
  the same adapter compiled into the application, not a test-only gain model.
  The test checks committed Session values and revisions after drag-like sequences,
  an actual native keyboard event, one-step Undo, cancellation, common limits and
  automation/recording-policy rejection. It is not a hardware recording test.
- **Full arm64 My DAW.app build and strict codesign verification PASS**. Bundle
  version 1.73.0, build commit `3223426`, minimum macOS 14.0. Manifest records
  Apple Swift 6.1.2, SDK 15.5 and macOS runner 15.7.9.
- The four offscreen AppKit images were retained. The group and send fixtures were
  visually inspected; levels/channels are synthetic. The downloaded tracked-source
  archive matches the local code byte for byte and has no staging-patch remnants.
- Local Clang ASan/UBSan group regression also passed (5.93 seconds). A broader
  local Linux selection timed out in `session_storage_bridge`; that is not reported
  as a full Linux pass. The separate generic Ubuntu workflow remains outside the
  green native gate and still has the existing GCC warning/build problem.

Roundoff-only group changes now compare actual resulting gains against captured
values before deciding to commit: a nonzero delta that rounds away does not create
an empty Undo entry or discard Redo. This is covered by the `1e-300` regression.

Artifacts: `mixer-appkit-322342668...` (four PNGs, AppKit log, tracked source) and
`mixer-core-app-322342668...` (application ZIP, build/CTest/C-ABI logs, manifest).
The inner app ZIP has SHA-256
`445cb2a6dfa722ee21b94d68aab41a9bc17ca8da89c1a1e23e07a8abbbfd94b0`.
This candidate is ad-hoc signed, not notarized. Its optional VST3 SDK is not linked;
it packages the AU scan helper and VST3 fallback, not VST3 runtime helpers.
The preceding send controls already require draft v22; **use project copies**,
because pre-v22 application builds cannot reopen newly saved drafts. Linked static
levels themselves introduce no further storage or ABI-layout change.

The temporary source-publishing workflow has been removed. Ongoing validation is
read-only and now includes the real C-ABI integration test. Native documentation
checks passed, with JSON Schema validation skipped on that runner because the
module was absent. Full schema validation was run separately in the local container.
Physical device listening, full interactive app/VoiceOver acceptance, VCA, durable
mix snapshots and group automation remain separate, unclaimed work.
