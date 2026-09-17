# Mixer integration and development-stage acceptance

17 September 2026. The owner requested finishing, merging and closing the current
Mixer Console development task in [PR #3](https://github.com/ibotpafos/my-daw/pull/3).
This report supersedes the pre-integration build-status sections in
[console documentation](82-mixer-console.md), [send controls](83-send-controls.md)
and [linked levels](84-linked-mixer-levels.md); their feature contracts still apply.
Actual PR merge status and the final main commit are recorded in the PR timeline,
not inferred from the existence of this document. Manual release acceptance remains
open in [QA #13](https://github.com/ibotpafos/my-daw/issues/13).

## Preserved histories and conflict resolution

Tested source: `03994bcf590b2ad0f426d9352ccc1d0390a46ecd`, a real two-parent merge.
The integrated main parent is `176aeb314983295fcf234915ac97de362d567607` and includes
the already merged professional Piano Roll. The feature-side source descends from
`0c8034aae50d69db4ac7e07bca1f685d25d28057`. No history was reset or force-pushed.

The bridge, domain and draft storage retain main's compiler-clean formatting and
MIDI transaction safety while incorporating the mixer's existing gesture and send
controls. Conflicts were resolved against pinned files and verified by exact output
hashes. Both build and whole-app typecheck now include the complete Mixer source
family AND the existing PianoRoll source family. The main app's MIDI binding,
paged reads, document identity and atomic 65536-note replacement remain intact.

The missing strict E2E coverage for `daw_begin_track_gain_group` was closed with
`tests/e2e/e2e_mixer_groups.cpp`. This is a black-box C ABI journey: import real WAV
fixtures, capture baseline exported PCM, edit linked levels, save while previewing,
commit once, Undo, cancel/Redo, reopen and compare exported PCM amplitudes. It uses
ordinary public bridge calls and independently reads the exported audio, not a
mocked gain model. CMake's existing E2E discovery includes it automatically.

## Confirmed integrated native validation

[Run 35243179971](https://github.com/ibotpafos/my-daw/actions/runs/35243179971)
completed all steps successfully before publishing the source merge to the feature
branch. Its `validation/revision.txt` identifies the actual tested source above;
the workflow trigger revision is a preceding setup commit, not the compiled code.

- Full Swift 6 application typecheck passed with warnings as errors.
- Real AppKit mixer, routing, send and linked-fader component tests passed.
- The existing Piano Roll AppKit smoke and musical-transform/edit-safety suites passed.
- **36/36 macOS core tests passed with ASan/UBSan**, total 99.17 seconds. All 16
  E2E scenarios passed, including the new exported-audio group journey.
- The complete arm64 application built and passed its strict codesign verification.
- Native mixer controls through the production `MixerGroupBinding` and real C ABI
  passed. The Piano Roll's **23 real C ABI integration checks** also passed,
  including save/open, rejection of stale edits, Undo and maximum-sized replacement.
- The documentation checks and strict E2E coverage ledger passed. The ledger has
  **223 public APIs, 221 covered by E2E, two explicit manual hardware gates**, zero
  white-box-only APIs and zero uncovered APIs. The two manual gates are live
  recording entrypoints requiring a physical input device and microphone consent.

The artifact `mixer-main-integration-35243179971` contains source history/archive,
build/CTest logs, both adapter logs, four real offscreen AppKit fixture images and
the application ZIP. Group-console rendering was visually inspected. Fixture images
are not evidence of physical playback. The ZIP has SHA-256
`01a9c24c382294e75f0b7b5d5007af8e3fe1d8c436e10487137b611ccd069c2f`.
A subsequent cleanup commit changes only documentation and workflows, not this
compiled implementation. The temporary integration publisher is removed; ongoing
mixer validation is contents-read-only on main, feature pushes and pull requests.

## Linux and documentation results

The same resolved implementation built locally with GCC 14, warnings-as-errors and
ASan/UBSan. **32/34 CTest cases passed**, total 189.24 seconds; all 16 E2E cases passed.
`session_storage_bridge` passed (16.05 seconds), so the earlier local timeout report
is not the outcome of this integration. The previous misleading-indentation build
blocker is resolved by preserving main's formatted source, not by disabling warnings.

The remaining failures are `background_wav_import` and `aiff_import`: the existing
non-macOS resampler stub does not support the sample-rate conversion required by
the fixtures. They were not skipped or weakened. Portable resampling and its added
dependency are separate work in PR #1; they were not silently included in this merge.
This integration therefore does NOT claim an all-platform-green CI result.

Local `scripts/check_docs.py --require-schemas` passed the complete schema checks
(three schemas, three examples and ten negative cases), plus links, script hygiene,
version and SQL checks. JSON Schema checking on native runners without jsonschema
remains explicitly skipped by the existing checker rather than falsely counted.

## What closure means

The integrated current development slice comprises the working native console,
inserts/routing, safe routing matrix, independent send balance/mute, Sends on Faders,
linked static track levels and their persistence/Undo/renderer bindings. It does not
assert that every advanced mixer feature has been implemented or that manual release
acceptance has passed. See QA #13 for the unexecuted device/GUI/VoiceOver checklist.

VCA, durable mix snapshots, bus sends/solo, group automation and live integrated/
true-peak metering remain unimplemented follow-up functionality, not functions that
need only testing. Structural routing/PRE-POST/add/remove still stop and rebuild
transport by the existing contract; supported scalar changes use the live target path.

**Use project copies.** Send controls require draft v22; older pre-v22 builds cannot
reopen newly saved drafts. Linked levels and this merge introduce no further format
change. The CI application is version 1.73.0 for Apple Silicon/macOS 14+, ad-hoc
signed and not notarized. Without the optional VST3 SDK it contains the AU scan
helper and VST3 fallback, not VST3 runtime helpers. Physical listening, third-party
plugin GUI compatibility, audible PDC/automation and manual accessibility acceptance
must not be marked passed merely because the code was built, tested or merged.
