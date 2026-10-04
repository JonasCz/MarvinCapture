# Pinnacle Studio 500-USB — open driver

An open user-space driver, a command-line program and a Windows capture app for the
**Pinnacle Studio 500-USB** (`USB\VID_2304&PID_0213`, codename *Marvin-Lite*), a
discontinued ~2005 analog/DV/HDV capture box. The **510-USB** (`PID_0223`)
is handled identically and verified on Windows too (bring-up, DV capture, deck
control, stdout streaming). The 700-USB, MovieBox Plus / 710-USB and MovieBox Deluxe are
implemented from the vendor driver's code but **untested** (flagged in the GUI
and the CLI). The models live in one table, see
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
- **MarvinCaptureGUI**, a WinUI 3 app: live preview, capture with scene splitting,
  DV/AVI/MOV/MKV/FFV1 outputs, deck control, audio monitoring.

## Quick start

```powershell
scripts\build.ps1
```

builds everything (needs MSYS2 UCRT64 and the .NET 10 SDK; see
[docs/building.md](docs/building.md)) into `build\windows-x86_64\dist`:

```
build\windows-x86_64\dist\MarvinCaptureGUI.exe        the GUI
build\windows-x86_64\dist\MarvinCaptureCLI.exe        the command-line program
build\windows-x86_64\dist\marvin-core.dll             the core library
build\windows-x86_64\dist\firmware\                   FPGA bitstreams
```

On macOS and Linux, `bash scripts/build.sh` builds the core and
`MarvinCaptureCLI` into `build/<os>-<arch>/dist` (for example
`build/macos-arm64/dist`, `build/linux-x86_64/dist`).

The device has to be bound to WinUSB, not the vendor driver
([docs/windows-driver.md](docs/windows-driver.md)). Then either run
`MarvinCaptureGUI.exe`, or from `build\windows-x86_64\dist`:

```powershell
.\MarvinCaptureCLI.exe                                   # help and the device list
.\MarvinCaptureCLI.exe --rew --wait --play --capture out.dv --wait --rew --wait
.\MarvinCaptureCLI.exe -i composite --capture out.avi --wait wallclock=00:01:00   # analog, 60 s
```

The command language is in [docs/cli.md](docs/cli.md); add `--debug` to see the
raw AV/C traffic and the bring-up steps.

The device needs about 5.5 s of bring-up before the first byte arrives, and the
camera must actually be transmitting; see [docs/usage.md](docs/usage.md) for how
to tell a fault from a quiet camera and how to check that nothing was dropped.

### Connection via USB hub

Plug the device directly into a USB port of the computer, not into a hub
(including front-panel hubs, docks and monitor hubs): behind a hub it shares
the hub's bandwidth with the other devices on it, which can drop frames. The
GUI and CLI warn when they detect a hub between the computer and the device
("connected via USB hub"). To see where it is plugged in: Windows,
[USBTreeView](https://www.uwe-sieber.de/usbtreeview_e.html) (Uwe Sieber);
Linux, `lsusb -t`; macOS, System Information > USB (`system_profiler
SPUSBDataType`). The warning is advisory: some computers wire their own ports
through a built-in hub, and then it can be ignored.

## How it is put together

```
MarvinCaptureGUI (WinUI 3)  ──┐
                              ├──▶  marvin-core.dll  (src/api/pin_api.h)
MarvinCaptureCLI            ──┘        └─ session engine (src/engine), file writers (src/sinks)
                                       └─ hardware layer (src/core) ──▶ libusb ──▶ device
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
