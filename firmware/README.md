# FPGA bitstreams

The Studio 500-USB's FPGA has no flash: the host uploads one of three
78,422-byte Altera bitstreams every time it brings the device up.

| File | Used for |
|---|---|
| `fpga-ohci.bin` | DV / HDV over FireWire |
| `fpga-capture.bin` | Analog capture (composite / S-video + line audio) |

Every supported model (500/510/700/710 and MovieBox Deluxe) uses the same two
files: the vendor driver embeds one set of bitstreams for every PID except the
Pro. The 500 and 510 need no FX2 (USB controller) firmware; the classic MovieBox
Deluxe gets `fx2-marvin.bin` (below). Which file a model takes is a column
of the model table (`src/core/pinnacle_model.c`).

A third design, Render (analog output from the PC), exists in the vendor driver
but nothing here uses it, so it is not shipped.

They were extracted from the vendor driver with `scripts/extract-bitstreams.py`
(which also writes the Render one) and are shipped next to the application so
it works out of the box: `scripts/build.ps1` copies this directory into
`build\dist\firmware\`. The core looks for
`firmware/` next to the core library, then next to the executable, unless the
`firmware_dir` setting says otherwise.

| File | MD5 |
|---|---|
| `fpga-ohci.bin` | `3888c23c9bcc81c88964c45d981a0b68` |
| `fpga-capture.bin` | `280bacc631c6a69b831734e3bc72969d` |
| `fx2-marvin.bin` | `22f873f4c47d52de4a4423ef19e6e29b` |

## FX2 firmware (`fx2-marvin.bin`)

A raw 8051 image (4,944 bytes, starts `02 0c 4c`, an LJMP) for the Cypress
FX2 USB controller, taken from the same `MarvinAVS64.sys` (VA `0x49720`,
`0x1350` bytes, written by `scripts/extract-bitstreams.py`). Only the MovieBox
Deluxe (2304:0206, "classic" firmware) uses it: when its `07 00` config probe
is not answered, the core downloads this file over EP0 (CPUCS held in reset,
vendor request `0xA0`, 512-byte blocks, CPUCS released), waits for the unit to
re-enumerate and carries on (`pinnacle_ensure_fx2`, `src/core/pinnacle_fx2.c`).
The 500/510/700/710 boot from EEPROM and never load it. The download has not
been tried on real hardware (no Deluxe unit available). Same copyright and
licence note as the bitstreams below.

**These files are Pinnacle Systems' copyright, not ours, and are not covered by
this project's licence.** They are included only because the hardware is inert
without them and the device has been discontinued since around 2005. If the
rights holder objects they will be removed, and the driver will fall back to
extracting them from the user's own vendor driver install. The vendor drivers
themselves are not distributed.
