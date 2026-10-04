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

#include "pin_pace.h"

/* loop gains: phase, and rate (critically damped: rate = phase^2 / 4) */
#define PACE_PHASE_GAIN 0.03
#define PACE_RATE_GAIN (PACE_PHASE_GAIN * PACE_PHASE_GAIN / 4)
#define PACE_LEARN_S 0.5   /* arrivals averaged for the first rate estimate */

double pin_pace_frame(pin_pace_t *pc, double now, int frames)
{
    if (frames < 1)
        frames = 1;
    double gap = now - pc->last;
    if (pc->last == 0 || gap > 0.5 || gap < -0.001) {
        /* first frame, or after a pause / no signal: learn the rate again */
        pc->period = 0;
        pc->learn_t = now;
        pc->learn_n = 0;
        pc->last = now;
        return now;
    }
    pc->last = now;

    if (pc->period == 0) {
        pc->learn_n += frames; /* frame periods since learn_t */
        double span = now - pc->learn_t;
        double per = pc->learn_n ? span / pc->learn_n : 0;
        pc->grid = now;
        if (span < PACE_LEARN_S || per < 1.0 / 120 || per > 0.1)
            return now; /* show it as it comes until the rate is known */
        pc->period = per;
    } else {
        pc->grid += frames * pc->period;
        double late = now - pc->grid;
        if (late > 0.25 || late < -0.25) {
            /* lost lock (the source's clock jumped): learn again */
            pc->period = 0;
            pc->learn_t = now;
            pc->learn_n = 0;
            return now;
        }
        pc->grid += PACE_PHASE_GAIN * late;
        pc->period += PACE_RATE_GAIN * late / frames;
        double decayed = pc->late * 0.995;
        pc->late = late > decayed ? late : decayed;
    }

    /* Grow the delay gently (a step would hold one frame visibly longer);
     * a frame later than its time is shown as soon as it is there anyway. */
    double d = PIN_PACE_DELAY_MIN + (pc->late > 0 ? pc->late : 0);
    if (d > PIN_PACE_DELAY_MAX)
        d = PIN_PACE_DELAY_MAX;
    if (pc->delay < PIN_PACE_DELAY_MIN)
        pc->delay = PIN_PACE_DELAY_MIN;
    pc->delay = d > pc->delay + PIN_PACE_DELAY_STEP ? pc->delay + PIN_PACE_DELAY_STEP : d;
    return pc->grid + pc->delay;
}
