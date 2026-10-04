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
 * Preview jitter buffer: when to show a frame that has just been decoded.
 *
 * Frames come from a steady source clock (25 or 29.97 fps) but reach the
 * preview with jitter: USB delivery, DV/HDV decode time, HDV pictures
 * decoded in bursts. Showing each one as soon as it is decoded passes that
 * jitter straight to the screen, where a frame landing just before or just
 * after a display refresh is held one refresh shorter or longer than its
 * neighbours. Instead each frame gets a slot on a grid at the source's
 * nominal frame rate (sources are within a few ppm of it), plus a delay
 * that covers how late frames come relative to that grid. A very slow phase
 * correction keeps the grid on the arrivals as the source's and the PC's
 * clocks drift apart, so that drift is absorbed smoothly instead of as a
 * repeated or skipped frame. A renderer that shows each frame on the first
 * refresh at or after its time gets a cadence as even as the display
 * allows. The delay adapts: a few ms for analog, more for a bursty
 * source; pin_previewer_delay() reports it so audio monitoring can be held
 * back by the same amount.
 *
 * Pure arithmetic on caller-supplied times, so it is unit-tested with
 * synthetic arrivals (tests/engine/test_pin_pace.c).
 */

#ifndef PIN_PACE_H
#define PIN_PACE_H

#ifdef __cplusplus
extern "C" {
#endif

#define PIN_PACE_DELAY_MIN 0.004
#define PIN_PACE_DELAY_MAX 0.200
#define PIN_PACE_DELAY_STEP 0.0002 /* most the delay grows by per frame */

typedef struct {
    double grid;    /* grid slot of the last frame, 0 = start over */
    double last;    /* arrival time of the last frame */
    double period;  /* the frame period the grid runs at */
    double late;    /* decaying peak of how late frames come relative to the grid */
    double delay;   /* current jitter delay */
} pin_pace_t;

/* A frame arrived at `now` (seconds, any monotonic clock): returns the time
 * to show it at. `period` is the source's nominal frame period (1/25,
 * 1001/30000, ...); 0 if not known, and the frame is shown as it comes.
 * `frames` is how many source frames it stands for, 1 plus any dropped
 * since the previous one, so the grid skips their slots too.
 * Zero-initialise the struct to start. */
double pin_pace_frame(pin_pace_t *pc, double now, double period, int frames);

#ifdef __cplusplus
}
#endif

#endif /* PIN_PACE_H */
