# Playout and virtual deck: feasibility

Written 2026-11-07. Nothing is implemented yet. The findings come from
disassembling the vendor installer `Pinnacle_Video_Driver_64bit.exe` (product
12.1.0.029) with Ghidra 12.2. No device was attached, so **nothing here was
tried on hardware**.

The goal:

- **DV/HDV playout.** Send a file out over FireWire.
- **Virtual deck.** The box, loaded with a file, appears on the 1394 bus as a
  DV/HDV VCR that another node (a PC with capture software, a camcorder, a
  second Marvin) can control with AV/C deck commands.
- **Analog playout** comes second.

## Verdict

| part | feasible? | confidence | blocker |
|---|---|---|---|
| DV playout (isochronous transmit) | **yes** | high: the vendor driver does it, and every layer is decoded (below) | the FPGA's transmit context has never been run by us |
| HDV playout | **yes** | medium-high: same transmit path, 584-byte packets are provisioned | MPEG-TS pacing from the PCR; source-packet timestamps |
| Virtual deck (AV/C target, plug registers) | **yes** | high: all host-side, built only from primitives we already use | real controllers (Windows MSDV, dvgrab, Final Cut) must accept us |
| Analog playout on the **500-USB / 510-USB** | **no** | high | these models have no video encoder chip (see "Analog") |
| Analog playout on the 700-USB / 710-USB / MovieBox Deluxe | probably | low | separate reverse engineering of the Render design, and a unit to test with |

Most of the DV/HDV work can be built and unit-tested here without the device
(see "What can be done without the device").

## Where the vendor's transmit path is

Ghidra functions are listed by address. They are from `marvinbus64.sys`
unless noted.

**Op table.** The op table at VA `0x4df50` is laid out as 32-byte rows
`[selected, index, back-end A, back-end B]`. Back-end B is the OHCI design we
use. The `DAT_0004xxxx` pointers the decompiler shows are the `selected`
slots.

| what | function |
|---|---|
| `REQUEST_ISOCH_TALK` (generic) | `FUN_0003db50` → op `0x1e` (`DAT_0004e310`) |
| IT context setup | OHCI `FUN_0002d7b0` |
| Build one transmit packet | op `0x1d` → `FUN_0002db20` |
| Packet loop over an attached buffer | `FUN_0003efd0` (CIP header list plus payload; chunks of about 30,000 bytes per USB transfer); `FUN_0003f550` for the simple layout |
| Start a context (CommandPtr, then run\|wake) | op `0xf` → `FUN_0002eb80` |
| Wake a context (`ContextControlSet = 0x9000`) | op `0xe` → `FUN_0002e3c0` |
| Context register table | `FUN_00014df0`: ATreq `0x180`, ATrsp `0x1a0`, ARreq `0x1c0`, ARrsp `0x1e0`, IR0 `0x400`, **IT0 `0x200` / `0x204` / `0x20c`** (context index 5) |
| OUTPUT_MORE_IMMEDIATE / OUTPUT_LAST builders | `FUN_000362f0` / `FUN_00036400` (standard OHCI layout) |
| Type-8 RAM write header | `FUN_0002b600` |
| **Type-11 message** (new) | `FUN_0002c400` |

### The endpoint is EP 0x06

`marvinbus64.sys` hands every USB transfer to `MarvinAVS64.sys` through the
`To_USB_Stack` interface. Each transfer carries a queue number: 1 for async,
3 for isochronous transmit (`FUN_00046960(..., 3, ...)`).

`MarvinAVS64.sys` maps the queue numbers to pipe indices
(`FUN_0002fe78`, `FUN_00030b40`):

| queue | pipe index | endpoint |
|---|---|---|
| 1 async write | 2 | EP 0x02 |
| 2 async read | 3 | EP 0x84 |
| **3 isochronous write** | **4** | **EP 0x06 OUT** |
| 4 isochronous read | 5 | EP 0x88 |

The pipe order is alt setting 1's endpoint order (`01 81 02 84 06 88`). The
three mappings we already knew (0x02, 0x84, 0x88) match, so the transmit
endpoint is **EP 0x06**. Alt 1, the setting we already use, has it.
`MarvinAVS64.sys` also has the registry tunable `MaxPending1394IsoWrite`: it
keeps several transmit transfers in flight, the same way we keep reads in
flight on EP 0x88.

### What goes down EP 0x06

EP 0x06 uses the **same message framing as EP 0x02** ([protocol.md](../protocol.md)).
The host builds the whole OHCI IT DMA program and streams it, packet by
packet.

**Device RAM layout** (`FUN_0002d7b0`):

- A ring of 4 descriptor blocks of `0x30` bytes each, at `0x16b0`, `0x16e0`,
  `0x1710` and `0x1740`.
- 4 payload buffers, starting at `0x1770` and spaced at the packet size
  rounded up to 4 bytes. The maximum packet size is `0x248` = 584 =
  8-byte CIP header + 3 × 192-byte MPEG-TS source packets. That sizing is HDV's;
  DV needs 488 bytes.

**Per isochronous packet n** (`FUN_0002db20`), with slot = (n+1) mod 4:

1. **Type 8**: write the CIP header (8 bytes) and the payload into payload
   buffer[slot].
2. **Type 8**: write descriptor block[slot] (`0x30` bytes):
   - OUTPUT_MORE_IMMEDIATE, `reqCount` 8. Its two immediate quadlets are
     the isochronous header:
     - `tag<<14 | chan<<8 | 0xa<<4 | sy` and `spd<<16`.
     - `dataLength<<16`.
   - OUTPUT_LAST (`s=1`, `b=3`), with `reqCount` = the packet length,
     `dataAddress` = payload buffer[slot], and the branch to the next block.
   - **Vendor bits:** the OUTPUT_LAST `dataAddress` word also carries bit 31
     and, in bits 29:24, a 6-bit packet sequence number (n+1) mod 64. RAM
     addresses are 16-bit, so these bits are free. On the receive side,
     bit 23 played a similar role.
   - A CIP-only packet (the empty packets DV uses to pad its rate) branches
     differently (`reqCount` 8).
3. **Once the first three packets are queued:**
   - **Patch** the previous block's OUTPUT_LAST branch word, with a type-8
     write of 4 bytes, to point at this block with Z=3.
   - **Wake** the context with a type-2 register write of `0x9000` to
     `0x200`. This register write travels **in-band on EP 0x06**.
4. **At packet 2 only:** start the context with `IT0.CommandPtr`
   (`0x20c`) = block 0 | Z, then `ContextControlSet` = run|wake.
5. **From packet 2 on:** append a **type-11 message**:

   ```
   0xb0000000 | first<<26 | ((seq - 3) & 0x3f)
   ```

   `first` is set only on the first one.

**How flow control most likely works (interpretation):**

- The FPGA holds back the rest of the EP 0x06 stream until the packet with
  that sequence number has been sent. A ring slot is therefore never
  overwritten while it is still queued.
- **Back-pressure is plain USB NAK** on EP 0x06.
- The host's only real-time duty is to keep enough bulk transfers queued.
  The vendor keeps about 30 KB (about 60 packets, about 7.5 ms) per transfer,
  with several transfers pending. Rate pacing is done by the 1394 cycle
  clock, one descriptor block per cycle, not by the host.

**Cost:** about 560 bytes of EP 0x06 traffic per DV packet, about 4.5 MB/s.
That is well within USB 2.0 bulk.

**Not seen:** cycleMatch. The vendor starts the context at once. CIP header
contents (DBC, SYT) are the client's job: Windows' `61883.sys` computed them,
and here our code would.

## Virtual deck

The bus driver has no AV/C code (as [deck-control.md](../deck-control.md)
says). On Windows the target side does not exist at all. Here it would be
entirely ours, built on primitives we already have:

### Incoming requests

The AR request context already receives every request to our node
(`AsynchronousRequestFilterHi` accepts all nodes).
`handle_ar_packet()` (`src/core/pinnacle_1394.c`) handles FCP responses and
logs everything else as unexpected. A target must add:

- **Quadlet reads** of oMPR / oPCR[0] / iMPR (`0xFFFFF0000900`, `…0904`,
  `…0980`). Reply with a read response, tcode 6.
- **Compare-swap locks** on oPCR[0] (IEC 61883-1 connection management).
  Reply with a lock response, tcode `0xb`. A point-to-point count rising
  above 0 means "start transmitting on the oPCR's channel". Dropping back to
  0 means "stop".
- **FCP command writes** to `0xFFFFF0000B00`. Send the write response we
  already send for FCP responses. Then write the AV/C response to the
  controller's `0xFFFFF0000D00`, with the existing `p1394_submit()` block
  write.
- **Timing.** All of these are split transactions (our link acks them
  `pending`). Each must be answered within the split timeout, 100 ms by
  default. Our turnaround is an EP 0x84 read plus an EP 0x02 send, a few ms.
- **What the OHCI core handles itself.** Config ROM reads are served from
  `ConfigROMmap`. The IRM registers (`BANDWIDTH_AVAILABLE`,
  `CHANNELS_AVAILABLE`, `BUS_MANAGER_ID`) go through the CSR lock registers
  `0x0c`/`0x10`/`0x14` (`FUN_0002e940`). This matters if our node becomes
  IRM: we set the contender bit.

### Config ROM

Controllers look for an AV/C unit directory (unit spec ID `0x00A02D`,
version `0x010001`). Our ROM's unit directory carries Pinnacle's own specifier
([startup.md](../startup.md#our-config-rom)). In virtual-deck mode,
`build_config_rom()` would publish the AV/C unit directory instead (CRCs
computed the vendor's way).

### AV/C target

The target would be a pure function: command frame in, response frame and
transport action out, with the deck state taken from the file position.

Commands the controllers send, taken from what Windows sent to the camera at
enumeration ([startup.md](../startup.md), "What was dropped"):

- UNIT INFO: answer VCR / tape recorder.
- SUBUNIT INFO, PLUG INFO.
- INPUT / OUTPUT PLUG SIGNAL FORMAT: DV SD 525-60 / 625-50, or `0x20`
  MPEG-TS for HDV.
- VCR TRANSPORT STATE, PLAY (forward, pause, slow, fast, reverse), WIND
  (FF, REW, stop), TIME CODE status, MEDIUM INFO, ABSOLUTE TRACK NUMBER
  status.
- RECORD: REJECTED.
- Vendor-dependent commands: NOT_IMPLEMENTED.

**A file seeks instantly.** Unlike both tested decks, the virtual deck can
implement **TIME CODE search** (`00 20 51 20 …`). FF/REW can step through the
file at a few times speed, sending timecode only (no stream), as the
camcorders do, or sending decimated frames.

### Choices for later

- **Who connects.** A local "play" (no controller) has to make the
  connection itself: allocate a channel and bandwidth at the IRM
  (`FUN_0003f850` shows the vendor's locks on `0x220`/`0x224`/`0x228`), set
  oPCR's broadcast count, and transmit. A controller connects by locking our
  oPCR.
- **SYT and source-packet timestamps** need the bus cycle time. Two ways:
  - Read `IsochronousCycleTimer` (`0xF0`, type 3) and add a margin.
  - Start with cycleMatch at a known cycle. That is standard OHCI, but the
    vendor does not use it, so it is untested on this FPGA.

  The `OUTPUT_LAST` status writeback timestamps give the true send cycle,
  if the FPGA forwards them on EP 0x84 as it does for the AT contexts. That
  would let the host correct drift.

## Analog

`MarvinAVS64.sys` `FUN_00019588` sets the capability word per PID. Nibble 1
selects the video **encoder** object:

- It is built by `FUN_00027680`, at I2C `0x54`.
- It holds subcarrier constants `0x167cf021` / `0xc68a092a`.

That nibble is **0 for 0213 (500-USB) and 0223 (510-USB)** (`0x101`), and 1
for 0206, 0212 and 0224 (`0x111`). So **the 500-USB and 510-USB have no
analog output**, and analog playout is possible at most on the 700-USB,
710-USB and MovieBox Deluxe.

Analog playout there needs:

- the Render bitstream. `scripts/extract-bitstreams.py` extracts it from this
  installer's `MarvinAVS64.sys` (checked; it is not in `firmware/`).
- the encoder's register setup.
- the render stream format. Alt 2 has only `0x01/0x81/0x02/0x06`, so the
  stream is probably on EP 0x06 under the Render design.
- the audio path: AC'97, plus `PlaybackAudioLevel` / `OutputAudioLevel*`.

That is a separate reverse-engineering job, starting at the
`AllocatedBuffersForRender*` / `MaxPendingRender*` users in
`MarvinAVS64.sys` and at the `marvinavrenderer.ax` DirectShow filter. No model
that has an output is available to test.

## What can be done without the device

Everything except the last step is pure code with ctest coverage, following
the core-first rule (`src/engine` + `pin_api.h`, GUIs only call it):

1. **IEC 61883 packetizers** (engine):
   - **DV** (61883-2): DBS `0x78`, FMT 0, FDF `0x00`/`0x80`, DBC, empty-packet
     pacing (PAL 300 data packets / 320 cycles per frame; NTSC 29.97 fractional
     pattern), SYT.
   - **HDV** (61883-4): 192-byte source packets, SPH timestamps, rate from
     the PCR.
   - **Loopback test.** Wrap generated packets in the IR buffer-fill format
     and feed them through the existing `dv_reassembler` / HDV demux. The
     output must equal the input file byte for byte. This reuses the capture
     path as the checker.
2. **EP 0x06 stream builder** (core): the exact message sequence above (ring,
   descriptors, vendor sequence bits, type-11 waits, in-band wake and start).
   Golden-vector tests built from the decompilation.
3. **Plug registers and AR-request answering**: read and lock responses, FCP
   command reception. Unit-test the request parsing and the response
   headers.
4. **AV/C target**: table-driven tests per command, including the
   NOT_IMPLEMENTED / REJECTED cases and timecode search.
5. **Config ROM** with the AV/C unit directory: CRC tests against the
   existing builder.
6. **File sources**:
   - raw `.dv`, DV-AVI type 1/2, `.m2t`/`.ts`.
   - MOV/MKV via the vendored FFmpeg.
   - Timecode from DV subcode / HDV aux (`dv_subcode.c` / `hdv_aux.c` already
     parse them).
7. **API and CLI**: `pin_playout_*` in `pin_api.h`; `MarvinCaptureCLI
   --playout FILE [--virtual-deck]`. GUIs after hardware validation.
8. **Optional: a transport seam** in `pinnacle_1394.c` so a fake device can
   answer EP 0x02/0x84/0x06 in tests. Today the code calls libusb directly.

**What can't be checked here:**

- whether the FPGA's IT context and EP 0x06 behave as the vendor code implies;
- the meaning of type 11;
- the real timing and underrun behaviour;
- acceptance by real controllers.

## Hardware test plan (later)

1. **Transmit only.** Use a DV camcorder in VCR / DV-in mode as the receiver,
   with the CLI playing a short `.dv`. The camcorder showing the picture
   proves EP 0x06, IT and the CIP framing. Watch the descriptor-status
   writebacks on EP 0x84 with `--debug`.
2. **Recording to tape.** Send AV/C RECORD to the camcorder while playing out
   ("export to tape").
3. **Virtual deck with two Marvins.** One box plays the virtual deck; the
   other runs our normal capture with deck control. That gives an end-to-end
   check of the AV/C target, plug connection, capture and frame comparison,
   with tools we already have. Then a PC with a FireWire port (Windows MSDV /
   WinDV, Linux dvgrab) as the controller.
4. **HDV**: the same steps with a `.m2t`; the Canon HDV camcorder as receiver.

## Reproducing the disassembly

The installer is an InstallShield `Setup.exe`. Its streams are obfuscated:
each byte is `~rot4(b) ^ name[j] ^ seed[j % 4]`, with `j = (i % 1024) %
len(name)` and seed `13 35 86 07`. That gives the MSI, which contains
`Data1.cab`, which contains `marvinbus64.sys`, `marvinavs64.cab`
(`MarvinAVS64.sys`, `MarvinUsb.ax`) and `marvinavrenderer.ax`.

- Ghidra 12.2 needs JDK 25.
- The `.sys` files are byte-identical in role to the ones the other docs
  cite. The `FUN_` addresses there (for example `FUN_0002cd00`,
  `FUN_0002c280`) match.
- The vendor files are Pinnacle's and are not committed.
