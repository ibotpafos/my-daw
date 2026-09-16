# Inspector signal chain and audio-clip presentation

The inspector now projects the same `RackDevice` descriptors as the lower device
rack. Native power buttons call its existing bypass operation. Plugin names open
the existing parameter editor; the row menu reorders or removes the same insert.
No vendor UI, analyzer curve or processing instance is invented by this surface.

Track sends are read through the existing C ABI and edited through the existing
`addSend` / `changeSendGain` / `toggleSendPre` / `removeSend` handlers. The native
menu excludes already-used buses. Send sliders commit once on release, retain the
existing -120…+24 dB range and display the saved value. Mutations retain the same
Undo and playback contracts as the mixer. Bus/master inspectors do not expose
track-only sends.

Each row captures both its owner and the exact revision it displayed. Detached
buttons, old menu actions and recording-time callbacks are checked again at the
controller boundary. A stale send gain must never recreate a removed route.
Changes to the view's own layout or read-only refresh never consume revisions.

Audio clips use the track accent (or the explicit persisted clip color), fill the
48-pt content area of a 56-pt lane, and reserve a small header for the real track
name. The peak source, take offset and edit geometry remain authoritative. Actual
nonzero fades are drawn; zero fades show handles only when selected/hovered, not
invented ramps. NSBezierPath clipping bounds the waveform and text to the clip.
Only the current track displays its remembered clip selection; switching tracks
does not erase the stored local selection/group.

Reuse: native AppKit NSStackView, NSButton, NSSlider, NSPopUpButton/NSMenu and
NSBezierPath plus the existing rack/send command paths. No dependency or DSP
changes. Primary API references:
[menu validation](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/MenuList/Articles/EnablingMenuItems.html),
[path clipping](https://developer.apple.com/documentation/appkit/nsbezierpath/addclip()),
[symbol configuration](https://developer.apple.com/documentation/appkit/nsimage/withsymbolconfiguration(_:)).

The production-source AppKit harness additionally exercises actual inspector
buttons, send menus, send gain/PRE/POST/removal and Undo, stale callbacks, audio
and MIDI recording guards, compact clip geometry, colors and real fade positions.
See `tests/workspace_signal_chain_tests.swift`. Native build/test evidence is
recorded in PR #6; physical audio and vendor plugin UI remain separate gates.
