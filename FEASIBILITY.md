# Pinnacle 500-USB Open Driver — Feasibility Plan

Goal: a modern, user-mode driver (WinUSB/libusb) plus a capture application for the
Pinnacle Studio 500-USB, supporting **DV capture over the FireWire port only**, with
AV/C deck control (Play / Pause / FF / REW), lossless DV to disk. No analog path, no
third-party software compatibility.

Device: `USB\VID_2304&PID_0213` — internal codename **"Marvin-Lite"** (from the vendor
INF; siblings are Marvin-classic 0206, Marvin-CR 0212, 510-USB 0223, 710-USB 0224 —
protocol work here likely transfers to those devices almost unchanged).

## 1. What we learned from a first look (2026-08-22)

Light inspection of `Pinnacle_Video_Driver_64bit.msi` (no deep disassembly yet):

- **Driver stack** (DV-relevant files only):
  - `MarvinBus64.sys` — root-enumerated ("virtual") bus driver, `root\MarvinBus`
  - `MarvinAVS64.inf` / `MarvinAVS64.sys` — AVStream capture driver bound to `VID_2304&PID_0213`
  - `MarvinUsb.ax`, `marvinavrenderer.ax` — DirectShow user-mode pieces
  - The `em*` files (`emDevice64.sys`, `emAudio64.sys`, …) are the **Empia analog/audio
    side — out of scope**. `bender64.sys` is for Pinnacle **PCI** cards — irrelevant.
- **Key architectural finding:** `MarvinBus64.sys` contains the complete Windows 1394
  bus-driver request vocabulary as strings (`REQUEST_ISOCH_ALLOCATE_CHANNEL`,
  `REQUEST_ISOCH_LISTEN/TALK`, `REQUEST_ISOCH_ATTACH_BUFFERS`, "Ack for incoming Async
  packet sent on 1394", "received incomplete header from 1394 bus on IR channel", …).
  This means the vendor design is: **a virtual IEEE-1394 bus tunneled over USB**. The
  MarvinBus driver emulates Microsoft's 1394 bus interface so that the *stock Windows
  DV stack* (`avc.sys`, `msdv.sys`) binds on top and does all the DV/deck work.
- Consequence for us: the USB wire protocol very likely maps ~1:1 onto raw 1394
  primitives — async quadlet/block read/write (for config-ROM and FCP/AV-C), isoch
  channel listen (for DV reception), bus-reset/self-ID notification. That is exactly
  the small primitive set we need, and it means **we do not have to understand the
  Agere LFW3226 or the FPGA internals** — only the tunnel protocol the vendor driver
  speaks over USB. The "incomplete header … on IR channel" string suggests isoch
  packets arrive with their CIP headers intact (host-side stripping — good: simple,
  well-documented format).

### Confirmed on real hardware (2026-08-22, USBPcap trace)

- The device is a **single vendor-class (0xFF) interface with four alternate
  settings**, and **every endpoint is bulk, 512-byte max packet — there are no
  isochronous endpoints at all**. Bulk means the host controller retries on error,
  so the lossless property is guaranteed by the transport, and WinUSB/libusb support
  is straightforward. Full decode: [docs/usb-descriptors.md](docs/usb-descriptors.md).
- Endpoint numbers (1, 2, 4, 6, 8) match the CY7C68013A FX2LP endpoint set exactly.
- **Capture uses alternate setting 1, streaming on bulk endpoint `0x88` IN** in
  20,480-byte transfers at ~536 transfers/s (measured via ETW while the camera was
  playing — this supersedes the earlier guess of alt 3). Raw buffer rate is
  10.47 MB/s; since DV is only ~3.6 MB/s, the device either partially fills each
  buffer or the stream carries more than DV essence. Measure exactly once payload
  capture works.
- **Capture tooling is currently the bottleneck, not the protocol.** USBPcap 1.5.4
  records literally nothing from this device on this machine — see
  [docs/capture-tooling.md](docs/capture-tooling.md) for the proof and the way out.
  Phase 0 cannot complete until a payload-capable capture method is working.
- The device enumerates as `2304:0213` with full descriptors, **not** as a bare
  Cypress `04B4:8613`. If a plug-in trace on a clean machine confirms no host-side
  `0xA0` firmware push, the FX2 boots from its own EEPROM and **most of Phase 1
  disappears**.
- **The device emits zero USB traffic when idle** — not even a keepalive poll. An
  empty capture means nothing is streaming; it is not a tooling failure.

### Confirmed from the Linux/usbmon rig (2026-09-22)

Full-payload capture now works on an Ubuntu host with the vendor driver running in a
VirtualBox guest — see [docs/linux-capture-setup.md](docs/linux-capture-setup.md).
First enumeration trace settles several open questions:

- **Phase 1 is largely eliminated: there is no host-side FX2 firmware download.**
  The enumeration contains *no* vendor-class control requests at all — no EZ-USB
  `0xA0` RAM load — and the device enumerates directly as `2304:0213`. The FX2 boots
  from its own EEPROM. We do not need to extract or redistribute 8051 firmware.
- **The FPGA bitstream is uploaded over bulk EP 0x02 OUT**: 78,422 bytes in five
  back-to-back transfers, captured as `traces/fpga-bitstream-candidate.bin`. Byte
  pattern (0xFF padding, repeating `6a d7 ff 40 00 a8 92 09 00` sync, 0xFF tail) and
  size are consistent with an Altera Cyclone `.rbf` for the EP1C3. Our driver replays
  this blob; it is the one vendor artefact we still depend on, so keep the
  extract-from-user's-own-install policy.
- **Alt setting 1 confirmed** as the operational config, selected immediately after
  the bitstream upload — matching the Windows ETW result independently.
- **EP 0x01 OUT / 0x81 IN is a general command/response channel** with a leading
  opcode byte; opcodes `0x01`/`0x02` are I2C write/read (observed initialising the
  SAA7113H at I2C address 0x4A). The analog init is irrelevant to us, but it gives us
  the channel framing for free — the DV/AV-C traffic should ride the same channel
  under different opcodes.
- **The DV payload on EP 0x88 is confirmed genuine DIF essence**, captured in both
  **PAL** (12 DIF sequences/frame, idle deck, 2.55 MB/s) and **NTSC** (10
  sequences/frame, camera playing, 3.13 MB/s) — so Phase 4 must handle both, and the
  sequence count is the cheapest system detector. Each DIF sequence carries ~850 bytes
  of extra framing beyond its 12,000 bytes in *both* systems, so the wrapper is
  per-sequence; identifying it (presumably CIP) is the main Phase-2 task remaining.
- **Caveat on the current traces:** the vendor stack drops DIF sequences under the VM
  (WinDV reports dropped frames and sometimes freezes outright). Our capture is
  faithful — `dumpcap` reported zero drops — so the traces are fine for protocol work
  but cannot demonstrate lossless reassembly. Ground truth for that:
  `traces/capture.26-09-22_12-11.00.avi`, WinDV's own recording of the same source.
- **Endpoint roles are now observed rather than guessed**, and the earlier guess was
  partly wrong. EP 0x01/0x81 is a *low-level config* channel (I2C to the SAA7113);
  the **1394/stream command channel is EP 0x02 OUT with replies on EP 0x84 IN** — the
  same EP 0x02 that carries the bitstream at init. EP 0x88 IN carries DV only.
- **Stream start and stop are captured with full payload.** Start is 46 command
  packets on EP 0x02 OUT (quadlet-aligned, visibly 1394-shaped) in the ~0.23 s before
  data flows; stop is 6 packets / 76 bytes. This is the concrete input Phase 3 needs.
- **Phase 2's remaining unknown is now precisely scoped:** each DIF sequence carries
  ~850 bytes of wrapper beyond its 12,000 bytes, in *both* PAL and NTSC, so the
  framing is per-sequence — presumably CIP/IEC 61883. Decoding it is the top task.

**Full handoff summary: [HANDOFF.md](HANDOFF.md). VM control and data transfer:
[docs/vm-control.md](docs/vm-control.md).**

### Hardware roles (working hypothesis)

| Chip | Role | Do we need its docs? |
|---|---|---|
| CY7C68013A (FX2LP) | USB interface; soft-loaded 8051 firmware; slave-FIFO bulk streaming | Public (EZ-USB TRM). Firmware load protocol is standard vendor request 0xA0 |
| Altera Cyclone EP1C3 | Glue: 1394 link ⇄ FX2 FIFO framing, likely CIP/packet handling | No — treat as black box; replay vendor bitstream |
| Agere LFW3226 | 1394a PHY+link layer | No datasheet available — treat as black box behind the tunnel |
| Empia EMP202, SAA7113H | Analog audio/video | Out of scope |

DV bandwidth is ~28.8 Mbit/s (plus CIP/IEC 61883 overhead) — trivially within USB 2.0
high-speed bulk throughput, so streaming is not a risk.

## 2. Prior art

- **SourceForge `pinnaclembusb`** — obtained and reviewed 2026-08-22, now in
  `pinnaclembusb/`. **Assessment: does not short-cut this project.** It targets the
  **MovieBox 2880/2288 analog** path, and contains *zero* references to DV, 1394,
  FireWire, AV/C or IEC 61883 — it is an FX2 firmware uploader (vendor request `0xA0`
  writing 8051 code into EZ-USB RAM) plus I2C register programming for an analog video
  decoder. Its real value to us is narrow but genuine:
  - a clean worked example of **FX2 RAM firmware download over libusb**, useful if our
    plug-in trace turns out to need one;
  - a libusb skeleton (open/claim/bulk read loop) worth reading for structure.
  - **Licence caution:** it is **GPLv2**. Read it for understanding, but do not paste
    code into this project unless you intend to ship GPLv2.
- Linux-media list threads about Pinnacle 700-USB (duplicate VID:PID discussion) —
  background only; no mainline Linux driver exists for the Marvin DV path.
- Standard specs that cover the payload once tunneling is understood: IEC 61883-1/-2
  (CIP framing, DV over 1394), AV/C Digital Interface Command Set + VCR Subunit spec
  (deck control opcodes: PLAY 0xC3, WIND 0xC4 = FF/REW, timecode status commands) —
  all publicly documented and implemented in open source (Linux `firewire` stack,
  `libavc1394`, `dvgrab`) that we can crib logic (not code, unless GPL is acceptable) from.

## 3. Reverse-engineering plan (the actual work)

### Phase 0 — Ground truth capture (no code)
1. On the machine where the vendor driver works: install **Wireshark + USBPcap**.
2. Record complete USB traces of:
   - device plug-in → renumeration (captures FX2 firmware upload + FPGA bitstream load),
   - Studio/capture-app start with camera attached (bus reset, config-ROM reads, isoch setup),
   - a short DV capture run,
   - each deck command individually: Play, Pause, Stop, FF, REW (isolated presses so
     the FCP frames are easy to spot),
   - idle timecode/status polling.
3. Dump USB descriptors (endpoints, interfaces, alt-settings) with USBView/libusb.
4. Fetch and read the `pinnaclembusb` tarball.
- **Exit criteria:** we can point at the bytes for "firmware", "bitstream", "start
  streaming", "a DV isoch packet", and "an AV/C PLAY command" in the trace.

### Phase 1 — Firmware & bitstream recovery
- FX2 firmware upload is the standard EZ-USB mechanism (control transfers, bRequest
  0xA0, CPUCS at 0xE600) — extract it **from the Phase 0 trace** (authoritative and
  simple) and/or locate the blob inside `MarvinBus64.sys`/`MarvinAVS64.sys` resources.
- Identify how the FPGA bitstream travels (likely bulk or repeated vendor writes after
  FX2 boot; EP1C3 raw bitstream is roughly 80 kB — look for a transfer of that size).
- Decide distribution strategy: **do not redistribute Pinnacle blobs**; ship an
  extractor that pulls firmware from the user's own MSI (like Linux `fwext` tools do).
- **Exit criteria:** a replay script (Python + pyusb is fine for this phase) brings a
  freshly plugged device to the "ready" state the vendor driver reaches.

### Phase 2 — Tunnel protocol mapping
- From the traces, catalogue: vendor control requests (init/reset/registers), which
  endpoint carries isoch DV data and its framing (CIP header layout, packet
  boundaries), how async 1394 transactions are submitted/completed (FCP writes to
  register 0xFFFF_F000_0B00, responses from 0xFFFF_F000_0D00), and how bus
  reset/camera-present events are signaled (likely an interrupt endpoint).
- Only if the trace is ambiguous: targeted Ghidra work on `MarvinBus64.sys` — the
  IRP dispatch for the `REQUEST_*` codes will name the USB commands for us. The
  debug-string density observed suggests this binary is pleasantly readable.
- **Exit criteria:** a written protocol doc (`PROTOCOL.md`) covering init, async TX/RX,
  isoch RX, and events.

### Phase 3 — Driver binding & streaming library
- Bind **WinUSB** to the device (dev-time: Zadig; ship: a small INF installed via
  `pnputil` — user-mode, no driver-signing problem).
- Library (C or Rust) implementing: device init (firmware+bitstream), async 1394
  transaction API, isoch stream start/stop with continuous bulk reads (overlapped I/O,
  a few MB of queued transfers — DV rate is low, this is easy).
- **Exit criteria:** sustained loss-free reception of raw isoch packets from a playing
  camera for ≥10 minutes.

### Phase 4 — DV capture application
- Strip CIP headers, reassemble DIF sequences into DV frames (PAL 144 000 B / NTSC
  120 000 B), detect frame boundaries via DIF header section/sequence numbers.
- Write **raw DV** (`.dv`) and/or type-2 DV AVI; audio is embedded in DV — nothing
  extra to do. Validate output with FFmpeg/VLC (`ffmpeg -i capture.dv`).
- Nice-to-haves, in order: dropped-frame counter, live preview (libavcodec DV decode —
  cheap), scene-split on recording-date discontinuities (dvgrab-style).
- **Exit criteria:** captured file is bit-identical DV essence vs. a vendor-driver
  capture of the same tape segment (this is the "lossless" proof).

### Phase 5 — Deck control
- AV/C over the async tunnel: FCP command frames (VCR subunit): PLAY, PAUSE (PLAY
  opcode with pause operand), STOP/WIND, FF/REW, plus TIME CODE status polling for
  position display. This is fully specified publicly; the only unknown is the tunnel
  encapsulation, solved in Phase 2.
- **Exit criteria:** all five transport buttons work; timecode display live.

## 4. Risks & unknowns

| Risk | Severity | Mitigation |
|---|---|---|
| FPGA bitstream load mechanism unclear from trace alone | Medium | It's still just USB transfers to replay verbatim; worst case, Ghidra on MarvinBus dispatch |
| Tunnel is not clean 1394 primitives but something bespoke | Medium | Strings strongly suggest otherwise; trace will settle it in Phase 0 |
| ~~Isoch data on USB *isochronous* endpoints (WinUSB iso support is clunkier)~~ | **Resolved** | Descriptor dump confirms **every endpoint is bulk**, no iso endpoints exist. See [docs/usb-descriptors.md](docs/usb-descriptors.md) |
| USBPcap drops packets at DV rate (default buffer is 1 MB, DV is ~3.6 MB/s) | Medium | Always capture with `-b 134217728 -s 65535`; `tools/capture-usb.ps1` does this |
| Device needs 1394 bus-master duties we must emulate (IRM, bandwidth alloc) | Low | Point-to-point with one camera; camera or link chip typically handles it; dvgrab shows minimal-host approach works |
| Firmware blobs can't be redistributed | Certain | Install-time extractor from user's own MSI |
| Vendor driver stops being installable for tracing (old MSI on Win10/11) | Low | You already have it working — capture traces early, archive them |

## 5. Tooling

Wireshark + USBPcap (tracing) · 7-Zip/lessmsi (done) · Python + pyusb (protocol
experiments/replay) · Ghidra (only as needed) · Zadig/WinUSB + libusb (driver) ·
FFmpeg (validation) · Cypress EZ-USB FX2LP TRM, IEC 61883, AV/C VCR subunit spec
(all public).

## 6. Effort estimate (focused hobby pace)

- Phase 0: 1–2 evenings
- Phase 1: 2–4 evenings (replay is quick; blob hunting can drag)
- Phase 2: 3–6 evenings (the core unknown; Ghidra could add a few more)
- Phase 3: 2–4 evenings
- Phase 4: 2–3 evenings (DV reassembly is well-trodden ground)
- Phase 5: 1–2 evenings

Roughly **3–6 weekends end-to-end** for a working capture tool; the long tail is
robustness (error recovery, hot-plug, drop handling).

## 7. Verdict

**Feasible, and more tractable than it first appears.** The decisive discovery is that
the vendor architecture is a 1394-over-USB tunnel with Microsoft's standard DV stack
on top: everything above the tunnel (CIP, DV framing, AV/C deck control) is publicly
documented with open-source reference implementations. The genuinely unknown surface
is limited to (a) the boot sequence (FX2 firmware + FPGA bitstream — replayable from a
USB trace without understanding it) and (b) the tunnel encapsulation, which a
methodical Wireshark session plus, at worst, light Ghidra work on a string-rich,
debug-friendly `MarvinBus64.sys` should yield. No kernel driver, no driver signing, no
datasheet for the Agere chip needed.
