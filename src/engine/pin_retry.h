/*
 * Pinnacle Studio 500-USB open driver
 * Copyright (C) 2026 Jonas Cz.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Retry of bad tape sections during a DV / HDV capture. Hardware-free: the session feeds it
 * the units that arrive (DV frames, HDV pictures) and the deck's state, and it hands back the
 * units to write (in order, possibly mended) and the deck commands to send.
 *
 * How it works
 *
 *   Every unit goes through a FIFO of "slots" before it reaches the file (in RAM, or in a
 *   temporary file next to the output: pin_retry_cfg_t.storage): a DV frame is
 *   one slot, an HDV GOP (a picture with a sequence header up to the next one) is one slot.
 *   Normally a slot is let go about a second after it arrived (the anchor margin: the frames
 *   a re-read lines up on). When a slot is bad, the FIFO stops letting go: a "section" is
 *   open. It stays open while more damage follows within the join gap, so bad spots close to
 *   each other are re-read in one go; it is retried once the clean run after the last bad
 *   slot reaches the join gap, the section reaches the hold window, or the signal ends.
 *
 *   Retry: REW, then PLAY a little before the section (the preroll). How far to rewind comes
 *   from the deck's TIME CODE answers while winding when it gives them, otherwise from the
 *   wind time at the rewind speed this deck has shown so far (learned on every retry; a
 *   first guess before). Landing after the section's start means winding back again;
 *   landing before it just means reading a few seconds more.
 *
 *   The re-read is lined up with the held slots by key: the DV subcode timecode, the HDV
 *   video decode time (DTS) of the GOP's first picture (the Canon writes 00:00:00:00 in every
 *   GOP header, DTS is what the tape carries). A unit without a key takes the slot after the
 *   previous one. A key between two held slots fills a hole (frames lost in transit).
 *   DV is mended block by block (dv_error_block_map(): the better grade wins); an HDV GOP is
 *   replaced whole when the re-read one has fewer damaged pictures (or as many, more of them).
 *   Once the re-read runs past the section, it either winds back again (damage left, attempts
 *   left, and the last attempt mended something) or the section is let go and the capture
 *   simply goes on from there: what the deck plays now is the tape after the section.
 *
 *   The units of each read carry a read number: HDV continuity counters are re-phased where
 *   reads join (pin_retry_cc_fix_t), so a raw .m2t and the MOV / MKV repair see one stream.
 *
 * As quick as possible: the join gap adapts to how damaged the tape is. It starts short (about
 * a second: one bad spot is re-read at once), doubles for each section that follows soon after
 * the previous one (a damaged stretch: fewer, longer sections, many bad spots per rewind), up to
 * half the window, and halves again as the tape gets clean. A section whose attempt mended
 * nothing gets no further attempts.
 *
 * Modes (pin_retry_cfg_t.mode): SECTIONS as above; WHOLE holds everything from the first damage
 * to the end of the recording (the signal stays away) in the temporary file, then winds back
 * once and re-reads it, pass after pass, each from the first frame still bad: fewer rewinds, less
 * deck and tape wear, the file is written at the end. AUTO (the default) starts with sections
 * and turns to WHOLE for the rest of the capture on a very poor tape (the join gap at its
 * maximum for several sections in a row).
 */

#ifndef PIN_RETRY_H
#define PIN_RETRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_RETRY_TRIGGER_ANY = 0,         /* any damage, concealed included (the default) */
    PIN_RETRY_TRIGGER_UNCORRECTED = 1, /* only what nobody concealed: DV STA 7/15, missing
                                          blocks, audio error code, lost frames; HDV damage */
} pin_retry_trigger_t;

typedef enum {
    PIN_RETRY_MODE_AUTO = 0,        /* sections; the whole tape once it is very poor */
    PIN_RETRY_MODE_SECTIONS = 1,    /* each bad section at once */
    PIN_RETRY_MODE_WHOLE = 2,       /* everything from the first damage, re-read at the end */
} pin_retry_mode_t;

typedef enum {
    PIN_RETRY_STORE_RAM = 0,
    PIN_RETRY_STORE_FILE = 1,       /* a temporary file in spool_dir (WHOLE always uses one) */
} pin_retry_store_t;

typedef struct {
    int attempts;       /* re-reads of one section, 1..10 */
    double window_s;    /* longest section held, seconds of tape (RAM: at most 600) */
    int trigger;        /* pin_retry_trigger_t */
    double preroll_s;   /* PLAY starts this long before the section */
    double join_s;      /* clean run that closes a section; <= 0: adaptive (see above) */
    int mode;           /* pin_retry_mode_t */
    int storage;        /* pin_retry_store_t */
    char spool_dir[1024]; /* folder of the temporary file ("" = the current one) */
} pin_retry_cfg_t;

/* Limits and defaults (also what the GUIs offer). */
#define PIN_RETRY_ATTEMPTS_DEFAULT 3
#define PIN_RETRY_ATTEMPTS_MAX 10
#define PIN_RETRY_WINDOW_DEFAULT 60
#define PIN_RETRY_WINDOW_MIN 10
#define PIN_RETRY_WINDOW_MAX 3600     /* with a temporary file */
#define PIN_RETRY_WINDOW_MAX_RAM 600
#define PIN_RETRY_PREROLL_DEFAULT 3
#define PIN_RETRY_PREROLL_MAX 30

void pin_retry_cfg_defaults(pin_retry_cfg_t *c);
/* Clamps every field into its range (0 or out-of-range values get the default). */
void pin_retry_cfg_clamp(pin_retry_cfg_t *c);

typedef enum {
    PIN_RETRY_IDLE = 0,     /* passing through (anchor margin only) */
    PIN_RETRY_HOLDING,      /* a section is open: damage seen, waiting for it to end */
    PIN_RETRY_REWINDING,    /* winding back to the section */
    PIN_RETRY_SEEKING,      /* playing toward the section, nothing lined up yet */
    PIN_RETRY_REREADING,    /* reading the section again, mending it */
} pin_retry_phase_t;

typedef enum {
    PIN_RETRY_ACT_NONE = 0,
    PIN_RETRY_ACT_REW,
    PIN_RETRY_ACT_PLAY,
    PIN_RETRY_ACT_STOP,
} pin_retry_action_t;

/* One unit as it arrived. */
typedef struct {
    int hdv;                /* 0 DV frame, 1 HDV unit */
    const uint8_t *data;
    size_t len;
    const void *meta;       /* the caller's descriptor of the unit, copied (meta_len bytes) */
    size_t meta_len;
    /* HDV only: */
    int picture;            /* a picture (0: PAT/PMT and other non-picture units) */
    int opens_gop;          /* carries a sequence header: starts a slot */
    int damaged;            /* hdv_unit_errors_t.frame_error */
    /* DV and HDV: the key a re-read is lined up by. DV: the subcode timecode as
     * pin_retry_tc_frames(); HDV: the DTS of the GOP's first picture (on the unit with opens_gop). */
    int key_valid;
    int64_t key;
    int lost_before;        /* data was lost in transit just before this unit */
    double fps;             /* frames per second of the stream (DV 25 / 29.97, HDV its rate) */
} pin_retry_unit_t;

/* A unit the session writes now, in order. read = which read it came from (0, 1, ...; HDV
 * continuity counters are re-phased where it changes). mended: 1 = a DV frame whose blocks came
 * partly from a re-read, or an HDV GOP replaced by one (its error statistics must be taken
 * anew); 2 = a frame / GOP the first read lost in transit, found by a re-read (pin_retry_lost()).
 * The data is the engine's: valid during the call, the callee may modify it. */
typedef void (*pin_retry_release_fn)(void *user, uint8_t *data, size_t len, const void *meta,
                                     unsigned read, int mended);

/* What the deck says, for pin_retry_tick(). */
typedef struct {
    int signal;             /* data arrived in the last second */
    int tc_valid;           /* the deck's TIME CODE poll answered recently (tc_frames) */
    int64_t tc_frames;      /* that answer as frames (pin_retry_tc_frames()) */
    int at_start;           /* stopped at the tape start (BOT) after a REW */
    int cmd_failed;         /* the last REW / PLAY was refused or not answered */
    int can_send;           /* nothing in flight: a command can go out */
} pin_retry_deck_t;

typedef struct {
    pin_retry_phase_t phase;
    int attempt;            /* the running attempt of the current section, 1-based; 0 = none */
    int attempts;           /* configured maximum */
    double held_s;          /* tape held in RAM (seconds) */
    double section_s;       /* length of the open / retried section */
    unsigned bad_slots;     /* bad slots in it */
    uint64_t sections;      /* sections retried so far (this capture) */
    uint64_t sections_fixed;    /* ... all damage mended */
    uint64_t sections_partly;   /* ... some mended, some left */
    uint64_t sections_failed;   /* ... nothing mended */
    uint64_t frames_mended; /* DV frames / HDV GOPs made better by a re-read */
    uint64_t rewinds;       /* REW commands sent */
    double rew_rate;        /* learned wind speed (x real time), 0 = not known yet */
    double cycle_s;         /* learned cost of one retry cycle (seconds), 0 = not known */
    double join_s;          /* join gap in use */
    int whole;              /* holding the whole tape (mode WHOLE, or AUTO turned to it) */
    int spooling;           /* units go to the temporary file */
    uint64_t spool_bytes;   /* its size now */
    int spool_failed;       /* it could not be written: RAM, sections only */
} pin_retry_stats_t;

typedef struct pin_retry pin_retry_t;

pin_retry_t *pin_retry_new(const pin_retry_cfg_t *cfg, pin_retry_release_fn release, void *user);
void pin_retry_free(pin_retry_t *r);

/* Rewind speed and cycle cost learned earlier (from another capture of this session, or the
 * settings for this camera model); 0 = unknown. */
void pin_retry_set_learned(pin_retry_t *r, double rew_rate, double cycle_s);

/* A unit arrived (now = seconds, any monotonic clock). Units that are part of the tape go into
 * the FIFO; units read while the deck is finding its place are dropped. May release units. */
void pin_retry_push(pin_retry_t *r, double now, const pin_retry_unit_t *u);

/* Units lost in transit that never arrived (dv_output_t.on_loss frames): a hole after the
 * newest slot that a re-read can fill. */
void pin_retry_lost(pin_retry_t *r, unsigned long frames);

/* Called often (dv_tick, ~10+ Hz): the timers, and the deck command to send now, if any. */
pin_retry_action_t pin_retry_tick(pin_retry_t *r, double now, const pin_retry_deck_t *deck);

/* Lets every held unit go (the capture ends): best-so-far, in order. */
void pin_retry_flush(pin_retry_t *r);

/* The same within the release budget (pin_retry_set_release_budget()): ends any section, lets go
 * as many of the oldest units as the budget takes. Returns how many slots are still held; call it
 * again (with a new budget) until 0. */
int pin_retry_flush_some(pin_retry_t *r);

/* Whether the units now arriving are read while the deck winds back or seeks (not part of the
 * output yet): the session does not count them, and the no-signal timer stands still. */
int pin_retry_busy(const pin_retry_t *r);

/* The session may let go of at most this much more now (writer queue room); the engine keeps
 * the rest and tries again on the next push / tick. SIZE_MAX = no limit (the default). */
void pin_retry_set_release_budget(pin_retry_t *r, size_t bytes);

void pin_retry_get_stats(const pin_retry_t *r, pin_retry_stats_t *out);

/* Timecode as a frame count at the nominal rate (non-drop arithmetic: an ordering key). */
int64_t pin_retry_tc_frames(int hour, int minute, int second, int frame, double fps);

/* HDV continuity counter re-phasing where reads join. */
typedef struct {
    int8_t last[8192];      /* last CC written per PID, -1 none */
    int8_t off[8192];       /* added to this read's CCs */
    uint32_t gen[8192];     /* the read the offset was taken for */
    uint32_t cur;           /* current read + 1 */
} pin_retry_cc_fix_t;

void pin_retry_cc_fix_init(pin_retry_cc_fix_t *f);
/* Rewrites the CCs of n 188-byte packets in place; read = the unit's read number. */
void pin_retry_cc_fix_apply(pin_retry_cc_fix_t *f, uint8_t *ts, size_t n, unsigned read);

#ifdef __cplusplus
}
#endif

#endif /* PIN_RETRY_H */
