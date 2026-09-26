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
 * Core-wide logging. Replaces the fprintf(stderr, ...) calls that used to be
 * scattered through src/core so a future GUI (which has nowhere sane to send
 * stderr) can install its own sink -- queue lines as events, show them in a
 * log pane, whatever -- without the core caring who is listening.
 *
 * The default sink writes exactly what pincli/pindeck/pinanalog have always
 * printed to stderr, so the existing CLIs are unaffected unless they choose
 * to install a sink of their own.
 */

#ifndef PIN_LOG_H
#define PIN_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PIN_LOG_DEBUG = 0, /* high-rate / diagnostic-only detail (per-transfer timing, hex dumps) */
    PIN_LOG_INFO,      /* normal progress ("device opened", "streaming started") */
    PIN_LOG_WARN,      /* recovered from, but worth a human's attention */
    PIN_LOG_ERROR,     /* the operation failed */
} pin_log_level_t;

/* Called once per pin_logf(), with the fully-formatted line (newline included
 * if the format string had one, exactly as fprintf would have produced).
 * May be called from any thread that calls into the core; the message is
 * already formatted into a caller-local buffer, so the sink itself only
 * needs to be safe to call concurrently with itself (the default one just
 * does an fwrite, which is). */
typedef void (*pin_log_sink_fn)(pin_log_level_t level, const char *msg, void *user);

/* Installs a new sink. Pass fn == NULL to restore the default (stderr) sink.
 * Not itself thread-safe to call concurrently with pin_logf() or with
 * another pin_log_set_sink() -- set it once at startup before other threads
 * are logging. */
void pin_log_set_sink(pin_log_sink_fn fn, void *user);

/* Formats and dispatches one log line to the current sink. printf-format
 * checked by GCC/Clang so a bad format string is still caught at compile
 * time as before. gnu_printf (not the bare "printf" archetype) is used
 * deliberately: on MinGW targets, "printf" resolves to the ms_printf
 * archetype, which doesn't know %zu/%lld and would warn on the size_t/long
 * formats this codebase uses throughout. gnu_printf is available and
 * correct on every GCC/Clang target this project builds on. */
#if defined(__GNUC__)
__attribute__((format(gnu_printf, 2, 3)))
#endif
void pin_logf(pin_log_level_t level, const char *fmt, ...);

void pin_vlogf(pin_log_level_t level, const char *fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif /* PIN_LOG_H */
