# Device start-up, step by step (2026-09-25)

What the driver sends between plugging in and receiving the first stream byte.
It explains each step and where it comes from. It also says what is still
unknown, and which steps were tested for being necessary.

The 1394 part used to be a verbatim replay of 232 EP 0x02 packets recorded
from Windows. It is now generated step by step in
[`src/core/pinnacle_1394.c`](../src/core/pinnacle_1394.c) (`p1394_link_init`,
`p1394_find_camera`, `p1394_connect`, `p1394_ir_start`), called from
`pinnacle_stream_start()`.

**Sources.** The meanings come from these places:

- `MarvinBus64.sys`, the vendor's virtual 1394 bus driver. It was decompiled
  with Ghidra; addresses below are Ghidra `FUN_` labels.
- `MarvinAVS64.sys`, the vendor's USB function driver.
- A usbmon trace of the vendor driver cold-booting the device (not kept in the repo).
- The OHCI 1.1 and IEEE 1394a / 1212 / IEC 61883-1 specifications.

**Confidence labels:**

- **code**: read directly from the vendor driver.
- **spec**: a standard register or field, used as specified.
- **trace**: the value was observed, but its meaning is not known.
- **tested**: we removed the step and watched what happened.

The same sequence runs unchanged on the 510-USB (PID 0223, verified on hardware
without a camera: both bitstreams load, GUID is read, `NodeID 0xc000ffc0`, bus of
one node). See [hardware.md](hardware.md#models).

## Phase 1 — USB side (unchanged, still a replay)

1. **Select alt setting 0.**
2. **Config-channel bring-up (EP 0x01 / 0x81).** This replays 80
   exchanges, `PINNACLE_CONFIG_PREBITSTREAM_SEQ` in `protocol_data.h`.
   - Most of them are SAA7113 analog-decoder I2C writes. The decoder sits at
     I2C address `0x4a`; that address is in the code of `MarvinAVS64.sys`,
     which uses `0x48` only for PID `0x20b`, a model this driver's INF does not
     bind (not 0206, as an earlier version of these notes said).
   - Removing the whole sequence stops the device coming up. That was tested
     earlier: see [protocol.md](protocol.md).
   - Two of the exchanges read the device's configuration memory:

     | request | reply bytes 4..11 on the development unit | use |
     |---|---|---|
     | `80 00 08 00…` | `fb82ad03 8d615d0e` | unknown ID |
     | `80 03 08 00…` | `fb82ad03 128d3000` | **the 1394 GUID**; see step 25 |

     `pinnacle_init_hardware()` keeps both replies (`dev->guid_hi/lo`,
     `dev->id0`).
   - `05 00` → `05 01` is the "FPGA loader ready" check. `MarvinAVS64.sys`
     sends the same `{5,0}` request and needs a reply of 1 before it loads a
     bitstream (**code**).
3. **FPGA bitstream (EP 0x02, 5 transfers, 78,422 bytes).**
   - This is the **"OHCI"** bitstream. `MarvinAVS64.sys` embeds three
     different bitstreams (`FUN_0002c280`), and we checked that this one is
     byte-identical to our file.
   - The other two, for analog capture and for render, are described in
     [analog.md](analog.md#background-the-vendor-driver-other-marvin-models-the-three-bitstreams).
4. **Wait 1.5 s**, then `06 00` → `06 01` (FPGA up).
5. **Select alt setting 1.**

## Phase 2 — the 1394 link (`p1394_link_init`)

### EP 0x02 message types

Each message starts with a little-endian u32. Bits 31:28 give the type;
bit 27 means a reply is wanted, with a tag in bits 26:20.

| type | meaning | emitter in MarvinBus64 |
|---|---|---|
| 2 / 3 | OHCI register write / read. Address `0x10000 + offset`. | `FUN_0002c5c0`, batched by `FUN_0002b9f0` |
| 4 | FPGA **USB-side register** write, index in bits 19:0. The driver keeps a shadow copy and always sends the whole word. | `FUN_0002bbb0(idx, clear, set)` |
| 5 | Read of that register bank; the reply comes back as type 5. | `FUN_0002c1e0` |
| 6 | FPGA **extension bank** write. It uses OHCI-style offsets (`0x10000 + off`) but is a separate register file. | `FUN_0002bd80` |
| 8 | Block write into device RAM (`len` in bits 26:16, address in bits 15:0) | |
| 9 / 10 | Device → host: DMA data into RAM / `IntEvent` | |

### The steps

"Needed?" is the result of leaving that one step out and running
`pindeck state play wait:4 stop` on the Canon HDV camera. The next run
always recovered without a replug.

| # | what | value | meaning | source | needed? |
|---|---|---|---|---|---|
| 1 | type 5 [0] | → `0x81` | Status read. The vendor proceeds only if the low byte is below `0x7a`, in a resume path. We only log it. It is a candidate for telling hardware revisions apart. | code, trace | no |
| 2 | type 4 [1] | `0x0000fe01` | Derived from the registry value `LengthOfIsochBuffer` (default `0x5000`): high byte = `0x5000/0x1d4a*2+0xfa`, low byte = `0x5000/0x4096`. The fields' meaning is unknown. | code | no; kept for parity |
| 3 | type 6 [0x40], [0x10], [0x04], [0x0c] | 0, `0x10000`, 6, `0x2008` | Literal constants in `FUN_0002cd00`; meaning unknown | code | **yes**: without them link init fails |
| 4 | HCControlSet | `softReset` | | spec | (not tested) |
| 5 | type 6 [0x40] | 0 | as step 3 | code | **yes** (tested together with step 3) |
| 6 | HCControlSet | `LPS` | link power on | spec | (not tested) |
| 7 | 13-register table | see below | the literal init table of `FUN_0002cd00` | code | partly tested |
| 8 | ATRetries | `0x40000fff` | 15 retries each; cycleLimit `0x200` | spec | no; kept |
| 9 | PHY reg 4 \|= `0x40` | `0x80` → `0xc0` | Contender. Link-active was already set. | spec | no; kept |
| 10 | PHY reg 7 = 1, reg 8 = 1; reg 7 = 2, reg 8 = 1 | | Port-status page: **disables PHY ports 1 and 2** (`FUN_0002cc80`). Only port 0 is wired on the 500-USB. | code, spec | no; kept |
| 11 | type 4 [0] | `0x00b40000` | see "USB-side register 0" below | code | no; set again at IR start |
| 12 | AR request context | 2 × INPUT_MORE on an 8 KiB buffer at RAM `0x3000`; descriptors at `0x1100/0x1110`, branching to each other; run | | spec | yes (nothing is received without it) |
| 13 | AsynchronousRequestFilterHiSet | `0x80000000` | accept requests from any node (FCP responses arrive this way) | spec | yes, for deck control |
| 14 | AR response context | same, buffer `0x5000`, descriptors `0x1140/0x1150` | | spec | yes |
| 15 | type 6 [0x74], [0x70] | GUID lo / hi, byte-swapped | `FUN_0002c770` mirrors the GUID into the extension bank | code | no |
| 16 | RAM `0x1000` ← config ROM; ConfigROMhdr, BusID, BusOptions, GUIDHi/Lo, VendorID, ConfigROMmap | see "Our config ROM" below | our node's identity | code, spec | no; kept (see below) |
| 17 | HCControlSet | `linkEnable \| LPS` (`0xa0000`) | link on | spec | yes |
| — | *(20 × type 5 [0], dropped)* | | `FUN_0002b5a0`: a fixed loop that ignores the answers, i.e. a ~40 ms settle delay | code | **no, removed** |
| 18 | PHY reg 1 \|= `0x7f` | | IBR + gap count 63: **bus reset** | spec | yes |
| 19 | ATreq/ATrsp ContextControlClear | run | the bus reset stops them anyway | spec | |
| 20 | read NodeID, SelfIDCount | e.g. `0x8000ffc0`, `0x00010014` | NodeID gives our node number (valid bit 31). SelfIDCount's size field of 5 quadlets means 2 nodes. | spec | used |
| 21 | IntEventClear | busReset | | spec | |

**Step 7, the 13-register table:**

- LinkControlClear all
- AsyncReqFilterHi/LoClear all
- LinkControlSet cycleTimerEnable, then cycleMaster
- IntMaskClear all
- IntEventClear all
- SelfIDBuffer = `0x2000`
- LinkControlSet rcvSelfID | rcvPhyPkt
- HCControlClear noByteSwapData, so payload bytes stay in wire order
- `0x800 = 0x7000`, `0x808 = 0x107000`, `0x840 = 0x10000020`. These three
  are FPGA registers beyond the OHCI map, and their meaning is unknown.
  `0x7000` is where the isochronous buffer starts.
  - **Tested as necessary:** without them the stream starts, and then the
    command channel stalls within a second.

### USB-side register 0 (type 4, index 0)

| bits | meaning | source |
|---|---|---|
| 25:16 | `ChangeModeTimeout` (registry, default 180, range 20..1000). The other hardware back-end writes the same value to its register named **"Async rx idle time"**, so this is most likely how long the FPGA waits before flushing a partly filled EP 0x84 packet. | code (`FUN_00048700` → op-table slot 0x16 → `FUN_00029fd0`; string table of the other back-end) |
| 9 | adaptive buffer "levels" on (registry `DisableLevels`; 0 by default) | code |
| 8 | isochronous data to USB **off**. Cleared when a listen starts (`FUN_0003cdc0` → op-table slot 4 → `FUN_0002eb20(1)`), set when it stops. | code |

In the trace, register 0 is written as `0x00b40000` at start-up and when a
listen starts, and as `0x00b40100` at stop. It is **not** a mode or crossbar
switch.

### Our config ROM

This is what the camera sees when it reads our node. The layout is
MarvinBus64's; only the GUID is unit-specific, and we now read it from the
device (Phase 1, request `80 03`).

```
0404xxxx  bus info block: info 4, crc_length 4, CRC
31333934  "1394"
f000a002  irmc cmc isc bmc, max_rec 10 (2048 B), link speed S400
<GUID hi> <GUID lo>                           e.g. fb82ad03 128d3000
0004xxxx  root directory, 4 entries
0c0083c0  node capabilities
03vvvvvv  module vendor ID (= GUID bits 63:40)
81000007  -> text leaf "Pinnacle Systems"
d1000001  -> unit directory
0004xxxx  unit directory: 12vvvvvv specifier ID, 13000000 version,
          17000000 model, 8100000d -> text leaf "Marvin Series  "
two text leaves (UTF-16LE, language 0x409)
```

The CRCs are computed the way the vendor computes them. That is CRC-CCITT
(0x1021) over each quadlet's bytes in **little-endian** order, where IEEE
1212 specifies big-endian. For the development unit this reproduces the
vendor ROM byte for byte (checked with `PINNACLE_DEBUG_1394=2`).

The camera does not appear to need our ROM: leaving it out changed nothing.
It is kept for three reasons:

- IEEE 1394 nodes are expected to publish one.
- Other cameras, or the Windows-style bus managers some cameras implement,
  may read it.
- It must carry the unit's own GUID. Other units, revisions and the other
  Marvin models (510-USB, 700-USB, 710) do not share the development
  unit's GUID, so the GUID is read from the device and never hardcoded (on the
  510-USB the read works the same way).

## Phase 3 — the camera

| step | how |
|---|---|
| **Find it** | Read the output master plug register (`oMPR`, CSR `0xFFFFF0000900`) of every other node. The first node that answers is the camera. The Canon HDV gives `0x3fff0001`: broadcast channel base 63, 1 output plug. |
| **Connect** (IEC 61883-1) | Read `oPCR[0]` (`…0904`), then lock/compare-swap it with the point-to-point count + 1. Retry with the returned value if something changed in between. The Canon gives `0x003f0092` → `0x013f0092`: channel 63, S100, 146 quadlets per packet. When playing, the online bit is also set (`0x813f…`). |
| **Listen** | The channel is taken from the oPCR, not hardcoded. |

The vendor trace did the same lock, but with its expected old value
hardcoded (`0xC03F3C7A`). A compare-swap with a stale "expected" value
silently does nothing. That was one reason to generate this step instead of
replaying it.

## Phase 4 — isochronous receive (`p1394_ir_start`)

| what | value | meaning |
|---|---|---|
| type 4 [0] | `0x00b40000` | isochronous-to-USB on (bit 8 clear) |
| IR0.ContextMatch | `0x20000000`, then `0x20000000 \| channel` | tag1 (CIP), channel |
| IR0.ContextControlSet | `0xc0000000` | bufferFill \| isochHeader |
| IR0.ContextControlClear | `0x38000000` | cycleMatch, multiChan, dualBuffer off |
| RAM `0x1180`, `0x1190` | `280c9000 00807000 00801191 00009000` and `…00801181…` | Two INPUT_MORE descriptors filling the same 36 KiB buffer (`0x807000`–`0x80ffff`), branching to each other: "listen forever". Bit 23 is set on every address of this path, apparently selecting the memory that is forwarded to EP 0x88. |
| IR0.CommandPtr, ContextControlSet | `0x00801181`, run\|wake | go |

**Stop** (`pinnacle_stream_stop`), with EP 0x88 kept drained throughout:

1. Type 4 [0] = `0x00b40100` (isochronous-to-USB off).
2. IR0.ContextControlClear run.
3. Wait until IR0 is no longer active.
4. **Release the connection:** compare-swap `oPCR[0]` with the
   point-to-point count − 1. The vendor's stop trace ends with the matching
   oPCR read.

## What was dropped, and why

The replayed start sequence also contained the following. None of it is
needed for capture or deck control:

- **Windows' enumeration of the camera** (start-sequence packets 64–128,
  plus parts of 129–221): quadlet reads of its config ROM (`…0400`–`…047c`),
  its plug registers (`…0900`, `…0904`, `…0980`, `…0984`) and some unit
  directory entries.
- **About 45 AV/C inquiries** (packets 129–219). MarvinBus64 contains no
  AV/C code at all, so these came from Windows' `avc.sys`/`msdv.sys`:
  - UNIT INFO and SUBUNIT INFO.
  - OUTPUT/INPUT PLUG SIGNAL FORMAT for eleven formats each (`80 00`,
    `80 80`, … `a0`).
  - Two vendor-dependent commands, with Panasonic (`00 80 45`) and Canon
    (`00 00 85`) company IDs.
  - VCR opcodes `0x71`, `0xda`, `0x78`, `0x79`, `0xd0`.
  - Timecode / RTC / ATN reads (`0x51`, `0x52`, `0x57`).
- **Write responses to the camera's replies, with the tlabels of the camera
  the trace was recorded against.** This camera's real replies therefore
  went unanswered. It kept re-sending them, and pending replies got mixed up
  with our first commands.
  - This is what earlier looked like a **"one command behind" Canon quirk**
    in [deck-control.md](deck-control.md). It does not happen with the
    generated start-up: every command is answered on a single send within a
    few milliseconds.
- **The hardcoded oPCR lock** (see Phase 3).
- **The initial register-0 writes** (`0x00b40000`, then `0`, 191 ms apart)
  and the **20 status reads**: tested unnecessary.

Bring-up is 5.4 s instead of ~16 s, and most of what remains is the
bitstream upload and the 1.5 s FPGA settle time.

## Still unknown

- Extension bank offsets `0x04`, `0x0c`, `0x10` and `0x40`, and registers
  `0x800`/`0x808`/`0x840`. All of them are required, but their meaning is
  unknown.
- The fields of USB-side register 1 (`0xfe01`).
- The status register (type 5 [0], `0x81`), and the `80 00` ID. The status
  register may identify the hardware revision; that is worth checking on a
  second unit.
- The config-channel opcodes other than `80` (read) and `05`/`06`
  (ready). See [analog.md](analog.md#background-the-vendor-driver-other-marvin-models-the-three-bitstreams).

## Verification (2026-09-25, Canon HDV, after a replug)

| test | result |
|---|---|
| Full deck test: state, play, timecode, pause, ff, rew, stop | every command answered on a single send; EP 0x88 rate matches the transport state |
| `pincli -t 15` and `-t 60` while playing | `tscheck` RESULT: clean; 0 continuity-counter errors; 0 CIP/DBC discontinuities |
| Back-to-back `pincli -t 10` | clean |
| oPCR after exit | back to `0x003f0092` (connection released) |
