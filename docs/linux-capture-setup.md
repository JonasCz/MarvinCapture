# Linux capture rig — setup, how-to, and first findings

Host: the Ubuntu capture host (`$PIN_HOST`) — Ubuntu 25.04, kernel 6.14.0-36-generic,
VirtualBox 7.0.20, guest VM **`Win7`** running the Pinnacle vendor driver + WinDV.
Device: `2304:0213` on **Bus 001 Device 002**, sysfs `/sys/devices/…/usb1/1-8`,
passed through to the VM as `/dev/vboxusb/001/002`.

> **USBPcap is Windows-only.** On Linux the equivalent is **`usbmon`**, a kernel
> facility that taps the USB core. VirtualBox passthrough submits the guest's URBs
> through that same core, so usbmon sees *everything* the Windows driver does —
> including full payload. This is the approach that failed on the bare-metal Windows
> box (see `capture-tooling.md`) and works perfectly here.

Bus 001 is a **USB 2.0** bus (`1d6b:0002`), so none of the xHCI problems from the
Windows attempt apply.

## What was installed / changed on the Ubuntu host

| Change | Why |
|---|---|
| `apt-get install tshark` (4.4.5), preseeded `wireshark-common/install-setuid=true` | capture + analysis |
| `modprobe usbmon` + `/etc/modules-load.d/usbmon.conf` | the capture tap; now loads at boot |
| `/etc/udev/rules.d/99-usbmon.rules` → `SUBSYSTEM=="usbmon", GROUP="wireshark", MODE="0640"` | `/dev/usbmon*` was root-only |
| `usermod -aG wireshark jonas` | applies on next login |
| `/etc/apparmor.d/local/tshark` → `file r /**.pcap{,ng}{,.gz},` | **see gotcha 2** |
| `/usr/local/bin/pincap` | capture helper (below) |
| `/home/jonas/pinnacle-traces/` | where traces are kept |

### Gotcha 1 — dumpcap cannot write into `$HOME`

Ubuntu confines dumpcap with the AppArmor profile `tshark//dumpcap`. It may write
`*.pcapng` only where the abstractions allow — `/tmp` works, `/home/jonas/...` gives
`Permission denied` **even as root**. The nested subprofile has no
`include if exists <local/...>` hook, so it cannot be extended cleanly.

**Workaround (what `pincap` does): capture to `/tmp`, then `mv` the finished file into
`~/pinnacle-traces`.** `mv` is not confined, so this is reliable.

### Gotcha 2 — tshark cannot *read* capture files outside `/tmp`

The stock `tshark` profile grants only `abstractions/user-tmp`; it has **no rule
allowing it to read capture files at all**, so `tshark -r ~/pinnacle-traces/x.pcapng`
fails with "You don't have permission to read the file" — again even as root, which
makes it look like a filesystem problem when it is not. `capinfos` is unconfined and
works, which is a good way to spot this.

Fixed via the profile's supported override hook — write
`/etc/apparmor.d/local/tshark` containing `file r /**.pcap{,ng}{,.gz},` and reload
with `sudo apparmor_parser -r /etc/apparmor.d/tshark`.

## The `pincap` helper

```bash
sudo pincap timed steady 10
```

```bash
sudo pincap start play
```

```bash
sudo pincap stop
```

`pincap status` reports whether a capture is running. It captures `usbmon1` with
`-s 0` (no payload truncation) and a 128 MB kernel buffer, writing
`~/pinnacle-traces/<timestamp>-<label>.pcapng`. Override the bus with
`PINCAP_IFACE=usbmon0` (all buses).

**Always check the "dropped" count** dumpcap prints. DV runs multi-MB/s; if drops
appear, raise the `-B` buffer.

## Detaching / reattaching the device without touching the VM window

Query the UUID from `VBoxManage list usbhost` (the entry with `0x2304`), then
`VBoxManage controlvm Win7 usbdetach <uuid>` returns it to the host and
`VBoxManage controlvm Win7 usbattach <uuid>` makes the guest re-enumerate and
re-initialise the driver.

Run these **as `jonas`**, not root (they talk to that user's VBoxSVC). Current UUID:
`f6ca57d3-602e-4e3b-b3e9-83bebc378cd0` — re-query it, it changes on physical replug.

## Capture protocol — does WinDV need closing?

**Yes, for some traces — and it matters.** The device is completely silent when idle
(no keepalive whatsoever), so each trace isolates cleanly if you control the state.

| # | Trace | WinDV | Procedure |
|---|---|---|---|
| 1 | **enumeration + init** | **CLOSED** | `pincap start` → `usbdetach` → wait 5 s → `usbattach` → wait ~15 s → `pincap stop`. Captures descriptor reads, FPGA bitstream upload, alt-setting selection. |
| 2 | **stream start** | **closed → open** | `pincap start` → launch WinDV and let preview begin → `pincap stop` after ~10 s. Isolates the handshake that starts streaming. |
| 3 | **steady-state DV** | previewing | `pincap timed steady 10`. ~40–110 MB. Gives DV payload for frame reassembly. |
| 4 | **deck control** | previewing | One trace per button. `pincap start play` → press **only** Play → `pincap stop`. Repeat for Pause/Stop/FF/REW. |

Rules of thumb: start the capture **before** the action; do **one** action per trace;
close WinDV before trace 1 so driver init is not mixed with streaming.

## Findings from the first trace (`20260922-135315-enumeration-init.pcapng`)

1,299 packets / 14.5 s, zero drops.

| Endpoint | Dir | Packets | Bytes | Role |
|---|---|---|---|---|
| EP0 | ctrl | 50 | 1,376 | descriptors, SET_CONFIGURATION, SET_INTERFACE |
| **0x02** | OUT | 396 | **86,914** | **FPGA bitstream upload** + some commands |
| 0x01 / 0x81 | OUT/IN | 162 / 162 | 385 / 267 | **command / response channel** |
| 0x84 | IN | 480 | 4,328 | status polling |
| 0x88 | IN | 1 | 1,536 | (stream endpoint, idle) |

### 1. There is **no host-side FX2 firmware download**

Not a single vendor-class control request (`0x40`/`0xC0`) appears — only standard
`GET_DESCRIPTOR`, `SET_CONFIGURATION`, `SET_INTERFACE`. The classic EZ-USB `0xA0`
RAM-download request is absent, and the device enumerates directly as `2304:0213`.
**The FX2 boots from its own EEPROM.** This removes most of Phase 1 from the plan.

### 2. The FPGA bitstream goes over bulk EP 0x02 OUT

At t≈11.32 s, five back-to-back transfers of 17408 + 16384 + 16384 + 16384 + 11862 =
**78,422 bytes**, saved as `traces/fpga-bitstream-candidate.bin`. It begins with 0xFF
padding then a repeating `6a d7 ff 40 00 a8 92 09 00` sync pattern and ends in 0xFF —
consistent with an Altera Cyclone raw bitstream (`.rbf`) for the EP1C3, whose expected
size is ~80 KB. Our own driver will need to replay this blob.
(`traces/ep02-out-payload.bin` is everything sent on EP 0x02, commands included.)

### 3. Alt setting 1 is the operational configuration

`SET_INTERFACE` sequence: **alt 0** (t=3.06) → alt 0 (t=9.38) → **alt 1 (t=12.44,
immediately after the bitstream upload)**. This independently confirms the Windows ETW
finding that streaming happens in alt 1 on EP 0x88.

### 4. The EP 0x01/0x81 command channel is decipherable, and carries I2C

Requests on EP 0x01 OUT with matching replies on EP 0x81 IN:

| OUT (EP 0x01) | IN (EP 0x81) |
|---|---|
| `07 00` | `07 01` |
| `0c 01` | `0c 01` |
| `03 4a` | `03 02` |
| `04 4a` | `04 03` |
| `02 4a 01 01 00` | `02 01 11` |
| `01 4a 02 01 08` | `01 01 08` |
| `01 4a 02 02 c0` | `01 01 08` |
| `01 4a 02 03 33` | `01 01 08` |

The `01 4a 02 <reg> <val>` form is an **I2C write to device address 0x4A** — the
**SAA7113H** video decoder. The register/value pairs (`02`←`c0`, `03`←`33`, …) match
the SAA711x init sequence seen in the `pinnaclembusb` prior art. `02 4a 01 01 00` →
`02 01 11` is the corresponding I2C **read** (register 0x01 returns 0x11).

So EP 0x01/0x81 is a general command channel with a leading opcode byte, where `0x01`
and `0x02` are I2C write/read. This particular traffic is the **analog** decoder being
initialised, which we do not care about — but it hands us the channel's framing for
free. The DV/1394 side will use different opcodes on the same channel, which is
exactly what traces 2–4 should reveal.

## Findings from the streaming trace (`20260922-140208-windv-open-bars.pcapng`)

12 s, 14,139 packets, **32,375,376 bytes on EP 0x88 IN, zero drops**, steady
2.5–2.7 MB/s. Source was a DV deck sitting **idle, emitting colour bars** (not a
moving image). Transfer sizes cluster around 8108/8124/7804/7820 and 3684–4028 bytes.

### The payload is genuine, well-formed PAL DV

Searching for DV DIF header blocks (`byte0 == 0x1f`, `byte1 == (Dseq<<4)|0x07`,
`byte2 == 0x00` — note byte 1 encodes the sequence number, so matching only `1f 07 00`
finds just sequence 0 and badly undercounts) gives **455 headers**, with all twelve
sequence numbers present and evenly distributed:

```
Dseq histogram: 0:36  1:36  2:39  3:44  4:32  5:33  6:36  7:37  8:41  9:36  10:45  11:40
```

**Twelve DIF sequences per frame means PAL** (NTSC has ten). The block IDs at an
80-byte stride from a header read `1f, 3f, 3f, 5f, 5f, 5f, …` — textbook DV: one
header block, two subcode, three VAUX, then audio/video. This is real DV essence.

The ~60% zero-byte fraction is **not** emptiness: flat colour bars produce almost-zero
DCT coefficients, and DV pads every frame to a fixed size, so zero-heavy content is
exactly what idle bars should look like.

### …but the stream is incomplete

- A DIF sequence is 150 blocks × 80 = **12,000 bytes**. Observed header-to-header
  spacing is mostly **12,828–12,868** — roughly 850 bytes of per-sequence overhead,
  presumably CIP/packet headers from the tunneled 1394 isochronous channel. Confirming
  that framing is a Phase-2 task.
- But the spacing intermittently jumps to 19,000 / 21,704 / 22,616 / 47,172, and the
  gap between successive `Dseq == 0` headers (a full frame, expected to be a constant
  ~154,000 bytes) swings from 74,704 to 233,632. **Sequences are being dropped.**
- This matches the throughput: 2.55 MB/s against the ~3.6 MB/s a complete DV25 stream
  needs — about 71%, i.e. roughly a third of the stream missing.
- **The loss is not ours.** dumpcap reported `dropped 0`, so usbmon recorded
  faithfully; the device/driver genuinely did not send those sequences.

## Findings from the NTSC trace (`20260922-141844-ntsc-camera-playing.pcapng`)

Same rig, but with a **camera playing a moving image** instead of an idle deck.
15 s, 20,086 packets, **46,889,868 bytes on EP 0x88 IN at 3.13 MB/s**, zero drops.
Motion verified independently: three VM screenshots taken across the capture window
had three different MD5s.

- **NTSC confirmed from the wire**: `Dseq` histogram runs 0–9 with ~100 headers each
  (10 sequences per frame). The PAL trace ran 0–11. So the driver must handle both;
  the sequence count is the cheapest system detector we have.
- **Zero-byte fraction fell from 0.599 to 0.054.** Real moving video is high-entropy.
  This retroactively confirms that the zero-heavy colour-bars payload was flat DCT
  content padded to DV's fixed frame size, not an empty stream.
- The ~850-byte-per-DIF-sequence overhead appears in **both** PAL and NTSC traces, so
  the extra framing is **per DIF sequence**, not per frame — a useful Phase-2 clue.

### Both streams have gaps, and it is not our capture

`dumpcap` reported `dropped 0` throughout, so usbmon recorded faithfully. The loss is
upstream in the device → vendor-driver path: WinDV itself reports dropped frames
(VM/passthrough overhead), and it is also prone to freezing outright — which is what
produced the earlier frozen-preview confusion, and is part of the motivation for this
project.

Sequence spacing is often far below the expected ~12,850 (1,435 / 3,617 / 4,748 /
6,656 …) and frame spacing ranges 65,140–199,400 where ~128,500 is expected.

**Implication:** the traces are fully adequate for protocol work — framing, endpoints,
command channel and init sequence are all intact — but they cannot prove lossless
end-to-end DV reassembly. For that, capture with WinDV actually **recording to a file**
rather than previewing, or measure the future Linux driver directly.

### Ground truth available

`traces/capture.26-09-22_12-11.00.avi` is WinDV's own recording of the same NTSC
source — verified `dvsd`, 720×480, 29.971 fps, 198 frames. The guest's `H:` drive is
the VirtualBox shared folder `/home/jonas/Desktop/vm-shared`, which maps straight onto
the Linux host, so guest files need no special transfer step.

## Corrected endpoint map, and the stream start/stop sequences

Later traces revised the endpoint roles — the earlier reading of EP 0x01/0x81 as *the*
command channel was only half right:

| Endpoint | Dir | Actual role |
|---|---|---|
| EP0 | ctrl | descriptors, `SET_CONFIGURATION`, `SET_INTERFACE`. **Never any vendor-class request.** |
| 0x01 / 0x81 | OUT/IN | low-level config; opcodes `01`/`02` are I2C write/read (SAA7113 analog init) |
| **0x02** | OUT | FPGA bitstream at init, **and the 1394/stream command channel** |
| **0x84** | IN | replies/status for the EP 0x02 command channel |
| 0x88 | IN | the DV stream, nothing else |

`20260922-144400-stream-start-transition-TRIMMED.pcapng` holds the start handshake:
46 EP 0x02 OUT commands with 70 EP 0x84 IN replies in the ~0.23 s before the first DV
byte at t=8.6048. They are quadlet-aligned and 1394-shaped, e.g.

```
OUT 76B  e0 11 38 80 10 00 00 02 … c1 ff …
OUT 52B  c0 11 20 80 0c 00 0c 12 …
OUT  8B  00 00 30 40 00 00 b4 00
OUT  8B  0c 04 01 20 81 11 80 00
OUT  8B  00 04 01 20 00 90 00 00      <- last command before data flows
IN  40B  f0 34 1c 90 10 51 c0 ff ff ff c1 ff 00 0d 00 f0 …
```

Stop is far simpler: closing WinDV emits **6 packets / 76 bytes on EP 0x02 OUT** and
nothing else (`20260922-142455-stream-stop.pcapng`).

Driving WinDV for these traces is done with `vmctl` — see
[vm-control.md](vm-control.md). Consolidated findings: [../HANDOFF.md](../HANDOFF.md).

## Analysis recipes

Endpoint/byte breakdown:

```bash
tshark -r trace.pcapng -T fields -e usb.device_address -e usb.endpoint_address -e usb.transfer_type -e usb.data_len | awk -F'\t' 'NF>=3{k=sprintf("addr=%-3s ep=%-5s tt=%-4s",$1,$2,$3);n[k]++;b[k]+=$4} END{for(x in n)printf "  %s pkts=%-7d bytes=%d\n",x,n[x],b[x]}' | sort
```

Command-channel conversation:

```bash
tshark -r trace.pcapng -Y 'usb.endpoint_address==0x01 && usb.data_len>0' -T fields -e frame.time_relative -e usb.capdata
```

Extract a bulk endpoint's payload to a binary file:

```bash
tshark -r trace.pcapng -Y 'usb.endpoint_address==0x88 && usb.data_len>0' -T fields -e usb.capdata | tr -d ':\n' | xxd -r -p > ep88.bin
```

Note `usb.device_address` is **2** in these traces; it changes on re-enumeration, so
confirm with `lsusb` or by filtering on the 164-byte configuration descriptor.
