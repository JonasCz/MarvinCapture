# Protocol: the command channel, OHCI, and EP 0x88 framing

How the host talks to the device once the FPGA bitstream is loaded. Start with
[hardware.md](hardware.md) for the endpoints. How the 1394 link is brought up
step by step is in [startup.md](startup.md), AV/C deck control in
[deck-control.md](deck-control.md), and how to run and verify a capture in
[usage.md](usage.md).

## The device is an OHCI-1394 controller

Disassembling `MarvinBus64.sys` (Ghidra) showed the driver carries **two**
complete hardware back-ends, selected at runtime from a 47-entry op table at VA
`0x4df50`. Our device uses the second one, which is a **standard OHCI-1394 host
controller implemented in the FPGA**. The FPGA register table in the binary's
strings ("Isoch rx timeout", "Event mask", "Throttle Isoch RX", ...) belongs to
the *other* back-end and is dead code on this hardware.

Confirmation is byte-exact: `FUN_0002cd00` contains a literal 13-entry
{register, value} init table
(`{0xe4,0xffffffff},{0x104,0xffffffff},...,{0x840,0x10000020}`) that is, in
order, exactly one of the packets the vendor driver sends.

### The command word

Everything on EP 0x02 / EP 0x84 / EP 0x88 is a stream of messages sharing
one little-endian `u32` header:

```
bits 31:28  type
bit  27     reply wanted; bits 26:20 = 7-bit tag echoed in the reply
bits 19:0   register address      (register messages)
bits 26:16  payload byte length   (payload messages)
bits 15:0   device RAM address    (payload messages)
```

| type | meaning |
|------|---------|
| 2 | OHCI register write — address is `0x10000 + reg_offset`, value follows |
| 3 | OHCI register read — same addressing, reply carries the value |
| 4 | FPGA USB-side register write (indexed; the driver keeps a shadow copy). Index 0 = EP 0x84 idle time / isochronous-to-USB gate; see [startup.md](startup.md). |
| 5 | read of that bank (reply comes back as type 5) |
| 6 | FPGA extension-bank write (OHCI-style offsets, separate register file) |
| 8 | block write to device RAM (`len` bytes follow) |
| 9 | block read / device→host data (`len` bytes follow) |
| 10 | OHCI `IntEvent` report |

So the sequences we had been replaying blind are readable now. For example
`00 04 01 20 | 00 90 00 00` is "write OHCI `0x400`
(IsoRecvContextControlSet) = `0x9000`", i.e. `run|wake` — literally *start
isochronous receive*. The stop sequence's `04 04 01 20 | 00 80 00 00`
clears `run`. `0c 04 01 20 | 81 11 80 00` sets `IsoRecvCommandPtr` to
descriptor `0x801180` with Z=1.

### Isochronous receive is a closed 2-descriptor ring

`REQUEST_ISOCH_LISTEN` builds two descriptors that both fill the same
36,864-byte buffer, with the last branching back to the first:

```
desc @0x801180 = 280C9000 00807000 00801191 00009000
desc @0x801190 = 280C9000 00807000 00801181 00009000   <- branches back
0x40C IR0.CmdPtr  = 0x00801181
0x400 IR0.CtrlSet = 0x00009000   (run|wake)
```

It is a "listen forever" switch — the host never re-attaches buffers. That
explains the earlier observation that the vendor driver sends **zero**
commands during steady streaming. There is no keepalive, no timer, and no
watchdog anywhere in the binary (`KeSetTimer` does not appear at all).

## EP 0x88 has two layers of framing

EP 0x88 is not raw DV. It carries the *same* type-9 message framing as the
command channel, wrapping the OHCI isochronous-receive DMA ring:

1. **Type-9 messages**: `u32 = (9<<28) | flags | (len<<16) | ram_addr`,
   then `len` bytes. `ram_addr` `0x7000`–`0xFFFF` is the isoch ring;
   `0x118C` / `0x119C` are IR descriptor status writebacks carrying no
   stream data. **Message lengths vary and are not aligned to anything in
   the payload**, so a 4-byte header routinely lands in the middle of a DIF
   block. Treating the stream as raw DV therefore produces misaligned video, not damaged video.
2. **Inside the ring**, OHCI buffer-fill records:
   `[isoch header u32][payload, quadlet-padded][trailer u32]`, where the
   isoch header is `dataLength<<16 | tag<<14 | chan<<8 | tcode<<4 | sy` and
   the payload is an IEC 61883 packet: 8-byte CIP header
   (`01 78 00 <DBC> 80 00 <SYT>`, DBS=0x78=480 bytes, FMT=0 DVCR) followed
   by 480 bytes of DIF — or nothing at all for the empty packets DV sends
   to pad its rate to the 8 kHz cycle clock.

Stripping both layers yields a byte-exact DIF stream. Verified over a
16 MB slice of a live capture: **1306 sequence headers, every stride
exactly 12,000 bytes, 0/195,750 wrong block SCTs, 0 wrong Dseq, 0 wrong
DBN, 0 video blocks with a non-zero STA**, and every packet's OHCI
`xferStatus` is `0x8411` (`run|active|ack_complete`) — no overruns, no
errors. End to end, a 128 MB / 1073-frame capture decodes with **zero**
ffmpeg errors, versus tens of thousands before.

This is implemented in `dv_reassembler.c` as three stacked layers
(message demux → buffer-fill records → DIF sequence/frame assembly), each
resynchronising independently.

## Behaviours that matter

**The config channel must be brought up first.** After the bitstream upload and
alt setting 1, the command channel (EP 0x02) accepts only 2 writes and then NAKs
forever, unless the low-level config channel (EP 0x01/0x81) has first been
through the vendor's bring-up: 80 request/reply exchanges between
`SET_INTERFACE` alt 0 and the bitstream upload (mostly SAA7113 register writes
plus two 20-byte EEPROM/serial reads), and one after it. That sequence flips the
`05`/`06` status reads from "not ready" to "ready". It is still replayed
verbatim from `protocol_data.h` (`PINNACLE_CONFIG_PREBITSTREAM_SEQ` /
`_POSTBITSTREAM_SEQ`), even for pure DV capture.

**The command channel blocks while EP 0x88 has an unread backlog.** The stop
sequence used to fail on packet 3 of 4 (the OHCI read of `0x404`, straight after
clearing `run`) because the read loop had been torn down while DV was still in
flight. Anything written to EP 0x02 while the receive context runs must keep
reading EP 0x88 too; the stop sequence does.

**A quiet camera looks exactly like a wedged device.** The `05`/`06` status reads
around the bitstream are checked (`PINNACLE_ERR_NOT_READY`, "needs a physical
power cycle"), and `IR0.ContextControl` read back with `run` and `active` set
and `dead` clear says the receiver is fine. See [usage.md](usage.md).

**Receive must be queued, not synchronous.** With one synchronous
`libusb_bulk_transfer()` at a time the endpoint is unserviced between transfers,
the FX2's FIFO backs up and ~0.05% of data blocks are lost. The vendor driver
keeps ~80 URBs outstanding. The read loop is a ring of asynchronous transfers,
each resubmitted from its completion callback and consumed strictly in
submission order; the EP 0x84 status drain rides the same event loop so only one
thread calls `libusb_handle_events()`. Queue depth and disk-stall handling are
in [usage.md](usage.md).

**Nothing is sent during steady streaming.** The isochronous receive context is
a "listen forever" switch: no keepalive, no timer, no watchdog anywhere in the
vendor binary.

## Open items

- Decode the EP 0x84 event stream properly. Type-10 messages carry the OHCI
  `IntEvent` register (bit 17 selfIDComplete, 18 busReset, 23 cycleLost, 24
  cycleInconsistent, 26 cycleTooLong). That would turn most failures into a
  printed reason. A bus reset *during* capture is noticed by polling
  `SelfIDCount` and handled by restarting the stream (see hardware.md); reading
  the busReset bit from these records instead would notice it sooner.
- Timecode-based drop detection is not worth doing: the camera emits none in
  live view, and CIP/DBC continuity is finer-grained. Revisit only if capture
  from *tape* is added.
- The reassembler reports a small number of layer-2 resyncs per run (3 to 123,
  not correlated with duration). They are ring and descriptor boundaries, not
  loss: the same runs report zero DBC discontinuities.
