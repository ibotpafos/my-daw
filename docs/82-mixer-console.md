# Professional Mixer Console

The shipping mixer extends the existing Session/C ABI and renderer; it does not create a second routing graph. Existing docs/30-mixer.md, docs/35-routing-buses-sends.md, AU hosting and automation/PDC contracts remain authoritative.

## Console UX

Selected-channel inspector on the left, scrollable track/bus strips in the center and pinned Master on the right. Console supports search, Audio/Bus filters, compact/regular/wide strips, stereo meters, exact numeric dB entry, inserts, sends, output routing, balance, arm/mute/solo and automation state. Master exposes peak plus existing BS.1770 momentary/short-term LUFS.

Sends on Faders is explicit. Selecting a bus maps only eligible track faders to that send gain; a track without the send must never silently edit main volume. Exiting the mode restores main faders.

## Editing

Fader travel is non-linear with useful resolution around unity while engine values remain exact -120...+24 dB. A pointer drag is one Undo gesture. Automation-writing gestures remain distinct from static volume edits. Option-click Solo keeps the existing exclusive-solo behavior.

## Realtime

Audio callback must not allocate, lock, perform file I/O or traverse mutable domain structures. Structural route/PRE-POST changes may rebuild a render plan. Scalar gain changes should use prepared atomic/smoothed targets when the engine supports them. UI must not fake unsupported bus solo or bus sends. Track pan remains documented stereo balance.

## Completion gate

Required before calling the mixer complete: domain/C ABI/storage regression; routing cycle rejection; Undo/Redo; save/open; automation and PDC regression; macOS AppKit build; usable 256-track scrolling/filtering; meter polling without rebuilding strips; AU insert state and bypass; sends PRE/POST; bus/master routing; keyboard/accessibility; and separate real-device listening/plugin-GUI/visual acceptance.
