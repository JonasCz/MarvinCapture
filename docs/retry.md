# Retry of bad sections

A DV or HDV capture with **Retry bad sections** on (GUI check box on the DV / HDV tab, settings
behind the gear next to it; `MarvinCaptureCLI --retry`; `pin_capture_opts_t.retry`) reads a
damaged stretch of tape again, and keeps the best of every read in the file: DV block by block,
HDV GOP by GOP. It replaces the old "capture passes" (capture the whole tape N times, merge
afterwards with dvrescue): only the damaged spots are read again, the merge happens during the
capture, and the result is one file, also for MOV / MKV / AVI.

It needs deck control: "Automatic rewind & capture" in the GUI (Manual capture never retries), a
camera that takes AV/C transport commands, and not a capture to standard output. Analog captures
have no retry.

## How it works

The code is `src/engine/pin_retry.[ch]` (hardware-free, unit-tested against a simulated tape and
deck in `tests/engine/test_pin_retry.c`); the session feeds it the units and drives the deck for it
(`retry_tick()` in `src/engine/pin_session.c`).

1. **Hold back.** Every DV frame / HDV picture goes through a FIFO before it reaches the file:
   in memory, or (setting **Keep held tape in**) in a temporary file next to the output, deleted
   afterwards. A DV frame is one slot, an HDV GOP (from a picture with a sequence header to the next) is
   one slot. Normally a slot leaves about a second after it arrived: those frames are the anchor a
   re-read is lined up on.
2. **A section opens** at the first bad slot. What counts as bad is the setting **Re-read on**:
   - *Any damage* (default): DV video blocks with any STA error or concealment (2-15), audio carrying
     the error code, the Sony partial mute, missing blocks (lost in transit, zero-padded), whole
     frames lost in transit; HDV pictures with TEI, a continuity gap, a lost or broken PES, data
     lost in transit, and what depends on them in the GOP (`hdv_error.c`).
   - *Uncorrected errors only*: DV STA 7 / 15, missing blocks, the audio error code, lost frames;
     HDV as above (HDV has no concealment flags).
3. **The section grows** while more damage follows within the **join gap**, and the join gap
   follows the damage, to spend as little time as possible: it starts at about a second, so a lone
   bad spot is re-read at once (one short rewind). Each section that starts soon after the previous
   one ended (closer than the measured cost of a retry cycle, about 10 s until measured, or twice
   the join gap) doubles it: on a damaged stretch the sections get longer and one rewind takes many
   bad spots. A long clean stretch halves it again, back to short rewinds. The section is retried
   when the clean run after the last bad slot reaches the join gap, when it reaches the **longest
   section** (default 60 s of tape, about 220 MB; 10-600 s in memory, up to 3600 s in a temporary
   file; the join gap is at most half of it), or when the signal ends (the end of the recording).
4. **Wind back.** REW, then PLAY a **preroll** (default 3 s) before the section. While a retry winds,
   the deck's TIME CODE is asked every 0.2 s: it shows when the deck really winds (a deck plays on
   for a moment after REW, and a PLAY in that moment leaves the tape where it is, so PLAY never goes
   out before the timecode goes back; a deck that answers nothing while winding gets a second), it
   measures the wind speed, and PLAY goes out when the tape will be a preroll before the section.
   Without timecode answers it is the wind time at the speed learned so far (20x before the first
   measurement). A **stop lead** is learned from where the deck landed against where it should have
   (the deck winds on until it takes PLAY). Landing after the start of the damage winds back again
   (at most three times per attempt). A PLAY the deck refuses straight from winding is retried
   after a STOP. A stale frame right after PLAY (the one shown before the wind, sent once more) is
   not taken for the landing: two frames in a row must agree. Frames the deck sent before the wind
   took effect, arriving late, are the next frames of the tape and are kept.
5. **Re-read and mend.** The re-read is lined up with the held slots by key: the DV subcode timecode,
   the HDV decode time stamp (DTS) of the GOP's first picture (the Canon writes 00:00:00:00 into
   every GOP header; the DTS is what the tape carries). A unit without a readable key takes the slot
   after the previous one, or the slot its distance from the last keyed one says; a key between two
   held slots is a frame or GOP the first read lost in transit, and is put in. DV is merged block by
   block (`dv_error_block_map()`: the better grade wins), an HDV GOP is replaced whole when the
   re-read has fewer damaged pictures (or as many, and more of them). HDV continuity counters are
   re-phased where reads join (`pin_retry_cc_fix_t`), so the raw `.m2t` and the MOV / MKV repair
   (`sink_hdvmux.c`) see one continuous stream; the PAT / PMT at the start of a file follow the
   file's own order (`hdv_psi_out`).
6. **Again, or go on.** When the re-read runs past the section, the section is done if it is clean.
   Otherwise, with **attempts** left (default 3, 1-10) and if this attempt mended something, it winds
   back again, now to the first slot still bad. Two attempts in a row that mended nothing end the
   section's attempts (the damage is on the tape, not dirt; dirt can take two reads to come off). Then the section leaves the FIFO and the capture
   simply goes on: what the deck plays now is the tape after the section, so nothing is read twice
   into the file and nothing is skipped.

### Modes

- **Each bad section** (`--retry-mode section`): as above. Quickest for a tape with a few bad spots.
- **Whole tape, then re-read** (`whole`): holds everything from the first damage to the end of the
  recording (the signal away for 15 s) in a temporary file, then winds back once and re-reads it,
  pass after pass, each pass from the first frame still bad. Far fewer rewinds and stops: less wear
  on the deck and the tape (the GUI says so under the choice). The file is written when the passes
  are done, and the output drive needs room for the tape twice. This is the old "capture passes,
  merge afterwards with dvrescue", done in one go.
- **Automatic** (`auto`, the default): each section; once the tape turns out very poor (three
  sections in a row that start right after the previous one while the join gap is already at its
  longest) the rest of the capture is done as a whole tape, in a temporary file.

If the temporary file cannot be written (drive full, no permission), the engine goes on in memory,
with sections only (`retry_stats.spool_failed`).

While it winds back and re-reads, the no-signal timeout and the time limit stand still, the status
says what it does (`retry_phase`, `pin_format_retry_status()`: "Retry 2/3: re-reading 14 s (5 bad
left)"), and a `PIN_EVT_RETRY` reports each change. The status keeps the counts (`retry_sections`,
`retry_fixed`, `retry_partly`, `retry_failed`, `retry_mended`, `retry_rewinds`, the learned
`retry_wind_speed`, `retry_whole_tape`, `retry_spool_bytes`) after the capture, and `pin_format_retry_tooltip()` has them as text. Frame
counts are those of what reached the file: a mended frame is counted once, as it is now; a frame a
re-read found after it was lost in transit is taken off `frames_dropped`.

The writer queue is never flooded (a full queue would end the capture): what leaves the FIFO is
held to a quarter of the queue's capacity in backlog (`retry_budget()`), while new frames queue
behind it; at the end of a capture the FIFO drains the same way (`pin_retry_flush_some()`), however
much a whole-tape retry holds.

## Camera quirks

Quirks are handled from what the frames say, so they work for any camera (the rules come from
dvrescue's dvmerge; see `dv_error.h`):

- **Re-encoding cameras** (Samsung VP-D340: every video block STA 14). A block counts as concealed
  only where its payload equals the previous frame's same block (stuck concealment); new picture
  data is good. Without this every frame of such a camera would ask for a re-read.
- **Silent mute** (Sony DSR-1500P writes 0x0000 instead of the error code): a partial zero pattern in
  the first channel pair is concealed audio.
- **Copied audio** (JVC, DSR-1500P and others patch an unreadable audio block with another block's
  payload, without saying so): a non-constant payload that occurs twice in a frame, or the same
  block of the previous frame repeated, is *suspect*. A suspect block is replaced by a clean copy
  from a re-read, but never asks for a re-read by itself: a periodic test tone looks the same.
- **No timecode in the HDV GOP header** (Canon): the DTS lines re-reads up, the deck's TIME CODE poll
  finds the section while winding.
- **Decks that give no timecode while winding** (the DV camcorder answers during REW only to the
  alternating poll): the wind time at the learned speed.

The camera's name is read from its configuration ROM at bring-up (`p1394_read_identity()`: vendor and
model textual leaves, IEEE company and model id) and logged
(`pinnacle: camera: Canon HV20 (vendor 0x000085, model 0x...)`); the status carries it
(`camera_name`), the retry tooltip shows it. The wind speed and the cost of a retry cycle are learned
per session (kept across captures while the device is open); they are not stored per model yet.

## Testing it without a dirty tape

`MarvinPlayout` on a second unit is a virtual deck that answers AV/C (PLAY, REW, TIME CODE, wind at
`--wind-speed`). Its test seam `POUT_TEST_DIRT=FROM-TO[:READS],...` sends the given tape frames damaged
on their first READS reads (default 1; 0 = every read), as dirt a second pass wipes off
(`src/playout/pout_dirt.h`):

```sh
# unit A: a 10-minute DV tape with dirt at 1:00 (one read), 4:00 (two reads) and a crease at 7:00
POUT_TEST_DIRT=1500-1510,6000-6003:2,10500-10501:0 MarvinPlayout tape.dv
# unit B
MarvinCaptureCLI --input dv --rew --wait --capture out.dv --retry --play --wait nosignal=00:00:30 --stop
```

Expected: three sections, two mended (the second after two attempts), one unchanged after one
attempt; `out.dv` has every frame of `tape.dv` once. Add `--retry-mode whole` for one pass to the
end and re-reads from 1:00 (two rewinds in all).

`tests/engine_replay/test_replay_retry.c` checks that a capture through the retry engine writes the
same file and counts as one without it (DV and HDV, the replay device has no deck: seam
`PIN_TEST_RETRY_REPLAY`).

## Bench results (2026-11-20, 510-USB as virtual deck, 500-USB capturing)

MarvinPlayout (wind speed 30x, plays on 0.5 s after REW, 1 s from PLAY to the first frame) with
`POUT_TEST_DIRT`, `MarvinCaptureCLI --retry`; checked frame by frame against the tape
(`build/bench/check_dv.py`, `check_hdv.py`, `tei.py`, `cc.py`, not in git):

| tape | result |
|---|---|
| DV PAL 120 s, clean | 2999 frames = tape 0..2998 identical, no section |
| DV, dirt at 0:30 (1 read), 1:00 (2 reads) + 1:01 (1 read), crease at 1:30 | every frame once, in order; the dirt mended, the crease left; 3 sections, 4 rewinds, 212 s |
| the same, `--retry-mode whole` | the same frames (3000), 4 passes, 474 s |
| HDV 1080i50 60 s, dirt at 0:15, 0:30 (2 reads), crease at 0:48, raw `.m2t` | 1499 pictures identical to the tape; TEI only at 0:48; video and audio continuity counters unbroken across the joins; 5 rewinds |
| the same into HDV MOV | the dirt pictures identical to the tape; the crease's GOPs re-encoded by the MOV repair, as without retry |

What the bench taught (fixed): PLAY sent before the deck had started winding; a frame sent before
the wind arriving after PLAY taken for the landing; the stale frame after PLAY; wind speeds learned
from near-zero wind times; the distance played into blank tape at the end of a recording; a 2-read
spot given up after one fruitless attempt.

Still to know: a real camcorder's REW / PLAY timing and its TIME CODE answers while winding (the DV
camcorder answers only every other poll during REW), HDV DTS between reads on a real tape, the join
gap on a really damaged tape. On this deck a retry costs about 20 s: a 25x wind cannot stop closer
than about 10-15 s before the section (wind delay plus the time to see it moving), so that much is
read twice; reverse play (AV/C PLAY reverse) would land closer but is not tried.
