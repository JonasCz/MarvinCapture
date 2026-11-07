# MarvinPlayout: implementation plan (DV/HDV)

Written 2026-11-07, from [playout-feasibility.md](playout-feasibility.md). This is
the contract the modules were built against. Nothing has run on hardware yet:
everything that touches the device is a best guess from the decompiled vendor
driver and must be checked once a device is attached.

## What it is

`MarvinPlayout`, a separate command-line program next to `MarvinCaptureCLI`:

- **A virtual deck** that plays a "tape" made of one or more `.dv` or
  `.m2t`/`.ts` files, concatenated in the order given (each file is one
  recording; timecode comes from the files, or is made up where they have none).
- It answers **AV/C deck commands** from another node (PLAY, PAUSE, STOP,
  FF, REW, TIME CODE search, status queries), so a capture program on the
  other side can drive it like a real VCR.
- It can also **run its own actions**, with the same left-to-right syntax as
  MarvinCaptureCLI (`--rew --wait --play --wait timecode=00:01:00:00 --stop`),
  plus `--goto HH:MM:SS:FF` (timecode search). A receiver without deck control
  (a camcorder in DV-in mode) then just gets the stream.
- Transport actions **take time** like a real deck: mode changes take a
  moment, winding runs at a fixed multiple of real time, a timecode search
  winds to the target and then settles.
- Analog playout is not implemented, but the code is split so it fits later:
  the tape (source) and the output are separate interfaces; an analog output
  would load the Render bitstream instead of the OHCI one and take decoded
  frames from a decoding source.

## Code layout

Everything new lives in `src/playout/` and `tests/playout/`, so the existing
capture code is touched as little as possible. It reuses the engine's
hardware-free modules (`pin_tc_*`, `pin_script_cond_parse`, `pin_script_eval`,
`dv_subcode`, `hdv_aux`) and the core's 1394 layer (`pinnacle_1394.c`, with a
small hook for incoming requests).

| file | what | depends on |
|---|---|---|
| `pout_cip.[ch]` | IEC 61883-2 (DV) and 61883-4 (HDV) packetizer: frame in, one isochronous packet per bus cycle out (CIP header, DBC, SYT/SPH, empty packets) | — |
| `pout_tape.[ch]` | the virtual tape: file list, frame index, timecode per frame, timecode → frame, read a frame | `dv_subcode`, `hdv_aux` |
| `pout_deck.[ch]` | transport model with timing (state, position, winding, search) | — (tape through `pout_tape_ops_t`) |
| `pout_avc.[ch]` | AV/C target: command frame in, response frame out, deck command applied | `pout_deck` |
| `pout_script.[ch]` | MarvinPlayout's argument parser (files, settings, actions) and the action sequencer's pure logic | `pin_script`, `pin_script_eval` |
| `pout_it.[ch]` | EP 0x06 message builder (OHCI IT program, vendor sequence bits, type 11) | — |
| `pout_csr.[ch]` | target side of the 1394 link: plug registers (oMPR/oPCR/iMPR), incoming read/lock/FCP requests → responses, the AV/C config ROM | — |
| `pout_out.h`, `pout_out_file.c` | output interface; the file/loopback output (no device) | `pout_cip` |
| `pout_hw_*.c` | everything that needs the device: link glue, 1394 output, session loop | `pinnacle_core` |
| `marvin_playout.c` | `main()` | all |

`src/playout/CMakeLists.txt` builds `pout_*.c` except `pout_hw_*.c` into
`pinnacle_playout_pure` (no libusb), the rest into `MarvinPlayout`, and every
`tests/playout/test_*.c` into a ctest test linked against
`pinnacle_playout_pure` and `pinnacle_core` (the loopback tests use the
capture side's `dv_reassembler` as the checker).

## Interfaces

Times are `double` seconds on a caller-supplied monotonic clock, so all pure
modules are testable with a fake clock. Frame numbers are `int64_t` tape
frame indexes from 0.

### Tape (`pout_tape.h`)

```c
typedef enum { POUT_MEDIA_DV = 0, POUT_MEDIA_HDV } pout_media_t;
typedef struct pout_tape pout_tape_t;

/* Opens and indexes the files (all must be the same media and standard). */
int  pout_tape_open(pout_tape_t **out, const char *const *paths, int n, char *err, size_t err_cap);
void pout_tape_close(pout_tape_t *t);
pout_media_t pout_tape_media(const pout_tape_t *t);
int     pout_tape_is_625(const pout_tape_t *t);   /* PAL/50 Hz */
double  pout_tape_fps(const pout_tape_t *t);      /* 25 or 30000/1001 (HDV 720p: 50/59.94 shown as 25/29.97 units) */
int64_t pout_tape_frames(const pout_tape_t *t);
int     pout_tape_tc_of_frame(const pout_tape_t *t, int64_t frame, pin_tc_t *tc);
int64_t pout_tape_frame_of_tc(const pout_tape_t *t, const pin_tc_t *tc);  /* -1 if not on the tape */
/* DV: one DIF frame. HDV: the TS packets (n*188 bytes) whose time falls in
 * this frame's period. Returns 0 on success. */
int     pout_tape_read_frame(pout_tape_t *t, int64_t frame, uint8_t *buf, size_t cap, size_t *len);
size_t  pout_tape_max_frame_bytes(const pout_tape_t *t);
int64_t pout_tape_play_start(const pout_tape_t *t, int64_t frame);  /* HDV: GOP start at or before; DV: frame */
void    pout_tape_ops(pout_tape_t *t, pout_tape_ops_t *ops);  /* fills the deck's view */
```

### Deck's view of the tape (`pout_deck.h`)

```c
typedef struct {
    void *ctx;
    int64_t frames;
    double fps;
    int (*tc_of_frame)(void *ctx, int64_t frame, pin_tc_t *tc);
    int64_t (*frame_of_tc)(void *ctx, const pin_tc_t *tc);
} pout_tape_ops_t;
```

### Packetizer (`pout_cip.h`)

One call per frame period produces exactly that period's cycles: data packets
plus empty (CIP-only) packets. `pout_cip_blank()` produces a period of empty
packets (deck stopped or winding). The callback gets each packet (CIP header +
payload) in order.

### Output (`pout_out.h`)

```c
typedef struct pout_out {
    int  (*start)(struct pout_out *o);
    int  (*packet)(struct pout_out *o, const uint8_t *pkt, unsigned len); /* one isochronous packet */
    void (*stop)(struct pout_out *o);
    void (*destroy)(struct pout_out *o);
} pout_out_t;
```

Implementations: the 1394 IT output (`pout_hw_out1394.c`, EP 0x06) and the
file/loopback output (`pout_out_file.c`: wraps packets in the EP 0x88
buffer-fill format and feeds `dv_reassembler`, writing `.dv`/`.m2t`, for
`--dry-run`). An analog output would be a third, taking frames, not packets.

## Timing model (guesses, all adjustable on the command line)

| what | default |
|---|---|
| stop → play, pause → play | 1.0 s before the picture starts |
| play → pause, play → stop | 0.3 s |
| FF / REW speed | 30× real time |
| timecode search | wind at 30× to the target, then 1.5 s settle, ends paused at the target |
| end of tape in play/FF | stops (play: STOP; FF: wind stop) |
| start of tape in REW | stops |
| what goes out while stopped / winding | empty CIP packets (DV), nothing (HDV) |
| what goes out while paused | DV: the paused frame repeated; HDV: nothing |

## Not done here

- GUI support. When it comes, the logic moves behind `pin_api.h` (core-first rule).
- Analog playout (no encoder on the 500/510-USB).
- MOV/MKV/AVI input (no demux, as agreed).
