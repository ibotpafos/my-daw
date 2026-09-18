#ifndef MY_DAW_H
#define MY_DAW_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental bridge v1. The workflow surface (preview/commit and vocal
 * preparation) is part of this same API, not a separate future one. Numeric
 * input ranges of those calls are not spelled out here; they are enforced in
 * engine/bridge/daw.cpp, asserted by tests/e2e/e2e_workflow_vocal.cpp and
 * tabulated in docs/40-vocal-preparation-workflow.md.
 * All calls: same owning non-real-time thread, including destruction. No concurrency.
 * Handle owns state; caller owns all input/output buffers. No borrowed pointers escape.
 * Names/paths are NUL-terminated UTF-8; valid buffer storage is caller's responsibility.
 * 0 = success; 1 = failure. daw_error copies the latest error into caller storage.
 * Snapshot sizes must equal sizeof(struct). All mutations use expected_revision.
 */
typedef struct daw_session daw_session;
/* Recording clock diagnostics v1. Read-only, no polling of HAL or I/O.
 * validated_frames includes pre-roll and the full validated device block; it
 * is NOT the count of samples committed to disk. Anchors describe the first
 * valid callback, not physical ADC/DAC timestamps or measured round-trip delay.
 * initial_flags: bit0 sample valid, bit1 host valid (raw platform ticks).
 * fault: 0 none, 1 invalid timestamp, 2 sample gap/repeat, 3 host reversal/repeat.
 * Fault sample values are sanitized; meaningful only when fault != 0. An idle
 * session has a zero report. Session's owner thread only. Never mutates Undo. */
enum { DAW_RECORDING_CLOCK_VERSION = 1 };
typedef struct {
    uint32_t struct_size, version, initial_flags, fault;
    uint64_t validated_frames, first_host_time;
    double first_sample_time, expected_sample_time, observed_sample_time;
} daw_recording_clock;
int daw_get_recording_clock(daw_session*, daw_recording_clock*);

enum { DAW_AUDIO_DEVICE_VERSION = 1, DAW_AUDIO_DEVICE_CONFIG_VERSION = 1 };
typedef struct {
    uint32_t struct_size, version, device_id, input_channels, output_channels, buffer_frames;
    double sample_rate;
    int32_t is_default_input, is_default_output;
    char uid[481], name[481];
} daw_audio_device;
typedef struct {
    uint32_t struct_size, version, input_channel, output_left, output_right;
    char input_uid[481], output_uid[481];
} daw_audio_device_config;
/* Owner-thread, read-only hardware discovery. Refresh atomically replaces the
 * enumeration snapshot used by get; indices are not persistent identities.
 * Non-Apple: empty catalog. No microphone is opened and no TCC prompt requested. */
int daw_refresh_audio_devices(daw_session*, uint32_t* count);
int daw_get_audio_device(daw_session*, uint32_t index, daw_audio_device*);
/* Machine/session preference, not project/Undo state. Zero-based channels.
 * Empty UID follows system default; explicit missing UID MUST NOT fall back.
 * Set validates syntax without opening hardware (offline preferences allowed).
 * Start re-resolves UID and validates channels/48 kHz/buffer <=4096. Configuration
 * changes are rejected during playback/preparation/recording/MIDI capture.
 * Selection does not change hardware rate/buffer; use the explicit hardware job. */
int daw_set_audio_device_config(daw_session*, const daw_audio_device_config*);
int daw_get_audio_device_config(daw_session*, daw_audio_device_config*);

/* Hardware format controls v1: explicit UID, 48 kHz project target, buffer
 * 1..4096 within HAL bounds. Selection/preferences/project data are unchanged.
 * Read is metadata-only. Begin requires idle audio/MIDI; the worker owns a
 * process-wide exclusive HAL lease and does not capture a session pointer.
 * Expected snapshot prevents stale panel writes (UID + ID + rate + buffer).
 * Poll: 0 pending, 1 confirmed, 2 failed. Failure includes best-effort rollback;
 * restored=0 means manual refresh/recovery is required, NOT successful change.
 * Release requests cancellation, but never blocks; exclusion lasts through
 * rollback even after release/session destruction. UI must poll without sleep.
 * Hardware changes can affect other applications; require explicit user action.
 * Never call begin automatically on project open or on restoring preferences. */
enum { DAW_AUDIO_HARDWARE_VERSION = 1 };
typedef struct {
    uint32_t struct_size, version, device_id, buffer_frames, minimum_buffer, maximum_buffer;
    double sample_rate;
    int32_t supports_48k, rate_writable, buffer_writable;
    char uid[481];
} daw_audio_hardware_settings;
typedef struct daw_audio_hardware_job daw_audio_hardware_job;
typedef struct {
    uint32_t struct_size, version, status, restored, actual_known, actual_buffer;
    double actual_rate;
    char error[1024];
} daw_audio_hardware_status;
int daw_get_audio_hardware_settings(daw_session*, const char* uid, daw_audio_hardware_settings*);
daw_audio_hardware_job* daw_begin_audio_hardware_change(daw_session*, const daw_audio_hardware_settings* expected, uint32_t buffer);
int daw_poll_audio_hardware_change(daw_audio_hardware_job*, daw_audio_hardware_status*);
void daw_release_audio_hardware_change(daw_audio_hardware_job*);

typedef struct daw_save_job daw_save_job;
typedef struct daw_export_job daw_export_job;
typedef struct daw_dawproject_job daw_dawproject_job;
typedef struct daw_au_scan_job daw_au_scan_job;
typedef struct daw_vst3_scan_job daw_vst3_scan_job;
typedef struct daw_import_job daw_import_job;
typedef struct { uint32_t struct_size; int32_t status; uint64_t revision; char error[512]; } daw_save_status;
/* Capture on session owner thread. Job is independent of session lifetime.
 * Poll status: 0 running, 1 successful, 2 failed. Destroy releases the handle;
 * it does not cancel or wait for the worker. Caller must serialize writes per destination.
 * App maintains at most one manual-save and one recovery job. */
daw_save_job* daw_begin_save(daw_session*, const char* path);
int daw_poll_save(daw_save_job*, daw_save_status*);
void daw_release_save(daw_save_job*);
typedef struct { uint32_t struct_size; int32_t status; uint64_t revision; uint64_t rendered_frames; uint64_t total_frames; char error[512]; } daw_export_status;
/* Export-tail selection is intentionally per job, not part of the durable
 * project.  Automatic preserves the historic bounded VST3-tail behavior;
 * None omits only an infinite VST3 component while retaining finite tails;
 * ManualLimit limits only that infinite component to manual_tail_frames.
 * Every caller must supply this versioned structure to the *_with_options APIs. */
enum { DAW_EXPORT_OPTIONS_VERSION = 1 };
enum { DAW_EXPORT_TAIL_AUTOMATIC = 1, DAW_EXPORT_TAIL_NONE = 2, DAW_EXPORT_TAIL_MANUAL_LIMIT = 3 };
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    uint32_t tail_mode;
    uint32_t manual_tail_frames;
} daw_export_options;
/* A non-mutating preview of the frozen-current project's tail under one
 * options value.  finite_tail_frames excludes an infinite VST3 component;
 * selected_tail_frames is the value this export job will append. */
enum { DAW_EXPORT_TAIL_SUMMARY_VERSION = 1 };
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    uint32_t finite_tail_frames;
    uint32_t selected_tail_frames;
    int32_t infinite_tail_detected;
} daw_export_tail_summary;
/* format: 1=stereo PCM24 with deterministic TPDF dither, 2=stereo float32.
 * Poll status: 0 running, 1 successful, 2 failed, 3 canceled. The worker owns
 * a frozen snapshot and publishes the final path only after fsync + rename. */
daw_export_job* daw_begin_export(daw_session*,const char* path,int32_t format);
daw_export_job* daw_begin_export_range(daw_session*,const char* path,int32_t format,uint64_t start_frame,uint64_t end_frame);
int daw_get_export_tail_summary(daw_session*,const daw_export_options*,daw_export_tail_summary*);
daw_export_job* daw_begin_export_with_options(daw_session*,const char* path,int32_t format,const daw_export_options*);
daw_export_job* daw_begin_export_range_with_options(daw_session*,const char* path,int32_t format,uint64_t start_frame,uint64_t end_frame,const daw_export_options*);
/* Per-track stem export: one pre-master WAV per audible track into an existing
 * directory ("NN - name.wav"). Silent (user-muted or empty) tracks produce no
 * file; progress aggregates across stems and cancel lands between files. */
daw_export_job* daw_begin_stem_export(daw_session*,const char* directory,int32_t format,const daw_export_options*);
/* Stems for only the listed tracks (ids resolved against the current
 * revision). track_count 0 or NULL ids exports every audible track. */
daw_export_job* daw_begin_stem_export_tracks(daw_session*,const char* directory,int32_t format,const daw_export_options*,const uint64_t* track_ids,uint32_t track_count);
/* BS.1770-4 loudness/true-peak measurement of a finished WAV on disk:
 * K-weighted integrated loudness with the standard gates, and dBTP from 4x
 * oversampled interpolation. gated_silence marks files that never rise
 * above -70 LUFS. */
typedef struct { uint32_t struct_size; double integrated_lufs; double true_peak_db; int32_t gated_silence; } daw_loudness_report;
int daw_measure_wav(daw_session*,const char* path,daw_loudness_report* out);
/* .mydawzip project archive: packs a saved draft file (self-contained
 * SQLite + manifest) into one distributable ZIP, and extracts it back to a
 * target draft path (the caller picks a non-colliding name). */
int daw_package_project(daw_session*,const char* draft_path,const char* zip_path);
int daw_extract_package(daw_session*,const char* zip_path,const char* target_draft_path);
int daw_poll_export(daw_export_job*,daw_export_status*);
void daw_cancel_export(daw_export_job*);
void daw_release_export(daw_export_job*);
typedef struct { uint32_t struct_size; int32_t status; uint64_t revision; uint32_t completed_entries; uint32_t total_entries; uint32_t warning_count; uint32_t info_count; char error[512]; } daw_dawproject_status;
/* Exports a frozen session snapshot as DAWproject 1.0. The package embeds
 * project.xml, metadata.xml, loss-report.json, referenced WAV media and AU state.
 * Poll status: 0 running, 1 successful, 2 failed, 3 canceled. */
daw_dawproject_job* daw_begin_dawproject_export(daw_session*,const char* path,double tempo_bpm,uint32_t numerator,uint32_t denominator,const char* title);
int daw_poll_dawproject_export(daw_dawproject_job*,daw_dawproject_status*);
void daw_cancel_dawproject_export(daw_dawproject_job*);
void daw_release_dawproject_export(daw_dawproject_job*);
typedef struct { uint32_t struct_size; uint64_t revision; uint32_t track_count; int32_t can_undo; int32_t can_redo; double master_gain_db; uint32_t bus_count; uint32_t master_insert_count; } daw_snapshot;
typedef struct { uint32_t struct_size; uint64_t id; double gain_db; char name[481]; uint64_t audio_frames; uint32_t clip_count; double pan; int32_t muted; int32_t solo; uint32_t take_count; uint64_t output_bus_id; uint32_t send_count; uint32_t color; } daw_track;
typedef struct { uint32_t struct_size; uint64_t start; uint64_t source_offset; uint64_t length; uint64_t fade_in; uint64_t fade_out; uint32_t take_index; uint32_t color; double gain_db; uint32_t muted; uint32_t looped; double pan; } daw_clip;
typedef struct { uint32_t struct_size; uint32_t index; uint64_t start; uint64_t frames; char name[481]; } daw_take;
typedef struct { uint32_t struct_size; uint64_t id; double gain_db; double pan; int32_t muted; uint64_t output_bus_id; char name[481]; } daw_bus;
typedef struct { uint32_t struct_size; uint64_t bus_id; double gain_db; int32_t pre_fader; } daw_send;
typedef struct { uint32_t struct_size; uint64_t frame; double gain_db; } daw_automation_point;
/* Gesture target: 1=track volume, 2=track pan, 3=bus gain, 4=master gain.
 * mode: 1=touch, 2=latch. target_id is the track/bus ID, or zero for master.
 * A gesture buffers ordered samples privately and consumes exactly one project
 * revision at end. Latch writes the final value through end_frame; Touch only
 * writes supplied samples. Mutations/Undo are rejected while it is active. */
enum { DAW_AUTOMATION_TRACK_VOLUME=1, DAW_AUTOMATION_TRACK_PAN=2, DAW_AUTOMATION_BUS_GAIN=3, DAW_AUTOMATION_MASTER_GAIN=4 };
enum { DAW_AUTOMATION_TOUCH=1, DAW_AUTOMATION_LATCH=2 };
typedef struct { uint32_t struct_size; uint32_t type; uint32_t subtype; uint32_t manufacturer; char name[481]; } daw_au_component;
typedef struct { uint32_t struct_size; uint64_t id; uint32_t type; uint32_t subtype; uint32_t manufacturer; int32_t bypassed; uint32_t latency_frames; char name[481]; int32_t available; } daw_plugin;
/* Insert owner: master uses owner_id=0; track and bus use their durable IDs.
 * Generic calls cover all processing strips. The historic master-only calls
 * below remain available for existing callers. */
enum { DAW_INSERT_OWNER_MASTER=1, DAW_INSERT_OWNER_TRACK=2, DAW_INSERT_OWNER_BUS=3 };
/* Plug-in hosting policy is a durable per-insert setting.  The status ABI is
 * versioned independently so host capabilities can grow without changing the
 * insert enumeration ABI.  An unsupported requested mode is an error: the
 * bridge never silently substitutes in-process hosting. */
enum { DAW_INSERT_HOSTING_STATUS_VERSION = 1 };
enum { DAW_INSERT_HOSTING_MODE_IN_PROCESS = 1, DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS = 2 };
enum { DAW_INSERT_HOSTING_FORMAT_AUV2 = 1, DAW_INSERT_HOSTING_FORMAT_AUV3 = 2, DAW_INSERT_HOSTING_FORMAT_VST3 = 3 };
enum { DAW_INSERT_HOSTING_MODE_FLAG_IN_PROCESS = 1u << 0, DAW_INSERT_HOSTING_MODE_FLAG_OUT_OF_PROCESS = 1u << 1 };
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    uint32_t format;
    uint32_t selected_mode;
    uint32_t supported_modes;
} daw_insert_hosting_status;
/* Volatile execution state for a persisted insert. This ABI intentionally
 * complements (and never changes) daw_insert_hosting_status: hosting policy
 * is saved in the project, while this reports one prepared graph only. */
enum { DAW_INSERT_RUNTIME_STATUS_VERSION = 1 };
enum {
  DAW_INSERT_RUNTIME_UNPREPARED = 0,
  DAW_INSERT_RUNTIME_ACTIVE_IN_PROCESS = 1,
  DAW_INSERT_RUNTIME_ACTIVE_ISOLATED = 2,
  DAW_INSERT_RUNTIME_DRY_FALLBACK = 3,
  DAW_INSERT_RUNTIME_FAILED = 4
};
enum { DAW_INSERT_RUNTIME_FAULT_NONE = 0, DAW_INSERT_RUNTIME_FAULT_PREPARE_FAILED = 1,
       DAW_INSERT_RUNTIME_FAULT_RESTART_REQUIRED = 2,
       DAW_INSERT_RUNTIME_FAULT_DEADLINE_MISSED = 3,
       DAW_INSERT_RUNTIME_FAULT_PROTOCOL_ERROR = 4,
       DAW_INSERT_RUNTIME_FAULT_HELPER_EXITED = 5 };
typedef struct {
  uint32_t struct_size;
  uint32_t version;
  uint32_t state;
  uint32_t extra_pipeline_latency_frames;
  uint32_t fault_code;
} daw_insert_runtime_status;
typedef struct { uint32_t struct_size; uint32_t id; float minimum; float maximum; float value; float normalized_value; int32_t writable; int32_t logarithmic; int32_t indexed; char name[481]; } daw_au_parameter;
/* Plug-in parameter automation points are normalized values at 48 kHz project
 * frames. Owner uses DAW_INSERT_OWNER_*; Master owner_id remains zero. */
typedef struct { uint32_t struct_size; uint64_t frame; double normalized_value; } daw_plugin_parameter_automation_point;
typedef struct { uint32_t struct_size; int32_t status; uint32_t available_count; uint32_t quarantined_count; char error[512]; } daw_au_scan_status;
/* VST3 metadata is bounded and comes exclusively from the isolated helper.
 * class_id is a canonical 32-hex FUID; module_fingerprint is SHA-256 hex.
 * flags is a bit set of scanner-confirmed class capabilities. It is
 * meaningful only when available != 0; unused bits are always reported as
 * zero, so callers must mask, never compare the field for equality.
 * DAW_VST3_FLAG_INSTRUMENT marks a class whose sub-categories declare
 * "Instrument". The struct_size gate rejects buffers from a pre-flags build. */
enum { DAW_VST3_FLAG_INSTRUMENT = 1u };
typedef struct { uint32_t struct_size; int32_t available; uint32_t flags; char class_id[33]; char module_fingerprint[65]; char module_path[4097]; char name[481]; char vendor[481]; char version[257]; char quarantine_reason[513]; } daw_vst3_component;
typedef struct { uint32_t struct_size; int32_t status; uint32_t available_count; uint32_t quarantined_count; char error[512]; } daw_vst3_scan_status;
/* kind: 1 rename track, 2 set track gain. */
typedef struct { uint32_t struct_size; int32_t kind; uint64_t track_id; double gain_db; char name[481]; } daw_workflow_operation;
typedef struct { uint32_t struct_size; uint64_t track_id; double before_gain_db; double after_gain_db; char before_name[481]; char after_name[481]; } daw_workflow_change;
typedef struct { uint32_t struct_size; uint64_t track_id; uint64_t analyzed_frames; double peak_db; double rms_db; double proposed_gain_db; double predicted_peak_db; double predicted_rms_db; int32_t peak_limited; char before_name[481]; char after_name[481]; } daw_vocal_preview;
typedef struct { uint32_t struct_size; int32_t playing; uint64_t frame; uint64_t duration; uint64_t callbacks; uint64_t clipped_frames; float peak; int32_t loop_enabled; uint64_t loop_start; uint64_t loop_end; uint64_t plugin_errors; uint32_t output_latency_frames; } daw_transport;
/* Per-channel post-insert/post-fader linear peaks. version is currently 1;
 * callers set struct_size and receive the version used to populate the value.
 * owner uses DAW_INSERT_OWNER_*; master owner_id remains zero. */
enum { DAW_CHANNEL_METER_VERSION = 1 };
typedef struct { uint32_t struct_size; uint32_t version; float left_peak; float right_peak; } daw_channel_meter;
/* Live BS.1770-4 master loudness while an output or duplex device is running:
 * momentary (400 ms) and short-term (3 s) windows in LUFS, updated at 100 ms
 * block boundaries by the audio thread. -200 means fully quiet or unmeasured;
 * live is 0 when no renderer is playing and both values report -200. */
enum { DAW_MASTER_LOUDNESS_VERSION = 1 };
typedef struct { uint32_t struct_size; uint32_t version; float momentary_lufs; float short_term_lufs; int32_t live; } daw_master_loudness;
/* state: 0 idle, 1 running, 2 stopped, 3 device lost/changed, 4 stalled, 5 callback buffer error. */
/* Output states: 0 idle, 1 running, 2 stopped, 3 device lost, 4 stalled,
 * 5 callback error, 6 render graph preparing, 7 preparation failed. */
enum { DAW_OUTPUT_IDLE=0, DAW_OUTPUT_RUNNING=1, DAW_OUTPUT_STOPPED=2, DAW_OUTPUT_DEVICE_LOST=3, DAW_OUTPUT_STALLED=4, DAW_OUTPUT_CALLBACK_ERROR=5, DAW_OUTPUT_PREPARING=6, DAW_OUTPUT_PREPARATION_FAILED=7 };
typedef struct { uint32_t struct_size; int32_t state; uint32_t device_id; uint64_t generation; uint64_t callbacks; uint64_t callback_errors; } daw_output_status;
typedef struct { uint32_t struct_size; int32_t recording; int32_t overflowed; uint64_t frames; uint64_t callbacks; uint64_t target_track_id; int32_t loop_recording; uint32_t pass_count; } daw_recording;
#define DAW_RECORDING_PROGRESS_VERSION 1
/* Captured-frame clock, NOT hardware/PDC latency compensation. Separate from
 * legacy daw_recording so old structure layouts remain valid. Idle = zeros. */
typedef struct {
    uint32_t struct_size, version;
    uint64_t capacity_frames, preroll_remaining_frames, timeline_frame;
    int32_t limit_reached;
} daw_recording_progress;
/* Background PCM-WAV import. A job owns only a source path plus immutable
 * intent; it never retains a session. Poll and cancel are thread-safe while
 * the caller retains the handle; release must be serialized with all handle
 * uses and invalidates it. Apply must happen
 * on the session's owner/control thread. status: 0=running, 1=ready,
 * 2=failed, 3=canceled, 4=applied. phase: 0=none, 1=reading, 2=decoding,
 * 3=converting, 4=ready.  base_revision is captured at begin; source fields
 * describe the validated input, while output_frames describes fixed 48 kHz
 * stereo project audio. All text is copied into caller storage. */
enum { DAW_IMPORT_STATUS_VERSION = 1 };
enum { DAW_IMPORT_RUNNING=0, DAW_IMPORT_READY=1, DAW_IMPORT_FAILED=2, DAW_IMPORT_CANCELED=3, DAW_IMPORT_APPLIED=4 };
enum { DAW_IMPORT_PHASE_NONE=0, DAW_IMPORT_PHASE_READING=1, DAW_IMPORT_PHASE_DECODING=2, DAW_IMPORT_PHASE_CONVERTING=3, DAW_IMPORT_PHASE_READY=4 };
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    int32_t status;
    int32_t phase;
    uint64_t base_revision;
    uint32_t progress;
    uint32_t source_sample_rate;
    uint32_t source_channels;
    uint64_t source_frames;
    uint64_t output_frames;
    char error[512];
} daw_import_status;
/* Begin validates arguments and captures the session identity, project epoch,
 * and optimistic base revision without mutating the project. Apply performs
 * exactly one model mutation only after the job is ready and all three guards
 * still match. expected_revision is checked at apply time; base_revision is
 * informational and a failed optimistic apply leaves a ready job retryable. */
daw_import_job* daw_begin_import_wav(daw_session*,const char* path,const char* name,uint64_t base_revision);
daw_import_job* daw_begin_import_take_wav(daw_session*,uint64_t track_id,const char* path,const char* name,uint64_t start_frame,uint64_t base_revision);
daw_import_job* daw_begin_import_aiff(daw_session*,const char* path,const char* name,uint64_t base_revision);
daw_import_job* daw_begin_import_take_aiff(daw_session*,uint64_t track_id,const char* path,const char* name,uint64_t start_frame,uint64_t base_revision);
int daw_poll_import(daw_import_job*,daw_import_status*);
void daw_cancel_import(daw_import_job*);
int daw_apply_import(daw_session*,daw_import_job*,uint64_t expected_revision);
/* Release asks a running worker to cancel and returns immediately. */
void daw_release_import(daw_import_job*);
int daw_import_wav(daw_session*, const char* path, const char* name, uint64_t expected_revision);
int daw_import_take_wav(daw_session*,uint64_t track_id,const char* path,const char* name,uint64_t start_frame,uint64_t expected_revision);
int daw_import_aiff(daw_session*, const char* path, const char* name, uint64_t expected_revision);
int daw_import_take_aiff(daw_session*,uint64_t track_id,const char* path,const char* name,uint64_t start_frame,uint64_t expected_revision);
int daw_get_take(daw_session*,uint64_t track_id,uint32_t take_index,daw_take*);
int daw_get_take_waveform(daw_session*,uint64_t track_id,uint32_t take_index,float* peaks,uint32_t count);
int daw_comp_range(daw_session*,uint64_t track_id,uint32_t take_index,uint64_t start_frame,uint64_t end_frame,uint64_t expected_revision);
/* Seek stops playback; position is ephemeral and does not create a revision.
 * Play starts from current position (at EOF restarts from zero). Stop retains position. */
int daw_get_clip(daw_session*, uint64_t track_id, uint32_t clip_index, daw_clip*);
int daw_edit_clip(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t start, uint64_t offset, uint64_t length, uint64_t expected_revision);
int daw_edit_clip_full(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t start, uint64_t offset, uint64_t length, uint64_t fade_in, uint64_t fade_out, uint64_t expected_revision);
int daw_split_clip(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t frame, uint64_t expected_revision);
int daw_duplicate_clip(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t expected_revision);
int daw_delete_clip(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t expected_revision);
int daw_set_clip_fades(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t fade_in, uint64_t fade_out, uint64_t expected_revision);
int daw_set_crossfade(daw_session*,uint64_t track_id,uint32_t left_clip_index,uint64_t duration,uint64_t expected_revision);
/* Clip/track editing surface (project schema v19). Colors are 24-bit RGB
 * packed as 0xRRGGBB; 0 means "no user color". Clip gain is dB at the mix-in
 * with the same -60..12 dB clamp the domain applies to track gain. All of
 * these follow the usual rules: expected_revision, silent no-op on an
 * identical value, one revision and one Undo entry per committed command. */
int daw_set_track_color(daw_session*,uint64_t track_id,uint32_t color,uint64_t expected_revision);
/* Duplicates the whole track (clips, takes, routing, automation, inserts and
 * MIDI clips) in one revision; the fresh track ID is reported via out_new_id. */
int daw_duplicate_track(daw_session*,uint64_t track_id,uint64_t* out_new_id,uint64_t expected_revision);
int daw_set_clip_color(daw_session*,uint64_t track_id,uint32_t clip_index,uint32_t color,uint64_t expected_revision);
int daw_set_clip_gain(daw_session*,uint64_t track_id,uint32_t clip_index,double gain_db,uint64_t expected_revision);
/* Clip playback state (schema v20): mute drops the region's voice from the
 * render plan entirely; loop lets a region outlive its slice, wrapping reads
 * inside [source_offset, source frames). Revision-checked; identical values
 * are silent no-ops. Un-looping a region longer than its slice rejects. */
int daw_set_clip_muted(daw_session*,uint64_t track_id,uint32_t clip_index,uint32_t muted,uint64_t expected_revision);
int daw_set_clip_looped(daw_session*,uint64_t track_id,uint32_t clip_index,uint32_t looped,uint64_t expected_revision);
/* Clip pan, normalized -1 (left) .. 1 (right); linear law with unity center,
 * identical to the track pan. */
int daw_set_clip_pan(daw_session*,uint64_t track_id,uint32_t clip_index,double pan,uint64_t expected_revision);
/* Group operations on one track's multi-selection: the whole group commits in
 * a single revision. Indices must name existing clip positions on the track;
 * nudge translates together and re-sorts, validate() owns the aftermath. */
int daw_delete_clips(daw_session*,uint64_t track_id,const uint32_t* clip_indices,uint32_t index_count,uint64_t expected_revision);
int daw_nudge_clips(daw_session*,uint64_t track_id,const uint32_t* clip_indices,uint32_t index_count,int64_t delta_frames,uint64_t expected_revision);
int daw_seek_frame(daw_session*, uint64_t frame);
/* Loop is ephemeral transport state over a half-open [start,end) frame range.
 * Changing it stops playback. Pass enabled=0 to clear; start/end are ignored. */
int daw_set_loop(daw_session*, int32_t enabled, uint64_t start_frame, uint64_t end_frame);
/* Copies exactly 512 cached, linear, pre-gain stereo absolute peaks. Non-RT. */
int daw_get_waveform(daw_session*, uint64_t track_id, float* peaks, uint32_t count);
/* Starts bounded asynchronous graph preparation. Poll daw_get_transport on
 * the owning control thread; it atomically claims the current prepared graph
 * and starts hardware only if project revision and transport intent still match. */
int daw_play(daw_session*);
int daw_stop(daw_session*);
/* Poll on control thread; detects output device changes and missing callbacks. */
int daw_get_transport(daw_session*, daw_transport*);
/* Reads live channel telemetry only. If no renderer is playing (including a
 * stopped session), both peaks are zero. It never changes project state. */
int daw_get_channel_meter(daw_session*,int32_t owner,uint64_t owner_id,daw_channel_meter*);
int daw_get_master_loudness(daw_session*,daw_master_loudness*);
int daw_get_output_status(daw_session*, daw_output_status*);
/* All recording uses one AUHAL duplex callback: dry mono to disk, project
 * playback/click and optional direct MON to the stereo output. At most 60 s,
 * 48 kHz; one I/O or aggregate device required, no silent input-only fallback.
 * An armed take with an enabled loop commits each pass in one revision;
 * otherwise Stop adds one track/take. Pre-roll is excluded from captured media.
 * Stop before any capture is an empty successful no-op. Stop requires the
 * revision captured at start; cancel preserves confirmed recovery media.
 * The app must obtain microphone permission before calling start. */
int daw_record_start(daw_session*, uint64_t start_frame, const char* recovery_path);
int daw_record_start_take(daw_session*,uint64_t track_id,uint64_t start_frame,const char* recovery_path);
int daw_record_stop(daw_session*, const char* name, uint64_t expected_revision);
int daw_record_cancel(daw_session*);
int daw_get_recording(daw_session*, daw_recording*);
int daw_get_recording_progress(daw_session*, daw_recording_progress*);
/* Imports the confirmed part of a stale .mydawtake. File removal is attempted
 * only after the model commit succeeds. */
int daw_recover_take(daw_session*, const char* path, const char* name, uint64_t expected_revision);
daw_session* daw_create(void);
void daw_destroy(daw_session* session);
int daw_get_snapshot(daw_session*, daw_snapshot*);
int daw_get_track(daw_session*, uint32_t index, daw_track*);
int daw_add_track(daw_session*, const char* name, uint64_t expected_revision);
/* Removes one complete track in a single revision.  Its clips, takes, sends,
 * automation and inserts are removed with it; Undo restores the exact track. */
int daw_remove_track(daw_session*, uint64_t track_id, uint64_t expected_revision);
/* Moves track_id to zero-based new_index in the resulting track order. The
 * index must be less than the current track count; a same-position move is a
 * successful no-op and does not advance the revision. */
int daw_move_track(daw_session*, uint64_t track_id, uint32_t new_index, uint64_t expected_revision);
int daw_rename_track(daw_session*, uint64_t id, const char* name, uint64_t expected_revision);
/* Scalar mixer gesture; one history entry at end, zero at cancel/no-op.
 * begin/end take the same revision. Snapshot/save read committed state during
 * preview. Mutations and nested gestures reject until end/cancel. Main-thread
 * API: previews publish atomic targets to an already prepared renderer. */
enum { DAW_MIXER_TRACK_GAIN=1, DAW_MIXER_TRACK_PAN=2, DAW_MIXER_BUS_GAIN=3,
       DAW_MIXER_MASTER_GAIN=4, DAW_MIXER_BUS_PAN=5, DAW_MIXER_SEND_GAIN=6, DAW_MIXER_SEND_PAN=7 };
int daw_begin_mixer_gesture(daw_session*,int32_t target,uint64_t id,uint64_t send_bus_id,uint64_t expected_revision);
/* Linked static track faders: 2..256 unique existing IDs, no volume automation.
 * The input array is copied; no pointer is retained. Master and buses reject.
 * After this begin, daw_write_mixer_gesture takes a dB DELTA from gesture start
 * (not an incremental delta or an absolute level). One common clamp preserves
 * relative gains. Use existing end/cancel; no project format change. */
int daw_begin_track_gain_group(daw_session*,const uint64_t* track_ids,uint32_t count,uint64_t expected_revision);
int daw_write_mixer_gesture(daw_session*,double value);
int daw_end_mixer_gesture(daw_session*,uint64_t expected_revision);
void daw_cancel_mixer_gesture(daw_session*);
int daw_set_gain(daw_session*, uint64_t id, double gain_db, uint64_t expected_revision);
int daw_set_pan(daw_session*,uint64_t id,double pan,uint64_t expected_revision);
int daw_set_mute(daw_session*,uint64_t id,int32_t muted,uint64_t expected_revision);
/* Exclusive solo (id=0,solo=0 clears all): one revision, one Undo entry. */
int daw_set_solo_exclusive(daw_session*,uint64_t id,int32_t solo,uint64_t expected_revision);
int daw_set_solo(daw_session*,uint64_t id,int32_t solo,uint64_t expected_revision);
int daw_set_master_gain(daw_session*,double gain_db,uint64_t expected_revision);
int daw_add_bus(daw_session*,const char* name,uint64_t expected_revision);
int daw_get_bus(daw_session*,uint32_t index,daw_bus*);
int daw_rename_bus(daw_session*,uint64_t bus_id,const char* name,uint64_t expected_revision);
int daw_set_bus_gain(daw_session*,uint64_t bus_id,double gain_db,uint64_t expected_revision);
int daw_set_bus_pan(daw_session*,uint64_t bus_id,double pan,uint64_t expected_revision);
int daw_set_bus_mute(daw_session*,uint64_t bus_id,int32_t muted,uint64_t expected_revision);
/* output_bus_id=0 means Master. Cycles and missing destinations are rejected atomically. */
int daw_set_track_output(daw_session*,uint64_t track_id,uint64_t output_bus_id,uint64_t expected_revision);
/* Track grouping: buses are the folders. Creates a fresh bus (name gates
 * like track names), reports its id, and enumerates buses for the UI. */
int daw_create_bus(daw_session*,const char* name,uint64_t* out_bus_id,uint64_t expected_revision);
int daw_get_bus_count(daw_session*,uint32_t* count);
int daw_delete_bus(daw_session*,uint64_t bus_id,uint64_t expected_revision);
int daw_set_bus_output(daw_session*,uint64_t bus_id,uint64_t output_bus_id,uint64_t expected_revision);
int daw_get_send(daw_session*,uint64_t track_id,uint32_t index,daw_send*);
/* Additive send-controls ABI. Get by stable bus ID, not a visible row index.
 * Callers set struct_size; version is returned. No borrowed memory escapes.
 * pan is unity-center stereo balance -1..+1, not an equal-power mono panner.
 * independent_pan=0 preserves historic PRE/POST behavior; =1 bypasses track
 * balance on this send only (POST still follows track fader automation).
 * Muting preserves the send level/route/tap/pan. Setters require existing sends,
 * reject non-boolean flags, stale revisions and active recording, commit one
 * Undo entry or no-op, and publish smoothed targets without stopping playback.
 * The legacy upsert API preserves these controls on an existing send. */
enum { DAW_SEND_CONTROLS_VERSION=1 };
typedef struct { uint32_t struct_size; uint32_t version; double pan; int32_t muted; int32_t independent_pan; } daw_send_controls;
int daw_get_send_controls(daw_session*,uint64_t track_id,uint64_t bus_id,daw_send_controls*);
int daw_set_send_muted(daw_session*,uint64_t track_id,uint64_t bus_id,int32_t muted,uint64_t expected_revision);
int daw_set_send_pan(daw_session*,uint64_t track_id,uint64_t bus_id,double pan,int32_t independent_pan,uint64_t expected_revision);
int daw_upsert_send(daw_session*,uint64_t track_id,uint64_t bus_id,double gain_db,int32_t pre_fader,uint64_t expected_revision);
int daw_remove_send(daw_session*,uint64_t track_id,uint64_t bus_id,uint64_t expected_revision);
/* Automation points are ordered 48 kHz project-frame fader values. Upsert
 * replaces a point at the same frame; an empty lane means static track gain. */
int daw_get_track_volume_automation_count(daw_session*,uint64_t track_id,uint32_t* count);
int daw_get_track_volume_automation_point(daw_session*,uint64_t track_id,uint32_t index,daw_automation_point*);
int daw_upsert_track_volume_automation_point(daw_session*,uint64_t track_id,uint64_t frame,double gain_db,uint64_t expected_revision);
int daw_remove_track_volume_automation_point(daw_session*,uint64_t track_id,uint64_t frame,uint64_t expected_revision);
int daw_get_track_pan_automation_count(daw_session*,uint64_t track_id,uint32_t* count);
int daw_get_track_pan_automation_point(daw_session*,uint64_t track_id,uint32_t index,daw_automation_point*);
int daw_upsert_track_pan_automation_point(daw_session*,uint64_t track_id,uint64_t frame,double pan,uint64_t expected_revision);
int daw_remove_track_pan_automation_point(daw_session*,uint64_t track_id,uint64_t frame,uint64_t expected_revision);
int daw_get_bus_gain_automation_count(daw_session*,uint64_t bus_id,uint32_t* count);
int daw_get_bus_gain_automation_point(daw_session*,uint64_t bus_id,uint32_t index,daw_automation_point*);
int daw_upsert_bus_gain_automation_point(daw_session*,uint64_t bus_id,uint64_t frame,double gain_db,uint64_t expected_revision);
int daw_remove_bus_gain_automation_point(daw_session*,uint64_t bus_id,uint64_t frame,uint64_t expected_revision);
int daw_get_master_gain_automation_count(daw_session*,uint32_t* count);
int daw_get_master_gain_automation_point(daw_session*,uint32_t index,daw_automation_point*);
int daw_upsert_master_gain_automation_point(daw_session*,uint64_t frame,double gain_db,uint64_t expected_revision);
int daw_remove_master_gain_automation_point(daw_session*,uint64_t frame,uint64_t expected_revision);
/* Buffered automation write gestures (Touch/Latch). One gesture may be open per
 * session, so write carries no expected_revision: the open gesture IS the
 * mutation guard — while it is open, every other mutating call is rejected and
 * reads, snapshot and seeking stay free of charge. begin consumes no revision,
 * end consumes exactly one. end_frame must not precede the last accepted sample;
 * a rejected end leaves the gesture OPEN and it must be cancelled explicitly.
 * A rejected per-sample value does not move the gesture's ordering cursor, and a
 * take whose values all already match the lane still commits one revision — the
 * silent no-op belongs to the single-point upsert commands, not to gestures.
 * Ranges: -120..+24 dB for volume/bus/master lanes and -1..1 for the pan lane
 * (carried in gain_db); 2048 points per lane, 10-minute timeline ceiling.
 * See docs/44-automation-write-pdc-plan.md and tests/e2e/e2e_automation.cpp. */
int daw_begin_automation_gesture(daw_session*,int32_t target,uint64_t target_id,int32_t mode,uint64_t expected_revision);
int daw_write_automation_gesture(daw_session*,uint64_t frame,double value);
int daw_end_automation_gesture(daw_session*,uint64_t end_frame,uint64_t expected_revision);
void daw_cancel_automation_gesture(daw_session*);
/* First host slice: explicit scan returns only the approved Apple AU matrix.
 * Instantiation/state capture happens on the control thread; arbitrary third-party
 * discovery remains delegated to the disposable scanner helper. */
int daw_scan_supported_au(daw_session*,uint32_t* count);
/* Scans all installed effects in disposable helper processes. Poll status:
 * 0 running, 1 complete, 2 helper failure. Apply replaces the session catalog. */
daw_au_scan_job* daw_begin_installed_au_scan(const char* helper_path,uint32_t timeout_ms);
int daw_poll_installed_au_scan(daw_au_scan_job*,daw_au_scan_status*);
int daw_apply_installed_au_scan(daw_session*,daw_au_scan_job*,uint32_t* available_count,uint32_t* quarantined_count);
int daw_save_installed_au_scan_cache(daw_session*,daw_au_scan_job*,const char* cache_path);
int daw_load_installed_au_scan_cache(daw_session*,const char* helper_path,const char* cache_path,uint32_t* available_count,uint32_t* quarantined_count,uint32_t* invalidated_count);
void daw_release_installed_au_scan(daw_au_scan_job*);
/* Each VST3 module and probe runs in its own disposable helper process.
 * Poll status: 0 running, 1 complete, 2 helper failure. The cache loader
 * re-enumerates metadata before accepting an old verdict. */
daw_vst3_scan_job* daw_begin_installed_vst3_scan(const char* helper_path,uint32_t timeout_ms);
int daw_poll_installed_vst3_scan(daw_vst3_scan_job*,daw_vst3_scan_status*);
int daw_apply_installed_vst3_scan(daw_session*,daw_vst3_scan_job*,uint32_t* available_count,uint32_t* quarantined_count);
int daw_save_installed_vst3_scan_cache(daw_vst3_scan_job*,const char* cache_path);
int daw_load_installed_vst3_scan_cache(daw_session*,const char* helper_path,const char* cache_path,uint32_t* available_count,uint32_t* quarantined_count,uint32_t* invalidated_count);
void daw_release_installed_vst3_scan(daw_vst3_scan_job*);
int daw_get_installed_vst3(daw_session*,uint32_t index,daw_vst3_component*);
int daw_get_installed_vst3_count(daw_session*,uint32_t* count);
/* Adds an available cached VST3 class as a master insert. The host captures
 * its initial component/controller state before the project mutation. */
int daw_add_master_vst3(daw_session*,uint32_t catalog_index,uint64_t expected_revision);
int daw_get_supported_au(daw_session*,uint32_t index,daw_au_component*);
int daw_add_master_au(daw_session*,uint32_t type,uint32_t subtype,uint32_t manufacturer,uint64_t expected_revision);
int daw_get_master_insert(daw_session*,uint32_t index,daw_plugin*);
int daw_get_master_insert_parameter_count(daw_session*,uint64_t plugin_id,uint32_t* count);
int daw_get_master_insert_parameter(daw_session*,uint64_t plugin_id,uint32_t index,daw_au_parameter*);
int daw_set_master_insert_parameter(daw_session*,uint64_t plugin_id,uint32_t parameter_id,float native_value,uint64_t expected_revision);
int daw_set_master_insert_bypass(daw_session*,uint64_t id,int32_t bypassed,uint64_t expected_revision);
int daw_move_master_insert(daw_session*,uint64_t id,uint32_t new_index,uint64_t expected_revision);
int daw_remove_master_insert(daw_session*,uint64_t id,uint64_t expected_revision);
int daw_add_insert_vst3(daw_session*,int32_t owner,uint64_t owner_id,uint32_t catalog_index,uint64_t expected_revision);
int daw_add_insert_au(daw_session*,int32_t owner,uint64_t owner_id,uint32_t type,uint32_t subtype,uint32_t manufacturer,uint64_t expected_revision);
int daw_get_insert_count(daw_session*,int32_t owner,uint64_t owner_id,uint32_t* count);
int daw_get_insert(daw_session*,int32_t owner,uint64_t owner_id,uint32_t index,daw_plugin*);
/* Reads the saved policy and the modes this build can actually host for one
 * insert. format distinguishes AUv2 from AUv3 using the native component
 * capability where available; VST3 out-of-process mode is advertised only
 * when the isolated runtime is linked into this build. */
int daw_get_insert_hosting_status(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,daw_insert_hosting_status*);
/* Reads only the live prepared graph. With no active graph it returns
 * UNPREPARED + RESTART_REQUIRED; it never changes project revision. */
int daw_get_insert_runtime_status(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,daw_insert_runtime_status*);
/* mode uses DAW_INSERT_HOSTING_MODE_*.  Out-of-process mode is rejected when
 * supported_modes does not advertise it, leaving the project unchanged. */
int daw_set_insert_hosting_mode(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t mode,uint64_t expected_revision);
int daw_get_insert_parameter_count(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t* count);
int daw_get_insert_parameter(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t index,daw_au_parameter*);
int daw_set_insert_parameter(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t parameter_id,float native_value,uint64_t expected_revision);
int daw_set_insert_bypass(daw_session*,int32_t owner,uint64_t owner_id,uint64_t id,int32_t bypassed,uint64_t expected_revision);
int daw_move_insert(daw_session*,int32_t owner,uint64_t owner_id,uint64_t id,uint32_t new_index,uint64_t expected_revision);
int daw_remove_insert(daw_session*,int32_t owner,uint64_t owner_id,uint64_t id,uint64_t expected_revision);
int daw_get_insert_parameter_automation_count(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t parameter_id,uint32_t* count);
int daw_get_insert_parameter_automation_point(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t parameter_id,uint32_t index,daw_plugin_parameter_automation_point*);
int daw_upsert_insert_parameter_automation_point(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t parameter_id,const char* parameter_name,uint64_t frame,double normalized_value,uint64_t expected_revision);
int daw_remove_insert_parameter_automation_point(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t parameter_id,uint64_t frame,uint64_t expected_revision);
/* One session-wide buffered parameter gesture. Values are normalized 0…1;
 * mode uses DAW_AUTOMATION_TOUCH or DAW_AUTOMATION_LATCH. */
int daw_begin_insert_parameter_automation_gesture(daw_session*,int32_t owner,uint64_t owner_id,uint64_t plugin_id,uint32_t parameter_id,const char* parameter_name,int32_t mode,uint64_t expected_revision);
int daw_write_insert_parameter_automation_gesture(daw_session*,uint64_t frame,double normalized_value);
int daw_end_insert_parameter_automation_gesture(daw_session*,uint64_t end_frame,uint64_t expected_revision);
void daw_cancel_insert_parameter_automation_gesture(daw_session*);
/* MIDI clips are data-only until the renderer arc publishes them: notes are
 * clip-relative 48 kHz frames, pitch 0..127, channel 0..15, velocity 1..127.
 * Every call validates struct_size/version and rejects obviously invalid
 * notes before touching the domain; the domain owns full timeline validation. */
enum { DAW_MIDI_NOTE_VERSION = 1 };
typedef struct { uint32_t struct_size; uint32_t version; uint64_t start; uint64_t length; uint8_t pitch; uint8_t channel; uint8_t velocity; } daw_midi_note;
enum { DAW_MIDI_CLIP_VERSION = 2 };
/* lane is the editor row of the clip, not a track reference. note_count is
 * only filled by the caller when supplying the notes array to add. Version 2
 * carries the clip color (RGB packed in the low 24 bits; 0 means unset). */
typedef struct { uint32_t struct_size; uint32_t version; uint64_t start; uint64_t length; int32_t lane; uint32_t note_count; uint32_t color; } daw_midi_clip;
/* A read of at most 8192 notes per call; note_offset pages larger clips. */
enum { DAW_MIDI_NOTES_PER_CALL = 8192 };
/* Full replacement is atomic, up to the project-wide note budget. It must
 * not be split into append calls (which would create multiple Undo entries).
 * Add/append batches and paged reads retain their 8192-note call limit. */
enum { DAW_MIDI_NOTES_PER_REPLACEMENT = 65536 };
/* Adds one clip (with its whole note array) to the track in one revision. */
int daw_add_midi_clip(daw_session*, uint64_t track_id, const daw_midi_clip* clip, const daw_midi_note* notes, uint32_t note_count, uint64_t expected_revision);
int daw_remove_midi_clip(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t expected_revision);
/* Replaces the full note array of one clip; empty notes clears the clip.
 * At most DAW_MIDI_NOTES_PER_REPLACEMENT notes; the domain additionally checks
 * the total across all clips. Oversized count is rejected before reading notes. */
int daw_set_midi_notes(daw_session*, uint64_t track_id, uint32_t clip_index, const daw_midi_note* notes, uint32_t note_count, uint64_t expected_revision);
/* Appends the batch after the clip's existing notes in one revision — the
 * manual-edit counterpart of the live-capture stop path. The clip window
 * never grows; a note that no longer fits whole rejects the whole batch. */
int daw_append_midi_notes(daw_session*, uint64_t track_id, uint32_t clip_index, const daw_midi_note* notes, uint32_t note_count, uint64_t expected_revision);
/* Clip color, semitone transposition (each pitch clamps to 0..127) and grid
 * quantization: note starts snap to the nearest multiple of grid_beats on the
 * tempo map; grid_beats must be > 0 (0.25 = 1/16 in 4/4 terms). */
int daw_set_midi_clip_color(daw_session*, uint64_t track_id, uint32_t clip_index, uint32_t color, uint64_t expected_revision);
int daw_transpose_midi_clip(daw_session*, uint64_t track_id, uint32_t clip_index, int32_t semitones, uint64_t expected_revision);
int daw_quantize_midi_clip(daw_session*, uint64_t track_id, uint32_t clip_index, double grid_beats, uint64_t expected_revision);
int daw_move_midi_clip(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t new_start, uint64_t expected_revision);
/* Growing shifts clip-relative notes with the window; shrinking drops notes
 * that no longer fit whole. Both parts after a split stay within bounds. */
int daw_trim_midi_clip(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t new_start, uint64_t new_length, uint64_t expected_revision);
int daw_split_midi_clip(daw_session*, uint64_t track_id, uint32_t clip_index, uint64_t at_frame, uint64_t expected_revision);
/* Number of MIDI clips on the track; independent of audio clip_count. */
int daw_get_midi_clip_count(daw_session*, uint64_t track_id, uint32_t* count);
/* Reads clip metadata and a page of notes. out->note_count reports the total;
 * *written reports notes copied for this page. Pass notes=NULL/capacity=0 to
 * query metadata only. capacity above DAW_MIDI_NOTES_PER_CALL is an error. */
int daw_get_midi_clip(daw_session*, uint64_t track_id, uint32_t clip_index, daw_midi_clip* out, uint32_t note_offset, daw_midi_note* notes, uint32_t capacity, uint32_t* written);
/* ---------------------------------------------------------------------------
 * Live MIDI input capture and metronome monitoring (macOS capture v0).
 *
 * PROTOTYPE LIMIT: a session owns at most ONE open capture source. Its id is
 * reported by daw_midi_input_active; daw_set_midi_input with another id closes
 * the previous source first. Per-source armed tracks are later work.
 *
 * Recording is a poll loop, never a callback: the UI timer calls
 * daw_midi_record_poll, which drains the CoreMIDI ring, converts each event to
 * a project frame and feeds the take buffer — committing nothing. Only
 * daw_midi_record_stop commits, as one Session::appendMidiNotes command under
 * the revision current at stop, so the caller refreshes exactly as after any
 * other command. Arming takes no revision, poll takes no revision, and an
 * empty take stops as a silent rc=0 no-op.
 *
 * V0 TIMING ACCURACY: a note's frame is computed at DRAIN time, not at the
 * moment the key was pressed. The base is the (transport frame, host clock)
 * pair sampled at the previous poll or at arming, advanced by the event's
 * CoreMIDI host-time delta; that base is up to one UI poll interval (100 ms)
 * stale and it jumps whenever the transport starts, seeks, stops or wraps a
 * loop between polls. The base frame is the audible playhead — the value
 * daw_get_transport reports — so RT graph latency is not compensated either.
 * Frames are clip-relative: the target clip start is subtracted, and a take
 * begun before the clip opens clamps onto frame 0. No beat snapping happens at
 * stop; that is a later UI pass over the finished batch.
 *
 * V0 KNOWN LOSS: a note that does not fit whole inside the target clip fails
 * validate() and the whole batch is refused (rc=1, the take is discarded).
 * Stopping before the clip edge, or lengthening the clip first, is the
 * caller's job until a truncating pass exists.
 * --------------------------------------------------------------------------- */
enum { DAW_MIDI_DEVICE_VERSION = 1 };
/* uniqueID is the value daw_set_midi_input expects; name is display text
 * copied into caller storage; online is the owning device's online flag. */
typedef struct { uint32_t struct_size; uint32_t version; uint32_t uniqueID; int32_t online; char name[129]; } daw_midi_device;
enum { DAW_MIDI_RECORD_STATUS_VERSION = 1 };
/* Counters of the current take, all zero when nothing is armed. open_notes is
 * keys no note-off has closed yet, recorded the notes already closed, dropped
 * everything the take refused to hold — malformed or over-budget events plus,
 * since arming, the packets the 4096-deep capture ring overflowed on — and
 * unmatched the note-offs no open key answers, which is what a key held across
 * the arming looks like from the converter's side. */
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    int32_t armed;
    uint64_t open_notes;
    uint64_t recorded;
    uint64_t dropped;
    uint64_t unmatched;
} daw_midi_record_status_t;
/* Enumerates live CoreMIDI sources. The count call refreshes the list, so the
 * indexed reads report that same snapshot; a headless system reports 0. */
int daw_get_midi_input_device_count(daw_session*, uint32_t* count);
int daw_get_midi_input_device(daw_session*, uint32_t index, daw_midi_device* out);
/* Opens uniqueID, closing any previously open source, or closes the current
 * one for 0. Closing with nothing open is a successful no-op. Switching is
 * refused while a take is armed. 1 means the id is unknown or CoreMIDI
 * refused, which leaves the previously open input untouched. */
int daw_set_midi_input(daw_session*, uint32_t uniqueID);
int daw_midi_input_active(daw_session*, uint32_t* uniqueID);
/* Arms a take into the MIDI clip at clipIndex of track_id. Requires an open
 * input and an existing clip; otherwise it returns 1 and changes nothing.
 * Re-arming abandons the previous take. Never consumes a revision. */
int daw_midi_record_arm(daw_session*, uint64_t track_id, uint32_t clip_index);
/* Drains the capture ring into the armed take. A no-op with no open input or
 * no armed take. Commits nothing. */
int daw_midi_record_poll(daw_session*);
/* Stops the take at the current transport frame and appends its notes to the
 * target clip under the current revision: success therefore means a new
 * revision. With nothing armed, or with an empty take, it is a silent rc=0
 * no-op and the revision does not move. */
int daw_midi_record_stop(daw_session*);
int daw_midi_record_status(daw_session*, daw_midi_record_status_t* out);
/* Metronome monitoring. The session controller holds the flag: it is pushed to
 * a live playback or duplex renderer at once and applied to the next graph this
 * session prepares, so a play started later still clicks. It is never
 * persisted, and export render always suppresses the click. */
/* Pre-roll for ordinary and loop recording: the transport rolls this many frames before
 * the punch-in point (with the metronome, when enabled) and those frames are
 * discarded from the take. Clamped at project start; 0 disables pre-roll,
 * up to 30 seconds. Changes during capture are rejected. */
int daw_set_record_preroll(daw_session*, uint64_t preroll_frames);
int daw_get_record_preroll(daw_session*, uint64_t* preroll_frames);
/* Direct input monitor during ordinary and loop recording: dry mono input
 * is summed after project/master FX with a 240-frame ramp, independently of
 * the dry recording file. Live toggle, session-only; no device-direct-monitor
 * changes. The output sum is clamped and clipping reported. */
int daw_set_record_monitor(daw_session*, int32_t on);
int daw_get_record_monitor(daw_session*, int32_t* on);
/* Auto-enable monitoring when arming a track (default: on). Session-scoped,
   not persisted in project. */
int daw_set_auto_monitor_on_arm(daw_session*, int32_t on);
int daw_get_auto_monitor_on_arm(daw_session*, int32_t* on);
int daw_set_metronome(daw_session*, int32_t on);
int daw_get_metronome(daw_session*, int32_t* on);
/* Project tempo and time-signature maps: ordered by 48 kHz frame, frame-0
 * anchored (120 BPM, 4/8 per kDefaultTimeSignature*), at most 64 points each. Upsert replaces a point at
 * the same frame; an identical value is a successful no-op without a revision.
 * The first (frame-0) point cannot be removed. The bridge pre-validates ABI
 * shape and obvious value ranges; ordering, timeline and capacity rules stay
 * inside the domain. */
enum { DAW_TEMPO_POINT_VERSION = 1 };
typedef struct { uint32_t struct_size; uint32_t version; uint64_t frame; double bpm; } daw_tempo_point;
enum { DAW_TIME_SIGNATURE_POINT_VERSION = 1 };
typedef struct { uint32_t struct_size; uint32_t version; uint64_t frame; uint8_t numerator; uint8_t denominator; } daw_time_signature_point;
int daw_set_tempo(daw_session*, uint64_t frame, double bpm, uint64_t expected_revision);
int daw_remove_tempo(daw_session*, uint64_t frame, uint64_t expected_revision);
int daw_set_time_signature(daw_session*, uint64_t frame, uint32_t numerator, uint32_t denominator, uint64_t expected_revision);
int daw_remove_time_signature(daw_session*, uint64_t frame, uint64_t expected_revision);
/* Indexes are vector positions in frame order; count covers the whole map. */
int daw_get_tempo_count(daw_session*, uint32_t* count);
int daw_get_tempo_point(daw_session*, uint32_t index, daw_tempo_point* out);
int daw_get_time_signature_count(daw_session*, uint32_t* count);
int daw_get_time_signature_point(daw_session*, uint32_t index, daw_time_signature_point* out);
/* Project markers: named locators ordered strictly by 48 kHz frame, unique
 * frames, at most 256 entries, names 1-120 UTF-8 characters (domain-owned
 * validation). A duplicate frame rejects; an identical rename is a silent
 * no-op; every committed change costs one revision and one Undo step. The
 * lane never affects audio, so marker commands prepare nothing. */
enum { DAW_MARKER_VERSION = 1 };
typedef struct { uint32_t struct_size; uint32_t version; uint64_t frame; char name[128]; } daw_marker;
int daw_get_marker_count(daw_session*, uint32_t* count);
int daw_get_marker(daw_session*, uint32_t index, daw_marker* out);
int daw_add_marker(daw_session*, uint64_t frame, const char* name, uint64_t expected_revision);
int daw_rename_marker(daw_session*, uint64_t frame, const char* new_name, uint64_t expected_revision);
int daw_remove_marker(daw_session*, uint64_t frame, uint64_t expected_revision);
/* Cross-track clipboard. Copy keeps the source, move erases it (a lone region
 * cannot leave its imported track). An audio paste requires the target track
 * to resolve the same take source at the same take start (take 0 is the base
 * audio; indices are track-local), and region overlap follows the crossfade
 * rule. MIDI clips keep their lane; note and clip limits stay domain-owned. */
int daw_copy_clip_to_track(daw_session*, uint64_t source_track, uint32_t source_index, uint64_t target_track, uint64_t start, uint64_t expected_revision);
int daw_move_clip_to_track(daw_session*, uint64_t source_track, uint32_t source_index, uint64_t target_track, uint64_t start, uint64_t expected_revision);
int daw_copy_midi_clip_to_track(daw_session*, uint64_t source_track, uint32_t source_index, uint64_t target_track, uint64_t start, uint64_t expected_revision);
int daw_move_midi_clip_to_track(daw_session*, uint64_t source_track, uint32_t source_index, uint64_t target_track, uint64_t start, uint64_t expected_revision);
/* Preview is read-only. Pass changes=NULL/capacity=0 to query change_count.
 * Commit validates the entire batch before creating one revision/Undo entry. */
int daw_preview_workflow(daw_session*,const daw_workflow_operation* operations,uint32_t operation_count,uint64_t expected_revision,daw_workflow_change* changes,uint32_t capacity,uint32_t* change_count,uint64_t* after_revision);
int daw_commit_workflow(daw_session*,const daw_workflow_operation* operations,uint32_t operation_count,uint64_t expected_revision);
/* Built-in vocal.prepare action. selected_track_ids order defines Lead then Doubles.
 * Preview may be queried with items=NULL/capacity=0. Commit recomputes the same
 * deterministic pre-fader analysis against expected_revision. */
int daw_preview_vocal_preparation(daw_session*,const uint64_t* selected_track_ids,uint32_t selected_count,const char* base_name,double target_rms_db,double peak_ceiling_db,double double_offset_db,uint64_t expected_revision,daw_vocal_preview* items,uint32_t capacity,uint32_t* item_count,uint64_t* after_revision);
int daw_commit_vocal_preparation(daw_session*,const uint64_t* selected_track_ids,uint32_t selected_count,const char* base_name,double target_rms_db,double peak_ceiling_db,double double_offset_db,uint64_t expected_revision);
int daw_undo(daw_session*, uint64_t expected_revision);
int daw_redo(daw_session*, uint64_t expected_revision);
int daw_save_draft(daw_session*, const char* path);
/* Successful open replaces state and clears undo/redo. Failed open changes no state. */
int daw_open_draft(daw_session*, const char* path);
void daw_error(daw_session*, char* buffer, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
