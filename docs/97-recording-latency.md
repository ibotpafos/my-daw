# Recording latency and capture placement (P0-04)

## Contract

Recording uses a single native Core Audio device clock (or an explicitly
configured aggregate). The duplex I/O adapter uses `AudioDeviceIOProc` to obtain
**separate input and output sample/host timestamps for the same cycle**. The
previous output-only AUHAL render timestamp was not an input-acquisition time.
The renderer, capture processor, dry writer, recovery and project commands are
reused; ordinary playback still uses its existing output adapter.

Before audio starts, the adapter resolves the selected UID and input/master
channels, finds their streams by `kAudioStreamPropertyStartingChannel`, and reads
device/stream presentation latency, safety offsets, buffer size and virtual
format. Unreadable values are errors, not assumed zero. Input/output must share
a device; selected stereo input streams and master L/R streams must report
matching latency within their respective pair. Native
48 kHz packed Float32 streams are supported, interleaved or planar. Other virtual
formats are explicitly rejected without changing hardware or substituting a
default. This is a narrower format contract than AUHAL's implicit conversion.

## Time mapping

Let `S = output.sampleTime - input.sampleTime` for a paired I/O cycle. Let
`I = input device latency + selected input stream latency`,
`O = output device latency + selected master stream latency`, and
`G = prepared renderer master latency`.

The offset used for this take is `D = S + I + O + G`. Device/stream values are
presentation latency; `S` already contains the scheduling/buffer/safety
separation, so buffer and safety values are displayed but **not added again**.
Graph delay is a separate measured preparation result, not a replacement for
hardware latency. A driver's unreported or dynamically changing plugin delay
cannot be corrected by this reported-latency model.

The first valid pair fixes `S`. Both sample clocks must remain continuous,
including pre-roll/loop boundaries. Optional host timestamps cross-check the
same delta using the system timebase, with two frames of conversion tolerance;
callback arrival time (`inNow`) is not used. Sample delta must be nonnegative,
within two seconds and integral to 0.001 sample. Missing flags, invalid values,
gaps or a changed delta latch an error before disk acceptance. The existing
clock checks remain in force. Hardware reports/layout are revalidated on the
control-thread polls; changes stop I/O instead of shifting a take midway.
Polling is not a guarantee against a hidden transient between polls.

For requested start `R` and pre-roll `P = min(requested pre-roll, R)`, input at
callback frame `E` maps to `R - P + E - D`. The writer accepts only the requested
recording interval. Thus leading input is discarded rather than clamped onto
frame zero or turned into artificial silence. Recovery keeps the requested
start and already-aligned PCM using the unchanged version-1 take format.
Existing projects/recordings are not shifted when hardware settings change.

## Stop, ends and loop boundaries

Natural recording limits feed the requested interval through the renderer,
flush its `G` pending frames, then silence output while the last delayed input
arrives. The post-insert recording metronome follows the same delayed musical
clock as the backing; offline export remains click-free. Direct MON still uses
its existing ramp and is never mixed into the file.

Manual Stop is nonblocking. The producer latches it at the next callback
boundary and stops emitting backing/MON. Only the input corresponding to the
already-emitted musical interval is drained. A stop during pre-roll produces
no clip/Undo. Repeated Stop does not move the endpoint or create extra commands.
The UI remains in recording/busy state while draining, shows a completion
message and prevents editing/export/device reconfiguration until finalization.

`daw_record_request_stop` requests this transition. `daw_get_recording_timing`
is a size/version-checked snapshot of the individual terms, stream IDs,
leading discard, pending drain and `can_finish`. Legacy `daw_record_stop` also
requests Stop, but rejects premature finalization **without consuming the
active recorder**; clients then poll recording status/errors and timing before
retrying. This is an explicit asynchronous-completion contract for timed input.
Cancel/destruction/failure stop I/O and preserve confirmed PCM instead of
pretending the unavailable tail was recorded.

Ordinary clips and loop passes use the same aligned stream. Existing loop
splitting therefore starts exactly on the requested loop boundary, including
partial passes. A seamless transport punch interface is not introduced here;
this implementation handles the existing requested Record In, pre-roll,
manual Record Out and loop contracts. P0-05A extends the same timing snapshot to
an explicit stereo input pair. Recording remains 48 kHz and bounded by the
existing 60-second/media budget; file-backed long recording remains P0-05B.

## Real-time and lifetime boundaries

No device property reads, allocations, locks, waits or file I/O were added to
the callback. Prepared arrays are used for channel extraction/output mapping.
On Stop/cancel the control thread detaches the callback's owner, unregisters
I/O, waits for entered callbacks, then finishes the writer. If a defective
driver refuses unregistering, only a detached two-atomic sentinel is retained,
not the session, capture, renderer or project. Stop/unregister failure preserves
recovery and is not reported as a successful commit. Synchronous HAL driver
calls themselves do not have a hard timeout guarantee.

## Validation and remaining acceptance

`recording_latency` checks impulse placement/partition invariance, project-zero
and pre-roll boundaries, loop positions, finite drain, repeated/premature Stop,
input-clock failures, immutable paired offsets, actual writer recovery, and an
independent simulated wire loop fed from the real renderer output (including a
97-frame insert). `e2e_duplex_recording` exercises public C ABI timing/Stop,
WAV export, recovery, SQLite and one-Undo/Redo/reopen behavior.

On macOS `recording_native_hal` links **the production native adapter and
property reader** against test implementations of HAL C calls. It checks actual
stream selection, non-first/swap L/R buffer routing, input extraction, malformed
profiles, native callback timestamps, draining, shutdown and changed reports.
It never starts physical hardware. The native recording UI test uses real
AppKit controls and production bridge/capture/writer with a separate link-only
hardware factory. Both are synthetic, not physical microphone acceptance.

Record actual CI results/head/tree in the PR, not inferred from the presence of
a test. Physical loopback/acoustic residual measurement, device/driver matrix,
disconnect/reconnect, full VoiceOver and listening remain manual gates in #20.
Do not claim this model eliminates unreported converter, acoustic or vendor
plugin latency. A calibration UI is not part of this change.

## Primary API references

- [AudioDeviceIOProc input/output timestamp contract](https://developer.apple.com/documentation/coreaudio/audiodeviceioproc).
- [Presentation latency and its device/stream properties](https://developer.apple.com/documentation/avfaudio/avaudioionode/presentationlatency).
- [Stream starting channel](https://developer.apple.com/documentation/coreaudio/kaudiostreampropertystartingchannel).
- [Device safety offset](https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertysafetyoffset).
