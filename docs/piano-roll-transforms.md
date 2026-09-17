# Piano roll: tested MIDI note transformations

This increment adds standalone composition operations on the existing PRNoteEntity / PRTimeMap model. It does not replace the UI, extend the project schema, add CC events, or claim complete AppKit integration. The richer local UI integration is still unpublished.

- Velocity ramp interpolates by note-on position in quarter-note beats. Simultaneous notes receive the same velocity; unselected notes remain bit-exact. A single onset receives the first endpoint.
- Ratchet splits selected notes into 1–32 equal musical subdivisions, with gate 5–100%. Gate 100% tiles the original note without frame loss. The first segment keeps the original ID and later segments receive fresh IDs. Invalid sub-frame divisions, ID collisions and project note-budget overflow reject the entire operation.
- Strum spreads simultaneous selected pitches per MIDI channel, ascending or descending. Duplicate occurrences of a pitch stay together. Note lengths are retained in musical time. Notes outside the clip or longer than the domain limit reject atomically rather than disappearing or being silently shortened.

No new library is required: these are product-specific composition transforms over the already existing model and tempo adapter. No DSP, MIDI parser, resampler, persistence mechanism, audio callback or second undo stack is introduced.

Run `bash scripts/test-piano-roll-transforms.sh` with Swift 6. The suite has 31 checks including 1,000 seeded partition iterations, tempo changes, 65,536-note budget rejection and UInt64 overflow inputs. A separate macOS workflow reports this narrow proof independently of the pre-existing C++ build failure. It is not an AppKit UI or full-application build check.
