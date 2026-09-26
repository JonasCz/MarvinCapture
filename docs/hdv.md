# HDV capture

Status as of **2026-09-25**. HDV over the FireWire port works with the same
hardware, the same unmodified FPGA bitstream and the same start sequence as DV.
Only the host-side reassembly is different. Read
[protocol.md](protocol.md) first for the two
outer framing layers, which are shared.

## Why it works

The FPGA is a generic OHCI-1394 controller and EP 0x88 tunnels its isochronous
receive DMA. OHCI does not care what the isochronous payload is, so an HDV
camera in live mode "just arrives" on the same ring. First contact (HDV camera
in live/E-E mode, 2026-09-25) needed no change to the device side: the
blind `oPCR[0]` compare-swap and `IR0.ContextMatch` channel 63 that were captured
from a DV session connect to this camera as well.

Bandwidth is not the constraint: HDV is 19–25 Mbit/s against DV's 28.8, and the
bulk pipe carries ~3.4 MB/s of ring data either way.

## What is on the wire

Same layers, different CIP payload (IEC 61883-4, MPEG2-TS):

| | DV (IEC 61883-2) | HDV (IEC 61883-4) |
|---|---|---|
| CIP quadlet 0 | `01 78 00 <DBC>` | `01 06 c4 <DBC>` |
| CIP quadlet 1 | `80 00 <SYT>` (FMT 0x00) | `a0 00 00 00` (FMT 0x20) |
| DBS | 0x78 (480-byte data block) | 6 (24-byte data block) |
| FN / SPH | – | 3 (8 data blocks per source packet) / 1 |
| isoch payload | 1 × 480 B DIF block | 1–3 × 192 B **source packets** |
| source packet | – | 4-byte timestamp + one 188-byte TS packet |

Observed stream from the test camera: MPEG-2 Main, 1440x1080 (SAR 4:3), 25 fps,
25 Mbit/s, plus MP2 48 kHz stereo audio. PIDs: video `0x810`, audio `0x814`,
PAT `0x0`, PMT `0x81`. The DV CIP validation in `dv_reassembler.c` distinguishes
the two by the FMT field (`h[8] & 0x3f`) and, for HDV, `DBS == 6` and a payload
that is a multiple of 192 bytes; the first data packet locks the format.

## What `pincli` does

`pincli -o out.ts ...` detects HDV automatically; no flag. It strips the two
outer layers, drops the 4-byte source-packet timestamps and writes the 188-byte
TS packets back to back. The output plays directly (`ffplay out.ts`).

**It starts on a real boundary and ends on one.** Nothing is written until a
video picture that carries an MPEG-2 sequence header (the start of a GOP) is
seen — the ring is already running when we join, so what precedes that is a
partial picture and possibly stale ring contents. This mirrors DV waiting for
`Dseq 0`. When the gate opens, the most recently seen PAT and PMT are written
first, so the file is decodable from its first packet. On stop, the trailing
partial picture is cut, so the file ends on a whole one. (A non-seekable
output, e.g. a pipe, cannot be cut and keeps the tail.)

## Verifying integrity — read this before trusting the DBC line

`pincli` reports two independent checks, but **they are not equally strong for
HDV**:

- **TS continuity counters (authoritative).** Per-PID 4-bit CC. Every lost
  packet shows up as a jump on the affected PIDs.
- **CIP DBC (weak).** The CIP data-block counter advances by **8 per source
  packet** and is 8 bits wide, so a hole of exactly 32 source packets
  (6,144 bytes) wraps it back to the expected value and is **invisible**. A
  60 s capture showed exactly that: video, audio and PAT CC all jumped (about
  32 packets lost) while the DBC line said "no discontinuity". For DV the DBC
  window is 256 blocks = 123 KB, which is why it was sufficient there.

Because one hole surfaces once per PID, CC jumps that follow an already-counted
DBC discontinuity are reported separately ("explained by CIP discontinuities")
rather than being double-counted as loss.

Independent check on the file:

```bash
ffmpeg -v error -i out.ts -map 0:v:0 -f null -    # a healthy file prints nothing or one join warning
```

## Known behaviour

- **A clean 5-minute run exists**: 7,487 pictures, 0 CC errors, 0 DBC discontinuities, ffmpeg decodes it
  with no messages at all.
- **Earlier sporadic holes were a host-side stall, now fixed.** Early runs
  occasionally had one hole of ~25 bus cycles (~3 ms, a damaged picture)
  about 5 s in. Cause: a filesystem flush blocked the thread that keeps the
  USB queue full, the FPGA's receive FIFO overran and dropped bus cycles. Not
  the camera, not the bus. `pincli` now decouples disk writes from USB reads;
  see [usage.md](usage.md#the-usb-thread-must-never-wait-on-the-disk)
  for the evidence and the A/B. A hole, if one ever occurs, is kept as received
  and only counted (TS has no fixed-size unit to zero-pad, unlike DV).
- **Live mode only.** As with DV, this receives whatever the camera transmits.
  Tape transport control is separate: see [deck-control.md](deck-control.md).
- Not tested: 720p HDV, S400 cameras, PAL/NTSC variants (only 1080i/25).
