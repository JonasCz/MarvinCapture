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

/* Phase correction per frame: slow enough that arrival jitter barely moves
 * the grid (a 16 ms jitter moves it by under 0.2 ms), fast enough to follow
 * a source a fraction of a percent off its nominal rate. */
#define PACE_PHASE_GAIN 0.01

double pin_pace_frame(pin_pace_t *pc, double now, double period, int frames)
{
    if (frames < 1)
        frames = 1;
    double gap = now - pc->last;
    pc->last = now;
    if (period <= 0) {
        pc->grid = 0;
        return now;
    }
    if (pc->grid == 0 || period != pc->period || gap > 0.5 || gap < -0.001) {
        /* first frame, a new rate, or after a pause / no signal */
        pc->grid = now;
        pc->period = period;
    } else {
        pc->grid += frames * period;
        double late = now - pc->grid;
        if (late > 0.25 || late < -0.25) {
            pc->grid = now; /* lost lock (the source's clock jumped): start over */
        } else {
            pc->grid += PACE_PHASE_GAIN * late;
            double decayed = pc->late * 0.995;
            pc->late = late > decayed ? late : decayed;
        }
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
