# Stereo recording — P0-05A

## Scope

This slice extends the existing timestamped full-duplex recording path from one
selected input channel to an explicit mono/stereo input pair. It does **not**
lift the one-minute capture or decoded-project memory limits. Long recordings
are P0-05B because the current project model still materializes every `Clip`
in RAM and stores PCM inside the draft database.

The durable project representation does not change: audio clips are already
interleaved stereo Float32 at 48 kHz. Mono recording therefore keeps its
existing behavior by duplicating the selected source into L/R; stereo mode
preserves two independently selected dry inputs.

## User contract

Audio Settings exposes a recording mode:

- **Mono** — one selected input is written equally to L/R.
- **Stereo** — Input L and Input R must be different available channels.

The setting is machine-local, not project state. Existing v1 device
preferences migrate to Mono. A project never silently changes another
machine's recording pair.

Input and output still have to use one Core Audio device or an explicitly
configured aggregate device. Stereo recording does not combine unrelated
clock domains.

## Native path

Before starting capture, the macOS adapter resolves both requested input
channels into the device buffer layout and their Core Audio streams. Stereo
capture rejects a pair whose selected streams report different latency rather
than shifting one side heuristically. The existing 48 kHz native Float32,
buffer-size and device-identity checks remain in force.

The timestamped `AudioDeviceIOProc` extracts L/R into fixed callback scratch
buffers and passes them to the shared `DuplexCapture`. No allocation, file
I/O or lock was added to the audio callback.

`DuplexCapture` applies the same clock-integrity and recording-latency model
to both channels. Dry stereo PCM is written as a pair; live MON preserves L/R.
The project backing track, metronome and MON sum are never written into the
recording file.

## Recovery and compatibility

`RecordingWriter` now accepts canonical stereo frames directly. Its ring,
checkpoint file and recovery payload remain interleaved two-channel Float32.
The recovery-file version therefore does not change. Mono callers use the
same writer by passing the same input for both sides.

Ordinary recording, armed takes, loop passes, Undo/Redo, Save/Open and WAV
export continue to consume the existing stereo `Clip` representation. No
project database migration or new dependency is introduced by P0-05A.

## Automated acceptance

Required regression layers:

- audio-device preference migration and invalid stereo pairs;
- public C ABI read/write of the two input channels;
- portable writer/recovery with deliberately different L/R samples;
- portable `DuplexCapture` dry stereo and stereo MON;
- native macOS HAL adapter with a non-default stereo pair;
- end-to-end C ABI recording into a WAV whose L/R samples remain different;
- complete existing Linux/macOS/VST3/workspace/mixer/piano-roll suites.

Native HAL fixtures replace only the operating-system calls for the dedicated
test executable. They are not physical-interface proof.

## Remaining P0-05B

The one-minute limit cannot be safely removed by changing a constant.
`RecordingWriter` already streams capture to disk, but `finish()` currently
reconstructs the whole recording into an in-memory `Clip`; the renderer reads
`Clip::samples()`, Undo accounts decoded bytes, and draft storage embeds PCM
BLOBs. P0-05B must introduce bounded file-backed media/read-ahead and durable
media ownership before 30-minute stereo acceptance can be claimed.

Physical stereo recording, listening, device disconnect/reconnect and
acoustic round-trip verification remain manual gates in issue #20.
