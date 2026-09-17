# Send mute and independent stereo balance

17 September 2026. Extends the existing mixer, Session, C ABI, renderer and draft
format; no parallel routing graph or new plug-in dependency.

## Signal contract

A send retains its gain, PRE/POST tap, destination, mute, balance (-1…1) and an
independent-balance switch. **Linked is the legacy default**: PRE bypasses track
fader/balance, POST follows both. Independent PRE applies its own balance to the
pre-fader tap. Independent POST follows the track fader (including automation)
but takes its signal before track balance, so hard-panning the main channel does
not destroy the opposite side of the send. Both modes obey track mute/solo.

This is unity-center **stereo balance** (center = 1/1; hard left = 1/0), not an
equal-power mono panner or a stereo-width control. It cannot undo clip pan or
processing already applied inside inserts. Muting a send changes only its target
gain to zero; its route and stored fader value survive. Existing effect tails and
PDC delay buffers may decay after muting; mute does not reset a shared return.

Mute, gain, balance and mode changes publish atomic targets into a prepared graph.
Render-thread-only smoothers use the existing coefficient 0.004166667 at 48 kHz;
mode changes crossfade between linked and independent paths. No allocation, lock,
file I/O or domain traversal was added to the audio callback. PRE/POST tap changes
and adding/removing routes still use the existing structural stop/rebuild path.

## History, persistence and interop

`setSendMuted` and `setSendPan` validate first and commit one Undo/revision, or none
for unchanged values. Send-pan gestures use target 7 and require independent mode;
preview stays out of snapshots/saves until commit, and cancel/no-op uses no history.
Legacy gain/tap upserts preserve all newly added controls.

The additive `daw_send_controls` ABI reports version 1 and resolves track + bus IDs.
The old `daw_send` layout is unchanged. Invalid flags, non-finite/out-of-range pan,
missing sends, stale revisions and incompatible structure sizes reject without
mutation. Setters are unavailable during recording.

Draft **v22** adds `pan`, `muted`, `independent_pan` columns to `sends`. Readers open
v1–21 with linked, unmuted, centered defaults. Types, booleans and finite pan are
checked when reading. Native save/open, recovery and .mydawzip preserve the controls.
**Earlier app builds cannot open v22 projects**; use copies when testing.

WAV mixdown uses the same renderer and honors these controls. The current
DAWproject exporter does not yet represent independent send balance: it emits an
explicit `independent-send-pan-omitted` warning. Muted sends are omitted with a
`muted-send-omitted` warning rather than exported as unexpectedly audible routes.
The original native project retains all settings; this is an interchange limitation.

## UI

Select a bus in Sends on Faders. **SM** mutes only that send. **LINK / IND** switches
between the legacy tap and independent send balance. In IND mode the native pan
slider edits the send, with one Undo per drag, keyboard action or accessibility
increment. Double-click centers it; Option-drag is fine adjustment. The selected
inspector mirrors previews. Channels missing the selected send cannot accidentally
change main mute or balance. Exit Sends on Faders to restore main controls.

Send context menus and the routing matrix expose mute/unmute and independent
balance. A muted send remains visible with its original gain and tap. Captured
send menu commands reject if their source settings have changed. Matrix cell/menu
snapshot guards remain in place. Main channel balance, solo and arm stay separate.

## Verification

`send_controls` covers additive ABI guards, no-op/revision rules, one-gesture Undo,
preview isolation, cancel, save/open, a real v21 schema without the new columns,
malformed SQLite controls, bus deletion/Undo, package round-trip, interchange loss
reporting and live prepared-renderer amplitude/automation/mute/smoothing behavior.
`e2e_send_controls` uses only the public C ABI and an independent WAV reader to
check rendered channel amplitudes, mute silence, Undo/Redo and save/open/unmute.
`MixerSendTests` checks real native button actions, pan gesture callbacks, selected
inspector mirroring, linked/independent switching, disabled/missing sends, restoration
of main controls, accessibility increments and routing-menu actions. It writes
`build/mixer-send-ui.png` from an actual AppKit fixture.

Native macOS build and full regression results must be recorded for the published
revision; local Swift syntax parsing is not a native build. Physical device listening,
third-party plug-in GUI and manual VoiceOver acceptance remain separate.

Workflow references: [Logic independent send pan](https://support.apple.com/en-mide/guide/logicpro/lgcp434ffa19/mac)
and [Cubase send panners](https://www.steinberg.help/r/cubase-pro/15.0/en/cubase_nuendo/topics/audio_effects/audio_effects_send_effects_setting_pan_t.html).
