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

/**
 * One status snapshot of the open session as Core.getStatus fills it: the numbers and the core's
 * ready-made texts and enable rules (the app never computes a text or a rule itself).
 */
final class Status {
    final int[] i = new int[Core.ST_COUNT];
    final long[] l = new long[Core.SL_COUNT];
    final double[] d = new double[Core.SD_COUNT];
    final String[] s = new String[Core.SS_COUNT];

    Status() {
        i[Core.ST_STATE] = Core.STATE_CLOSED;
        java.util.Arrays.fill(s, "");
        d[Core.SD_PEAK_L] = d[Core.SD_PEAK_R] = -144;
        d[Core.SD_IDLE_LEFT] = d[Core.SD_DURATION_LEFT] = -1;
    }

    boolean read(long session) {
        return session != 0 && Core.getStatus(session, i, l, d, s) == Core.OK;
    }

    int state() { return i[Core.ST_STATE]; }
    int input() { return i[Core.ST_INPUT]; }
    boolean isCapturing() { return i[Core.ST_IS_CAPTURING] != 0; }
    boolean signal() { return i[Core.ST_SIGNAL] != 0; }
    boolean manualAllowed() { return i[Core.ST_MANUAL_ALLOWED] != 0; }
    boolean stopAllowed() { return i[Core.ST_STOP_ALLOWED] != 0; }
    boolean analog() { return i[Core.ST_INPUT] != Core.INPUT_DV; }
    String text(int k) { return s[k] == null ? "" : s[k]; }
}
