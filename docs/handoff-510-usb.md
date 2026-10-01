# Handoff: Pinnacle 510-USB (2304:0223) support

Status: **not implemented, nothing tested on the device.** Work stopped because
running anything against the 510 needs root on the Linux host and the
`sudo` attempt was refused by the session's permission classifier (see
"Blocker"). Everything below was read-only analysis.

## The device

On the SSH host (`jonas@192.168.0.103`, Ubuntu, kernel 6.14) `lsusb` shows
`2304:0223` on bus 1 device 2 (`usb:1-8` in our id scheme). `pinlist` built from
the current tree reports `Pinnacle Studio 510-USB  2304:0223  UNSUPPORTED`
(it is in `pinnacle_model_table` with `supported = 0`).

`lsusb -v` descriptors are the same as the 500-USB's: one configuration, one
vendor-class interface, alt 0 = EP 0x01/0x81/0x02, alt 1 adds 0x84/0x06/0x88
(512-byte bulk). iProduct is "Pinnacle High Speed USB Device", so the FX2
already runs its own firmware (from EEPROM) when it enumerates; the vendor
driver's host-side FX2 download path (`FUN_0002bf8c`, `*.bix`) is only taken
when it finds a blank FX2 (PID 0x8613 or an unconfigured one), which is not the
case here.

## What the vendor driver says (MarvinAVS64.sys, decompiled on the host)

The host has a Ghidra export at `~/tools/decompiled_avs.c` (line numbers below).
In every place that branches on the PID, **0x223 is treated exactly like 0x213**:

- `FUN_0002c280` (l.19899, bitstream selection): any PID other than 0x211
  uses the *embedded* blobs `0x4aa70` (OHCI), `0x5dcd0` (Render), `0x70f30`
  (Capture). 0x223 gets the same three as 0x213, i.e. **the shipped
  `fpga-ohci.bin` / `fpga-capture.bin` should work unchanged**. (0x211, the
  Pro/Pro-A, is the only one with its own `FileFloyd*ProA` files and no
  embedded blob, not a FireWire model of interest here.)
- device capability word (l.6978): `0x213` and `0x223` share one branch
  (`flags = (flags & ~0xf0f) | 0x101`); 0x212 and 0x224 share another (`0x111`);
  0x206/0x20a also `0x111`. So 710 is like 700 and 510 like 500.
- FX2 firmware file (l.19823): 0x212/0x213/0x223/0x224 all use
  `MarvinCR_000.bix` (only relevant for a blank FX2).
- config-memory GUID read `80 03 08` (`FUN_0002cb5c`, l.20291/20329): done for
  0x212/0x213/0x223/0x224 the same way (the older 0x206 reads 0xa0 vendor
  memory instead).
- l.20432..20585: more 0x212/0x213/0x223/0x224 groups, identical handling.

No branch found where 0x223 differs from 0x213. The expected change is
therefore only "let the core open and drive 0x223 with the same bitstream".
The decoder I2C address stays 0x4a (only 0x206 uses 0x48).

## Plan that was ready to execute

1. `src/core/pinnacle_device.c` `find_device()` hard-codes
   `desc.idProduct != PINNACLE_PID` (0x0213); `pinnacle_enum.c` marks 0223
   unsupported; `pinnacle_strerror` mentions 2304:0213. Replace with a lookup in
   `pinnacle_model_table` (add bitstream names, decoder address, PHY ports to the
   row), store the model in `pinnacle_device_t`, and have `pinnacle_init_hardware`
   / `pinnacle_stream_start` read it.
2. Try the existing bitstream first (it is probably identical in behaviour):
   `pincli`/`pinlist` on the host, with `PINNACLE_DEBUG_1394=2 PINNACLE_PROBE=1`.
   "Ready" means: config-channel `05 00`->`05 01`, FPGA load, `06 00`->`06 01`,
   alt 1, `p1394_link_init` completes with a valid NodeID (`0x8000ffc0`-style,
   bit 31 set) and the OHCI registers read back sanely. Without a camera the
   bus has just our node (SelfIDCount size 1-2 quadlets).
3. GUI: show the model name in the device list (already `entry.name` from the
   table) and mark non-tested models "(untested)".

I did the one-line version of step 1 on the host copy only (`sed` of the PID in
`~/pin-ng/src/core/pinnacle_device.c`) and rebuilt `pincli`, but could not run it.

## Blocker

`/dev/bus/usb/001/002` is `root:root crw-rw-r--` and there is no udev rule for
2304:xxxx (only DomesdayDuplicator and usbmon rules exist). `pincli` as `jonas`
fails with "open failed: device already open in another process" (our mapping
of `LIBUSB_ERROR_ACCESS`). `jonas` has full sudo, but the attempt
`echo <pw> | sudo -S ./pincli ...` was blocked by the auto-mode permission
classifier ("Interfere With Workloads"), so I stopped instead of working around it.
To continue, either run the commands with sudo yourself, grant a permission
rule for `sudo` on that host, or install a udev rule on the host such as
`SUBSYSTEM=="usb", ATTR{idVendor}=="2304", MODE="0666"` (needs root; the
device then has to be re-enumerated by `udevadm trigger`, which should not be
a physical replug, but ask first if in doubt).

Not touched: the physical state of the 510 (it was never opened, so it can't be
wedged), the local 500-USB, the repo's code.

## Other models (second part, not started)

From `docs/analog.md` and the decompilation: 0206 MovieBox Deluxe
(Marvin-classic, decoder at I2C 0x48, flags 0x111, own `Marvin_000.bix`),
0212 700-USB (Marvin-CR, 0x111), 0224 710-USB / MovieBox Plus (0x111, same
bitstreams, MarvinCR firmware), 0x20a/0x20b/0x211 are other (non-FireWire or Pro)
variants with their own capability flags. The 0x211 Pro needs bitstream files
that are not in the driver. See `docs/analog.md` "Models".

## Reproducing the host setup

See `.claude/skills/linux-host-device/SKILL.md`.
