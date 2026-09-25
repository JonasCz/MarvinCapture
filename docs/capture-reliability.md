# Capture reliability — how to run it, and how to prove it worked

Status as of **2026-09-22**. Companion to
[command-channel-findings.md](command-channel-findings.md), which explains
*why* each of these things is the way it is.

## Where it stands

A **5-minute continuous capture** (`-t 300`), verified three independent ways:

| measure | result |
|---|---|
| Captured | 1,152,448,728 raw bytes → 1,076,040,000 bytes of DV |
| Frames | **8,967** in 300 s = 99.73% of the 8,991 NTSC expects |
| DIF sequences | 89,670 — exactly 10 per frame, **0 zero-filled** |
| CIP/DBC continuity | **0 discontinuities** over 2,242,030 data blocks |
| `tools/dvcheck.py` | 0 bad headers, 0 bad section types, 0 bad DBNs — `RESULT: clean` |
| `ffmpeg -f null -` | **0 decode errors**, counts exactly 8,967 frames |
| Stop sequence | all 4 packets ACK; device streams normally on the next run |
| Ctrl+C | shuts down in **0.31 s**, file ends on a whole frame boundary |
| Repeatability | three consecutive start/capture/stop cycles, all clean |

The shortfall against 8,991 is start-up latency, not loss: the device needs
~14 s of bring-up (bitstream upload, a 1.5 s FPGA settle, then the 232-packet
start sequence) before the first byte arrives, and `-t` starts counting when
the start sequence returns.

---

## 1. Running a capture

```bash
sudo ./build/pincli -o out.dv -b traces/fpga-bitstream-candidate.bin -t 300
```

| flag | meaning |
|---|---|
| `-o` | raw DV output (required) |
| `-b` | FPGA bitstream blob (default `traces/fpga-bitstream-candidate.bin`) |
| `-t` | stop after N seconds; without it, runs until Ctrl+C |

`-t` takes exactly the same shutdown path as Ctrl+C, so it is the right way to
test stop behaviour repeatably — particularly over ssh, where delivering
Ctrl+C to the right process is awkward.

Either way the shutdown is: stop the receive queue, replay the 4-packet stop
sequence while still draining EP 0x88, then discard any frame the stop landed
in the middle of, so the file always ends on a whole frame. Measured at
0.31 s from signal to exit.

> The bitstream is **Pinnacle's**, extracted from the user's own driver
> install. It is kept in `traces/` so the driver runs out of the box on this
> machine, and `.gitignore` excludes it. Never redistribute it.

### Running it over ssh

Anything longer than the ssh client's read timeout must be detached, or the
channel closing will SIGHUP the capture mid-write and leave a truncated file:

```bash
setsid nohup ./build/pincli -o out.dv -b traces/fpga-bitstream-candidate.bin -t 300 > run.log 2>&1 < /dev/null &
```

### The camera has to be playing

Capture produces **zero bytes** unless the camera is actually transmitting —
in tape mode with the tape stopped, it is not. This looks identical to a dead
device from the host side, so check the device state before assuming a bug
(next section).

---

## 2. Telling device faults apart from "nothing is being sent"

Zero bytes on EP 0x88 has several completely different causes that used to be
indistinguishable. Two checks now separate them:

**The FPGA did not come up.** The `05`/`06` status reads bracketing the
bitstream upload answer `<cmd> 01` when ready and `<cmd> 00` when not. This is
checked, and `pincli` aborts with:

```
pinnacle: WARNING: device reports NOT READY to status read 05 ...
pincli: init failed: device reports not ready (FPGA did not come up; needs a physical USB power cycle)
```

A warm reboot does **not** clear this, and neither does unbinding the xHCI
controller for 15 seconds — neither cuts VBUS. It needs a physical replug.

**The receiver is fine, the source is quiet.** `PINNACLE_PROBE=1` reads seven
OHCI registers after the start sequence:

```
probe IntEvent        (0x080) = 0x04d10030
probe SelfIDCount     (0x068) = 0x00010014
probe LinkControlSet  (0x0e0) = 0x00300600
probe NodeID          (0x0e8) = 0x8000ffc0
probe CycleTimer      (0x0f0) = 0x339dc36a
probe IR0.CtrlSet     (0x400) = 0xc0009400
probe IR0.Match       (0x410) = 0x2000003f
```

Read it like this:

| register | this value means |
|---|---|
| `NodeID` `0x8000ffc0` | `iDValid` set, bus `0x3FF`, **we are node 0** (so the camera is node 1, which is what the start sequence hardcodes) |
| `SelfIDCount` `0x00010014` | generation 1, no `selfIDError` — the bus reset completed |
| `LinkControlSet` `0x00300600` | cycle master + cycle timer enabled, receiving self-ID and PHY packets |
| `CycleTimer` non-zero, changing | the link is clocked and alive |
| `IR0.CtrlSet` `0xc0009400` | bufferFill + isochHeader; `run` (b15), `wake` (b12) and `active` (b10) set, **`dead` (b11) clear** — the receiver is healthy |
| `IR0.Match` `0x2000003f` | listening on **channel 63**, tag 1 |

If `IR0.CtrlSet` shows `run` and `active` with `dead` clear and you still get
no data, the receive side is working and the camera is not transmitting.

---

## 3. Proving no data was dropped

### The camera gives us no timecode

Checked directly, don't re-litigate it:

```bash
python3 tools/dvcheck.py out.dv --dump-packs
```

```
vaux     pack 0x60  x200    samples: ffff00ff
vaux     pack 0x61  x200    samples: 0fc8fcff
vaux     pack 0x62  x200    samples: ffffffff      <- REC DATE: "no information"
vaux     pack 0x63  x200    samples: ffffffff      <- REC TIME: "no information"
vaux     pack 0x70  x20     samples: c6f6ff7f
vaux     pack 0x7f  x20     samples: ffff0d82
```

No SMPTE timecode pack (0x13) anywhere, and no subcode data at all. That is
expected in live/E-E mode: timecode and subcode come off tape. So timecode
cannot be used to verify continuity here.

### What we use instead

**CIP/DBC continuity**, reported by `pincli` at the end of every run:

```
pincli: continuity: OK — 335515 data blocks, no CIP/DBC discontinuity (1 at stream join, expected)
```

The IEC 61883 CIP header carries a data block continuity counter that the
*camera* increments once per 480-byte data block, modulo 256, across the whole
connection. A jump means data was lost somewhere between the camera's
transmitter and our file. It is finer-grained than a frame and needs no camera
feature. (Empty rate-padding packets are skipped — implementations disagree
over whether their DBC is the previous or the next value.)

The "at stream join" count is not loss. The isochronous ring is already
running when we attach, so the first read lands mid-record and the framing
layer resyncs a quadlet at a time, discarding bytes by design. Exactly one
discontinuity always appeared there — independent of run length, measured at
20 s, 30 s, 45 s, 60 s and 300 s — so it is counted separately rather than
reported as a permanent false positive. A healthy run reports `0` real
discontinuities and `1` join.

**Structural verification**, `tools/dvcheck.py`:

```bash
python3 tools/dvcheck.py out.dv --duration-seconds 300
```

It checks that the file is a whole number of 12,000-byte DIF sequences, that
each sequence is where its Dseq says it should be, that all 150 blocks per
sequence carry the section type and DBN the DV spec requires, that no sequence
is the all-zero placeholder the reassembler writes for a gap, and that the
frame count matches wall-clock duration.

The two are independent and should agree. If `dvcheck` reports zero-filled
sequences, `pincli`'s DBC line says whether they were really lost in transit.

### The USB thread must never wait on the disk

Found while chasing sporadic single holes in HDV captures (2026-09-25); it
applies equally to DV. The evidence, from raw EP 0x88 dumps:

- The holes always sat ~5 s into a run — the first time the kernel's writeback
  flusher fires — and each followed a stall of 40–50 ms in the capture
  callback (`fwrite` blocked on the filesystem).
- The type-9 ring addresses were continuous, so nothing was lost on USB. The
  OHCI record trailers carry the 1394 cycle count and jumped by ~25 cycles
  (~3 ms). Just before the jump, one packet was **aborted mid-write** and the
  next packet re-used the same ring address — the controller's receive FIFO
  overran while the host was not reading, dropped whole bus cycles, then
  resumed at the un-advanced buffer position.
- The queue was 32 transfers, but the device ends each transfer after only
  ~4 KB, so it held ~125 KB ≈ 36 ms, not the ~1 MB the comment claimed. Any
  consumer stall longer than that overran the FPGA.

Fix: 256 transfers (~1 MB, ~290 ms), and `pincli` copies each transfer into a
64 MB ring buffer drained by a second thread that runs the reassembler and
writes the file through a 4 MB stdio buffer. `pincli` prints the writer
buffer's high-water mark at the end (under a deliberate `dd … fdatasync` disk
storm it stayed under 4 MB) and stops with a warning if it ever overflowed.
A/B under that storm, 30 s HDV runs: old binary 2 of 5 clean, new 5 of 5;
a further 4×60 s and a 5-minute run were also clean.

**Ring addresses are ignored by the reassembler.** It concatenates type-9
payloads in arrival order. That is correct while addresses advance
contiguously, but an OHCI overrun makes the controller rewrite an address, and
the aborted partial record is then followed by the good one at the same place.
With the stall fixed this no longer occurs in practice; if it ever shows up
again (`hdvraw`-style analysis: look for a message whose address goes
backwards), honour the addresses rather than the arrival order.

---

## 4. Tunables (all opt-in; defaults are the right values)

| variable | effect |
|---|---|
| `PINNACLE_QUEUE_DEPTH=<n>` | EP 0x88 transfers kept in flight (default 256, max 256). `1` reproduces the old synchronous loop **and its data loss** — A/B only. |
| `PINNACLE_STOP_DRAIN=0` | Stop draining EP 0x88 during the stop sequence, i.e. go back to the stop failing on packet 3 of 4. A/B only. |
| `PINNACLE_EP84_DRAIN=0` | Disable the EP 0x84 status drain. A/B only. |
| `PINNACLE_PROBE=1` | Dump the OHCI registers above after the start sequence. |
| `PINNACLE_DEBUG_EP88=1` | Log every EP 0x88 completion: size, delivery time (`t=`), the time libusb completed it (`c=`), and any callback that took over 1 ms (`cbslow`). Not for loss testing: the log and `PINNACLE_RAW_DUMP` are written on the USB thread and add their own disk stalls. |
| `PINNACLE_DEBUG_EP84=1` | Log every EP 0x84 record with its arrival time. |
| `PINNACLE_RAW_DUMP=<path>` | Write the raw EP 0x88 byte stream to a file before the reassembler touches it. |

---

## 5. Two traps worth knowing

**The command channel blocks while EP 0x88 has an unread backlog.** Any code
that writes to EP 0x02 while the isochronous receive context is in `run` must
keep reading EP 0x88 at the same time, or the write times out. Both the stop
sequence and `PINNACLE_PROBE=1` do this now. This cost two wrong diagnoses
before it was isolated.

**A non-transmitting camera is indistinguishable from a dead device** unless
you check the two things in section 2. One earlier investigation concluded the
device had been bricked by the stop sequence when it had not been.
