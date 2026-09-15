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
_Static_assert(DAW_MIDI_NOTE_VERSION == 1 && DAW_MIDI_CLIP_VERSION == 1, "MIDI ABI versions");
_Static_assert(DAW_MIDI_NOTES_PER_CALL == 8192, "MIDI per-call limit");
_Static_assert(offsetof(daw_midi_note, struct_size) == 0, "MIDI note prefix");
_Static_assert(sizeof(daw_midi_note) == 32, "MIDI note ABI size");
_Static_assert(offsetof(daw_midi_clip, struct_size) == 0, "MIDI clip prefix");
_Static_assert(sizeof(daw_midi_clip) == 32, "MIDI clip ABI size");
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
    /* Undo(9) left a MIDI-only session; since the instrument-voice rule (MIDI clips or
       inserts make a track renderable) such a project previews as silent, not error. */
    if (daw_get_export_tail_summary(session, &export_options, &tail_summary) != 0) result |= 1;
    daw_destroy(session);
    return result || snapshot.track_count != 0 || component.struct_size == 0 || plugin.struct_size == 0 || hosting.struct_size == 0 || runtime.struct_size == 0;
}
