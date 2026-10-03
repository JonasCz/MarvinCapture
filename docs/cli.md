# MarvinCaptureCLI

One command-line program for everything the GUI does: device bring-up, deck
control, DV/HDV and analog capture, and streaming to stdout. It is a thin front
end over `marvin-core`: the argument parser, the step sequencer and the help
text live in the core (`pin_api.h`), so the GUI's command line accepts exactly
the same syntax.

> Status: implemented. The parser, the step sequencer and the help text are in
> the core (`pin_script_parse`, `pin_script_run`, `pin_script_cancel`,
> `pin_script_help`, API version 3), used by `MarvinCaptureCLI`
> (`src/cli/marvin_capture_cli.c`, in `build\dist` next to the GUI) and by the
> GUI's command line. Only sections marked *(planned)* are not built yet:
> `--capture -` parses but fails at run time with "not supported yet".

## Model

Arguments are processed **left to right**.

- **Settings** change state for the steps that follow (`--input`, `--format`,
  `--brightness`, ...).
- **Actions** do one thing each. Transport actions return as soon as the deck
  accepted the command; only `--wait` blocks.

An action's argument is the next word unless it starts with `--`; `--wait=idle`
also works.

Run without arguments, or with `-h` / `--help`: print the help, then the device
table (id, name, vid:pid, serial, state, owner pid), to stdout, exit 0.

## Settings

| Setting | Default | Notes |
|---|---|---|
| `-d, --device ID` | first device | Device id (`usb:2-1`), 16-hex serial, or a path to a `.dv`/`.ts` file (replay device). Only before the first action. |
| `-i, --input dv\|svideo\|composite` | `dv` | `dv` means DV and HDV over FireWire (auto-detected). |
| `--std S` | `auto` | `auto pal ntsc pal-m pal-n pal-60 ntsc-443 ntsc-j secam` (analog). |
| `--format KEY` | from the file extension | `dv dv-avi dv-mov hdv-ts hdv-mov hdv-mkv avi ffv1-mkv`; may be given once per kind. |
| `--aspect auto\|4:3\|16:9` | `auto` | |
| `--split` | off | Scene split (DV/HDV). |
| `--title T` | none | Metadata title, where the format supports it. |
| `--keep-raw` | off | Keep the raw `.dv`/`.ts` next to a rewrapped file. |
| `--overwrite` | off | Without it an existing target file is an error. |
| `--brightness --contrast --saturation --hue --sharpness --audio-gain N` | device default | Analog controls. |
| `--debug` | off | Status as one plain line per second instead of a redrawn line, plus debug logging (raw AV/C commands and answers, bring-up details). |

Settings cannot change while a capture is open (between `--capture` and the
action that closes it).

File extension to format, per detected kind:

| Kind | `.dv` | `.avi` | `.mov` | `.ts`/`.m2t` | `.mkv` |
|---|---|---|---|---|---|
| DV | dv | dv-avi | dv-mov | — | — |
| HDV | — | — | hdv-mov | hdv-ts | hdv-mkv |
| Analog | — | avi | — | — | ffv1-mkv |

If the kind that arrives cannot use the extension, that kind's default format
is used (DV raw, HDV TS, analog AVI) and a warning is printed.

## Actions

| Action | Effect |
|---|---|
| `--rew` `--ff` `--play` `--pause` `--stop` | Transport command. Any transport action first closes an open capture (tape keeps running); `--stop` also stops the tape. Not available on analog inputs. |
| `--capture PATH` | Start capturing to PATH. The capture stays open until the next transport action, the next `--capture`, or the end of the arguments. `-` writes the stream to stdout *(planned)*. |
| `--wait [COND[,COND...]]` | Block until the first of the conditions is met. Default `idle`. |

### Wait conditions

| Condition | Met when |
|---|---|
| `idle` | The deck was moving after the last transport command and is now stopped or paused (stable ~3 s). Covers "at the start" after `--rew`, "at the end" after `--ff`, end of tape after `--play`. Already idle with no transport command since: met at once. |
| `nosignal[=+HH:MM:SS:FF]` | No signal (no DV data / no analog lock) for that long, counted from the start of the wait. Default `+00:01:00:00`. |
| `signal` | A signal is present. |
| `HH:MM:SS:FF` | The deck timecode reaches or passes this value in the direction the tape moves. Fails (exit 3) if the deck goes idle first. |
| `+HH:MM:SS:FF` | That much time has passed. |

Timecodes and durations share one format; a leading `+` makes it a duration.

Winding for a duration (`--rew --wait +00:00:30:00`) depends on the deck's
speed; do not rely on it for precise positioning. Many decks answer the
timecode query only while playing or stopped, not while winding (see
[deck-control.md](deck-control.md)), so timecode waits are most reliable
during `--play`.

On analog inputs only `signal`, `nosignal` and durations are allowed.

## Output

All human output goes to stderr (except the help text): the device, one
`[i/n] step` line per step as it starts, warnings and errors from the core,
file opened/closed lines, why a capture ended, then a status line (timecode
while winding, REC line while capturing), redrawn in place at about 5 Hz on a
terminal and printed once per second when stderr is not a terminal or with
`--debug` (which also prints the core's debug log). Ctrl-C once stops
gracefully (`Stopping: finalising files...`), a second Ctrl-C exits at once
with 130.

### Streaming to stdout *(planned)*

`--capture -` writes DV as raw DIF, HDV as MPEG-TS, analog as NUT (YUY2 +
PCM). `--format` is ignored. The pipe gets a large queue; if the reading
program cannot keep up and the queue fills, or it exits, the capture stops with
an error (exit 4) instead of silently dropping frames.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | OK |
| 1 | Usage error |
| 2 | Device or bring-up error |
| 3 | Deck error (no camera, no tape, command refused, timecode wait failed) |
| 4 | Capture ended abnormally (disk full, camera lost, write error, pipe) |
| 130 | Ctrl-C: the capture is finalised and the tape stopped |

## Examples

```
MarvinCaptureCLI --rew --wait --play --capture tape01.avi --wait idle,nosignal --rew --wait
MarvinCaptureCLI --rew --wait --play --wait 00:14:30:00 --capture clip.dv --wait 00:21:00:00 --stop
MarvinCaptureCLI --play --capture - --wait idle | ffmpeg -f dv -i - out.mp4
MarvinCaptureCLI -i svideo --std pal --capture vhs.mkv --wait signal --wait nosignal=+00:00:30:00,+04:00:00:00
```

Multi-pass capture is written out: repeat the steps with another file name.

## Implementation notes

Where the implementation had to choose (parser: `src/engine/pin_script.c`,
conditions: `pin_script_eval.c`, sequencer: `pin_script_run.c`).

- **Syntax.** Every long option also accepts `--name=value`. A setting's value is
  the next word whatever it looks like (`--hue -10`); `--capture` and `--wait`
  take the next word only if it does not start with `--`. `-h` / `--help`
  anywhere, or no arguments at all, ask for help and the rest of the line is not
  checked. Timecodes are exactly `HH:MM:SS:FF` (two digits each; `;` before FF
  allowed); `--std` also accepts `ntsc443`.
- **State.** Settings are state for the *steps* after them: the format, aspect,
  split, title, keep-raw and overwrite in force are copied into each `--capture`.
  `--input`, `--std` and the controls are applied in order as the script runs.
  Every setting except `--debug` is an error between `--capture` and the action
  that closes it (including `--input`).
- **Formats.** The extension decides the format per kind (table above) unless
  `--format` was given for that kind. A path whose extension is not one of
  `dv avi mov ts m2t mkv` is a usage error unless some `--format` is given.
  A known extension that does not suit the kind that arrives (for example
  `clip.mov` on analog) uses that kind's default and logs a warning when the
  capture starts. Only a recognised extension is stripped from the path; the
  core appends the format's own.
- **Exit codes.** 1 is also used by the sequencer for a failure that is
  really a usage problem (target file exists without `--overwrite`, `--capture -`
  not supported yet). An existing file is detected by checking the file name of
  every kind that could arrive (a stopped deck has not told us DV from HDV yet).
  `--std` and the analog controls on the DV input are ignored with a warning,
  not rejected.
- **Existing files** are checked for every `--capture` before the first step
  runs (for the kinds the selected input can deliver), so a script that would
  fail on its last capture does not first start the tape.
- **Verified on hardware** (510-USB + DV camcorder): rewind with live
  timecode, `--ff --wait HH:MM:SS:FF` (this deck reports the timecode while
  winding too; with a timecode beyond the end of the tape the wait ends when the
  deck stops at the end of the tape, exit 3), timecode-driven capture (20 s =
  72 MB of NTSC DV), a duration wait, `--pause --wait`, the whole-tape example
  (capture ends 1 min after the recording, tape rewound, exit 0) and Ctrl-C
  during a capture (file finalised, tape stopped, exit 130). FFmpeg's own
  messages go to the core log (debug level; warnings for errors), not stderr.
- **Capture is manual** (no deck driving, one pass, no limits). It is READY
  until the first frame arrives; that is normal. A capture that ends by itself
  normally (end of a replay file) just stops being open; an abnormal end aborts
  the script with exit 4 and the core's sentence.
- **Input.** If the script names an input before its first action the sequencer
  switches to it (unless it is already active) and waits for READY or ERROR
  (exit 2); a session that was never brought up gets DV.
- **Deck commands** wait until the command was sent (the engine's command slot
  holds one command) and, for a real deck, acknowledged; a refusal or a camera
  that is not on the bus is exit 3.
- **Waits.** `idle` needs motion seen since the last transport command, then
  stopped/paused for 3 s; with no motion seen it still counts 5 s after the
  command (the early status can say "stopped"); with no transport command since
  the script started or the last idle wait, a stopped deck is idle at once. No
  tape or no camera is exit 3. `nosignal` counts from the start of the wait and
  restarts when the signal comes back. Timecode waits compare field by field in
  the direction of the last wind/play command (`--rew` = down); the deck going
  idle first is exit 3. Durations with frames use the video frame rate, else 25.
- **Ctrl-C** (`pin_script_cancel`): the open capture is finalised, the tape is
  stopped if the script moved it and its last command was not `--stop`, and the
  script ends with exit 130.
