# Professional Mixer Console — integrated implementation

16 September 2026. This console extends the existing AppKit/Session/C ABI/renderer.
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
