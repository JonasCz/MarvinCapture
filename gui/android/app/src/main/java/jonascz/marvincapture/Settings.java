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

package jonascz.marvincapture;

import java.util.HashMap;

/**
 * The app's options, stored by the core (pin_settings_get / set: settings.ini in the app's files
 * directory) under the desktop GUIs' "gui.*" keys. Global, not per device: the app drives one
 * device. Reads are cached (this process is the only writer), writes go straight through.
 * Main thread only.
 *
 * Keys: gui.last_input, gui.std, gui.control_N, gui.format_KIND, gui.aspect_KIND, gui.name_analog,
 * gui.name_dv (DV and HDV share it), gui.split_dv / _hdv, gui.idle_KIND, gui.duration_KIND,
 * gui.deinterlace_analog / _dv, gui.quality_analog / _dv, gui.ffv1_compression, gui.muted,
 * gui.keep_screen_on, gui.cpu_keep_busy, gui.audio_keepalive, gui.storage (volume id), gui.kind_tab, gui.deck_controls
 * (KIND = analog, dv, hdv).
 */
final class Settings {
    private Settings() {}

    private static final HashMap<String, String> cache = new HashMap<>();

    static String get(String key, String fallback) {
        String v = cache.get(key);
        if (v == null) {
            v = Core.settingsGet(key);
            cache.put(key, v);
        }
        return v.isEmpty() ? fallback : v;
    }

    static int getInt(String key, int fallback) {
        try {
            return Integer.parseInt(get(key, "").trim());
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    static boolean getBool(String key, boolean fallback) {
        String v = get(key, "");
        return v.isEmpty() ? fallback : !v.equals("0");
    }

    static void set(String key, String value) {
        if (value.equals(cache.get(key))) return;
        cache.put(key, value);
        Core.settingsSet(key, value);   // best effort
    }

    static void setInt(String key, int v) { set(key, Integer.toString(v)); }
    static void setBool(String key, boolean v) { set(key, v ? "1" : "0"); }

    static String kindKey(int kind) {
        return kind == Core.KIND_ANALOG ? "analog" : kind == Core.KIND_HDV ? "hdv" : "dv";
    }

    // ---- typed accessors -------------------------------------------------------------------

    static int input() { return clamp(getInt("gui.last_input", Core.INPUT_DV), 0, 2); }
    static int standard() { return clamp(getInt("gui.std", 0), 0, Core.STD_COUNT - 1); }
    static int aspect(int kind) { return clamp(getInt("gui.aspect_" + kindKey(kind), 0), 0, 2); }
    static boolean split(int kind) { return getBool("gui.split_" + kindKey(kind), false); }
    static int idleStop(int kind) { return Math.max(0, getInt("gui.idle_" + kindKey(kind), 5)); }
    static int maxDuration(int kind) { return Math.max(0, getInt("gui.duration_" + kindKey(kind), 0)); }
    static boolean muted() { return getBool("gui.muted", true); }
    /** Show the deck buttons (rewind, play, stop, fast forward) and make the capture button "rewind and capture". */
    static boolean deckControls() { return getBool("gui.deck_controls", false); }
    static boolean keepScreenOn() { return getBool("gui.keep_screen_on", false); }
    static boolean cpuKeepBusy() { return getBool("gui.cpu_keep_busy", false); }
    static boolean audioKeepAlive() { return getBool("gui.audio_keepalive", false); }

    /** File base name; also the embedded title. DV and HDV share one. */
    static String name(boolean analog) {
        String n = get(analog ? "gui.name_analog" : "gui.name_dv", "").trim();
        return n.isEmpty() ? (analog ? "analog" : "tape") : n;
    }

    /** The format chosen for a kind, if it is still offered, else the kind's default. */
    static int format(int kind) {
        int[] offered = Formats.list(kind);
        int f = getInt("gui.format_" + kindKey(kind), -1);
        for (int o : offered) if (o == f) return f;
        for (int o : offered) if ((Core.formatFlags(o) & Core.FMT_DEFAULT) != 0) return o;
        return offered.length > 0 ? offered[0] : -1;
    }

    /** H.264 / HEVC deinterlace and quality: per input kind (analog, DV); -1 = the format's default. */
    static int deinterlace(int kind) { return getInt("gui.deinterlace_" + kindKey(kind), -1); }
    /** The quality slider's top (the core accepts 51; this app stops at 35, where the picture gets poor). */
    static final int QUALITY_MAX = 35;
    static int quality(int kind) {
        int q = getInt("gui.quality_" + kindKey(kind), -1);
        return q < 0 ? -1 : Math.min(q, QUALITY_MAX);
    }
    static int ffv1() { return getInt("gui.ffv1_compression", -1); }

    static int clamp(int v, int lo, int hi) { return Math.max(lo, Math.min(hi, v)); }
}
