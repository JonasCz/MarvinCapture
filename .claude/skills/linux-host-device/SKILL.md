---
name: linux-host-device
description: Build and run the core and MarvinCaptureCLI on the Linux SSH host (jonas@192.168.0.103) where the Pinnacle 510-USB (2304:0223) is attached; sync the tree, build, no-sudo udev access, where the Ghidra decompilation of the vendor driver lives.
---

# Linux host with the 510-USB

Host: `jonas@192.168.0.103` (Ubuntu). Plain `ssh` key login works from Windows Git Bash (BatchMode);
no password is ever used. If key login fails, stop and ask the user.

## Sync the tree (from the repo root in Git Bash)

```
git ls-files -z CMakeLists.txt src tests firmware scripts docs third_party LICENSE | tar --null -T - -czf -   | ssh jonas@192.168.0.103 'rm -rf ~/marvin-main && mkdir -p ~/marvin-main && cd ~/marvin-main && tar xzf -'
```
Only tracked files are sent (commit or `git add` new files first). The gui/ folder is Windows-only.

## Build on the host

FFmpeg is built once per tree with the repo script (needs the current config: the nut muxer etc.;
an old prebuilt slice makes test_stdout_sinks fail with "no NUT muxer"). Source tarballs can be
reused by copying `third_party/ffmpeg-src` from a previous tree first:
```
cd ~/marvin-main && bash scripts/build-ffmpeg.sh        # ~30 s with the tarball present, minutes otherwise
bash scripts/build.sh --skip-gui                         # core, CLI, ctest
```
Outputs: `build/linux-x86_64/dist/MarvinCaptureCLI`, `build/linux-x86_64/dist/libmarvin-core.so`
(CMake tree in `build/linux-x86_64/core`), 27 ctests (all pass on Linux as of the rename).
`scripts/build.sh` needs `ninja` (installed on the host); it works with the per-platform layout.
Passwordless sudo is enabled for jonas. The attached device may be the 500-USB (2304:0213) or
the 510-USB; the CLI's device table says which. `/dev/cpu_dma_latency` is `root 0600`; for
capture tests that should be clean on a quiet machine, `sudo chmod 666 /dev/cpu_dma_latency`
(and back to 600 after), see docs/analog.md "Power saving and capture reliability". Do not
`pkill -f` a pattern that appears in your own ssh command line (it kills the session).
`MarvinCaptureCLI --help` ends with "No devices found." when no device is attached.

## Running against the device

`/etc/udev/rules.d/99-pinnacle.rules` (`SUBSYSTEM=="usb", ATTR{idVendor}=="2304", MODE="0666"`)
makes the node world-accessible: run everything as `jonas`, **no sudo** (the auto-mode
classifier refuses `sudo -S` anyway). Check with `ls -l /dev/bus/usb/001/*`. Without access
the CLI says `device already open in another process` (that is EACCES).

No camera is attached, so only bring-up can be tested. Quick checks from `~/marvin-main`:

- `./build/linux-x86_64/dist/MarvinCaptureCLI` (no arguments): help and the device table -- model name, `(untested)` flag, GUID serial.
- DV bring-up to "ready": `timeout 60 ./build/linux-x86_64/dist/MarvinCaptureCLI --debug --wait wallclock=00:00:05 2> d.log`
  (the first action is a wait, so the session comes up as DV) expects `NodeID 0xc000ffc0 ... node 0 of 1`
  and "no camera answered on the 1394 bus" in the log.
- Analog bring-up: `timeout 40 ./build/linux-x86_64/dist/MarvinCaptureCLI --debug -i composite --wait wallclock=00:00:05 2> a.log`
  (decoder answers, "no signal", exit 0). **Always** wrap the CLI in `timeout`: a capture without a
  terminating `--wait` runs until Ctrl-C and hangs the ssh call (then `pkill MarvinCaptureCLI`).
- Crash hunting: `gdb -batch -ex run -ex bt --args ./build/linux-x86_64/dist/MarvinCaptureCLI ...` (gdb is installed).

Do not run anything that loads the FX2 firmware (`pinnacle_ensure_fx2`) against
the 510-USB: it is a no-op there (`fx2_firmware` is NULL for every model but
0206), and the 510 has its firmware in ROM.

## Vendor driver decompilation on the host

`~/tools/decompiled_avs.c` (MarvinAVS64.sys, Ghidra, `FUN_0002c280` bitstream select at
line ~19899, PID branches by grepping `0x223`), `~/tools/decompiled_all.c` (MarvinBus64),
`~/pinnacle-driver/marvin/*.sys` (the binaries), Ghidra in `~/tools/ghidra_12.1.4_PUBLIC`.
What the PID branches say is summarised in docs/hardware.md (Models).

Useful starting points in `decompiled_avs.c` (see docs/hardware.md, Models): `FUN_00019588`
(capability word per PID, `0xc4` = PID field), `FUN_0002c280` (bitstream per PID), `FUN_0002bf8c`
(FX2 `.bix` download), `FUN_0002cb5c`/`FUN_0002cc90` (identity reads: `80 idx 08` for the CR
family, vendor request 0xA0 for classic). `grep -n '0x206\|0x212\|0x224'` finds every PID branch.
Edit scripts: when patching C with python, do not put `\n` inside a non-raw triple-quoted string
that is also a C string literal; use the Edit tool, and grep the build log for `error` (a stale
binary makes a failed build look like a pass).
