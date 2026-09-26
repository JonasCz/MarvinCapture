# Analog capture (composite / S-video)

Status as of **2026-09-25**: PAL composite capture works with `pinanalog`,
from a cold device or switched over from DV without a replug. Output is an
AVI with uncompressed YUY2 video and 48 kHz stereo PCM. NTSC, S-video and
the other colour standards are implemented from the vendor driver's tables
but have not been tested against a real source yet.

How the vendor driver, the other Marvin models and the three FPGA
bitstreams fit together is in the last section.

**Sources**, as before:

- `MarvinAVS64.sys` from the vendor driver install, decompiled with Ghidra
  (not in this repo). `FUN_xxxxx` names below are Ghidra's, at the addresses
  of that file.
- A usbmon trace of the vendor driver doing a VirtualDub preview (not kept
  in the repo): plug-in, OHCI
  bitstream, switch to the Capture bitstream, 40 s of PAL preview. usbmon
  lost most of the EP 0x82 video payload (the host's IOMMU merges the
  scatter-gather buffers of big URBs, and usbmon cannot read those), but
  the headers, the audio and every control exchange are complete.

## Quick start

```bash
pinanalog -o out.avi -t 60           # uses firmware/fpga-capture.bin
pinanalog --status                   # signal check
```

Options: `-i composite|svideo`, `-s auto|pal|ntsc|pal-m|pal-n|pal-60|ntsc-443|ntsc-j|secam`
(`auto`, the default, picks PAL or NTSC from what the decoder sees),
`--brightness/--contrast/--saturation/--hue/--sharpness`, `--tv-mode`.

The progress line counts **missing**, **truncated** and **audio missing**
frames. A clean capture shows zeros for all three, and `pinanalog` exits
with status 4 if any is non-zero.

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

Measured over 10-40 s runs and over the 41 s of
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

---

## Background: the vendor driver, other Marvin models, the three bitstreams

What turned up in the vendor driver (`MarvinBus64.sys`, `MarvinAVS64.sys`,
unpacked from the vendor MSI and read in Ghidra; none of that is in this
repo) while this driver was being written.

**Confidence labels:** **code** = read directly from the vendor driver;
**inferred** = derived from nearby code plus general knowledge; **guess** =
plausible, no direct evidence.

### Driver stack

| file | what it is |
|---|---|
| `marvinavs64.cab` → **`MarvinAVS64.sys`** | The USB function driver for all Marvin models. It is an AVStream capture filter plus a crossbar filter, it talks to the USB stack itself, and it owns the FPGA bitstream load and (presumably) the config channel. **This is where analog lives.** |
| `MarvinUsb.ax` | the DirectShow proxy / property pages for it |
| `marvinbus64.sys` | A virtual 1394 bus (`root\MarvinBus`) layered on top, for the DV/HDV path. It contains no analog, AV/C or I2C code (**code**, grepped exhaustively). |
| `bender64.sys` / `pclebend64.inf` | Pinnacle 1394 support, not analysed. `\Device\Pcle1394` is referenced by `MarvinAVS64.sys`. |
| `dvc64.cab` → `em*64.sys` | **Not ours.** An Empia EM2821 driver for the Dazzle DVC100 (`USB\VID_2304&PID_021A`), shipped in the same MSI. |

### Models (from `marvinavs64.inf`)

| PID | internal name | product |
|---|---|---|
| 0206 | Marvin-classic | MovieBox Deluxe |
| 0212 | Marvin-CR | **700-USB** (more inputs and outputs) |
| 0213 | Marvin-Lite | **500-USB** (this project) |
| 0223 | Marvin-510 | 510-USB |
| 0224 | Marvin-710 | 710-USB |

Differences visible in code:

- **Decoder I2C address.** The video decoder is at I2C `0x4a` on 0213 and
  the later models, and at `0x48` only on 0206 (**code**, in a crossbar
  object constructor in `MarvinAVS64.sys`).
- **Chip objects are data-driven.** A per-device capability word selects
  which chip objects get instantiated. It has three nibbles: decoder type,
  and two other categories. **code**, structure only; the values were not
  decoded.
- **FX2 firmware file per model.** Each model family has its own:
  `Marvin_000.bix`, `MarvinCR_000.bix`, `MarvinPro_000.bix`, overridable via
  the registry values `FileMarvinFX2`, `FileMarvinFX2CR` and
  `FileMarvinFX2Pro` (**code**, `FUN_0002bf8c`).
  - Our device never has FX2 firmware uploaded, so the 500-USB presumably
    boots its FX2 from EEPROM. Worth rechecking on other models.
- **Unit-specific identity** comes from the device's configuration memory,
  via config-channel reads `80 <index> 08`. Index 3 is the 1394 GUID and
  index 0 is another 8-byte ID ([startup.md](startup.md)). Anything that
  identifies a unit, and possibly its revision, should come from there
  rather than from constants.
- **PHY ports.** On the 500-USB, `MarvinBus64` disables PHY ports 1 and 2
  (only port 0 is wired). A 700-USB may wire more; do not assume.

**For future multi-model support:** keep PID-dependent choices in one table
(decoder I2C address, FX2 firmware, which inputs exist, PHY ports), keyed by
PID and possibly by the status and ID reads above.

### Three FPGA bitstreams: the key to analog

`MarvinAVS64.sys` (`FUN_0002c280`) loads one of **three different
78,422-byte bitstreams**, depending on what the device is to do (**code**).
Each one is embedded in the driver and can be overridden by a file in
`system32\drivers` named by a registry value.

| mode | embedded at (VA) | MD5 | registry override |
|---|---|---|---|
| OHCI (1394, DV/HDV) | `0x4aa70` | `3888c23c…` = **identical to the bitstream pincli uploads** | `FileFloydOHCI` |
| Render (output to TV / analog out) | `0x5dcd0` | `cacaa36d…` | `FileFloydRender` |
| Capture (analog in) | `0x70f30` | `280bacc6…` | `FileFloydCapture` |

("Floyd" appears to be the FPGA's name.)

- The Render and Capture blobs differ from the OHCI one in about 41 KB and
  45 KB respectively, so they are genuinely different designs, not
  variants.
- Before any bitstream is loaded, the driver requires the config-channel
  `05 00` → `05 01` ready reply (**code**).
- `scripts/extract-bitstreams.py MarvinAVS64.sys outdir/` writes all three
  from a copy of the vendor driver, checking each MD5. The OHCI and Capture
  ones ship in `firmware/` (see its README for the licence caveat).

**Consequence:** analog capture is **not** a mode switch inside the running
DV design. The device is re-initialised with the Capture bitstream. The
FPGA then presumably exposes a completely different register map and
stream, and **none of the OHCI/1394 machinery applies**. The "other hardware
back-end" in MarvinBus64 (register names such as `Isoch rx fw Config` and
`USB ep flush`) belongs to a different generation of Marvin FPGA, not to
the analog mode.

### Mode and analog settings in `MarvinAVS64.sys` (registry-backed properties)

| name | what we know |
|---|---|
| `CaptureVideoSource`, `CaptureVideoStandard`, `CaptureAudioSource`, `ColorModeCapture` | Named, persisted properties. Not yet traced to bytes on the wire. The INF exposes only one crossbar pin ("Analog Audio In"), so **video input selection (composite / S-video) is most likely a SAA7113 register setting**: register 0x02, input mode AI11..AI24 (**inferred**). |
| `LoopThrough` / `UseNativeMode` | An analog pass-through switch. It cross-connects the video decoder object to the video encoder object (monitoring on a TV) and matches the encoder's video standard to what the decoder detected (**code**, at vtable-call level). |
| `VcrMode` | Restored together with the decoder's picture settings. Probably the SAA7113 VCR/"VTR" timing mode for tape sources (**guess**). |
| `DenyLoading1394BusDriver` | An on/off flag. Probably suppresses the virtual 1394 bus, i.e. forces analog-only use (**guess**). |
| `MARVIN_VIDEODECODER`, `MARVIN_VIDEOENCODER`, `MARVIN_AC97_AUDIO`, `MARVIN_ASIC_AUDIO`, `MARVIN_PRO_AUDIO` | Keys under which chip-object settings are stored. So there is an AC'97 codec, and/or audio handled in the FPGA ("ASIC"), plus a video encoder for the outputs. |
| `CaptureAudio*`, `OutputAudioLevel*`, `PlaybackAudioLevel`, `SampleCorrectionNtsc44K`, `MaxPendingCapture*/Render*`, `MinBuffers*TillStart`, `OverrideBlankingArea*` | Audio and streaming tunables. |
