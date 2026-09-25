# Handoff — Pinnacle 500-USB open driver

Status as of **2026-09-22**. Goal: a modern user-space driver + DV capture app for the
Pinnacle Studio 500-USB (`USB\VID_2304&PID_0213`, codename **Marvin-Lite**), DV over
the FireWire port only, with deck control. Analog path is out of scope.

> **Stage 1 (capture CLI) works and is verified.** A 5-minute continuous capture
> produced 8,967 frames with zero dropped DIF sequences, zero CIP/DBC
> discontinuities and zero ffmpeg decode errors, and stopped cleanly. See
> [docs/capture-reliability.md](docs/capture-reliability.md) for how to run it and
> how that was proven. Since 2026-09-25 the 1394 start-up is generated
> step by step instead of replayed. It discovers the camera and its channel
> and makes a real oPCR connection ([docs/startup.md](docs/startup.md)).
> Deck control works ([docs/deck-control.md](docs/deck-control.md)).

**Approach decided:** build the driver on **Linux**, using a Windows VM running the
vendor driver as the reference implementation to observe.

Read next, in order:

0. **[docs/capture-reliability.md](docs/capture-reliability.md)** — how to run a capture, how to tell a device fault from a quiet camera, and how to prove no data was dropped. Start here if you just want to use the thing.
1. **[traces/README.md](traces/README.md)** — every capture, what it contains, what it proves.
2. **[docs/vm-control.md](docs/vm-control.md)** — how to drive the VM and move data around.
3. **[docs/linux-capture-setup.md](docs/linux-capture-setup.md)** — the capture rig and detailed findings.
4. **[FEASIBILITY.md](FEASIBILITY.md)** — the overall plan and phases.
5. [docs/usb-descriptors.md](docs/usb-descriptors.md), [docs/capture-tooling.md](docs/capture-tooling.md) — descriptor decode; why the Windows-native attempt failed.

---

## 1. The rig

| | |
|---|---|
| Capture host | Ubuntu 25.04, kernel 6.14, `$PIN_HOST` (see tools/README.md) |
| Device | `2304:0213` on **Bus 001 Device 002**, a genuine USB **2.0** bus |
| Guest | VirtualBox 7.0.20 VM **`Win7`**, auto-logs in as user **`user`**, runs the vendor driver + WinDV |
| Capture method | **`usbmon`** + tshark 4.4.5 — taps the USB core, so it sees every URB the Windows driver issues, with full payload |
| Traces | `~/pinnacle-traces` on the host; copies in [traces/](traces/) here |

VirtualBox passthrough submits the guest's URBs through the host's USB core, which is
exactly what usbmon taps. Zero packets were dropped in any capture.

> The earlier attempt to capture on bare-metal Windows with USBPcap **failed
> completely** — it recorded nothing from this device, not even enumeration. Don't
> retry that path; see [docs/capture-tooling.md](docs/capture-tooling.md).

---

## 2. What we know about the hardware

Single vendor-class (0xFF) interface, **four alternate settings, every endpoint bulk,
512-byte max packet — no isochronous endpoints at all.** Bulk means the host controller
retries, so losslessness is a property of the transport, and WinUSB/libusb support is
straightforward. Full decode in [docs/usb-descriptors.md](docs/usb-descriptors.md).

### Endpoint roles (observed, not guessed)

| Endpoint | Dir | Role |
|---|---|---|
| EP0 | ctrl | Descriptors, `SET_CONFIGURATION`, `SET_INTERFACE`. **No vendor-class requests ever.** |
| **0x01 / 0x81** | OUT/IN | Low-level config channel. Opcode-prefixed; `01`/`02` are **I2C write/read**, used to initialise the SAA7113H analog decoder at I2C address 0x4A. Irrelevant to DV, but reveals the framing. |
| **0x02** | OUT | Two jobs: **FPGA bitstream upload** at init, then the **1394/stream command channel**. |
| **0x84** | IN | Responses/status for the EP 0x02 command channel. |
| **0x88** | IN | **The DV stream.** Only endpoint carrying video. |

### Initialisation sequence (identical on cold boot and on replug)

```
GET_DESCRIPTOR ×n  →  SET_CONFIGURATION 1  →  SET_INTERFACE alt 0
  →  78,422-byte FPGA bitstream pushed on EP 0x02 OUT (5 transfers:
     17408 + 16384 + 16384 + 16384 + 11862)
  →  SET_INTERFACE alt 1        ← operational setting; EP 0x88 lives here
  →  SAA7113 I2C init on EP 0x01/0x81   (analog side, skippable for us)
```

Two findings that materially shrink the work:

- **There is no host-side FX2 firmware download.** Not one vendor-class control
  request (`0x40`/`0xC0`) appears anywhere, on a clean cold boot. The classic EZ-USB
  `0xA0` RAM load is absent and the device enumerates directly as `2304:0213`. **The
  CY7C68013A boots from its own EEPROM** — no 8051 blob to extract or redistribute.
- **The FPGA bitstream is a static blob.** Byte-identical across two completely
  different init paths (cold VM boot vs. detach/reattach): 78,422 bytes, MD5
  `3888c23c9bcc81c88964c45d981a0b68`. It is not negotiated or parameterised, so the
  driver can simply replay it. Saved as `traces/fpga-bitstream-candidate.bin` and
  `traces/fpga-bitstream-coldboot.bin` (identical). Pattern (0xFF padding, repeating
  `6a d7 ff 40 00 a8 92 09 00` sync, 0xFF tail) and size match an Altera Cyclone
  EP1C3 `.rbf`.

> **Licensing:** the bitstream is **Pinnacle's copyright, not ours.** It is included in
> this repo at `traces/fpga-bitstream-candidate.bin` as a pragmatic decision — the
> device was discontinued around 2005 and the hardware is useless without it. See the
> note in [README.md](README.md). It is not ours to license; if Pinnacle/Corel object,
> it comes out and the driver falls back to extracting it from the user's own install.

### The DV stream on EP 0x88

Genuine DV DIF essence. Both video systems captured:

| | PAL (idle deck, colour bars) | NTSC (camera playing) |
|---|---|---|
| DIF sequences/frame | **12** (Dseq 0–11) | **10** (Dseq 0–9) |
| Rate observed | 2.55 MB/s | 3.13 MB/s |
| Zero-byte fraction | 0.599 | 0.054 |

DIF header blocks are `byte0 == 0x1f`, `byte1 == (Dseq<<4)|0x07`, `byte2 == 0x00`.
**Matching only `1f 07 00` finds sequence 0 alone and undercounts ~10×** — a trap worth
avoiding. Block IDs at an 80-byte stride read `1f, 3f, 3f, 5f, 5f, 5f, …` (one header,
two subcode, three VAUX), which is textbook DV.

The high zero fraction on the PAL trace is **not** emptiness — flat colour bars produce
near-zero DCT coefficients and DV pads every frame to a fixed size. With real moving
video it drops to 0.054.

**Per-sequence framing — SOLVED**, see
[docs/command-channel-findings.md](docs/command-channel-findings.md). A DIF sequence
is 150 × 80 = 12,000 bytes, but observed header-to-header spacing is ~12,850 in *both*
PAL and NTSC. That ~850 bytes is not one wrapper: EP 0x88 carries the FPGA's OHCI-1394
isochronous-receive DMA tunnelled over USB, with two layers of framing — variable-length
type-9 messages, and OHCI buffer-fill records holding IEC 61883 CIP packets. Stripping
both yields a byte-exact DIF stream (strides exactly 12,000, zero block-ID errors) and
captures that decode with zero ffmpeg errors. Implemented in `dv_reassembler.c`.

### Stream start / stop

Captured in `traces/20260922-144400-stream-start-transition-TRIMMED.pcapng`.
Streaming begins at t=8.6048; the preceding ~0.23 s carries 46 command packets on
EP 0x02 OUT with matching replies on EP 0x84 IN. They are quadlet-aligned and look
strongly 1394-shaped, e.g.:

```
OUT 76B  e0 11 38 80 10 00 00 02 ... c1 ff ... 
OUT 52B  c0 11 20 80 0c 00 0c 12 ...
OUT  8B  00 00 30 40 00 00 b4 00
OUT  8B  0c 04 01 20 81 11 80 00
OUT  8B  00 04 01 20 00 90 00 00      <- last command before data flows
IN  ...  0c 12 04 90 bb 28 12 84 / f8 57 10 90 20 05 c0 ff ...
```

**Stop** is tiny and clean: closing WinDV produces **6 packets / 76 bytes on EP 0x02
OUT** and nothing else (`traces/20260922-142455-stream-stop.pcapng`).

> **The command channel blocks while EP 0x88 has an unread backlog.** Anything that
> writes to EP 0x02 while the isochronous receive context is in `run` must keep
> reading EP 0x88 at the same time, or the write times out (`LIBUSB_ERROR_TIMEOUT`).
> This is the single most load-bearing fact about the command channel; it cost two
> separate wrong diagnoses before it was isolated. See
> [docs/command-channel-findings.md](docs/command-channel-findings.md).

---

## 3. Known limitation of the current traces

**The captures are faithful — `dumpcap` reported `dropped 0` everywhere.** But the
*vendor stack* drops DIF sequences under the VM: WinDV reports dropped frames and
sometimes freezes outright (itself part of the motivation for this project).

Evidence: sequence spacing is often far below the expected ~12,850 (1,435 / 3,617 /
4,748 / 6,656 …) and frame spacing ranges 65,140–199,400 where ~128,500 is expected.

**So:** these traces are fully adequate for protocol work — framing, endpoints, command
channel and init are all intact. They **cannot** be used to prove lossless end-to-end
reassembly. For that, use `traces/capture.26-09-22_12-11.00.avi`, WinDV's own recording
of the same NTSC source (verified `dvsd`, 720×480, 29.971 fps, 198 frames).

---

## 4. What is still missing

| Gap | Notes |
|---|---|
| **Deck control (Play/Pause/FF/REW)** | **Not captured.** Pressing buttons on the camera never touches USB, and no software on the guest sends host→device AV/C — WinDV has no transport controls. Decision: **reverse the AV/C encapsulation out of `MarvinBus64.sys`** rather than hunt for software. We now know the command channel is EP 0x02 OUT / 0x84 IN, which narrows the search a lot. |
| ~~The ~850-byte per-sequence wrapper~~ | **Solved** — type-9 messages + OHCI buffer-fill records + IEC 61883 CIP. See section 3 and `docs/command-channel-findings.md`. |
| ~~Command channel semantics~~ | **Solved** — the FPGA is an OHCI-1394 controller; the command word layout and message types are documented in `docs/command-channel-findings.md`. |
| ~~A drop-free DV trace~~ | **Have one** — `captures/20260922-clean-3s.dv`, decodes with zero errors. |
| ~~Clean stream stop~~ | **Solved** — the command channel blocks while EP 0x88 has an unread backlog, so the stop sequence now keeps EP 0x88 drained while it runs. All 4 packets ACK and the device streams normally afterwards. |
| ~~Connection management~~ | **Done** (2026-09-25). The camera node is found by reading each node's oMPR. The oPCR is read, then compare-swapped with its current value, and released on stop. IR0 listens on the channel the plug reports. See [docs/startup.md](docs/startup.md). Still open: re-connecting after a bus reset *during* capture. |
| ~~Deck control~~ | **Working** (2026-09-25) with `build/pindeck`: Play/Pause/Stop/FF/REW and timecode, verified on a Canon HDV camera. Every command is answered on a single send. The transport is in the core library (`p1394_avc`). See [docs/deck-control.md](docs/deck-control.md). Next: wire it into `pincli`. |

---

## 5. Orientation notes that will save time

- In every usbmon trace the Pinnacle is **`usb.device_address == 2`**, and the **first
  12 packets (all at t≈0.000) are a synthetic descriptor snapshot**, not real traffic.
  Filter with `frame.time_relative > 0.001`.
- **The device is completely silent when idle** — no keepalive at all. An empty capture
  means nothing is streaming; it is not a tooling failure.
- **WinDV takes >20 s after launch** to build its DirectShow graph and start streaming.
  A capture window shorter than that will look empty and mislead you.
- **WinDV must be restarted after any device re-enumeration** — its graph dies and the
  preview freezes on the last frame while zero bytes cross USB.
