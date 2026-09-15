#include "daw.h"
#include <stddef.h>
_Static_assert(DAW_INSERT_HOSTING_STATUS_VERSION == 1, "hosting status ABI version");
_Static_assert(DAW_INSERT_HOSTING_MODE_IN_PROCESS == 1, "in-process ABI value");
_Static_assert(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS == 2, "out-of-process ABI value");
_Static_assert(DAW_INSERT_HOSTING_FORMAT_AUV2 == 1 && DAW_INSERT_HOSTING_FORMAT_AUV3 == 2 && DAW_INSERT_HOSTING_FORMAT_VST3 == 3, "hosting format ABI values");
_Static_assert(DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS == 1u && DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS == 2u, "hosting flag ABI values");
_Static_assert(offsetof(daw_insert_hosting_status, struct_size) == 0, "hosting status prefix");
_Static_assert(sizeof(daw_insert_hosting_status) == 20, "hosting status ABI size");
_Static_assert(DAW_INSERT_RUNTIME_STATUS_VERSION == 1, "runtime status ABI version");
_Static_assert(DAW_INSERT_RUNTIME_UNPREPARED == 0 && DAW_INSERT_RUNTIME_ACTIVE_IN_PROCESS == 1 && DAW_INSERT_RUNTIME_ACTIVE_ISOLATED == 2 && DAW_INSERT_RUNTIME_DRY_FALLBACK == 3 && DAW_INSERT_RUNTIME_FAILED == 4, "runtime state ABI values");
_Static_assert(DAW_INSERT_RUNTIME_FAULT_NONE == 0 && DAW_INSERT_RUNTIME_FAULT_PREPARE_FAILED == 1 && DAW_INSERT_RUNTIME_FAULT_RESTART_REQUIRED == 2 && DAW_INSERT_RUNTIME_FAULT_DEADLINE_MISSED == 3 && DAW_INSERT_RUNTIME_FAULT_PROTOCOL_ERROR == 4 && DAW_INSERT_RUNTIME_FAULT_HELPER_EXITED == 5, "runtime fault ABI values");
_Static_assert(offsetof(daw_insert_runtime_status, struct_size) == 0, "runtime status prefix");
_Static_assert(sizeof(daw_insert_runtime_status) == 20, "runtime status ABI size");
_Static_assert(DAW_EXPORT_OPTIONS_VERSION == 1, "export options ABI version");
_Static_assert(DAW_EXPORT_TAIL_AUTOMATIC == 1 && DAW_EXPORT_TAIL_NONE == 2 && DAW_EXPORT_TAIL_MANUAL_LIMIT == 3, "export tail ABI values");
_Static_assert(DAW_EXPORT_TAIL_SUMMARY_VERSION == 1, "export tail summary ABI version");
_Static_assert(offsetof(daw_export_options, struct_size) == 0, "export options prefix");
_Static_assert(sizeof(daw_export_options) == 16, "export options ABI size");
_Static_assert(offsetof(daw_export_tail_summary, struct_size) == 0, "export tail summary prefix");
_Static_assert(sizeof(daw_export_tail_summary) == 20, "export tail summary ABI size");
_Static_assert(DAW_IMPORT_STATUS_VERSION == 1, "import status ABI version");
_Static_assert(DAW_IMPORT_RUNNING == 0 && DAW_IMPORT_READY == 1 && DAW_IMPORT_FAILED == 2 && DAW_IMPORT_CANCELED == 3 && DAW_IMPORT_APPLIED == 4, "import status ABI values");
_Static_assert(DAW_IMPORT_PHASE_NONE == 0 && DAW_IMPORT_PHASE_READING == 1 && DAW_IMPORT_PHASE_DECODING == 2 && DAW_IMPORT_PHASE_CONVERTING == 3 && DAW_IMPORT_PHASE_READY == 4, "import phase ABI values");
_Static_assert(offsetof(daw_import_status, struct_size) == 0, "import status prefix");
_Static_assert(sizeof(daw_import_status) == 568, "import status ABI size");
_Static_assert(DAW_MIDI_NOTE_VERSION == 1 && DAW_MIDI_CLIP_VERSION == 2, "MIDI ABI versions");
_Static_assert(DAW_MIDI_NOTES_PER_CALL == 8192, "MIDI per-call limit");
_Static_assert(offsetof(daw_midi_note, struct_size) == 0, "MIDI note prefix");
_Static_assert(sizeof(daw_midi_note) == 32, "MIDI note ABI size");
_Static_assert(offsetof(daw_midi_clip, struct_size) == 0, "MIDI clip prefix");
_Static_assert(sizeof(daw_midi_clip) == 40, "MIDI clip ABI size");
_Static_assert(DAW_MIDI_DEVICE_VERSION == 1 && DAW_MIDI_RECORD_STATUS_VERSION == 1, "MIDI capture ABI versions");
_Static_assert(offsetof(daw_midi_device, struct_size) == 0 && offsetof(daw_midi_device, uniqueID) == 8 && offsetof(daw_midi_device, name) == 16, "MIDI device prefix and name placement");
_Static_assert(sizeof(daw_midi_device) == 148, "MIDI device ABI size");
_Static_assert(offsetof(daw_midi_record_status_t, struct_size) == 0 && offsetof(daw_midi_record_status_t, armed) == 8 && offsetof(daw_midi_record_status_t, open_notes) == 16, "MIDI capture status prefix");
_Static_assert(sizeof(daw_midi_record_status_t) == 48, "MIDI capture status ABI size");
_Static_assert(DAW_MARKER_VERSION == 1, "marker ABI version");
_Static_assert(offsetof(daw_marker, struct_size) == 0 && offsetof(daw_marker, frame) == 8 && offsetof(daw_marker, name) == 16, "marker layout");
_Static_assert(sizeof(daw_marker) == 144, "marker ABI size");
_Static_assert(DAW_VST3_FLAG_INSTRUMENT == 1u, "VST3 component flag ABI value");
_Static_assert(offsetof(daw_vst3_component, struct_size) == 0, "VST3 component prefix");
_Static_assert(offsetof(daw_vst3_component, available) == 4 && offsetof(daw_vst3_component, flags) == 8 && offsetof(daw_vst3_component, class_id) == 12, "VST3 component flag placement");
_Static_assert(sizeof(daw_vst3_component) == 5940, "VST3 component ABI size");
int main(void) {
    daw_session* session = daw_create();
    if (!session) return 1;
    daw_snapshot snapshot = {0}; snapshot.struct_size = sizeof(snapshot);
    daw_recording recording = {0}; recording.struct_size = sizeof(recording);
    daw_export_status export_status = {0}; export_status.struct_size = sizeof(export_status);
    daw_import_status import_status = {0}; import_status.struct_size = sizeof(import_status);
    daw_output_status output_status = {0}; output_status.struct_size = sizeof(output_status);
    daw_bus bus = {0}; bus.struct_size = sizeof(bus);
    daw_send send = {0}; send.struct_size = sizeof(send);
    daw_au_component component = {0}; component.struct_size = sizeof(component);
    daw_plugin plugin = {0}; plugin.struct_size = sizeof(plugin);
    daw_insert_hosting_status hosting = {0}; hosting.struct_size = sizeof(hosting);
    daw_insert_runtime_status runtime = {0}; runtime.struct_size = sizeof(runtime);
    daw_export_options export_options = {0}; export_options.struct_size = sizeof(export_options); export_options.version = DAW_EXPORT_OPTIONS_VERSION; export_options.tail_mode = DAW_EXPORT_TAIL_AUTOMATIC;
    daw_export_tail_summary tail_summary = {0}; tail_summary.struct_size = sizeof(tail_summary);
    int result = daw_get_snapshot(session, &snapshot);
    result |= daw_get_recording(session, &recording);
    result |= daw_get_output_status(session, &output_status);
    result |= daw_set_loop(session, 0, 0, 0);
    result |= daw_add_track(session, "Header track", 0);
    result |= daw_move_track(session, 1, 0, 1);
    result |= daw_remove_track(session, 1, 1);
    /* MIDI bridge smoke: add, query, edit, split, remove and undo in pure C. */
    result |= daw_add_track(session, "Midi bridge", 2);
    daw_midi_note notes[2] = {{sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 100, 2400, 60, 0, 90}, {sizeof(daw_midi_note), DAW_MIDI_NOTE_VERSION, 3000, 1800, 64, 15, 127}};
    daw_midi_clip clip = {0}; clip.struct_size = sizeof(clip); clip.version = DAW_MIDI_CLIP_VERSION; clip.start = 0; clip.length = 48000; clip.lane = 3; clip.note_count = 2;
    result |= daw_add_midi_clip(session, 2, &clip, notes, 2, 3);
    uint32_t midi_count = 0;
    result |= daw_get_midi_clip_count(session, 2, &midi_count);
    if (midi_count != 1) result |= 1;
    daw_midi_note read_notes[2] = {{0}};
    uint32_t written = 0;
    result |= daw_get_midi_clip(session, 2, 0, &clip, 0, read_notes, 2, &written);
    if (clip.note_count != 2 || written != 2 || read_notes[0].pitch != 60 || read_notes[0].velocity != 90 || read_notes[1].channel != 15 || clip.lane != 3) result |= 1;
    result |= daw_set_midi_notes(session, 2, 0, notes, 1, 4);
    result |= daw_move_midi_clip(session, 2, 0, 96000, 5);
    result |= daw_trim_midi_clip(session, 2, 0, 0, 48000, 6);
    result |= daw_split_midi_clip(session, 2, 0, 24000, 7);
    result |= daw_get_midi_clip_count(session, 2, &midi_count);
    if (midi_count != 2) result |= 1;
    result |= daw_remove_midi_clip(session, 2, 1, 8);
    result |= daw_undo(session, 9);
    /* ABI rejections must fail without consuming a revision. */
    daw_midi_clip bad = clip; bad.struct_size = 0;
    if (daw_add_midi_clip(session, 2, &bad, notes, 2, 10) == 0) result |= 1;
    daw_midi_note bad_note = notes[0]; bad_note.pitch = 128;
    if (daw_add_midi_clip(session, 2, &clip, &bad_note, 1, 10) == 0) result |= 1;
    if (daw_get_midi_clip(session, 2, 0, &bad, 0, read_notes, DAW_MIDI_NOTES_PER_CALL + 1, &written) == 0) result |= 1;
    /* ---- Clip/track editing ABI smoke (project schema v19): current revision 10 ---- */
    result |= daw_add_track(session, "Clip bridge", 10);                      /* rev 11, track id 3 */
    result |= daw_set_track_color(session, 3, 0xABCDEFu, 11);                 /* rev 12 */
    daw_track colored = {0}; colored.struct_size = sizeof(colored);
    result |= daw_get_track(session, 1, &colored);                            /* [Midi bridge, Clip bridge] */
    if (colored.id != 3 || colored.color != 0xABCDEFu) result |= 1;
    uint64_t duplicate_id = 0;
    result |= daw_duplicate_track(session, 3, &duplicate_id, 12);             /* rev 13 */
    if (duplicate_id == 3 || duplicate_id == 0) result |= 1;
    daw_snapshot after_dup = {0}; after_dup.struct_size = sizeof(after_dup);
    result |= daw_get_snapshot(session, &after_dup);
    if (after_dup.track_count != 3) result |= 1;
    if (daw_set_clip_color(session, 3, 0, 0x1u, 13) == 0) result |= 1;        /* no regions: rejected */
    if (daw_set_clip_gain(session, 3, 0, -3.0, 13) == 0) result |= 1;         /* ditto, no revision spent */
    if (daw_duplicate_track(session, 3, NULL, 13) == 0) result |= 1;          /* missing out param is refused */
    result |= daw_set_midi_clip_color(session, 2, 0, 0x10203u, 13);           /* rev 14 */
    result |= daw_get_midi_clip(session, 2, 0, &clip, 0, NULL, 0, NULL);      /* metadata-only read */
    if (clip.color != 0x10203u) result |= 1;
    result |= daw_append_midi_notes(session, 2, 0, notes, 1, 14);             /* rev 15: the undo-restored clip is empty */
    result |= daw_transpose_midi_clip(session, 2, 0, 2, 15);                  /* rev 16: 60 -> 62 */
    result |= daw_get_midi_clip(session, 2, 0, &clip, 0, read_notes, 2, &written);
    if (clip.note_count != 1 || written != 1 || read_notes[0].pitch != 62 || read_notes[0].start != 100) result |= 1;
    result |= daw_quantize_midi_clip(session, 2, 0, 1.0, 16);                 /* rev 17: note start 100 -> 0 */
    result |= daw_get_midi_clip(session, 2, 0, &clip, 0, read_notes, 2, &written);
    if (read_notes[0].start != 0) result |= 1;
    if (daw_transpose_midi_clip(session, 2, 0, 128, 17) == 0) result |= 1;    /* out-of-int8 range refused */
    if (daw_quantize_midi_clip(session, 2, 0, 0.0, 17) == 0) result |= 1;     /* zero grid refused */
    daw_snapshot held = {0}; held.struct_size = sizeof(held);
    result |= daw_get_snapshot(session, &held);
    if (held.revision != 17) result |= 1;                                     /* the two rejects spent nothing */
    /* Undo(9) left a MIDI-only session; since the instrument-voice rule (MIDI clips or
       inserts make a track renderable) such a project previews as silent, not error. */
    if (daw_get_export_tail_summary(session, &export_options, &tail_summary) != 0) result |= 1;
    /* VST3 component ABI smoke: a headless run has no installed plug-ins, so
     * the catalog is empty and this covers only the ABI shape plus the new
     * flags field structurally; the instrument bit itself is produced by the
     * scanner helper and round-tripped by the v2 cache. */
    uint32_t vst3_count = 0;
    result |= daw_get_installed_vst3_count(session, &vst3_count);
    if (vst3_count != 0) result |= 1;
    daw_vst3_component vst3 = {0}; vst3.struct_size = sizeof(vst3); vst3.flags = DAW_VST3_FLAG_INSTRUMENT;
    if (daw_get_installed_vst3(session, 0, &vst3) == 0) result |= 1; /* empty catalog must reject */
    if (vst3.flags != DAW_VST3_FLAG_INSTRUMENT) result |= 1; /* rejection must not touch caller storage */
    daw_vst3_component stale = vst3; stale.struct_size = sizeof(stale) - 4;
    if (daw_get_installed_vst3(session, 0, &stale) == 0) result |= 1; /* pre-flags buffers are rejected */
    /* ---- Live MIDI capture + metronome ABI (v0) ----
     * No controller is attached to this runner, so this is the headless
     * ceiling of the feature: it proves the ABI shape and every path that has
     * to be safe without hardware — rejection, idle no-op, no revision spent —
     * but never that a real key press reaches a clip. */
    uint32_t midi_inputs = 7;
    if (daw_get_midi_input_device_count(session, &midi_inputs) != 0) result |= 1;
    if (daw_get_midi_input_device_count(session, NULL) == 0) result |= 1;
    daw_midi_device device = {0}; device.struct_size = sizeof(device); device.version = DAW_MIDI_DEVICE_VERSION; device.uniqueID = 12345; device.online = 77;
    if (midi_inputs == 0) {
        if (daw_get_midi_input_device(session, 0, &device) == 0) result |= 1; /* a headless run has no source at index 0 */
        if (device.uniqueID != 12345 || device.online != 77) result |= 1;     /* rejection must not touch caller storage */
    } else {
        if (daw_get_midi_input_device(session, 0, &device) != 0) result |= 1;
        if (device.struct_size != sizeof(device) || device.version != DAW_MIDI_DEVICE_VERSION) result |= 1;
    }
    if (daw_get_midi_input_device(session, midi_inputs, &device) == 0) result |= 1; /* one past the list is always nobody's */
    daw_midi_device short_device = device; short_device.struct_size = sizeof(short_device) - 1;
    if (daw_get_midi_input_device(session, 0, &short_device) == 0) result |= 1;     /* struct_size-1 is rejected */
    daw_midi_device long_device = device; long_device.struct_size = sizeof(long_device) + 1;
    if (daw_get_midi_input_device(session, 0, &long_device) == 0) result |= 1;      /* and so is struct_size+1 */
    daw_midi_device stale_device = device; stale_device.version = DAW_MIDI_DEVICE_VERSION + 1;
    if (daw_get_midi_input_device(session, 0, &stale_device) == 0) result |= 1;     /* an unknown version is refused too */
    uint32_t active_input = 7;
    if (daw_midi_input_active(session, &active_input) != 0) result |= 1;
    if (active_input != 0) result |= 1;                                             /* nothing was ever opened */
    if (daw_midi_input_active(session, NULL) == 0) result |= 1;
    if (daw_set_midi_input(session, 0) != 0) result |= 1;                           /* closing with none open is a no-op */
    if (daw_set_midi_input(session, 0xFFFFFFFFu) == 0) result |= 1;                /* an unknown source must be refused */
    if (daw_midi_record_arm(session, 2, 0) == 0) result |= 1;                       /* no open input: arming must fail */
    daw_midi_record_status_t capture = {0}; capture.struct_size = sizeof(capture); capture.version = DAW_MIDI_RECORD_STATUS_VERSION;
    if (daw_midi_record_status(session, &capture) != 0) result |= 1;
    if (capture.armed || capture.open_notes || capture.recorded || capture.dropped || capture.unmatched) result |= 1;
    if (daw_midi_record_status(session, NULL) == 0) result |= 1;
    daw_midi_record_status_t short_capture = capture; short_capture.struct_size = sizeof(short_capture) - 1;
    if (daw_midi_record_status(session, &short_capture) == 0) result |= 1;          /* the size gate runs before any read */
    daw_midi_record_status_t long_capture = capture; long_capture.struct_size = sizeof(long_capture) + 1;
    if (daw_midi_record_status(session, &long_capture) == 0) result |= 1;
    daw_midi_record_status_t stale_capture = capture; stale_capture.version = DAW_MIDI_RECORD_STATUS_VERSION + 1;
    if (daw_midi_record_status(session, &stale_capture) == 0) result |= 1;
    if (daw_midi_record_poll(session) != 0) result |= 1;                            /* draining an idle take is safe */
    if (daw_midi_record_stop(session) != 0) result |= 1;                            /* stopping one is a silent no-op */
    daw_snapshot idle = {0}; idle.struct_size = sizeof(idle);
    result |= daw_get_snapshot(session, &idle);
    if (idle.revision == 0) result |= 1;                                             /* the MIDI smoke above did move it */
    for (int repeat = 0; repeat < 2; ++repeat) {
        if (daw_midi_record_stop(session) != 0) result |= 1;                        /* twice in a row is still a no-op */
        daw_snapshot again = {0}; again.struct_size = sizeof(again);
        result |= daw_get_snapshot(session, &again);
        if (again.revision != idle.revision) result |= 1;                           /* and it never spends a revision */
    }
    for (int metronome = 0; metronome <= 1; ++metronome) {
        int read_back = -1;
        result |= daw_set_metronome(session, metronome);
        result |= daw_get_metronome(session, &read_back);
        if (read_back != metronome) result |= 1;                                    /* set/get symmetry on the stored flag */
    }
    if (daw_set_metronome(session, 2) == 0) result |= 1;                            /* only 0 and 1 are states */
    if (daw_get_metronome(session, NULL) == 0) result |= 1;
    if (daw_set_midi_input(session, 0) != 0) result |= 1;                           /* still idempotent after all of it */
    daw_snapshot after = {0}; after.struct_size = sizeof(after);
    result |= daw_get_snapshot(session, &after);
    if (after.revision != idle.revision) result |= 1;                               /* capture and click are never commands */
    /* Markers (domain v18): add, rename, identical-rename no-op, readback,
     * duplicate-frame rejection, remove, undo/redo on the revision ladder. */
    const uint64_t base = after.revision;
    if (daw_add_marker(session, 48000, "Intro", base) != 0) result |= 1;
    if (daw_add_marker(session, 192000, "Hook", base + 1) != 0) result |= 1;
    if (daw_rename_marker(session, 48000, "Verse", base + 2) != 0) result |= 1;
    if (daw_rename_marker(session, 48000, "Verse", base + 3) != 0) result |= 1;    /* no-op must still succeed */
    uint32_t marker_count = 9;
    if (daw_get_marker_count(session, &marker_count) != 0 || marker_count != 2) result |= 1;
    daw_marker marker = {0};
    marker.struct_size = sizeof(marker);
    if (daw_get_marker(session, 0, &marker) != 0 || marker.frame != 48000 || marker.version != DAW_MARKER_VERSION || marker.name[0] != 'V' || marker.name[5] != 0) result |= 1;
    if (daw_get_marker(session, 2, &marker) == 0) result |= 1;                      /* index out of range */
    if (daw_add_marker(session, 192000, "Dup", base + 3) == 0) result |= 1;        /* frame already taken */
    if (daw_add_marker(session, 48000, "", base + 3) == 0) result |= 1;            /* empty name rejected by the domain */
    if (daw_remove_marker(session, 48000, base + 3) != 0) result |= 1;
    if (daw_get_marker_count(session, &marker_count) != 0 || marker_count != 1) result |= 1;
    if (daw_undo(session, base + 4) != 0) result |= 1;
    if (daw_get_marker_count(session, &marker_count) != 0 || marker_count != 2) result |= 1;
    if (daw_redo(session, base + 5) != 0) result |= 1;
    if (daw_get_marker_count(session, &marker_count) != 0 || marker_count != 1) result |= 1;
    /* Cross-track clipboard on the post-undo state (tracks 2..4; track 2 owns the
     * coloured lane-3 clip at index 0). Audio paste speaks the domain rules, a
     * MIDI clip travels whole with its lane/notes/colour, validate owns overlaps. */
    daw_snapshot now = {0}; now.struct_size = sizeof(now);
    result |= daw_get_snapshot(session, &now);
    const uint64_t r = now.revision;
    if (daw_copy_clip_to_track(session, 2, 0, 999, 0, r) == 0) result |= 1;        /* source region missing */
    if (daw_move_clip_to_track(session, 999, 0, 2, 0, r) == 0) result |= 1;        /* source track missing */
    if (daw_copy_midi_clip_to_track(session, 2, 99, 3, 0, r) == 0) result |= 1;    /* clip index out of range */
    if (daw_copy_midi_clip_to_track(session, 2, 0, 3, 9600000, r) != 0) result |= 1;
    uint32_t clip_now = 0;
    if (daw_get_midi_clip_count(session, 3, &clip_now) != 0 || clip_now != 1) result |= 1;
    daw_midi_clip twin = {0}; twin.struct_size = sizeof(twin);
    if (daw_get_midi_clip(session, 3, 0, &twin, 0, NULL, 0, NULL) != 0 || twin.start != 9600000 || twin.lane != 3 || twin.note_count != 1 || twin.color != 0x10203u) result |= 1;
    if (daw_move_midi_clip_to_track(session, 2, 0, 3, 9700000, r + 1) != 0) result |= 1;
    if (daw_get_midi_clip_count(session, 3, &clip_now) != 0 || clip_now != 2) result |= 1;
    if (daw_get_midi_clip_count(session, 2, &clip_now) != 0 || clip_now != 1) result |= 1;
    if (daw_move_midi_clip_to_track(session, 3, 1, 3, 9602400, r + 2) == 0) result |= 1; /* lands on the pasted clip */
    if (daw_undo(session, r + 2) != 0) result |= 1;
    if (daw_get_midi_clip_count(session, 2, &clip_now) != 0 || clip_now != 2) result |= 1;
    /* Clip playback state (v20) on revision r+3: this session carries no audio
     * regions, so mute/loop ride the honest rejection paths — and the widened
     * daw_clip struct still passes the ABI gate. */
    daw_clip probe = {0}; probe.struct_size = sizeof(probe);
    if (daw_get_clip(session, 2, 0, &probe) == 0) result |= 1;                        /* no audio regions */
    if (probe.muted != 0 || probe.looped != 0) result |= 1;                          /* untouched fields */
    if (daw_set_clip_muted(session, 2, 0, 2, r + 3) == 0) result |= 1;                /* flag must be 0 or 1 */
    if (daw_set_clip_muted(session, 2, 0, 1, r + 3) == 0) result |= 1;                /* clip not found */
    if (daw_set_clip_looped(session, 4, 0, 1, r + 3) == 0) result |= 1;
    /* Multi-select group ops: one bad index poisons the batch, rejects spend
     * nothing, a missing array is refused and an empty group is a legal no-op. */
    const uint32_t group[2] = {0, 7};
    if (daw_delete_clips(session, 2, group, 2, r + 3) == 0) result |= 1;
    if (daw_nudge_clips(session, 2, group, 2, 4800, r + 3) == 0) result |= 1;
    if (daw_delete_clips(session, 2, NULL, 1, r + 3) == 0) result |= 1;
    if (daw_delete_clips(session, 2, NULL, 0, r + 3) != 0) result |= 1;
    /* Loudness report: unreadable files reject and the ABI gate holds. */
    daw_loudness_report loudness = {0}; loudness.struct_size = sizeof(loudness);
    if (daw_measure_wav(session, "/nonexistent/mydaw-tone.wav", &loudness) == 0) result |= 1;
    loudness.struct_size = sizeof(loudness) - 4;
    if (daw_measure_wav(session, "/tmp", &loudness) == 0) result |= 1;
    /* Clip pan gates: MIDI tracks have no audio region to pan, the range
     * check fires inside the domain, and nothing here spends a revision. */
    daw_clip panned = {0}; panned.struct_size = sizeof(daw_clip);
    if (daw_get_clip(session, 2, 0, &panned) == 0) result |= 1;
    if (daw_set_clip_pan(session, 2, 0, 1.5, r + 3) == 0) result |= 1;
    if (daw_set_clip_pan(session, 4, 0, -0.5, r + 3) == 0) result |= 1;
    /* Live master loudness: no running output means live=0 at the sentinel. */
    daw_master_loudness meter = {0}; meter.struct_size = sizeof(meter);
    if (daw_get_master_loudness(session, &meter) != 0 || meter.live != 0 || meter.momentary_lufs > -100.0f) result |= 1;
    meter.struct_size = sizeof(meter) - 4;
    if (daw_get_master_loudness(session, &meter) == 0) result |= 1;
    /* Pre-roll: 30 s ceiling, 0 allowed, getter mirrors, NULL out rejects. */
    uint64_t preroll = 7;
    if (daw_set_record_preroll(session, 48000ULL * 31) == 0) result |= 1;
    if (daw_set_record_preroll(session, 96000) != 0) result |= 1;
    if (daw_get_record_preroll(session, &preroll) != 0 || preroll != 96000) result |= 1;
    if (daw_get_record_preroll(session, NULL) == 0) result |= 1;
    if (daw_set_record_preroll(session, 0) != 0) result |= 1;
    if (daw_get_record_preroll(session, &preroll) != 0 || preroll != 0) result |= 1;
    /* Project package: absent drafts and NULL arguments reject. */
    if (daw_package_project(session, "/nonexistent/mydaw.draft", "/tmp/mydaw-pkg-gate.zip") == 0) result |= 1;
    if (daw_package_project(session, NULL, "/tmp/mydaw-pkg-gate.zip") == 0) result |= 1;
    if (daw_extract_package(session, "/tmp/mydaw-pkg-gate.zip", NULL) == 0) result |= 1;
    if (daw_undo(session, r + 3) != 0) result |= 1;                                   /* nothing spent a revision */
    daw_destroy(session);
    return result || snapshot.track_count != 0 || component.struct_size == 0 || plugin.struct_size == 0 || hosting.struct_size == 0 || runtime.struct_size == 0;
}
