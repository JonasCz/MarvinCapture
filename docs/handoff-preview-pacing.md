# Handoff: preview pacing (29.97 fps DV on a 144 Hz display) and meter rate

Status: code reviewed end to end, no obvious defect found, no code changed.
No camera was attached, so nothing was measured; this lists what to check.

## Path as it is today

1. USB read loop / replay thread -> `dv_on_unit()` (`pin_session.c`) ->
   `pin_previewer_push_dv()`: memcpy of the 120/144 KB frame into a single
   `pending` slot, signal. If the decoder is still busy the pending frame is
   overwritten (drop-if-busy). One memcpy, no allocation after the first frame.
2. `pv_thread` (`pin_preview.c`): copies pending to a work buffer, FFmpeg DV
   decode, `publish_from_avframe` copies planes into a triple-buffer slot,
   `seq++`, broadcast. Slots are never overwritten while locked.
3. GUI `D3DPreview` render thread: `PreviewWait(lastSeq, 100 ms)` -> `PreviewLock`
   -> map+copy 3 R8 planes (WriteDiscard) -> unlock -> Draw -> `Present(1)`.
   Event driven, no timer, no UI thread involvement, no per-frame allocation
   except `YuvToRgbMatrix` (a 12 float array per frame, trivial).
4. Swap chain: `CreateSwapChainForComposition`, FlipSequential, 2 buffers,
   `Present(1, None)`, no waitable object. Present is therefore paced by DWM.

Consequences: a frame is presented once, on the next vblank after it was decoded.
Frames cannot be presented twice or reordered. A frame can only be skipped when
decode of frame N+1 finishes before the render thread wakes for N (then N is
simply never shown; seq jumps by 2) or when the pending slot is overwritten.

## Why it can still look uneven (candidates, most likely first)

1. Inherent cadence: 144 / 29.97 = 4.805 vblanks per frame, so frames are held
   5,5,5,5,4 vblanks (+/- 7 ms). Any player without VRR shows this. Not a bug,
   but combined with (2) it is visible.
2. Arrival jitter is passed straight to the screen. The preview shows a frame
   as soon as it is decoded, so USB/FireWire delivery bursts (units arrive in
   chunks) or decode-time variation become display jitter of one or more
   vblanks (7 ms steps at 144 Hz). A fix would be a small (1-2 frame) jitter
   buffer that presents on a steady 29.97 Hz clock; that adds latency, so it
   was not done blind.
3. `Present(1)` blocks the render thread up to one vblank while it holds `_gate`
   (resize waits for it; harmless) and, with 2 buffers and no frame-latency
   waitable, a late-arriving frame can queue behind the previous present.
   Cheap experiment: BufferCount 3 and/or `SetMaximumFrameLatency(1)` with the
   waitable object.
4. UI thread stalls do not affect presentation (render thread is independent),
   but a stalled UI thread stalls DWM composition of the panel. The 100 ms
   status tick updates many bindings; check it is short.

## What to measure to confirm

Add (temporarily) to `RenderOne`: a `Stopwatch` timestamp per presented frame;
log delta between successive `Present` returns, `frame.Seq - _lastSeq` (>1 means
a frame was decoded but never shown), and the time spent in `Present`.
Expected for healthy 29.97 fps: deltas of 33.4 ms (not 25/50), seq step always 1.
- seq steps of 2: core drops (decoder busy or wake latency) -> look at decode time.
- deltas alternating ~17/50 ms: arrival bursts from USB -> look upstream of the
  previewer, log `dv_on_unit()` timestamps.
- steady 33 ms deltas but still looks uneven: it is the 4.8 vblank cadence ->
  try 120 Hz / 60 Hz display mode to compare; 144 Hz is the worst case.
A replay file (tests/data/dv-ntsc.dv through the replay source) removes the USB
variable: if it is smooth with replay but not live, the problem is delivery.

## Audio meters

- `feed_audio_locked` stores the peak/RMS of the *latest DV frame* (~33 ms of
  audio); `pin_get_status` copies the latest values. There is no max-since-read
  and no decay in the core.
- The GUI reads status on a 100 ms `DispatcherQueueTimer` (`MainWindow`
  `_statusTimer`), so the meter shows every 3rd frame's peak: 10 Hz updates, and
  transients between samples are missed. That matches "low fps" and aliased
  peaks. The peak-hold tick (10 s window) is computed GUI side from the same samples.
- Suggested fix (not done, it needs a core change plus hardware/replay check):
  keep a peak that is max-accumulated since the last status read (reset on read,
  under the same lock as `feed_audio_locked`), and apply a short exponential
  decay in the GUI, then run the meter at ~30-60 Hz via a separate light timer
  (`LevelMeter.Update` only sets a bar width). Do not just shorten the 100 ms
  status tick: it drives all the other bindings.
