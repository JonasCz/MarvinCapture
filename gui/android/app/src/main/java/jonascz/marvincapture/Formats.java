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

import android.os.Handler;
import android.os.Looper;

import java.util.ArrayList;

/**
 * The output formats the core offers per kind (pin_formats). Which H.264 / HEVC formats exist
 * depends on pin_encoders_probe(), which opens the phone's encoders and can take a second: it runs
 * once on a worker thread at start-up ({@link #probe}), and the lists are read only after it
 * finished ({@link #whenReady}), so the main thread never waits for it.
 */
final class Formats {
    private Formats() {}

    private static volatile boolean ready;
    private static final ArrayList<Runnable> waiting = new ArrayList<>();
    private static final int[][] lists = new int[3][];

    static void probe() {
        Thread t = new Thread(() -> {
            int enc = Core.encodersProbe();
            int[][] l = { Core.formats(Core.KIND_ANALOG), Core.formats(Core.KIND_DV), Core.formats(Core.KIND_HDV) };
            new Handler(Looper.getMainLooper()).post(() -> {
                System.arraycopy(l, 0, lists, 0, 3);
                ready = true;
                android.util.Log.i("MarvinCapture", "encoders probed: mask " + enc);
                for (Runnable r : waiting) r.run();
                waiting.clear();
            });
        }, "Encoder probe");
        t.setPriority(Thread.MIN_PRIORITY);
        t.start();
    }

    static boolean isReady() { return ready; }

    /** Runs r on the main thread once the probe is done (at once if it is). */
    static void whenReady(Runnable r) {
        if (ready) r.run();
        else waiting.add(r);
    }

    /** The kind's formats in display order; empty before the probe finished. Main thread. */
    static int[] list(int kind) {
        int[] l = ready ? lists[kind] : null;
        return l != null ? l : new int[0];
    }
}
