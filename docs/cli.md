<!-- Generated from docs/cli.md.template by scripts/sync-cli-help.sh (run by the build): edit the template, not this file. -->
## Help text

The text printed by `--help` (followed by the device list). It is defined once,
in `src/engine/pin_script.c`; the build (`scripts/build.sh`, `scripts/build.ps1`)
fills it in here, so edit the C source, not this block. On a
terminal the CLI wraps it to the window width; piped, it is left unwrapped.

```text
Usage: <program> [settings and actions...]

Arguments are processed left to right. Settings change what the steps after them do; actions each do one thing. An action's argument is the next word unless it starts with --; --wait=idle also works.
Transport actions (--rew --ff --play --pause --stop) return as soon as the deck has accepted the command, not when it has finished. Use a wait to wait for it to complete (for example --rew --wait).

Settings:
  -d, --device ID          device id (usb:2-1), 16-hex serial, or a .dv/.ts file to replay (default: first device). See the device list at the end of this text. Only before the first action.
  -i, --input dv|svideo|composite
                           dv = DV and HDV over FireWire (default)
  --std S                  analog standard: auto pal ntsc pal-m pal-n pal-60 ntsc-443 ntsc-j secam (default auto)
  --format KEY             output file format. Overrides the file extension if one is given with the file name passed to --capture; the format's extension is appended if none is given. One per input signal type:
                             analog (-i svideo|composite)  avi ffv1-mkv
                             DV  (-i dv, DV tape)          dv dv-avi dv-mov
                             HDV (-i dv, HDV tape)         hdv-ts hdv-mov hdv-mkv
                           A later --format for the same signal replaces the earlier one. If no --format suits the signal that arrives, the --capture file extension decides (.avi .mkv for analog; .dv .avi .mov for DV; .ts .m2t .mov .mkv for HDV). If there is no extension either, the default is used: the first format listed above for that signal. An extension the arriving signal cannot use falls back to the default with a warning.
  --aspect auto|4:3|16:9   (default auto) sets the display aspect ratio flag in the output file, for containers that can carry it: avi, mkv, mov, and the NUT stream on stdout. Raw .dv and .ts/.m2t files have no such flag and are unchanged. auto = from the stream for DV, 16:9 for HDV, 4:3 for analog.
  --split                  scene split (DV/HDV): a confirmed timecode or recording-date break starts a new file. Files are named NAME-0001.EXT, NAME-0002.EXT, ... (NAME is the --capture path without its extension). A new file is only started once the new scene is at least 1 s and 1 MB long; shorter scraps are appended to the previous file.
  --title T                metadata title, where the format supports it
  --keep-raw               keep the raw .dv/.ts next to a rewrapped file
  --overwrite              overwrite an existing target file (else an error)
  Analog picture and audio controls (svideo/composite only; with defaults):
  --brightness N           0 to 255 (default 128)
  --contrast N             0 to 127 (default 64)
  --saturation N           0 to 127 (default 64)
  --hue N                  -128 to 127 (default 0); works on NTSC only, ignored for PAL and SECAM
  --sharpness N            0 to 3 (default 2); the decoder chip has four steps only
  --audio-gain N           in tenths of a dB, -345 to 120 in steps of 15 (default 0 = 0 dB)
  --debug                  status as one plain line per second, plus debug logging (raw AV/C traffic, bring-up details)
  --exit-when-done         GUI only: close the window when the steps finish, with the exit code below. The CLI always exits when done and ignores this.
  Settings cannot change while a capture is open (from --capture until the next transport action, --capture or the end of the arguments).

Actions:
  --rew --ff --play --pause --stop
                           transport command; returns as soon as the deck accepted it (use a wait to wait for the action to complete). Closes an open capture first (the tape keeps running); --stop also stops the tape. DV/HDV input only.
  --capture PATH           start capturing to PATH; the file format comes from --format, else from the extension, else the default (see --format). It stays open across any number of waits and closes at the next transport action, the next --capture or the end of the arguments, whichever comes first. Put a wait after it, or the end of the arguments is reached at once and the capture closes immediately.
                           PATH - writes the stream to stdout (pipe it into a program): DV as raw DIF, HDV as MPEG-TS, analog as NUT (YUY2 + PCM). --format is ignored; --split is refused; once per command line. If the reader exits or cannot keep up the capture stops, exit 4.
  --wait-any COND[,COND...]
                           block until any condition is met
  --wait-all COND[,COND...]
                           block until all conditions are met
  --wait [COND[,COND...]]  same as --wait-any; bare --wait is --wait-any idle
  Any number of waits can follow each other; they run one after another.

Wait conditions (DUR is HH:MM:SS or HH:MM:SS:FF, no leading +):
  idle[=DUR]               the deck was moving after the last transport command and is now stopped or paused, and has stayed still for DUR (default 00:00:05; never less than about 3 s). Met at once if no transport command was sent since the last idle wait and the deck is stopped or paused. DV/HDV input only.
  signal[=DUR]             a signal has been present without a break for DUR (default 00:00:05)
  nosignal[=DUR]           no signal without a break for DUR (default 00:01:00)
  timecode=HH:MM:SS:FF     the deck timecode reaches or passes this value in the direction the tape moves; fails if the deck goes idle first. DV/HDV input only.
  wallclock=DUR            DUR of real time has passed since the wait began
  captured=DUR             the file of the open capture is DUR long: frames written, divided by the frame rate. Time without a signal does not count. Counted over the whole capture, not from the start of the wait. Needs an open --capture before it in the arguments; works on every input.
  Analog inputs only allow signal, nosignal, wallclock and captured.

How a wait is evaluated (polled about 10 times a second):
  - wallclock, timecode and captured latch: once met they stay met for the rest of that wait.
  - idle, signal and nosignal are states: the durations count only inside the current wait (from its start or from when the state began, if later), and under --wait-all they must all be true at the same moment.
  - --wait-any: a condition that is met beats one that failed (timecode when the deck went idle first), whatever the order; the wait fails only when every condition has failed. --wait-all: one failure fails the wait. No camera or no tape fails idle and timecode at once.
  - wallclock counts from the start of the wait, not from the first captured frame: after --play --capture f --wait wallclock=00:00:15 the file is shorter by the time the deck needs to start playing (about 3 s seen). Use captured= for the length of the file.
  - Winding for a duration (--rew/--ff --wait wallclock=00:00:30) depends on the deck's wind speed, so the position reached is not precise; do not use it to position the tape. Many decks report the timecode only while playing or stopped, so timecode waits are most reliable during --play.

Exit codes: 0 ok, 1 usage error, 2 device or bring-up error, 3 deck error, 4 capture ended abnormally, 5 a capture received no video (nothing written), 130 cancelled (Ctrl-C). A capture that gets no video, e.g. an empty tape, leaves no file; the later steps still run and the script exits 5 at the end, unless a more severe error (1-4, 130) occurs first.

Examples:
  Capture the whole tape; stops once the deck reaches the end:
    --rew --wait --play --capture tape01.avi --wait
  A clip between two timecodes, then stop the tape:
    --rew --wait --play --wait-any timecode=00:14:30:00 --capture clip.dv --wait-any timecode=00:21:00:00 --stop
  Analog: wait for a signal, stop 30 s after it is lost or after 4 hours:
    -i svideo --std pal --capture vhs.mkv --wait-any signal=00:00:05 --wait-any nosignal=00:00:30,captured=04:00:00
  Stop when the deck is idle and the picture has been gone for 10 s:
    --rew --wait --play --capture t.avi --wait-all idle,nosignal=00:00:10

-h, --help, or no arguments at all: this text, followed by the device list.
```

## Running the GUI from a shell

`MarvinCaptureGUI.exe` is a GUI-subsystem program, so PowerShell returns to the
prompt immediately and its console output (for example `--debug`) interleaves
with the prompt. Pipe the output to make the shell wait until the app exits and
to keep the output in order:

```powershell
.\MarvinCaptureGUI.exe --debug | Out-Host
.\MarvinCaptureGUI.exe --debug 2>&1 | Tee-Object gui.log   # also save a log
```

With stdout redirected or piped the app writes to that; otherwise it attaches
to the parent console. Started from Explorer there is no output.
Add `--exit-when-done` to close the window when the command-line steps finish;
the process exit code is the one listed in the help text (the CLI always exits
when done).
