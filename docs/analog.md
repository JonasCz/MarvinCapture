# Analog capture (composite / S-video)

Status as of **2026-09-25**: PAL composite capture works with `pinanalog`,
from a cold device or switched over from DV without a replug. Output is an
AVI with uncompressed YUY2 video and 48 kHz stereo PCM. NTSC, S-video and
the other colour standards are implemented from the vendor driver's tables
but have not been tested against a real source yet.

The background notes that led here are in [analog-notes.md](analog-notes.md).

**Sources**, as before:

- `MarvinAVS64.sys` from the user's own driver install, decompiled with
  Ghidra on the capture host and never committed. `FUN_xxxxx` names below
  are Ghidra's, at the addresses of that file.
- `traces/20260925-154656-analog-plug-virtualdub-preview.pcapng`, a usbmon
  trace of the vendor driver doing a VirtualDub preview: plug-in, OHCI
  bitstream, switch to the Capture bitstream, 40 s of PAL preview. usbmon
  lost most of the EP 0x82 video payload (the host's IOMMU merges the
  scatter-gather buffers of big URBs, and usbmon cannot read those), but
  the headers, the audio and every control exchange are complete.

## Quick start

```bash
python3 tools/extract-bitstreams.py marvin/marvinavs64.sys ~/bitstreams   # once
sudo ./build/pinanalog -b ~/bitstreams/fpga-capture.bin -o out.avi -t 60
sudo ./build/pinanalog -b ~/bitstreams/fpga-capture.bin --status        # signal check
```

Options: `-i composite|svideo`, `-s auto|pal|ntsc|pal-m|pal-n|pal-60|ntsc-443|ntsc-j|secam`
(`auto`, the default, picks PAL or NTSC from what the decoder sees),
`--brightness/--contrast/--saturation/--hue/--sharpness`, `--tv-mode`.

The progress line counts **missing**, **truncated** and **audio missing**
frames. A clean capture shows zeros for all three, and `pinanalog` exits
with status 4 if any is non-zero. `tools/analogcheck.py` checks a raw dump
(`--raw FILE`) independently.

## How analog differs from DV

Analog is a different FPGA design, not a mode of the DV one. The vendor
driver switches designs by reloading the FPGA:

```
alt 0  ->  "05 00" -> "05 01"  ->  Capture bitstream on EP 0x02 (78,422 B)
       ->  ~1 s   ->  "06 00" -> "06 01"  ->  alt 3
```

That is all a switch from DV to analog takes. No replug, no power cycle.
Tested both ways: `pinanalog` after `pincli`, and `pincli` after
`pinanalog` (its normal bring-up reloads OHCI and the 1394 link comes up).
The trace shows the vendor doing exactly this when VirtualDub opens the
device, which had come up in OHCI (DV) mode at plug-in. `pinnacle_fpga_load()`
implements it. `pinnacle_analog_open()` first asks "06 00" whether any
design is running. If none is (a cold device), it runs the power-up the
vendor sends at plug-in before its first bitstream.

After the Capture bitstream there is no EP 0x02 traffic at all. Everything
is I2C over the config channel:

| I2C address | what |
|---|---|
| `0x4a` | Philips SAA7113H video decoder |
| `0xf0` | the FPGA's capture block. It exists only with the Capture bitstream: probing it with "03 f0" answers `03` under OHCI and `cb` under Capture. |

Streams, alt setting 3:

| EP | what |
|---|---|
| 0x82 IN | video |
| 0x86 IN | audio |

## The config channel, decoded

Every request gets one reply that starts with the opcode. The builders are
in the transport object (vtable `0x421a0`) and the I2C bus object (vtable
`0x42c70`).

| request | reply | meaning | where |
|---|---|---|---|
| `01 a n sub data..` | `01 01 08` | I2C write to address `a`, `n` bytes | `FUN_0001fb28` |
| `02 a nr nw sub` | `02 01 data..` | I2C read (`01` = acknowledged) | `FUN_0001fbdc` |
| `03 a`, `04 a` | `03 xx`, `04 xx` | reset the chip at `a` (assert, release). The vendor waits 50 ms after resetting `f0` | `FUN_0002c7f4/85c`, `FUN_0001c060` |
| `05 00` | `05 01` | FPGA loader ready | `FUN_0002c280` |
| `06 00` | `06 01` | FPGA design running | same |
| `07 00` | `07 01` | capability probe at start-up | driver init |
| `08 00` | `08` | sent before the streams are (re)started | `FUN_0002c92c` |
| `09 00` | 13 bytes | a u64 time (x10) plus a u32; not used by us | `FUN_0002c98c` |
| `0c 01` | `0c 01` | power-up. For PIDs 0212/0213/0223/0224 the driver waits 1 s if the reply is 1 | `FUN_0002cabc` |
| `0f x` | | not seen in our traces | `FUN_0002c8c4` |
| `80 idx 08` | 20 bytes | read 8 bytes of configuration memory | [startup.md](startup.md) |

## Capture block registers (I2C `0xf0`)

From the capture object (vtable `0x42360` for PID 0213; its constructor
`FUN_0001c2ac` stores the address `0xf0`). The driver writes these through
a shadow copy: `vtable+0xa0` writes, `+0xa8` reads, and `+0xb0` does
read-modify-write as `(shadow & ~mask) | value`.

| reg | meaning | source |
|---|---|---|
| 0 | Video format. bit6 = 525-line frame (NTSC). bits 7/2/1 = horizontal scaling (720: none, 704: `80`, 480: `04`, 360: `02`, 352: `82`). bits 4:3 = field selection for half-height capture (from `dwInterlaceFlags` of the requested VIDEOINFOHEADER2). PAL 720x576 is `00`. | `FUN_0001aec4` |
| 1 | Control. bit0 RUN, bit1 video, bit2 audio. bit7 (read) = busy, polled after clearing RUN. bit5 is set once while the codec is first programmed (inferred from the trace; its writer was not found). | `FUN_0001b218`, `FUN_0001bf7c`, `FUN_0001bfbc` |
| 2, 3, 4 | AC'97 register index, data low, data high | `FUN_0002f0a4` |
| 5 | AC'97 command: bit0 write, bit1 busy (poll until clear, at most 5 reads). bit7 turns the audio packets on and is set together with the format. | same, `FUN_0001aec4` |
| 6, 7 | Audio packet length in bytes, 12-byte header included (u16 LE). PAL: `0x1e0c` = 7692 = 12 + 1920 x 4. | `FUN_0001aec4`, `FUN_000335c4` |

Start order (vendor trace, `pinnacle_analog_start`): reset the block, `08`,
format, control 0, packet length, reg5 |= 0x80, then control |= audio,
|= video, `08`, |= RUN. Stop: clear video and audio, clear RUN, then poll
reg 1 until bit 7 clears.

## SAA7113 (I2C `0x4a`)

The decoder object has vtable `0x431d0` (constructor `FUN_00039aac`).

- **Init table** (`FUN_00025bc4`): byte-identical to the trace.
  `pinnacle_analog.c` has it as `saa7113_init[]`.
- **Input** (`FUN_00039fd0`), reg 0x02 with FUSE = 11:

  | vendor input id | reg 0x02 | reg 0x09 bit7 (BYPS) | on the 500-USB |
  |---|---|---|---|
  | 1 | `c0` (AI11 CVBS) | 0 | composite (confirmed by the trace) |
  | 10 | `c9` (Y on AI12, C on AI22) | 1, chroma trap bypassed | S-video |
  | 30 | `c4` (AI23) | 0 | other models |
  | 40 | `c8` | 0 | other models |

- **Standard** (`FUN_00025fe8`), keyed by `KS_AnalogVideoStandard`:

  | | 0x0e | 0x0f | 0x10 | 0x08 FSEL | 0x40 | 0x5a |
  |---|---|---|---|---|---|---|
  | 50 Hz base (PAL B/D/G/H/I) | `81` | `2a` | `00` | 0 | `02` | `07` |
  | 60 Hz base (NTSC-M) | `89` | `2a` | `40` | 1 | `82` | `0a` |
  | PAL-N | `ad` | | | | | |
  | NTSC-4.43 | `ad` | | | | | |
  | PAL-M | `bd` | | | | | |
  | PAL-60 | `95` | | | | | |
  | NTSC-J | `cd` | | | | | |
  | SECAM | `50` | `80` | | | | |

- **Picture**: brightness = reg 0x0a (`FUN_0003a230`), contrast = 0x0b
  (`FUN_0003a2bc`), saturation = 0x0c (`FUN_0003a34c`), hue = 0x0d, NTSC
  only (`FUN_0003a3e8`), sharpness = reg 0x09 bits 1:0 (`FUN_0003a4a0`).
  The vendor maps its signed user values onto these; `pinanalog` takes the
  register values directly.
- **VCR mode** (`FUN_0003a1c8`): reg 0x08 bits 4:3, `01` = VTR timing (the
  vendor's default, suited to tape) or `11` = fast locking (`--tv-mode`).
- **Output enable** (`FUN_0003a178`): reg 0x11 bit3 (OEYC), set to `0c`.
- **Status and detection** (`FUN_000263d4`, `FUN_0003a53c`): reg 0x1f.
  bit6 HLVLN = no horizontal lock, bit5 FIDT = 60 Hz, bit7 = interlaced.
  The vendor picks NTSC-M or PAL-B from FIDT. `pinanalog -s auto` does
  the same.
- The vendor also writes register `0x3a = 00`, which is not an SAA7113
  register. We skip it.

## AC'97 codec (through the capture block)

The capture block bridges an AC'97 codec. The vendor sets it up as
follows (`pinnacle_analog.c`, `ac97_init`):

| reg | value | |
|---|---|---|
| 04, 06, 0a, 18 | `8000` | aux/headphone, mono, beep, PCM out: muted |
| 1a | `0505` | record source: stereo mix |
| 1c | `0000` | record gain 0 dB |
| 2a | `0001` | variable-rate audio on |
| 10 | `0808` | line in 0 dB, unmuted (the camera's audio) |
| 02 | `0000` | master 0 dB |
| 2c, 32 | `bb80` | DAC and ADC at 48,000 Hz |

## Stream format

Both endpoints use the same 12-byte header:

```
ff 00 <counter u16 LE> <device time u64 LE>
```

The time is a 64-bit count of ~10 MHz ticks, zeroed when the capture block is
reset. Its upper four bytes are 00 for the first 2^32 ticks, which is **429.5 s**
(10,737 frames); after that byte 8 becomes 01 and counting continues. A parser
that insists on 00 there loses every video header at that point (audio, which
only checks `ff 00`, carries on), and the capture silently ends up with
audio and no video. Found with a raw dump of a 520 s capture: the device
never stops, and the headers just change from `... 00 00 00 00` to
`... 01 00 00 00`.

- **Video, EP 0x82**: one header, then one frame as **YUYV 4:2:2**,
  720 x 576 x 2 = 829,440 bytes, sent **field by field**: all 288 lines of
  the first field, then all 288 of the second. The first field is the top
  one and the first in time, so it goes on the even lines (0, 2, ...).
  A weave of a still picture that way has 40% less line-to-line comb
  energy than the other way round, and the result looks right. The
  vendor's capture pin weaves too: it sets `fields = (height > 288) + 1`
  and a destination stride of two lines, and its render pin does the
  reverse conversion. `pinnacle_analog.c` weaves as the bytes arrive. The
  frame ends with a short USB packet (829,452 is not a multiple of 512).
  Black is `10 80`. Pixel data never contains `ff` (BT.656 reserves it), so
  a header cannot be confused with pixels.
- **Audio, EP 0x86**: one packet per transfer, 1920 stereo 16-bit LE
  samples after the header.
- Both counters start at 1 on RUN. Audio packet N holds the samples taken
  during video frame N: its timestamp is 680 ticks (68 us) after the
  frame's, every time.

## Clocks, frame drops and A/V sync

Measured over 10-40 s runs (`tools/analogcheck.py`) and over the 41 s of
the vendor trace, where the numbers were identical:

- Device ticks per video frame: **400,043.55**, with a residual under 1
  tick over the whole run.
- Device ticks per audio packet: **400,043.55**, the same.
- So there are exactly **1920.0000 audio samples per video frame**.
- Against the host clock: 24.9999 fps and 47,999.8 Hz. The device's own
  "10 MHz" counter runs at about 10.0010 MHz.

What that means:

1. **The frame rate is the source's.** The SAA7113 derives its clock from
   the incoming line sync, and the device passes every frame through. It
   does not retime to a crystal, so it does not drop or repeat frames to
   match one. A camera that runs 100 ppm fast yields frames 100 ppm fast.
2. **Audio is locked to video.** The number of samples per frame is fixed
   (the FPGA clocks the codec's sample timing from the video, or resamples
   into it; either way it is exact). Audio cannot drift against video, and
   no resampling is needed on our side.
3. **The only way to lose sync is to lose packets**, and both counters
   show it. `pinnacle_analog_capture_loop` keeps the output timeline whole:
   - A missing frame becomes a repeat of the previous one, and its audio
     becomes 1920 samples of silence.
   - A short frame keeps the previous frame's pixels below the point where
     its data stopped.
   Every repair is counted. The file therefore always has exactly one
   frame and 1920 samples per source frame, and a player at 25 fps plays
   it back in sync.

What the tool does not do is retime to a wall clock. That is deliberate:
inserting or dropping frames to match the host's clock would create the
very duplicates and drops we want to avoid. If the source's rate is
slightly off, the file is slightly longer or shorter than the wall-clock
time; the same is true of DV.

**No signal, no data.** With nothing on the selected input, the device
sends neither video nor audio: the audio packets are paced by the video
frames. A blank or unlockable stretch of tape therefore simply does not
appear in the file, as with DV. Still open: what exactly happens at the
moment the signal drops or returns mid-capture. The counters will show
it either way.

## USB transfer size matters (a lot)

The device buffers very little at 20.7 MB/s. When the host is late, the
FPGA drops the rest of the frame: data stops for 15-35 ms, the frame still
ends on time with its short packet, and the counters stay continuous. The
kernel had 255 of 256 transfers queued the whole time, so queue depth was
not the issue. The size of each transfer was:

| transfer size | result (Intel xHCI, IOMMU on) |
|---|---|
| 64 KiB and up | ~20% of the data lost, a third of all frames short |
| 32 KiB | a few percent short |
| 20 KiB (the vendor's URB size) | 3-9 short frames per 1000 |
| 16 KiB | 3-8 per 1000 |
| **8 KiB**, 4 KiB | **none** |

Default: 512 transfers of 8 KiB (200 ms queued). A 3-minute capture with
that setting had 4,500 frames with 0 missing, 0 truncated and 0 audio
gaps, and the 3.7 GB AVI (4 RIFF segments) decodes without an error. usbfs turns anything over
16 KiB into a scatter-gather list, and large buffers need several TRBs; the
per-transfer cost of those seems to be what opens the gaps. A second
thread writes the file, so disk stalls never reach the USB thread.

## Output file

OpenDML AVI (`src/core/avi_writer.c`): `YUY2` video and PCM audio,
interleaved one frame and its audio block at a time. RIFF segments stay
under 1 GiB, and the first also has a legacy `idx1`. A `vprp` header gives
the 4:3 frame aspect (ffprobe shows SAR 16:15). AVI cannot record field
order: the video is interlaced, **top field first**, so tell the encoder,
e.g. `ffmpeg -i in.avi -vf setfield=tff,bwdif ...` or
`-field_order tt` for an interlaced encode.

## Not done yet

- NTSC, S-video and the other standards are untested. NTSC's audio packet
  size is a guess (1600 samples; the vendor computes it in `FUN_000335c4`
  from fields we did not trace). Since audio is counted in samples, not
  packets, a different size only changes latency.
- Hardware downscaling (reg 0 bits 7/2/1) and single-field capture (bits
  4:3) are decoded but not exposed.
- Audio input level (the codec's line-in gain, reg 0x10) is fixed at 0 dB.
- Loop-through to the analog outputs (`LoopThrough`) and analog output
  (Render bitstream) are not implemented.
