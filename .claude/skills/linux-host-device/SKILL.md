---
name: linux-host-device
description: Build and run the core/CLIs on the Linux SSH host (jonas@192.168.0.103) where the Pinnacle 510-USB (2304:0223) is attached; sync the tree, build, no-sudo udev access, where the Ghidra decompilation of the vendor driver lives.
---

# Linux host with the 510-USB

Host: `jonas@192.168.0.103` (password in the task/user message; Ubuntu, paramiko works
from Windows: `pip install paramiko`). No ssh/sshpass needed.

## Run commands / sync the tree (paramiko, from the session scratchpad)

- `ssh.py "<cmd>" [sudo] [timeout]`: `paramiko.SSHClient().connect(host, username, password)`,
  `exec_command`, print stdout+stderr. (`sudo` mode = `echo pw | sudo -S -p '' bash -c '<cmd>'`.)
- `sync.py`: `git ls-files CMakeLists.txt src tests firmware scripts docs`, tar+gzip in
  memory, `sftp.putfo` to `/tmp/pin-src.tgz`, then `mkdir -p ~/pin-ng && cd ~/pin-ng && tar xzf`.

## Build on the host

```
cd ~/pin-ng; ln -sfn ~/pinnacle-oss/third_party third_party   # prebuilt linux FFmpeg slice
cmake -B build -S . && cmake --build build -j4 && (cd build && ctest)
```
The full build (engine, all CLIs, 23 ctests) works on Linux. Sync first (`sync.py` above; it also
sends untracked files under src/tests/firmware/scripts/docs).

## Running against the device

`/etc/udev/rules.d/99-pinnacle.rules` (`SUBSYSTEM=="usb", ATTR{idVendor}=="2304", MODE="0666"`)
makes the node world-accessible: run everything as `jonas`, **no sudo** (the auto-mode
classifier refuses `sudo -S` anyway). Check with `ls -l /dev/bus/usb/001/*`. Without access
the CLIs say `device already open in another process` (that is EACCES).

No camera is attached, so only bring-up can be tested. Quick checks from `~/pin-ng`:

- `./build/pinlist` / `./build/pinctl list` -- model name, `(untested)` flag, GUID serial.
- DV bring-up to "ready": `PINNACLE_DEBUG_1394=2 PINNACLE_PROBE=1 timeout 60 ./build/pindeck -vv state`
  expects `NodeID 0xc000ffc0 ... node 0 of 1` and "no camera on the 1394 bus".
- Analog bring-up: `timeout 40 ./build/pinanalog --status` (decoder answers, "NO SIGNAL").
  **Always** wrap CLIs in `timeout` and give `pinanalog` `-t`/`--status`: bare `pinanalog` runs forever
  and hangs the ssh call (then `pkill pinanalog`).
- Crash hunting: `gdb -batch -ex run -ex bt --args ./build/pindeck ...` (gdb is installed).

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
