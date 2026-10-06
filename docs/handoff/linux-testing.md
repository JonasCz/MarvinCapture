# Linux host testing: handoff

What is still unchecked on Linux after the warm-start, USB-transfer-size and macOS
CPU work, and how to check it. Host: `jonas@192.168.0.103` (Ubuntu, Intel xHCI), with a
**510-USB (2304:0223)** attached. The user attaches the device and, for the QR runs, the
source (QR test DVD into S-Video or composite). Background:
[startup.md](../startup.md#warm-start), [analog.md](../analog.md#usb-transfer-size-matters-a-lot),
`.claude/skills/linux-host-device/SKILL.md`.

## Rules

- **Replug rule.** If the device ends up needing a replug or power cycle (config channel
  wedged, "USB error" on every start, no answer to `06 00`), stop and ask the user. Try
  one normal run first; a full bring-up usually recovers a wedged channel.
- A DVD that is stopped or paused sends no picture: no QR codes, the run proves nothing.
  Ask the user to confirm it is playing before each capture.
- Always wrap the CLI in `timeout` (a capture without a terminating `--wait` hangs ssh).
- Do not run heavy jobs (builds, the analyzer) during a capture.
- Do not load FX2 firmware on the 510-USB (it has its own in ROM).

## Setup

Sync the working tree **without committing**: tracked files with their current content
plus new untracked ones (from the repo root in Git Bash, or the same on the Mac):

```
git ls-files -z -c -o --exclude-standard CMakeLists.txt src tests firmware scripts docs third_party LICENSE \
  | tar --null -T - -czf - \
  | ssh jonas@192.168.0.103 'rm -rf ~/marvin-main && mkdir -p ~/marvin-main && cd ~/marvin-main && tar xzf -'
```

To save the FFmpeg build, copy `third_party/ffmpeg-src` from the previous tree first. Then
on the host:

```
cd ~/marvin-main && bash scripts/build-ffmpeg.sh && bash scripts/build.sh --skip-gui
ls -l /dev/bus/usb/001/*                      # udev rule 99-pinnacle.rules gives 0666, no sudo
./build/linux-x86_64/dist/MarvinCaptureCLI    # device table: 510-USB, serial, not "in use"
```

`CLI=./build/linux-x86_64/dist/MarvinCaptureCLI` below. `scripts/build.sh` and the
`build/linux-x86_64/` layout have not been run on the host yet; if the script fails, fix
the skill (fallback: `cmake -B build -S . && cmake --build build -j6`).

## Checklist

### a. Cold start after a replug

Ask the user to unplug and replug the device, then:

```
timeout 40 $CLI --debug -i composite --wait wallclock=00:00:05 2> cold.log; echo rc=$?
```

Expect: rc 0, the fingerprint log line showing `06 00` = 0 (no design) and the full
cold path (power-up, upload, 1.1 s wait), Ready after ~3.4 s (Windows figure). Run it
again right away: that one must be warm (see b). Record the timings.

### b. Warm starts and switching

Each with `--debug`; note seconds to Ready and the fingerprint / warm lines. Expected
(510-USB on Windows): analog warm 0.26-0.32 s, S-Video <-> composite 0.06-0.25 s, analog
over OHCI 1.43 s, DV warm 1.68 s, DV over Capture 3.2 s. Linux should be in the same range.

| test | how | expect |
|---|---|---|
| analog warm | run the analog command twice | second run: Capture recognised, ~0.3 s, no upload |
| analog over OHCI | DV bring-up (`--wait wallclock=00:00:05`), then analog | OHCI recognised, upload runs, ~1.4 s |
| DV warm | DV bring-up twice | second run: OHCI recognised, ~1.7 s ("no camera answered" is normal) |
| DV over Capture | analog, then DV | ~3.2 s |
| input switch | `-i svideo --wait wallclock=00:00:05 -i composite --wait wallclock=00:00:05` | no second bring-up, only the decoder input write, 0.06-0.25 s |

Any warm failure must show a cold redo in the log and still end in a working run.

### c. QR capture, 5 minutes (S-Video, then composite)

```
timeout 400 $CLI --debug -i svideo --std pal --capture ~/qr-svideo.mkv --wait-any signal=00:00:05 --wait-any captured=00:05:00 2> qr-svideo.log
```

Repeat with `-i composite`. Expect a clean log (no short or repaired frames) and from the
analyzer 0 missing, 0 duplicate, 0 undecodable frames and 0 missing audio. Also run one
right after a DV -> analog switch and one after `kill -9` of a running capture; both
must still be clean.

Analyzer: `C:\Users\Jonas\Desktop\scripts\analyze-qrcode\analyze_qrcode_video.py`. Easiest
is to copy the capture to Windows (`scp jonas@192.168.0.103:qr-svideo.mkv H:\test\`) and run
`python analyze_qrcode_video.py "H:\test\qr-svideo.mkv" --max-duration 0` there. On Linux it
needs `numpy`, `zxingcpp` (pip), `ffmpeg` and `ltcdump`, and the script calls
`SCRIPT_DIR/ltcdump.exe` (lines 185 and 190): build `ltcdump` (libltc) and symlink it as
`ltcdump.exe` next to the script, or change those two paths.

### d. USB transfer size (experiment)

Linux stays at 512 x 8 KiB (200 ms queued). That is where the loss was measured: on Intel
xHCI 64 KiB and up lost ~20 % of the data, 16-20 KiB still gave 3-9 short frames per 1000,
8 KiB and 4 KiB none. Do not change the default. To re-check on this controller, edit
`VIDEO_XFER` / `VIDEO_QUEUE` in the non-Apple branch of `src/core/pinnacle_analog.c` (the
comment above them says how), rebuild, then for each size (say 16, 32, 64 KiB at the same
~4 MiB queued):

- run the 2-minute QR capture of c with the analyzer, and read the short-frame counters in
  the log;
- measure the USB thread's CPU (e).

Expected: 8 KiB clean, larger sizes show short frames. If a size is clean at 200 ms and
also with a short queue (25-50 ms) under CPU hogs (`yes > /dev/null`, one per core), report
numbers and the CPU saving. Revert the defines afterwards.

### e. CPU use

During a 2-minute analog capture:

```
top -H -p $(pgrep -n MarvinCaptureCLI)                # per-thread %CPU
pidstat -t -p $(pgrep -n MarvinCaptureCLI) 5 12       # if sysstat is installed
```

Report the read loop / libusb thread (macOS was ~15 % of a core at 8 KiB) and the whole
process. No target; this is the Linux baseline.

### f. Teardown crash hunt

One unreproduced segfault (rc 139 after Ready) was seen on Windows in a loop of short
analog runs, followed by a wedged config channel that needed a replug. About 290 later runs
were clean. Loop:

```
ulimit -c unlimited
for i in $(seq 50); do
  timeout 30 $CLI --debug -i composite --wait wallclock=00:00:03 2> run$i.log; rc=$?
  echo "$i rc=$rc"; [ $rc -ne 0 ] && break
done
```

Vary it: `-i svideo`, DV, and runs interrupted at different times (`timeout -s INT 2 ...`,
a close during bring-up). On a crash run `gdb -batch -ex bt $CLI core` (or run the loop body
under `gdb -batch -ex run -ex bt --args ...`), keep the log, and **stop**: a wedged channel
needs a replug, ask the user. A suspect that would hang rather than crash: a close during
bring-up can have its loop-stop flag reset by `do_run_analog` and similar in
`src/engine/pin_session.c`.

### g. ctest

`cd build/linux-x86_64/core && ctest`. 27 tests, all passed on Linux before this work;
expect the same, including `test_stdout_sinks` (needs the current FFmpeg build).

## Report back

Per item: pass or fail, the timings, analyzer counts, CPU numbers, and any log line that did
not match the expectations above. Update this file and the docs it links with what Linux
actually showed.

## Results, Linux (2026-10-06; i5-9500T HP ProDesk Mini, Ubuntu, kernel 6.14, **500-USB** 2304:0213)

- **Build:** needs `ninja-build` (installed by the user); `scripts/build.sh --skip-gui` works.
  ctest 33/33. Passwordless sudo is enabled on the host.
- **Warm starts (b):** all pass (analog warm, analog over DV with upload, DV warm, DV over analog,
  input switch with no second bring-up, 0.2 s for the switch itself). Time to Ready for a
  warm analog start 0.45-1.15 s (auto standard waits for lock), DV over analog ~2.6 s.
- **Transfer size (d) and the real cause:** at the old 512 x 8 KiB, an idle machine dropped
  frames (5-150 per 2 minutes) in 15-35 ms bursts. Not queue depth (at most 3 of 1024 pending),
  not priority (SCHED_FIFO changed nothing), not 8 KiB itself: wake-up latency from deep CPU idle
  states. Fixed by holding `/dev/cpu_dma_latency` at 0 (0 drops in 10 runs from 8 to 64 KiB), by
  disabling C3+ (0 in 3), or by a busy loop on any core. Without that, 4 KiB x 1024 had fewer drops
  (7 vs 47 in 4 x 90 s) and is now the Linux default. Details: docs/analog.md, "Power saving and
  capture reliability (Linux)". The core now requests the limit itself when the node is writable
  (udev rule in that section).
- **QR (c), composite, PAL, analyzer on Windows:** 5 min with the limit held: 0 undecodable, 0 missing,
  0 duplicate, 0 audio gaps. 5 min without it (default user): 13 missing, 10 undecodable, 3 duplicate
  (all in the first ~3 s). 3 min after a DV -> analog switch and 3 min after `kill -9` of a running
  capture, limit held: 0 everywhere. S-Video was not run on Linux (the DVD is on composite there).
- **CPU (e), 8 KiB / 4 KiB / 2 KiB:** USB read-loop thread ~13% / ~23% / ~24% of a core; the
  FFV1 encoder threads ~30% each x4; the sampling itself perturbs drops.
- **Teardown (f):** 50 composite runs, 15 S-Video, 10 DV, 15 SIGINT at 1-4 s, 5 `kill -9`: all rc 0
  (or 124/130 as expected), no core, device still fine afterwards. The Windows segfault did not show.
- **DV read loop** now gets the same boost (`pin_thread_boost.c`, shared with analog): a SCHED_FIFO 10
  thread is visible in `ps -eLo tid,cls,rtprio` for both DV and analog. Not yet verified on
  Windows/macOS beyond a Windows core build and ctest (43/43).
- **Recommendations block:** the CLI prints a "Performance recommendations" block after the device
  list and at the start of a run (hub + host items, `pin_perf_check`); the unprivileged-user
  fallback (idle-class spin thread) gave 0 drops in four 2-minute captures; the udev commands
  in the message were run and work (node 0666, "set", block gone; rule removed again afterwards).
- **Not done:** cold start after a replug (a), which needs the user.

