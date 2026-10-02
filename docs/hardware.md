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

| PID | internal name | product | status |
|---|---|---|---|
| `0213` | Marvin-Lite | Studio **500-USB** | supported, tested |
| `0223` | Marvin-510 | Studio **510-USB** | supported; DV/analog bring-up verified on hardware (no camera attached), capture untested |
| `0206` | Marvin-classic | MovieBox Deluxe | not supported yet |
| `0212` | Marvin-CR | Studio 700-USB | not supported yet |
| `0224` | Marvin-710 | Studio 710-USB / MovieBox Plus | not supported yet |

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
  the channel the plug reports, and it is released on stop. Re-connecting after
  a bus reset *during* a capture is not handled.
