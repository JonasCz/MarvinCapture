# Analog path, mode switching and other Marvin models — notes for later (2026-09-25)

This is not implemented yet. The notes record what turned up while the 1394
start-up was being reverse engineered ([startup.md](startup.md)), so the
analog work does not start from zero.

**Sources:**

- The vendor MSI in this repo (`Pinnacle_Video_Driver_64bit.msi` →
  `Data1.cab`), unpacked with 7-Zip.
- Ghidra decompiles of `MarvinBus64.sys` and `MarvinAVS64.sys`, made on the
  capture host and never committed.

**Confidence labels:**

- **code**: read directly from the vendor driver.
- **inferred**: derived from nearby code, plus general knowledge.
- **guess**: plausible, but with no direct evidence.

## Driver stack

| file | what it is |
|---|---|
| `marvinavs64.cab` → **`MarvinAVS64.sys`** | The USB function driver for all Marvin models. It is an AVStream capture filter plus a crossbar filter, it talks to the USB stack itself, and it owns the FPGA bitstream load and (presumably) the config channel. **This is where analog lives.** |
| `MarvinUsb.ax` | the DirectShow proxy / property pages for it |
| `marvinbus64.sys` | A virtual 1394 bus (`root\MarvinBus`) layered on top, for the DV/HDV path. It contains no analog, AV/C or I2C code (**code**, grepped exhaustively). |
| `bender64.sys` / `pclebend64.inf` | Pinnacle 1394 support, not analysed. `\Device\Pcle1394` is referenced by `MarvinAVS64.sys`. |
| `dvc64.cab` → `em*64.sys` | **Not ours.** An Empia EM2821 driver for the Dazzle DVC100 (`USB\VID_2304&PID_021A`), shipped in the same MSI. |

## Models (from `marvinavs64.inf`)

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

## Three FPGA bitstreams: the key to analog

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
- `tools/extract-bitstreams.py MarvinAVS64.sys outdir/` writes all three
  from the user's own driver copy, checking each MD5. **Never commit or
  redistribute them.**

**Consequence:** analog capture is **not** a mode switch inside the running
DV design. The device is re-initialised with the Capture bitstream. The
FPGA then presumably exposes a completely different register map and
stream, and **none of the OHCI/1394 machinery applies**. The "other hardware
back-end" in MarvinBus64 (register names such as `Isoch rx fw Config` and
`USB ep flush`) belongs to a different generation of Marvin FPGA, not to
the analog mode.

## Mode and analog settings in `MarvinAVS64.sys` (registry-backed properties)

| name | what we know |
|---|---|
| `CaptureVideoSource`, `CaptureVideoStandard`, `CaptureAudioSource`, `ColorModeCapture` | Named, persisted properties. Not yet traced to bytes on the wire. The INF exposes only one crossbar pin ("Analog Audio In"), so **video input selection (composite / S-video) is most likely a SAA7113 register setting**: register 0x02, input mode AI11..AI24 (**inferred**). |
| `LoopThrough` / `UseNativeMode` | An analog pass-through switch. It cross-connects the video decoder object to the video encoder object (monitoring on a TV) and matches the encoder's video standard to what the decoder detected (**code**, at vtable-call level). |
| `VcrMode` | Restored together with the decoder's picture settings. Probably the SAA7113 VCR/"VTR" timing mode for tape sources (**guess**). |
| `DenyLoading1394BusDriver` | An on/off flag. Probably suppresses the virtual 1394 bus, i.e. forces analog-only use (**guess**). |
| `MARVIN_VIDEODECODER`, `MARVIN_VIDEOENCODER`, `MARVIN_AC97_AUDIO`, `MARVIN_ASIC_AUDIO`, `MARVIN_PRO_AUDIO` | Keys under which chip-object settings are stored. So there is an AC'97 codec, and/or audio handled in the FPGA ("ASIC"), plus a video encoder for the outputs. |
| `CaptureAudio*`, `OutputAudioLevel*`, `PlaybackAudioLevel`, `SampleCorrectionNtsc44K`, `MaxPendingCapture*/Render*`, `MinBuffers*TillStart`, `OverrideBlankingArea*` | Audio and streaming tunables. |

## The config channel (EP 0x01 / 0x81)

It is still a replay of 80 exchanges, `PINNACLE_CONFIG_PREBITSTREAM_SEQ`.
Known so far:

| request | meaning | confidence |
|---|---|---|
| `01 4a 02 REG VAL`-style | SAA7113 register writes (I2C address `0x4a`) | inferred from the bytes; the I2C address is in the code |
| `80 <index> 08 00…` | Read 8 bytes of configuration memory. The reply echoes 4 header bytes, then the data. | tested (GUID) |
| `05 00` → `05 01` | FPGA loader ready (checked before the bitstream) | code + trace |
| `06 00` → `06 01` | FPGA up (checked after the bitstream) | trace |
| `07`, `0c`, `03`, `04` | unknown | — |

**Open work, in likely order:**

1. Find the config-channel request builder in `MarvinAVS64.sys`, starting
   from the URB submitter `FUN_000207ec` and its callers. Then write down
   the opcode table.
2. Capture a USB trace of the vendor software doing **analog capture** and
   **analog output**. None of our traces do: they are all DV. The trace
   would show:
   - which bitstream is loaded;
   - the config-channel and I2C traffic for input and standard selection;
   - which endpoint carries the analog stream, and its format. The FPGA
     may deliver DV-encoded frames or raw video; it is unknown which.
3. Follow `CaptureVideoSource`/`Standard` in `MarvinAVS64.sys` down to their
   I2C writes, to check them against the trace.
4. Decode the SAA7113 part of the current config replay register by
   register (datasheet), so the analog init can be written out as named
   steps too.
