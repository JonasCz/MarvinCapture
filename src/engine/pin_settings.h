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
 * A tiny INI-format settings store: [section] headers, key=value lines,
 * ';' or '#' comments (only recognised at the start of a line, after
 * optional whitespace). Round-tripping comments/formatting is not
 * attempted -- a save rewrites the file from the in-memory key/value
 * pairs, sections in first-seen order.
 *
 * Saving is atomic: write to "<path>.tmp" then rename it over the real
 * file, so a crash or power loss mid-save never leaves a half-written
 * settings file. On Windows the rename is MoveFileExW with
 * MOVEFILE_REPLACE_EXISTING (POSIX rename() already replaces atomically).
 * Paths are accepted as UTF-8 everywhere, including on Windows, where they
 * are converted to UTF-16 (MultiByteToWideChar) before any Win32 file
 * call, so a config directory under a non-ASCII user name works.
 */

#ifndef PIN_SETTINGS_H
#define PIN_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *section;
    char *key;
    char *value;
} pin_settings_entry_t;

typedef struct {
    pin_settings_entry_t *entries;
    size_t count;
    size_t capacity;
} pin_settings_t;

void pin_settings_init(pin_settings_t *s);
void pin_settings_free(pin_settings_t *s);

/* Loads path (UTF-8) into s (which must already be initialised; existing
 * entries are cleared first). A missing file is not an error: s ends up
 * empty and this returns 0, so first-run works with no special case.
 * Returns 0 on success, -1 if the file exists but couldn't be parsed
 * (currently: only an unreadable/unopenable-for-a-reason-other-than-
 * missing file causes this). */
int pin_settings_load(pin_settings_t *s, const char *path);

/* Atomically writes s to path (UTF-8). Returns 0 on success, -1 on
 * failure (path's directory doesn't exist, no permission, etc). */
int pin_settings_save(const pin_settings_t *s, const char *path);

const char *pin_settings_get_string(const pin_settings_t *s, const char *section, const char *key,
                                     const char *default_value);
int pin_settings_get_int(const pin_settings_t *s, const char *section, const char *key,
                          int default_value);
double pin_settings_get_double(const pin_settings_t *s, const char *section, const char *key,
                                double default_value);
/* Accepts (case-insensitively) 1/0, true/false, yes/no, on/off. Anything
 * else falls back to default_value. */
int pin_settings_get_bool(const pin_settings_t *s, const char *section, const char *key,
                           int default_value);

void pin_settings_set_string(pin_settings_t *s, const char *section, const char *key,
                              const char *value);
void pin_settings_set_int(pin_settings_t *s, const char *section, const char *key, int value);
void pin_settings_set_double(pin_settings_t *s, const char *section, const char *key,
                              double value);
void pin_settings_set_bool(pin_settings_t *s, const char *section, const char *key, int value);

/* Fills out (UTF-8, out_size bytes) with this platform's default settings
 * file path and makes sure its containing directory exists (best-effort;
 * pin_settings_save will still fail cleanly if that didn't work):
 *   Windows: %APPDATA%\PinnacleOSS\settings.ini
 *   Linux:   $XDG_CONFIG_HOME/pinnacle-oss/settings.ini, or
 *            ~/.config/pinnacle-oss/settings.ini if that's unset
 *   macOS:   ~/Library/Application Support/PinnacleOSS/settings.ini
 * Returns 0 on success, -1 if out_size was too small or no usable home/
 * config directory could be found in the environment. */
int pin_settings_default_path(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SETTINGS_H */
