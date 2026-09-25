# Pinnacle Studio 500-USB — open driver

An open user-space driver and DV capture tool for the **Pinnacle Studio
500-USB** (`USB\VID_2304&PID_0213`, codename *Marvin-Lite*), a discontinued
~2005 analog/DV capture box. The vendor driver is 32/64-bit Windows XP-era and
increasingly unusable; this reverse-engineers the protocol from scratch and
implements it on Linux with libusb.

**Status: DV capture over the FireWire port works and is verified, and HDV
(MPEG-2 transport stream) capture works too — see [docs/hdv.md](docs/hdv.md).** A
5-minute continuous capture produces 8,967 frames with zero dropped DIF
sequences, zero CIP/DBC discontinuities and zero ffmpeg decode errors, and
stops cleanly.

The analog input path is not implemented (the SAA7113 decoder is initialised,
because the device refuses to work otherwise, but its video is not captured).
Deck control (play/pause/stop/FF/REW, timecode) works with the `pindeck`
tool, but is not yet wired into `pincli`. See
[docs/deck-control.md](docs/deck-control.md).

---

## What the hardware actually is

The interesting finding, which took most of the work: **the FPGA implements a
standard OHCI-1394 host controller**, and EP 0x88 carries its isochronous
receive DMA tunnelled over USB. The stream is not raw DV — it has two layers of
framing on top of the DIF data (variable-length type-9 messages, then OHCI
buffer-fill records holding IEC 61883 CIP packets). Strip both and you get a
byte-exact DV elementary stream.

All four endpoints are bulk, 512-byte max packet; there are no isochronous USB
endpoints at all. There is no host-side FX2 firmware download — the CY7C68013A
boots from its own EEPROM. The FPGA bitstream is a static 78,422-byte Altera
Cyclone EP1C3 `.rbf` blob that the driver simply replays.

Full protocol decode: **[docs/command-channel-findings.md](docs/command-channel-findings.md)**.

---

## Building

Needs `libusb-1.0` development headers and a C11 compiler.

```bash
cd src && make
```

Produces `build/pincli` and `build/libpinnacle500.a`. The core library is kept
separate on purpose so a future GUI links the same code without the
file-writing concerns.

## Usage

```bash
sudo ./build/pincli -o out.dv -b traces/fpga-bitstream-candidate.bin -t 300
```

| flag | meaning |
|---|---|
| `-o`, `--output` | raw DV output path (required) |
| `-b`, `--bitstream` | FPGA bitstream blob (default `traces/fpga-bitstream-candidate.bin`) |
| `-t`, `--duration` | stop after N seconds; without it, runs until Ctrl+C |
| `-h`, `--help` | usage |

`root` (or a udev rule granting access to `2304:0213`) is required to claim the
interface.

The device needs **~5.5 seconds of bring-up** before the first byte arrives:
bitstream upload, a 1.5 s FPGA settle, then the 1394 link start-up, finding the
camera and connecting to its output plug ([docs/startup.md](docs/startup.md)). `-t`
starts counting after that. A capture window shorter than the bring-up will
look empty and mislead you.

**The camera must actually be transmitting.** In tape mode with the tape
stopped, the device produces zero bytes and looks identical to a hardware
fault. [docs/capture-reliability.md](docs/capture-reliability.md) explains how
to tell those apart.

### Output

The format is detected from the stream. A DV camera gives a raw DV elementary
stream (`.dv`); an **HDV camera gives an MPEG-2 transport stream (`.ts`)** —
name the output accordingly. HDV details, and why its integrity check is
different, are in [docs/hdv.md](docs/hdv.md).

DV: a raw elementary stream, frame-aligned, playable directly:

```bash
ffplay out.dv
ffmpeg -i out.dv -c:v copy -c:a copy out.avi     # rewrap, no re-encode
```

NTSC and PAL are both handled (10 vs 12 DIF sequences per frame, detected from
the stream). Audio comes through as two `pcm_s16le` 32 kHz stereo tracks.

### Did it drop anything?

Every run ends with a continuity line based on the IEC 61883 CIP data block
counter — a counter the *camera* maintains, so it catches loss anywhere between
the camera's transmitter and your file:

```
pincli: continuity: OK — 335515 data blocks, no CIP/DBC discontinuity (1 at stream join, expected)
```

Cross-check it structurally with an independent tool:

```bash
python3 tools/dvcheck.py out.dv --duration-seconds 300
```

Note that **the camera emits no timecode in live view** (verified — the SMPTE
timecode pack is absent and REC DATE/TIME read "no information"), so timecode
cannot be used to verify continuity here. ffmpeg will print `Detected timecode
is invalid` once; that is expected and is not a problem with the capture.

### Diagnostics

All opt-in; the defaults are the right values.

| variable | effect |
|---|---|
| `PINNACLE_PROBE=1` | dump OHCI registers (NodeID, SelfIDCount, IR context state…) after the start sequence |
| `PINNACLE_QUEUE_DEPTH=<n>` | EP 0x88 transfers in flight (default 32). `1` reproduces the old synchronous loop **and its data loss** |
| `PINNACLE_STOP_DRAIN=0` | don't drain EP 0x88 during the stop sequence (A/B only — the stop then fails partway) |
| `PINNACLE_EP84_DRAIN=0` | disable the EP 0x84 status drain (A/B only) |
| `PINNACLE_DEBUG_EP88=1` | log every EP 0x88 completion's size and arrival time |
| `PINNACLE_DEBUG_EP84=1` | log every EP 0x84 status record |
| `PINNACLE_RAW_DUMP=<path>` | dump the raw EP 0x88 stream before reassembly |

---

## Documentation

| | |
|---|---|
| **[docs/capture-reliability.md](docs/capture-reliability.md)** | **Start here to use it.** Running a capture, telling a device fault from a quiet camera, and proving no data was dropped. |
| [docs/hdv.md](docs/hdv.md) | HDV over FireWire: what's on the wire, how the `.ts` is produced, and why the DBC check is weak for it. |
| [docs/startup.md](docs/startup.md) | Every step from plug-in to the first stream byte, and where each value comes from. |
| [docs/deck-control.md](docs/deck-control.md) | Play/stop/FF/REW over AV/C, and the 1394 transaction layer. |
| [docs/analog-notes.md](docs/analog-notes.md) | Notes for later: analog path, the three FPGA bitstreams, the other Marvin models. |
| [docs/command-channel-findings.md](docs/command-channel-findings.md) | The protocol. Command word format, OHCI register usage, the two layers of EP 0x88 framing, the 1394 connection-management transaction, and what's still open. |
| [HANDOFF.md](HANDOFF.md) | Overall state of the reverse-engineering effort: what's known about the hardware, what's still missing. |
| [FEASIBILITY.md](FEASIBILITY.md) | The original plan and phases. |
| [docs/usb-descriptors.md](docs/usb-descriptors.md) | Full USB descriptor decode. |
| [docs/linux-capture-setup.md](docs/linux-capture-setup.md) | The usbmon capture rig. |
| [docs/vm-control.md](docs/vm-control.md) | Driving the Windows VM that runs the vendor driver as a reference. |
| [docs/capture-tooling.md](docs/capture-tooling.md) | Why the bare-metal Windows USBPcap approach failed. Don't retry it. |
| [tools/README.md](tools/README.md) | Verification and capture-host scripts. |
| [traces/README.md](traces/README.md) | What each usbmon trace contains and proves. |
| [captures/README.md](captures/README.md) | Reference captures made by this driver. |

### Layout

```
src/core/     pinnacle_device.c   open, bitstream upload, bring-up
              pinnacle_1394.c     1394 link layer: link start-up, transactions, oPCR, AV/C
              pinnacle_stream.c   start/stop, queued EP 0x88 read loop
              dv_reassembler.c    the two framing layers + DIF frame / MPEG2-TS assembly
              protocol_data.h     config-channel sequence still replayed verbatim
src/cli/      pindeck.c           deck control (play/stop/ff/rew/state/timecode)
              pincli.c            the capture tool
tools/        dvcheck.py, tscheck.py, ssh helpers, scripts that run on the capture host
docs/         protocol findings and rig documentation
```

---

## A note on the FPGA bitstream

`traces/fpga-bitstream-candidate.bin` is **Pinnacle's copyright, not ours.**
It is a static 78,422-byte Altera Cyclone EP1C3 configuration blob (MD5
`3888c23c9bcc81c88964c45d981a0b68`), extracted from the vendor driver by
observing what it pushes to the device at init. The hardware is inert without
it.

It is included here as a pragmatic decision: the device was discontinued around
2005, the vendor driver is no longer distributed or supported, and without the
blob this repository is useless to anyone who owns the hardware. No claim of
ownership is made and no license is granted by us. If the rights holder objects
it will be removed, and the driver will fall back to extracting it from the
user's own vendor driver install.

The vendor driver binary (`MarvinBus64.sys`) and installer are **not** included.

## Licence

**GNU Affero General Public License v3.0** ([LICENSE](LICENSE)). If you run a
modified version of this code as a network service, you must offer its source
to that service's users.

The FPGA bitstream is **excluded** from that grant — it is not ours to license.
See the note above.
