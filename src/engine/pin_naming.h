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
 * Output filename generation: base.ext / base-0001.ext (scene split) /
 * base-pass-2.ext (multi-pass, no split) / base-pass-2-0001.ext (both).
 * Never overwrites: pin_naming_collides() lets the GUI check first.
 *
 * Handles both '/' and '\\' as separators everywhere (a Windows path may
 * use either; POSIX paths only ever have '/', which this code treats as
 * just another separator, so it works unmodified there too).
 */

#ifndef PIN_NAMING_H
#define PIN_NAMING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int scene_split;   /* append -NNNN */
    int always_number; /* append -NNNN even without scene_split */
    unsigned scene_index; /* 1-based when scene_split / always_number is set */
    unsigned pass;      /* 1 = no "-pass-N" suffix; >=2 adds it */
} pin_naming_opts_t;

/* Strips a trailing "." + extension matching `known_ext` (case-insensitive,
 * without the dot, e.g. "dv") from base_path if present, into
 * out (out_size bytes). If it doesn't match, base_path is copied unchanged.
 * Returns 0 on success, -1 if out_size was too small. */
int pin_naming_strip_extension(const char *base_path, const char *known_ext, char *out,
                                size_t out_size);

/* Builds the output path into out (out_size bytes) from a base path that
 * has already had any extension stripped (pin_naming_strip_extension), the
 * options above, and the extension to add (without the leading dot).
 * Returns 0 on success, -1 if out_size was too small or arguments are
 * invalid (pass == 0, or scene_split with scene_index == 0). */
int pin_naming_build(const char *base_no_ext, const pin_naming_opts_t *opts, const char *ext,
                      char *out, size_t out_size);

/* Validates the final component of an output path (what follows the last
 * '/' or '\') as a portable file name: not empty, no control characters or
 * any of < > : " / \ | ? *, no leading or trailing space, no leading or
 * trailing '.', no Windows reserved device names (CON, PRN, AUX, NUL,
 * COM0-9, LPT0-9, with or without an extension), and short enough to leave
 * room for the "-pass-N-NNNN.ext" suffixes. Returns 1 if usable, 0 if not,
 * with a short human-readable reason in reason (reason_size bytes; may be
 * NULL). Deliberately stricter than the running OS so files move freely
 * between platforms and file systems. */
int pin_naming_validate(const char *path, char *reason, size_t reason_size);

/* Checks whether ANY file matching this capture's naming pattern already
 * exists on disk: base.ext, base-NNNN.ext, base-pass-N.ext and
 * base-pass-N-NNNN.ext, for the given base (already extension-stripped)
 * and ext. Returns 1 if at least one exists, 0 if none do, -1 on error
 * (e.g. the directory can't be opened -- treated by callers as "can't be
 * sure", not as "no collision"). first_match, if non-NULL, receives the
 * first colliding path found (first_match_size bytes); safe to pass NULL. */
int pin_naming_collides(const char *base_no_ext, const char *ext, char *first_match,
                         size_t first_match_size);

/* Free space below which the GUI asks "Only X GB free. Continue?" before a
 * capture starts (25 GiB). */
#define PIN_LOW_SPACE_BYTES (25ull << 30)

/* Returns 1 if free_bytes is a known amount (> 0) below PIN_LOW_SPACE_BYTES.
 * 0 free bytes means "unknown" (the query failed) and is not reported low. */
int pin_output_space_low(unsigned long long free_bytes);

#ifdef __cplusplus
}
#endif

#endif /* PIN_NAMING_H */
