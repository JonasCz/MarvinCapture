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
 * FFmpeg writes its own messages ("Concealing bitstream errors", "Detected
 * timecode is invalid", "MediaCodec configure failed", ...) straight to
 * stderr, which clutters a command line and is invisible in a GUI. This
 * routes them into the core log instead: errors as warnings, everything else
 * at debug level (the log level decides what shows). Installed by the
 * preview (pin_preview.c) and by the encoder probe (pin_hwenc.c), whichever
 * runs first.
 */

#include "pin_sink.h"
#include "../core/pin_log.h"

#include <libavutil/log.h>

#include <pthread.h>
#include <stdarg.h>
#include <string.h>

/* pin_sink_av_log_quiet(): an encoder probe expects some attempts to fail */
static __thread int t_quiet;

static void sink_av_log(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_INFO)
        return;
    /* what the HDV decoder says when it is handed the middle of a picture before the first
     * sequence header: harmless, the preview just waits for the next GOP */
    if (fmt && strstr(fmt, "Invalid frame dimensions"))
        level = AV_LOG_DEBUG + 1;
    /* libavformat's dv demuxer (the per-frame audio demux in sink_rewrap.c and sink_hwenc.c)
     * logs this at error level whenever the first subcode pack of a frame is not a timecode
     * pack (blank 0xFF subcode, as in tests/data/dv-ntsc.dv). It only feeds a metadata tag we
     * never use; the capture's own timecode comes from dv_subcode.c */
    if (fmt && strstr(fmt, "timecode is invalid"))
        level = AV_LOG_DEBUG + 1;
    if (t_quiet)
        level = AV_LOG_DEBUG + 1;
    static __thread int in_line_prefix;
    char line[512];
    int prefix = in_line_prefix;
    av_log_format_line(avcl, level, fmt, vl, line, (int)sizeof(line), &prefix);
    in_line_prefix = prefix;
    pin_logf(level <= AV_LOG_ERROR ? PIN_LOG_WARN : PIN_LOG_DEBUG, "ffmpeg: %s", line);
}

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void install(void) { av_log_set_callback(sink_av_log); }

void pin_sink_av_log_install(void)
{
    pthread_once(&g_once, install);
}

void pin_sink_av_log_quiet(int quiet)
{
    t_quiet = quiet != 0;
}
