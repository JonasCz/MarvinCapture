# FPGA bitstreams

The Studio 500-USB's FPGA has no flash: the host uploads one of three
78,422-byte Altera bitstreams every time it brings the device up.

| File | Used for |
|---|---|
| `fpga-ohci.bin` | DV / HDV over FireWire |
| `fpga-capture.bin` | Analog capture (composite / S-video + line audio) |

A third design, Render (analog output from the PC), exists in the vendor driver
but nothing here uses it, so it is not shipped.

They were extracted from the vendor driver with `scripts/extract-bitstreams.py`
(which also writes the Render one) and are shipped next to the application so
it works out of the box: `scripts/build.ps1` copies this directory into
`build\dist\firmware\` (and `build\dist\cli\firmware\`). The core looks for
`firmware/` next to the core library, then next to the executable, unless the
`firmware_dir` setting says otherwise.

| File | MD5 |
|---|---|
| `fpga-ohci.bin` | `3888c23c9bcc81c88964c45d981a0b68` |
| `fpga-capture.bin` | `280bacc631c6a69b831734e3bc72969d` |

**These files are Pinnacle Systems' copyright, not ours, and are not covered by
this project's licence.** They are included only because the hardware is inert
without them and the device has been discontinued since around 2005. If the
rights holder objects they will be removed, and the driver will fall back to
extracting them from the user's own vendor driver install. The vendor drivers
themselves are not distributed.
