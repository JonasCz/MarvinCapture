# Pinnacle Studio 500-USB — open driver

An open user-space driver, command-line tools and a Windows capture app for the
**Pinnacle Studio 500-USB** (`USB\VID_2304&PID_0213`, codename *Marvin-Lite*), a
discontinued ~2005 analog/DV/HDV capture box. The **510-USB** (`PID_0223`)
brings up identically (verified without a camera; capture itself not yet
tested on it). The 700-USB, MovieBox Plus / 710-USB and MovieBox Deluxe are
implemented from the vendor driver's code but **untested** (flagged in the GUI
and the CLIs). The models live in one table, see
[docs/hardware.md](docs/hardware.md#models). The vendor driver is XP/Vista-era
and increasingly unusable; this reverse-engineers the protocol from scratch and
implements it on libusb.

**What works**

- **DV capture** over FireWire, verified: a 5-minute capture is 8,967 frames with
  zero dropped DIF sequences, zero CIP/DBC discontinuities and zero ffmpeg decode
  errors.
- **HDV capture** (MPEG-2 transport stream), verified the same way.
- **Analog capture** (composite, PAL tested): AVI with uncompressed YUY2 video and
  48 kHz stereo PCM, frame-exact, audio locked to video. S-video and NTSC are
  implemented but untested.
- **Deck control**: play, pause, stop, FF, REW, timecode over AV/C.
- **PinnacleCapture**, a WinUI 3 app: live preview, capture with scene splitting,
  DV/AVI/MOV/MKV/FFV1 outputs, deck control, audio monitoring.

## Quick start

```powershell
scripts\build.ps1
```

builds everything (needs MSYS2 UCRT64 and the .NET 10 SDK; see
[docs/building.md](docs/building.md)) into `build\dist`:

```
build\dist\PinnacleCapture.exe        the GUI
build\dist\pinnacle-oss-core.dll      the core library
build\dist\firmware\                  FPGA bitstreams
build\dist\cli\                       pincli pinanalog pindeck pinlist pinctl
```

The device has to be bound to WinUSB, not the vendor driver
([docs/windows-driver.md](docs/windows-driver.md)). Then either run
`PinnacleCapture.exe`, or from `build\dist\cli`:

```powershell
.\pincli.exe -o out.dv -t 300         # DV/HDV capture (.dv, or .ts for HDV)
.\pinanalog.exe -o out.avi -t 60      # analog capture
.\pindeck.exe state play wait:5 stop  # deck control
```

The device needs about 5.5 s of bring-up before the first byte arrives, and the
camera must actually be transmitting; see [docs/usage.md](docs/usage.md) for how
to tell a fault from a quiet camera and how to check that nothing was dropped.

## How it is put together

```
PinnacleCapture (WinUI 3)  ──┐
pinctl                     ──┼──▶  pinnacle-oss-core.dll  (src/api/pin_api.h)
                             │        └─ session engine (src/engine), file writers (src/sinks)
pincli, pinanalog, pindeck ──┴──▶  hardware layer (src/core) ──▶ libusb ──▶ device
```

**The interesting finding:** the FPGA implements a standard OHCI-1394 host
controller, and EP 0x88 carries its isochronous receive DMA tunnelled over USB.
The stream is not raw DV: two layers of framing sit on top of the DIF data. Strip
both and you get a byte-exact DV elementary stream. All endpoints are bulk, so
there is no isochronous USB at all.

## Documentation

Everything is indexed in **[docs/README.md](docs/README.md)**. The main pages:

| | |
|---|---|
| [docs/building.md](docs/building.md) | Build script, output layout, tests, repository layout |
| [docs/usage.md](docs/usage.md) | Running captures, diagnostics, proving no data was dropped |
| [docs/hardware.md](docs/hardware.md) | USB descriptors, endpoints, initialisation, what the FPGA is |
| [docs/protocol.md](docs/protocol.md) | Command word, OHCI access, EP 0x88 framing |
| [docs/startup.md](docs/startup.md) | Plug-in to first stream byte, step by step |
| [docs/deck-control.md](docs/deck-control.md) | AV/C deck control |
| [docs/hdv.md](docs/hdv.md) | HDV capture |
| [docs/analog.md](docs/analog.md) | Analog capture, other Marvin models, the three bitstreams |
| [docs/windows-driver.md](docs/windows-driver.md) | Switching the device to WinUSB |
| [gui/windows/README.md](gui/windows/README.md) | The GUI |

## A note on the FPGA bitstreams

`firmware/fpga-ohci.bin` (DV/HDV) and `firmware/fpga-capture.bin` (analog) are
**Pinnacle's copyright, not ours.** They are static Altera Cyclone EP1C3
configuration blobs extracted from the vendor driver, and the hardware is inert
without them. They are included as a pragmatic decision: the device was
discontinued around 2005, the vendor driver is no longer distributed or
supported, and without them this repository would be useless to anyone who owns
the hardware. No claim of ownership is made and no licence is granted by us. If
the rights holder objects they will be removed and the driver will fall back to
extracting them from the user's own vendor driver install
(`scripts/extract-bitstreams.py`). See [firmware/README.md](firmware/README.md).

The vendor driver binaries and installer are **not** included.

## Licence

**GNU Affero General Public License v3.0** ([LICENSE](LICENSE)). If you run a
modified version of this code as a network service, you must offer its source to
that service's users.

The FPGA bitstreams are **excluded** from that grant; they are not ours to
license. FFmpeg is linked statically under LGPL-2.1+ ([third_party/README.md](third_party/README.md)).
