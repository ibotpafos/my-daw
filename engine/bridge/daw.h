#ifndef MY_DAW_H
#define MY_DAW_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental bridge v1, distinct from the planned workflow command API.
 * All calls: same owning non-real-time thread, including destruction. No concurrency.
 * Handle owns state; caller owns all input/output buffers. No borrowed pointers escape.
 * Names/paths are NUL-terminated UTF-8; valid buffer storage is caller's responsibility.
 * 0 = success; 1 = failure. daw_error copies the latest error into caller storage.
 * Snapshot sizes must equal sizeof(struct). All mutations use expected_revision.
 */
typedef struct daw_session daw_session;
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
typedef struct { uint32_t struct_size; uint64_t id; double gain_db; char name[481]; uint64_t audio_frames; uint32_t clip_count; double pan; int32_t muted; int32_t solo; uint32_t take_count; uint64_t output_bus_id; uint32_t send_count; } daw_track;
typedef struct { uint32_t struct_size; uint64_t start; uint64_t source_offset; uint64_t length; uint64_t fade_in; uint64_t fade_out; uint32_t take_index; } daw_clip;
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
 * class_id is a canonical 32-hex FUID; module_fingerprint is SHA-256 hex. */
typedef struct { uint32_t struct_size; int32_t available; char class_id[33]; char module_fingerprint[65]; char module_path[4097]; char name[481]; char vendor[481]; char version[257]; char quarantine_reason[513]; } daw_vst3_component;
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
/* state: 0 idle, 1 running, 2 stopped, 3 device lost/changed, 4 stalled, 5 callback buffer error. */
/* Output states: 0 idle, 1 running, 2 stopped, 3 device lost, 4 stalled,
 * 5 callback error, 6 render graph preparing, 7 preparation failed. */
enum { DAW_OUTPUT_IDLE=0, DAW_OUTPUT_RUNNING=1, DAW_OUTPUT_STOPPED=2, DAW_OUTPUT_DEVICE_LOST=3, DAW_OUTPUT_STALLED=4, DAW_OUTPUT_CALLBACK_ERROR=5, DAW_OUTPUT_PREPARING=6, DAW_OUTPUT_PREPARATION_FAILED=7 };
typedef struct { uint32_t struct_size; int32_t state; uint32_t device_id; uint64_t generation; uint64_t callbacks; uint64_t callback_errors; } daw_output_status;
typedef struct { uint32_t struct_size; int32_t recording; int32_t overflowed; uint64_t frames; uint64_t callbacks; uint64_t target_track_id; int32_t loop_recording; uint32_t pass_count; } daw_recording;
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
int daw_poll_import(daw_import_job*,daw_import_status*);
void daw_cancel_import(daw_import_job*);
int daw_apply_import(daw_session*,daw_import_job*,uint64_t expected_revision);
/* Release asks a running worker to cancel and returns immediately. */
void daw_release_import(daw_import_job*);
int daw_import_wav(daw_session*, const char* path, const char* name, uint64_t expected_revision);
int daw_import_take_wav(daw_session*,uint64_t track_id,const char* path,const char* name,uint64_t start_frame,uint64_t expected_revision);
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
int daw_get_output_status(daw_session*, daw_output_status*);
/* Recording is mono input duplicated to stereo, float32 at 48 kHz, at most 60 s.
 * An armed take with an enabled loop uses one AUHAL duplex callback and commits
 * each pass as a take in one revision. It requires one I/O or aggregate device.
 * The app must obtain microphone permission before calling start. */
int daw_record_start(daw_session*, uint64_t start_frame, const char* recovery_path);
int daw_record_start_take(daw_session*,uint64_t track_id,uint64_t start_frame,const char* recovery_path);
int daw_record_stop(daw_session*, const char* name, uint64_t expected_revision);
int daw_record_cancel(daw_session*);
int daw_get_recording(daw_session*, daw_recording*);
/* Imports the confirmed part of a stale .mydawtake. File removal is attempted
 * only after the model commit succeeds. */
int daw_recover_take(daw_session*, const char* path, const char* name, uint64_t expected_revision);
daw_session* daw_create(void);
void daw_destroy(daw_session* session);
int daw_get_snapshot(daw_session*, daw_snapshot*);
int daw_get_track(daw_session*, uint32_t index, daw_track*);
int daw_add_track(daw_session*, const char* name, uint64_t expected_revision);
int daw_rename_track(daw_session*, uint64_t id, const char* name, uint64_t expected_revision);
int daw_set_gain(daw_session*, uint64_t id, double gain_db, uint64_t expected_revision);
int daw_set_pan(daw_session*,uint64_t id,double pan,uint64_t expected_revision);
int daw_set_mute(daw_session*,uint64_t id,int32_t muted,uint64_t expected_revision);
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
int daw_set_bus_output(daw_session*,uint64_t bus_id,uint64_t output_bus_id,uint64_t expected_revision);
int daw_get_send(daw_session*,uint64_t track_id,uint32_t index,daw_send*);
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
