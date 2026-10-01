---
name: linux-host-device
description: Build and run the core/CLIs on the Linux SSH host (jonas@192.168.0.103) where the Pinnacle 510-USB (2304:0223) is attached; sync the tree, build, the sudo/permission gotcha, where the Ghidra decompilation of the vendor driver lives.
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
cmake -B build -S . && cmake --build build -j4 --target pincli pinlist pindeck pinanalog
```
A full `cmake --build` currently fails in `src/engine/pin_session.c` (`Dl_info`/`dladdr`
needs `_GNU_SOURCE`/`<dlfcn.h>` on glibc); the CLIs build fine without the engine.

## Running against the device

The USB node is root-only (no udev rule for 2304:*), so CLIs need root: `sudo ./pincli ...`
(use absolute paths, sudo's cwd/HOME is not jonas's). Without root, `open failed:
device already open in another process` is really EACCES. In the 2026-10-02 session the
auto-mode classifier refused `echo pw | sudo -S`; ask the user to allow it or to install a
udev rule before relying on it.

## Vendor driver decompilation on the host

`~/tools/decompiled_avs.c` (MarvinAVS64.sys, Ghidra, `FUN_0002c280` bitstream select at
line ~19899, PID branches by grepping `0x223`), `~/tools/decompiled_all.c` (MarvinBus64),
`~/pinnacle-driver/marvin/*.sys` (the binaries), Ghidra in `~/tools/ghidra_12.1.4_PUBLIC`.
Findings: docs/handoff-510-usb.md.
