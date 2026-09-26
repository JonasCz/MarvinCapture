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

#include "pin_log.h"

#include <stdio.h>
#include <string.h>

static void default_sink(pin_log_level_t level, const char *msg, void *user)
{
    (void)level;
    (void)user;
    fwrite(msg, 1, strlen(msg), stderr);
}

static pin_log_sink_fn g_sink = default_sink;
static void *g_sink_user = NULL;

void pin_log_set_sink(pin_log_sink_fn fn, void *user)
{
    g_sink = fn ? fn : default_sink;
    g_sink_user = fn ? user : NULL;
}

void pin_vlogf(pin_log_level_t level, const char *fmt, va_list ap)
{
    /* A local buffer, not a shared one: concurrent callers each format into
     * their own stack space, so nothing needs a lock until the sink call
     * itself (which the default sink handles with one atomic-ish fwrite,
     * same as the old fwrite-not-fprintf trick in the EP 0x84 debug path). */
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    g_sink(level, buf, g_sink_user);
}

void pin_logf(pin_log_level_t level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pin_vlogf(level, fmt, ap);
    va_end(ap);
}
