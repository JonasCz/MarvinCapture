# The hardware, as reverse-engineered

The Pinnacle Studio 500-USB (`USB\VID_2304&PID_0213`, codename **Marvin-Lite**)
is a ~2005 USB 2.0 box with a FireWire port and analog inputs. It contains a
Cypress **CY7C68013A (FX2LP)** USB controller and an **Altera Cyclone EP1C3
FPGA**, plus an SAA7113H video decoder and an AC'97 audio codec on the analog
side. Everything below was worked out by watching the vendor driver with usbmon
and by reading its decompiled code; how the driver uses it is in the other docs.

## Models

All Marvin-family units use VID `0x2304`. The core has one model table
(`src/core/pinnacle_model.c`, row = PID, name, supported / tested flags, the two
bitstream file names, decoder I2C address); nothing else hard-codes a PID.
The vendor driver (`MarvinAVS64.sys`) branches on the PID in a few places; where
it treats two PIDs alike they share a row's behaviour here.

|  | PID | internal name | product | status |
|---|---|---|---|---|
| <a href="images/500-usb.jpg"><img src="images/500-usb.jpg" width="80" alt="Studio 500-USB"></a> | `0213` | Marvin-Lite | Studio **500-USB** | supported, tested |
| <a href="images/510-usb.jpg"><img src="images/510-usb.jpg" width="80" alt="Studio 510-USB"></a> | `0223` | Marvin-510 | Studio **510-USB** | supported, tested (Windows: bring-up, DV capture, deck control, stdout streaming) |
| <a href="images/700-usb.jpg"><img src="images/700-usb.jpg" width="80" alt="Studio 700-USB"></a> | `0212` | Marvin-CR | Studio **700-USB** | supported, **untested** (same code path as the 500) |
| <a href="images/710-usb.jpg"><img src="images/710-usb.jpg" width="80" alt="MovieBox Plus 710-USB"></a> | `0224` | Marvin-710 | **MovieBox Plus / 710-USB** | supported, **untested** (same code path as the 700) |
| <a href="images/moviebox-deluxe.jpg"><img src="images/moviebox-deluxe.jpg" width="80" alt="MovieBox Deluxe"></a> | `0206` | Marvin-classic | **MovieBox Deluxe** | supported in part, **untested**; host-side FX2 firmware download implemented as a best guess (see below) |

Untested models appear as "(untested)" in the GUI device list and
`MarvinCaptureCLI --help`. The other PIDs in the vendor driver (`0x20a`, `0x20b`, `0x211`
Pro) are not in the INF's list for this driver and are not handled.

**Bitstreams**: `MarvinAVS64.sys` embeds one OHCI, one Render and one Capture
bitstream and uses them for every PID except `0x211`. So no model needs files
beyond `fpga-ohci.bin` / `fpga-capture.bin`.

**Capability word** (vendor `FUN_00019588`; nibble 0 = decoder type, nibbles 1 and 2 = two
further chip objects, probably the analog output side and the audio part):
`0x101` for 0213 and 0223, `0x111` for 0212, 0224
and 0206. The extra nibble on the 700/710/Deluxe instantiates one more I2C chip
object (address `0x54`), apparently on the analog output side. Nothing the
capture path uses depends on it; the extra inputs and outputs of the 700-USB
are not driven by this project. The decoder is at I2C `0x4a` on all of them.

**700-USB, 710-USB** (`0212`, `0224`): every PID branch found
(`0x212`/`0x213`/`0x223`/`0x224` in one `if`) treats them like the 500-USB:
the `0c` power-up, the `80 <idx> 08` configuration-memory reads, the GUID,
`MarvinCR_000.bix`. No difference was found, so they run the 500-USB's code
unchanged.

**MovieBox Deluxe** (`0206`, "classic" firmware): differs from the CR family
in the config channel and flags it by `cr_config = 0` in the model table:

- no `0c` power-up and no `80 <idx> 08` reads (the vendor driver only sends them
  for 0212/0213/0223/0224); we skip them (`config_replay_seq`, analog `power_up`).
- the identity (the GUID that goes into our 1394 config ROM) is read as eight
  single-byte FX2 vendor requests `0xA0`, wValue `0x78 + i` (`FUN_0002cb5c`,
  else branch). Implemented as a best guess from the decompilation
  (`read_guid_a0`); if it fails the 1394 code logs a warning and publishes the
  fallback GUID.
- the vendor driver sends the `07 00` probe first; if it is not answered `07 01`
  it assumes a blank FX2 and downloads firmware (embedded image at VA `0x49720`
  of the driver, 0x1350 bytes, or `Marvin_000.bix` / `MarvinCR_000.bix`) with
  vendor request `0xA0` over EP0, CPUCS (`0xE600`) held in reset, 512-byte
  blocks at wValue = block * 512, then CPUCS = 0, ~50 ms settle after each
  CPUCS write (**code**, `FUN_0002bf8c`). The vendor driver also recognises a
  blank FX2 at 04b4:8613; we do not claim that PID (it would hijack other
  Cypress devices), the trigger is the failed `07 00` probe on 2304:0206.
  **Implemented as a best guess, untested** (`pinnacle_ensure_fx2`,
  `src/core/pinnacle_fx2.c`; image shipped as `firmware/fx2-marvin.bin`, see
  firmware/README.md): download, close, re-open the same USB port path, retry
  the probe for up to 10 s (guess: that the unit re-enumerates with the same
  PID on the same port, and that the vendor's wait differs). Missing file or a
  unit that never comes back fails bring-up with `PINNACLE_ERR_NO_FX2_FIRMWARE`.
  The 500/510 boot from EEPROM and never need this.

**510-USB** (`0223`): in every PID branch of the vendor driver that was found,
`0x223` is handled exactly like `0x213`: same capability word, same embedded
bitstreams (OHCI, Render, Capture; only `0x211`, the Pro, has its own files),
same config-memory GUID read, same decoder I2C address `0x4a`, same
`MarvinCR_000.bix` for a blank FX2 (not needed: the unit enumerates with its
EEPROM firmware, iProduct "Pinnacle High Speed USB Device"). Descriptors and
alternate settings are identical to the 500-USB. Confirmed on a real unit:
config-channel handshake, both bitstream loads, GUID read, 1394 link up
(`NodeID 0xc000ffc0`, one node), SAA7113 answering at `0x4a` (no signal).
Its Linux id shows as "DazzleTV Sat BDA Device" in `lsusb` (the PID is shared
with a different Pinnacle product name in the USB id database).

## USB descriptors

One configuration (300 mA, bus powered), one interface of class **0xFF
(vendor-specific)** with **four alternate settings**. **Every endpoint is bulk,
512-byte max packet: there is no isochronous endpoint at all.** Bulk transfers
are retried by the host controller, so losslessness is a property of the
transport, and WinUSB/libusb support is straightforward.

| Alt | Endpoints | Use |
|---|---|---|
| 0 | 0x01 OUT, 0x81 IN, 0x02 OUT | idle / config channel and bitstream upload |
| 1 | 0x01, 0x81, 0x02, **0x84 IN, 0x06 OUT, 0x88 IN** | **DV/HDV**: the operational setting for the OHCI design |
| 2 | 0x01, 0x81, 0x02, 0x06 OUT | output to the camera (not used) |
| 3 | 0x01, 0x81, **0x82 IN, 0x86 IN** | **analog capture**: video on 0x82, audio on 0x86 |

The endpoint numbers 1, 2, 4, 6, 8 are exactly the FX2's endpoint set (EP1 plus
the four large slave-FIFO endpoints), consistent with the FX2 doing plain FIFO
streaming and the FPGA doing all the framing.

### Endpoint roles

| Endpoint | Dir | Role |
|---|---|---|
| EP0 | ctrl | Descriptors, `SET_CONFIGURATION`, `SET_INTERFACE`. **No vendor-class requests, ever.** |
| **0x01 / 0x81** | OUT/IN | Config channel. Opcode-prefixed: `01`/`02` are I2C write/read (SAA7113 at 0x4a), `80` reads configuration memory, `05`/`06` report FPGA-loader status. See [analog.md](analog.md). |
| **0x02** | OUT | The **FPGA bitstream upload**, then the **1394 command channel** (OHCI register and transaction messages). |
| **0x84** | IN | Replies and status for the EP 0x02 command channel. |
| **0x88** | IN | **The DV/HDV stream.** The FPGA's OHCI isochronous-receive DMA, tunnelled over USB. |
| 0x82 / 0x86 | IN | Analog video / audio (alt 3). |

## Initialisation

```
SET_CONFIGURATION 1 → SET_INTERFACE alt 0
  → config-channel bring-up on EP 0x01/0x81
  → 78,422-byte FPGA bitstream on EP 0x02 OUT (17408 + 16384×3 + 11862)
  → SET_INTERFACE alt 1 (DV/HDV) or alt 3 (analog)
```

Two facts that shrank the work:

- **There is no host-side FX2 firmware download.** Not one vendor-class control
  request appears anywhere, even on a cold boot. The FX2 boots from its own
  EEPROM, so there is no 8051 blob to extract or redistribute.
- **The FPGA bitstream is a static blob**, not negotiated or parameterised, so
  the driver simply replays it: 78,422 bytes, Altera Cyclone EP1C3 `.rbf`.
  There are three (OHCI for DV/HDV, Capture for analog, Render for analog
  output); see [analog.md](analog.md#three-fpga-bitstreams-the-key-to-analog).
  The OHCI one has MD5 `3888c23c9bcc81c88964c45d981a0b68`. They are Pinnacle's
  copyright; see [firmware/README.md](../firmware/README.md).

## What the FPGA is

**It implements a standard OHCI-1394 host controller**, and EP 0x88 carries its
isochronous receive DMA tunnelled over USB. The stream is not raw DV: two layers
of framing sit on top of the DIF data (variable-length type-9 messages, then
OHCI buffer-fill records holding IEC 61883 CIP packets). Strip both and you get a
byte-exact DV elementary stream (a DIF sequence is 150 × 80 = 12,000 bytes;
10 sequences per NTSC frame, 12 per PAL). The framing, the command word format
and the register usage are in [protocol.md](protocol.md); the bring-up sequence
in [startup.md](startup.md); AV/C deck control in [deck-control.md](deck-control.md);
HDV in [hdv.md](hdv.md).

DIF header blocks are `byte0 == 0x1f`, `byte1 == (Dseq<<4)|0x07`, `byte2 == 0x00`.
Matching only `1f 07 00` finds sequence 0 alone and undercounts ~10×.

## Things worth knowing

- **The device is completely silent when idle**: no keepalive. An empty capture
  means nothing is streaming, not that the tooling failed.
- **The command channel blocks while EP 0x88 has an unread backlog**, so anything
  written to EP 0x02 while the receive context is running must keep reading
  EP 0x88 too. It is the single most load-bearing fact about the command channel.
- **An FPGA that did not come up needs a physical USB power cycle**; a warm
  reboot does not cut VBUS ([usage.md](usage.md)).
- **The 1394 connection is managed properly**: the camera node is found by reading
  each node's oMPR, the oPCR is read and compare-swapped, receive listens on
  the channel the plug reports, and it is released on stop. A bus reset
  *during* a capture (cable moved, camera switched off and on, another node
  added) ends the capture with an error dialog ("the FireWire connection to
  the camera was interrupted"), because frames are lost while the plug is
  connected again; carrying on in the same file would hide a loose cable.
  The stream is then restarted as when idle (link init, find the camera,
  connect its plug: its node number may have changed). See
  [usage.md](usage.md#when-a-capture-stops-by-itself). **Untested**: no camera
  was attached when this was written.
