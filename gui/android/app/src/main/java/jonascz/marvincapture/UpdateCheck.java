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

import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * The update check. The core cannot download the published VERSION file on Android (no libcurl),
 * so this class only does the HTTP request; everything else is the core's (pin_update_*): the
 * address, whether a check is due (at most one a week, "update.last_check" in the settings),
 * parsing and comparing the file, and what is remembered, so the "update available" notice in the
 * options panel stays up between checks. A failed download is silent and tried again at the next
 * start. Main thread only (the core's settings file has one writer); the download runs on a worker.
 */
final class UpdateCheck {
    private UpdateCheck() {}

    private static final int TIMEOUT_MS = 5000;
    private static final int MAX_BYTES = 64 * 1024;

    private static boolean started;
    /** Called on the main thread when a check found something new; the options panel sets it. */
    static Runnable onChange;

    /** At most once per process (and once a week, the core decides): downloads and evaluates the file. */
    static void startIfDue() {
        if (started) return;
        started = true;
        long now = System.currentTimeMillis() / 1000;
        if (!Core.updateDue(now)) return;
        String url = Core.updateUrl();
        if (!url.startsWith("https://") && !url.startsWith("http://")) return;
        Handler main = new Handler(Looper.getMainLooper());
        Thread t = new Thread(() -> {
            byte[] body = download(url);
            if (body == null) return;
            main.post(() -> {
                if (Core.updateCheck(body, System.currentTimeMillis() / 1000) && onChange != null) onChange.run();
            });
        }, "Update check");
        t.setDaemon(true);
        t.start();
    }

    /** {notice, latest version, download address or ""} when a newer version is known, else null. */
    static String[] known() { return Core.updateKnown(); }

    /** Opens the download page in the browser. */
    static void openDownload(Context c, String url) {
        try {
            c.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (ActivityNotFoundException e) {
            Log.w(MarvinApp.TAG, "no browser for " + url);
        }
    }

    /** The file's bytes, or null on any failure (offline, not 200, too big). */
    private static byte[] download(String address) {
        HttpURLConnection con = null;
        try {
            con = (HttpURLConnection) new URL(address).openConnection();
            con.setConnectTimeout(TIMEOUT_MS);
            con.setReadTimeout(TIMEOUT_MS);
            con.setRequestProperty("User-Agent", "MarvinCapture/" + Core.appVersion());
            if (con.getResponseCode() != HttpURLConnection.HTTP_OK) return null;
            try (InputStream in = con.getInputStream()) {
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) {
                    if (out.size() + n > MAX_BYTES) return null;
                    out.write(buf, 0, n);
                }
                return out.toByteArray();
            }
        } catch (IOException | RuntimeException e) {
            Log.d(MarvinApp.TAG, "update check failed: " + e);
            return null;
        } finally {
            if (con != null) con.disconnect();
        }
    }
}
