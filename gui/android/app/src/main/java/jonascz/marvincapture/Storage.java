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

import android.content.Context;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Where captures go: one entry per mounted storage volume (the phone's shared storage, an SD card, a
 * USB drive). Needs no permission: on a volume the app writes to {@code Movies/MarvinCapture}, the
 * same shared place as on the phone, when Android lets it create files there (tested with a real
 * file); if not, to the app's own folder on that volume ({@code Android/data/<app>/files/Movies}),
 * which always works but is removed with the app. The choice is the setting {@code gui.storage}
 * (the volume's id); a volume that is gone falls back to the phone's storage. Main thread.
 */
final class Storage {
    private Storage() {}

    static final class Volume {
        final String id, label;
        final File dir;
        final boolean shared;   // Movies/MarvinCapture on the volume, else the app's own folder
        final boolean removable;
        final String where;     // the folder as shown to the user (relative to the volume)

        Volume(String id, String label, File dir, boolean shared, boolean removable, String where) {
            this.id = id;
            this.label = label;
            this.dir = dir;
            this.shared = shared;
            this.removable = removable;
            this.where = where;
        }

        String where() {
            return where;
        }
    }

    private static String cacheSig = "";
    private static List<Volume> cache = new ArrayList<>();

    /** Mounted volumes. The (file creating) write test runs again only when the set of volumes or a
     *  state changed. */
    static List<Volume> volumes() {
        Context c = MarvinApp.app;
        StorageManager sm = c.getSystemService(StorageManager.class);
        StringBuilder sig = new StringBuilder();
        for (StorageVolume v : sm.getStorageVolumes()) sig.append(v.getUuid()).append(v.getState()).append(';');
        if (sig.toString().equals(cacheSig)) return cache;
        cache = scan(c, sm);
        cacheSig = sig.toString();
        return cache;
    }

    private static List<Volume> scan(Context c, StorageManager sm) {
        ArrayList<Volume> out = new ArrayList<>();
        File[] appDirs = c.getExternalFilesDirs(Environment.DIRECTORY_MOVIES);
        for (StorageVolume v : sm.getStorageVolumes()) {
            File root = v.getDirectory();
            if (root == null || !Environment.MEDIA_MOUNTED.equals(v.getState())) continue;
            boolean primary = v.isPrimary();
            String id = primary ? "primary" : v.getUuid() != null ? v.getUuid() : root.getName();
            File shared = new File(new File(root, Environment.DIRECTORY_MOVIES), "MarvinCapture");
            boolean sharedOk = canCreateIn(shared);
            File dir = shared;
            if (!sharedOk) {
                dir = null;
                for (File a : appDirs) {
                    if (a != null && a.getPath().startsWith(root.getPath() + "/")) dir = a;
                }
                if (dir == null) continue;
            }
            String label = v.getDescription(c);
            if (primary) label = "Phone storage";
            else if (label == null || label.isEmpty()) label = "SD card";
            String where = sharedOk ? "Movies/MarvinCapture" : dir.getPath().substring(root.getPath().length() + 1);
            out.add(new Volume(id, label, dir, sharedOk, v.isRemovable(), where));
        }
        if (out.isEmpty()) {   // no shared storage mounted: the app's private storage
            out.add(new Volume("primary", "App storage", new File(c.getFilesDir(), "Movies"), false, false,
                    "the app's private folder"));
        }
        return out;
    }

    /** The chosen volume if it is mounted, else the first (the phone's own). */
    static Volume current() {
        List<Volume> all = volumes();
        String id = Settings.get("gui.storage", "primary");
        for (Volume v : all) if (v.id.equals(id)) return v;
        return all.get(0);
    }

    /** Can this app create a file there? Tried for real: only a file proves it (scoped storage). */
    private static boolean canCreateIn(File dir) {
        try {
            if (!dir.isDirectory() && !dir.mkdirs()) return false;
            File t = new File(dir, ".write-test.mkv");
            if (!t.createNewFile() && !t.exists()) return false;
            t.delete();
            return true;
        } catch (IOException | SecurityException e) {
            Log.i(MarvinApp.TAG, "no shared folder on " + dir + ": " + e.getMessage());
            return false;
        }
    }

    /** The files of this app in the folder (newest first): the captures the library shows. */
    static List<File> captures(File dir) {
        ArrayList<File> out = new ArrayList<>();
        File[] all = dir.listFiles();
        if (all == null) return out;
        for (File f : all) {
            if (f.isFile() && !f.getName().startsWith(".") && f.length() > 0) out.add(f);
        }
        out.sort((a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return out;
    }
}
