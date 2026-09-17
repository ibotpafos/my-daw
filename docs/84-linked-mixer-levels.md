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
without losing selection. A normal arranger/inspector selection replaces the
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
