# P0-05A — stereo recording source

## Scope

This is the first bounded slice of core task #42. It adds an explicit mono/stereo
recording source without changing musical project data or creating a second
recording engine.

The selected input mode is machine/session state:

- mono: one selected hardware input is written as dual-mono stereo;
- stereo: two distinct selected hardware inputs are written as independent L/R;
- both modes reuse the same timestamped full-duplex path, latency compensation,
  pre-roll, loop/take handling, recovery writer and direct monitoring.

Existing version-1 audio preferences migrate to version 2 as mono. A project
never silently changes another machine's recording source.

## Real-time path

The native HAL adapter resolves both selected input channels before starting I/O.
For stereo, both channels must exist and their selected streams must report the
same input-stream latency. The callback extracts the already-prepared channels
into fixed scratch arrays and passes them to the shared DuplexCapture.

The callback still performs no allocation, locking or file I/O. RecordingWriter
keeps a bounded SPSC ring and a worker writes interleaved stereo Float32 to the
existing recoverable take file.

Mono input is duplicated in the writer. Stereo input is sanitized/clamped
independently and stored as true L/R. Direct MON follows the same shape: mono is
heard in both outputs; stereo left/right are monitored independently.

## Public/UI contract

Audio Settings now provides:

1. input device;
2. recording mode Mono / Stereo;
3. recording input L / R;
4. output device and master L / R.

The old daw_audio_device_config remains version 1 for compatibility. A separate
versioned daw_record_input_config carries channels/left/right. It is rejected
while playback or recording is active and does not mutate project revision or
Undo.

## Disk-streaming boundary

RecordingWriter was already disk-backed; this slice generalizes its bounded ring
to final interleaved stereo frames and raises only the writer/recovery format
guard to a 30-minute capacity. This does **not** claim that 30-minute project
recording is complete.

DuplexCapture/startRecording and the in-memory Clip/project budget still impose
the current short-take limits. Removing those safely requires P0-05B: a
file-backed/read-ahead Clip/media path so Stop/Save/Open/render/export do not
materialize a 30-minute stereo take into hundreds of megabytes of RAM.

Therefore #42 remains open after P0-05A.

## Verification

Automated coverage must include:

- mono compatibility;
- independent stereo L/R through public C ABI → capture → writer → project →
  WAV export;
- native HAL extraction from non-default selected channels;
- stereo stream-latency mismatch rejection;
- preference v1 → v2 mono migration;
- New/session restoration of the stereo source;
- recovery preserves independent stereo samples;
- long-capacity writer construction remains bounded and does not allocate the
  full requested duration.

Physical two-input recording and a real 30-minute session remain manual gates.
