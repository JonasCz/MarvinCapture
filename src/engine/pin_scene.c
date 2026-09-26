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

#include "pin_scene.h"
#include <math.h>
#include <string.h>
#include <time.h>

void pin_scene_config_defaults(pin_scene_config_t *cfg, double fps)
{
    cfg->debounce_frames = 5;
    cfg->date_gap_seconds = 1.0;
    cfg->tc_jump_seconds = 1.0;
    cfg->fps = fps;
}

void pin_scene_init(pin_scene_detector_t *d, const pin_scene_config_t *cfg)
{
    memset(d, 0, sizeof(*d));
    d->cfg = *cfg;
}

unsigned pin_scene_debounce_window(const pin_scene_detector_t *d)
{
    return d->cfg.debounce_frames;
}

static void state_from_record(pin_scene_state_t *s, const pin_scene_record_t *r)
{
    memset(s, 0, sizeof(*s));
    if (r->tc_valid) {
        s->tc_valid = 1;
        s->tc_hour = r->tc_hour;
        s->tc_minute = r->tc_minute;
        s->tc_second = r->tc_second;
        s->tc_frame = r->tc_frame;
        s->tc_drop_frame = r->tc_drop_frame;
    }
    if (r->date_valid) {
        s->date_valid = 1;
        s->date_day = r->date_day;
        s->date_month = r->date_month;
        s->date_year = r->date_year;
    }
    if (r->time_valid) {
        s->time_valid = 1;
        s->time_hour = r->time_hour;
        s->time_minute = r->time_minute;
        s->time_second = r->time_second;
    }
}

/* Copies only the fields present in r into state, leaving fields r doesn't
 * have untouched (a frame with no date pack shouldn't erase the last known
 * date). */
static void state_advance(pin_scene_state_t *state, const pin_scene_record_t *r)
{
    if (r->tc_valid) {
        state->tc_valid = 1;
        state->tc_hour = r->tc_hour;
        state->tc_minute = r->tc_minute;
        state->tc_second = r->tc_second;
        state->tc_frame = r->tc_frame;
        state->tc_drop_frame = r->tc_drop_frame;
    }
    if (r->date_valid) {
        state->date_valid = 1;
        state->date_day = r->date_day;
        state->date_month = r->date_month;
        state->date_year = r->date_year;
    }
    if (r->time_valid) {
        state->time_valid = 1;
        state->time_hour = r->time_hour;
        state->time_minute = r->time_minute;
        state->time_second = r->time_second;
    }
}

/* Standard SMPTE drop-frame frame-count formula: for NTSC drop-frame
 * (nominal 30 fps), frame numbers :00 and :01 are skipped at the start of
 * every minute except multiples of ten, so
 *   count = (h*3600+m*60+s)*fps + f - 2*(total_minutes - total_minutes/10)
 * Non-drop and non-30fps rates use a plain fixed-fps count. */
static long tc_frame_count(const pin_scene_state_t *s, int nominal_fps)
{
    long total_minutes = (long)s->tc_hour * 60 + s->tc_minute;
    long count = ((long)s->tc_hour * 3600 + (long)s->tc_minute * 60 + s->tc_second) * nominal_fps +
                 s->tc_frame;
    if (s->tc_drop_frame && nominal_fps == 30) {
        long dropped_minutes = total_minutes - total_minutes / 10;
        count -= dropped_minutes * 2;
    }
    return count;
}

static long frames_per_day(int nominal_fps, int drop_frame)
{
    long count = (long)nominal_fps * 3600 * 24;
    if (drop_frame && nominal_fps == 30)
        count -= 2 * (1440 - 144);
    return count;
}

/* Seconds-since-an-arbitrary-epoch for a full date+time, only meaningful as
 * a difference between two such values. */
static double datetime_seconds(const pin_scene_state_t *s)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = s->date_year - 1900;
    tmv.tm_mon = s->date_month - 1;
    tmv.tm_mday = s->date_day;
    tmv.tm_hour = s->time_hour;
    tmv.tm_min = s->time_minute;
    tmv.tm_sec = s->time_second;
    /* Days since an epoch, computed by hand (no timegm on all platforms,
     * and mktime is timezone-dependent) -- only relative gaps matter here. */
    long y = tmv.tm_year + 1900;
    long m = tmv.tm_mon + 1;
    long a = (14 - m) / 12;
    long yy = y + 4800 - a;
    long mm = m + 12 * a - 3;
    long jdn = tmv.tm_mday + (153 * mm + 2) / 5 + 365L * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
    double seconds = (double)jdn * 86400.0 + tmv.tm_hour * 3600.0 + tmv.tm_min * 60.0 + tmv.tm_sec;
    return seconds;
}

typedef enum { CONT_NEUTRAL, CONT_YES, CONT_BREAK } cont_t;

static cont_t classify(const pin_scene_state_t *base, const pin_scene_record_t *r,
                        const pin_scene_config_t *cfg)
{
    if (r->rec_start_valid && r->rec_start)
        return CONT_BREAK;

    cont_t tc_result = CONT_NEUTRAL;
    if (base->tc_valid && r->tc_valid) {
        int nominal_fps = (int)(cfg->fps + 0.5);
        pin_scene_state_t rs;
        state_from_record(&rs, r);
        long a = tc_frame_count(base, nominal_fps);
        long b = tc_frame_count(&rs, nominal_fps);
        long delta = b - a;
        long day = frames_per_day(nominal_fps, base->tc_drop_frame);
        if (delta < -day / 2)
            delta += day; /* midnight wrap: forward, not backward */
        long jump_frames = (long)(cfg->tc_jump_seconds * cfg->fps + 0.5);
        if (delta < 0)
            tc_result = CONT_BREAK;
        else if (delta > jump_frames)
            tc_result = CONT_BREAK;
        else
            tc_result = CONT_YES;
    }

    cont_t date_result = CONT_NEUTRAL;
    if (base->date_valid && base->time_valid && r->date_valid && r->time_valid) {
        pin_scene_state_t rs;
        state_from_record(&rs, r);
        double gap = datetime_seconds(&rs) - datetime_seconds(base);
        if (fabs(gap) > cfg->date_gap_seconds)
            date_result = CONT_BREAK;
        else
            date_result = CONT_YES;
    }

    if (tc_result == CONT_BREAK || date_result == CONT_BREAK)
        return CONT_BREAK;
    if (tc_result == CONT_YES || date_result == CONT_YES)
        return CONT_YES;
    return CONT_NEUTRAL;
}

int pin_scene_feed(pin_scene_detector_t *d, const pin_scene_record_t *r, long *cut_frame_index)
{
    if (!d->has_baseline) {
        state_from_record(&d->baseline, r);
        d->has_baseline = 1;
        return 0;
    }

    if (!d->pending_active) {
        cont_t c = classify(&d->baseline, r, &d->cfg);
        if (c != CONT_BREAK) {
            state_advance(&d->baseline, r);
            return 0;
        }
        /* Possible break: open a pending run. */
        d->pending_active = 1;
        d->pending_start_index = r->frame_index;
        d->pending_saved_baseline = d->baseline;
        state_from_record(&d->pending_candidate, r);
        d->pending_run_length = 1;
        if (d->pending_run_length >= d->cfg.debounce_frames) {
            /* debounce_frames == 1: confirm immediately. */
            d->baseline = d->pending_candidate;
            d->pending_active = 0;
            if (cut_frame_index)
                *cut_frame_index = d->pending_start_index;
            return 1;
        }
        return 0;
    }

    /* A pending run is open: does this frame continue it? */
    cont_t continues_candidate = classify(&d->pending_candidate, r, &d->cfg);
    if (continues_candidate == CONT_BREAK) {
        /* Doesn't continue the candidate. Does it go back to the old
         * baseline instead (the anomaly was a transient glitch)? */
        cont_t continues_old = classify(&d->pending_saved_baseline, r, &d->cfg);
        if (continues_old != CONT_BREAK) {
            d->baseline = d->pending_saved_baseline;
            state_advance(&d->baseline, r);
            d->pending_active = 0;
            return 0;
        }
        /* Neither: a fresh, different anomaly starts its own run. The
         * original confirmed baseline (saved_baseline) is unaffected. */
        d->pending_start_index = r->frame_index;
        state_from_record(&d->pending_candidate, r);
        d->pending_run_length = 1;
        return 0;
    }

    if (continues_candidate == CONT_YES)
        state_advance(&d->pending_candidate, r);
    /* CONT_NEUTRAL: frame has no comparable data; leave the candidate and
     * run length untouched, it's neither confirming nor breaking. */
    if (continues_candidate != CONT_NEUTRAL)
        d->pending_run_length++;

    if (d->pending_run_length >= d->cfg.debounce_frames) {
        d->baseline = d->pending_candidate;
        d->pending_active = 0;
        if (cut_frame_index)
            *cut_frame_index = d->pending_start_index;
        return 1;
    }
    return 0;
}
