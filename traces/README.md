# Trace index

All captured 2026-09-22 on the Ubuntu/usbmon rig (the Ubuntu capture host), vendor driver
running in the `Win7` VirtualBox guest with the device passed through. Method:
[../docs/linux-capture-setup.md](../docs/linux-capture-setup.md). Overview:
[../HANDOFF.md](../HANDOFF.md).

**Every capture reported `dropped 0`** — where data is missing it is the vendor stack
dropping it, not us. See "Known limitation" at the bottom.

## Read this before opening a trace

- The Pinnacle is **`usb.device_address == 2`** on bus 1.
- **The first 12 packets (all at t≈0.000) are a synthetic descriptor snapshot**, not
  real traffic — the same idea as USBPcap's `--inject-descriptors`. Filter them out
  with `frame.time_relative > 0.001`.
- DV DIF header blocks are `byte0 == 0x1f`, `byte1 == (Dseq<<4)|0x07`, `byte2 == 0x00`.
  **Matching only `1f 07 00` finds sequence 0 alone and undercounts ~10×.**
  Sequence count identifies the system: **10 → NTSC**, **12 → PAL**.

## Captures

| File | Size | Contents |
|---|---|---|
| `20260922-142720-coldboot-driver-init.pcapng` | 240 KB | **Best init trace.** VM powered off, capture started, VM booted — so this is the driver initialising from scratch. Descriptors, 88,446 B on EP 0x02 (bitstream + commands), full EP 0x01/0x81 I2C init, `SET_INTERFACE` alt 0 → alt 1. No EP 0x88 (nothing streaming). |
| `20260922-135315-enumeration-init.pcapng` | 220 KB | Same thing via `usbdetach`/`usbattach` instead of a reboot. 1,299 packets / 14.5 s. Useful as a cross-check — it yields a **byte-identical** bitstream. |
| `20260922-144400-stream-start-transition-TRIMMED.pcapng` | 6.2 MB | **The stream start handshake.** All 46 EP 0x02 OUT commands and 70 EP 0x84 IN replies, plus the first ~2 s of DV. Streaming begins at t=8.6048. Trimmed from the 131 MB original. |
| `20260922-142455-stream-stop.pcapng` | 2.4 KB | **Stream teardown in isolation** — closing WinDV produces exactly 6 packets / 76 bytes on EP 0x02 OUT and nothing else. |
| `20260922-141844-ntsc-camera-playing.pcapng` | 47 MB | **Best DV content trace.** NTSC, camera playing a moving image. 15 s, 20,086 packets, 46,889,868 B on EP 0x88 at 3.13 MB/s. Zero-byte fraction 0.054. |
| `20260922-140208-windv-open-bars.pcapng` | 33 MB | **PAL**, idle deck emitting colour bars. 12 s @ 2.55 MB/s, zero-byte fraction 0.599 (flat DCT padded to DV's fixed frame size — not emptiness). Keep for PAL coverage. |
| `20260922-135855-idle-baseline.pcapng` | 1.7 KB | Device attached, nothing streaming. Proof the device is *totally* silent when idle. |
| `20260922-140110-stream-start.pcapng` | 1.7 KB | Negative control — empty because WinDV needs >20 s to build its DirectShow graph and the window closed too early. |
| `20260922-142935-stream-start-full.pcapng` | 1.7 KB | Negative control — Start-menu search opened the `WinDV-1.2.3` **zip folder**, not the exe, so nothing ever streamed. |
| `20260822-descriptors-idle.pcap` | 396 KB | Older USBPcap capture from the bare-metal Windows box. Descriptors only; that tool never captured this device. See [../docs/capture-tooling.md](../docs/capture-tooling.md). |

Not copied here: **`20260922-144400-stream-start-transition.pcapng`** (131 MB), the
untrimmed original, left on the host in `~/pinnacle-traces`.

## Extracted payloads

| File | Size | What it is |
|---|---|---|
| `fpga-bitstream-candidate.bin` | 78,422 B | The five large EP 0x02 OUT transfers (17408 + 16384×3 + 11862) from the detach/reattach init. Almost certainly the Altera Cyclone EP1C3 `.rbf`. |
| `fpga-bitstream-coldboot.bin` | 78,422 B | Same blob from the **cold boot** path. **Byte-identical**, MD5 `3888c23c9bcc81c88964c45d981a0b68` — proving the bitstream is static and can simply be replayed. |
| `ep02-out-payload.bin` | 86,914 B | *Everything* sent on EP 0x02 during init (bitstream plus interleaved commands). |
| `ep88-ntsc-sample.bin` | 12.2 MB | Contiguous EP 0x88 payload from the NTSC/camera trace. |
| `ep88-sample.bin` | 5.9 MB | Contiguous EP 0x88 payload from the PAL/colour-bars trace. |

## Ground truth

| File | What it is |
|---|---|
| `capture.26-09-22_12-11.00.avi` | WinDV's own recording of the same NTSC source. Verified `dvsd`, 720×480, 29.971 fps, 198 frames, 6.61 s. **Use this to validate DV frame reassembly**, since the pcap streams have gaps. |

## Known limitation: the streams have gaps

`dumpcap` reported `dropped 0` in every capture, so usbmon recorded faithfully. The
loss is upstream in the device → vendor-driver path: WinDV reports dropped frames
(VM/passthrough overhead) and sometimes freezes outright, which is part of why this
project exists.

Evidence in the NTSC trace: a DIF sequence is 150 × 80 = 12,000 bytes and observed
header-to-header spacing is frequently the expected ~12,850, but often far shorter
(1,435 / 3,617 / 4,748 / 6,656 …). Frame spacing (`Dseq 0` → next `Dseq 0`) should be a
constant ~128,500 but ranges 65,140–199,400. Throughput agrees: 3.13 MB/s against
DV25's ~3.6 MB/s.

**Implication:** adequate for *protocol* work — framing, endpoints, command channel and
init sequence are all intact. **Not** adequate to prove lossless reassembly; use the
AVI for that.

## Reproducing / extending

`sudo pincap timed <label> <seconds>`, or `sudo pincap start <label>` … `sudo pincap stop`
on the Ubuntu host. To drive WinDV while capturing, use `sudo vmctl start-windv` /
`sudo vmctl stop-windv` — see [../docs/vm-control.md](../docs/vm-control.md).
Remember WinDV needs **>20 s** after launch before data flows.
