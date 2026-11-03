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

import android.app.Application;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * Process start-up (port of the desktop apps' Startup): loads the native libraries, gives the core
 * the app's directories before any other core call (pin_set_app_dirs: settings in filesDir, the
 * lock files and the HDV remux temp file in cacheDir; without a runtime dir pin_open fails),
 * copies the FPGA bitstreams out of the APK on the first start of each installed version
 * (pin_set_firmware_dir), and starts the encoder probe. The preview renderer and the audio
 * monitor are process-wide native objects created here.
 */
public final class MarvinApp extends Application {
    static final String TAG = "MarvinCapture";

    /** Why the core cannot be used (library or API version mismatch), or null. */
    static String startupError;
    static long renderer;
    static long monitor;
    static MarvinApp app;

    @Override
    public void onCreate() {
        super.onCreate();
        app = this;
        try {
            System.loadLibrary("marvin-core");
            System.loadLibrary("marvin-jni");
        } catch (UnsatisfiedLinkError e) {
            startupError = "The native library could not be loaded: " + e.getMessage();
            Log.e(TAG, startupError);
            return;
        }
        if (Core.apiVersion() != Core.API_VERSION) {
            startupError = "The native library has API version " + Core.apiVersion() + "; this app needs "
                    + Core.API_VERSION + ".";
            return;
        }
        Core.setAppDirs(getFilesDir().getPath(), getCacheDir().getPath(), getCacheDir().getPath());
        File fw = new File(getFilesDir(), "firmware");
        try {
            copyFirmware(fw);
        } catch (IOException e) {
            Log.e(TAG, "firmware copy failed", e);
        }
        Core.setFirmwareDir(fw.getPath());
        Core.setCpuKeepBusy(Settings.cpuKeepBusy() ? Core.KEEP_BUSY_AUTO : Core.KEEP_BUSY_OFF);
        renderer = Core.previewCreate();
        monitor = Core.audioCreate();
        Formats.probe();
        Log.i(TAG, Core.versionString() + ", app " + Core.appVersion());
    }

    /** Copies assets/firmware/* to dir once per installed APK (stamped with its update time). */
    private void copyFirmware(File dir) throws IOException {
        String stamp;
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(getPackageName(), 0);
            stamp = pi.getLongVersionCode() + " " + pi.versionName + " " + pi.lastUpdateTime;
        } catch (Exception e) {
            stamp = "unknown";
        }
        File stampFile = new File(dir, ".stamp");
        if (stampFile.isFile()
                && new String(Files.readAllBytes(stampFile.toPath()), StandardCharsets.UTF_8).equals(stamp)) {
            return;
        }
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        AssetManager am = getAssets();
        String[] names = am.list("firmware");
        if (names == null) return;
        byte[] buf = new byte[64 * 1024];
        for (String n : names) {
            File tmp = new File(dir, n + ".part");
            try (InputStream in = am.open("firmware/" + n); OutputStream out = new FileOutputStream(tmp)) {
                int k;
                while ((k = in.read(buf)) > 0) out.write(buf, 0, k);
            }
            if (!tmp.renameTo(new File(dir, n))) throw new IOException("cannot write " + n);
        }
        Files.write(stampFile.toPath(), stamp.getBytes(StandardCharsets.UTF_8));
        Log.i(TAG, "firmware copied to " + dir);
    }
}
