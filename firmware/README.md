# FPGA bitstreams

The Studio 500-USB's FPGA has no flash: the host uploads one of three
78,422-byte Altera bitstreams every time it brings the device up.

| File | Used for |
|---|---|
| `fpga-ohci.bin` | DV / HDV over FireWire (same file as `traces/fpga-bitstream-candidate.bin`) |
| `fpga-capture.bin` | Analog capture (composite / S-video + line audio) |
| `fpga-render.bin` | Analog output from the PC; not used yet |

They were extracted from the vendor driver with `tools/extract-bitstreams.py`
and are shipped next to the application so it works out of the box. The core
looks for them in `firmware/` next to the executable unless the `firmware_dir`
setting says otherwise.

**These files are Pinnacle Systems' copyright, not ours, and are not covered by
this project's licence.** They are included only because the hardware is inert
without them and the device has been discontinued since around 2005. The
vendor drivers themselves are not distributed.
