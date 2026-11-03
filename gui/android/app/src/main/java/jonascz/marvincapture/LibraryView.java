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

import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Typeface;
import android.media.MediaMetadataRetriever;
import android.os.Handler;
import android.os.Looper;
import android.util.LruCache;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.FrameLayout;
import android.widget.GridView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.MediaController;
import android.widget.TextView;
import android.widget.VideoView;

import java.io.File;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Playback, the simple way: the files in the chosen storage place (only this app's captures are
 * visible to it anyway) as a grid of thumbnails, and a basic player. Both use Android's own media
 * stack (MediaMetadataRetriever, VideoView with a MediaController), so what plays is what the
 * phone can decode: H.264 / HEVC in Matroska or MP4 yes; DV, HDV (MPEG-2), FFV1 and uncompressed AVI no
 * (the core's FFmpeg build decodes those for the preview, but has no scaler and no H.264 decoder, so
 * a player of its own would be a project of its own). Such files are listed with a placeholder.
 * Thumbnails are made on two worker threads and cached.
 */
final class LibraryView extends FrameLayout {
    interface Host {
        void closeLibrary();
    }

    private final Host host;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService pool = Executors.newFixedThreadPool(2);
    private final LruCache<String, Bitmap> thumbs = new LruCache<>(48);
    private final LruCache<String, String> infos = new LruCache<>(256);
    private final List<File> files = new ArrayList<>();
    private final LinearLayout list;
    private final TextView subtitle, empty;
    private final GridView grid;
    private final Adapter adapter = new Adapter();
    private final FrameLayout player;
    private final VideoView video;
    private final TextView playerTitle, playerError;
    private final MediaController controller;
    private final LinearLayout playerBar;
    private boolean shown;
    private int insL, insT, insR, insB;
    private final float dp;

    LibraryView(Context c, Host host) {
        super(c);
        this.host = host;
        dp = c.getResources().getDisplayMetrics().density;
        setBackgroundColor(Ui.PANEL);
        setClickable(true);   // nothing under it gets touches

        list = Ui.column(c);
        LinearLayout bar = Ui.row(c);
        bar.setPadding(Ui.dp(c, 16), Ui.dp(c, 8), Ui.dp(c, 12), Ui.dp(c, 4));
        LinearLayout titles = Ui.column(c);
        TextView title = Ui.text(c, "Library", 20, Ui.TEXT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        subtitle = Ui.text(c, "", 12.5f, Ui.TEXT2);
        subtitle.setSingleLine(true);
        subtitle.setEllipsize(android.text.TextUtils.TruncateAt.END);
        titles.addView(title);
        titles.addView(subtitle);
        bar.addView(titles, Ui.weight(1));
        Glyphs.Button close = new Glyphs.Button(c, Glyphs.CLOSE, 44);
        close.setContentDescription("Close the library");
        close.setOnClickListener(v -> host.closeLibrary());
        bar.addView(close);
        list.addView(bar, Ui.fillWidth());

        FrameLayout body = new FrameLayout(c);
        grid = new GridView(c);
        grid.setNumColumns(GridView.AUTO_FIT);
        grid.setColumnWidth(Ui.dp(c, 180));
        grid.setStretchMode(GridView.STRETCH_COLUMN_WIDTH);
        grid.setHorizontalSpacing(Ui.dp(c, 10));
        grid.setVerticalSpacing(Ui.dp(c, 12));
        grid.setClipToPadding(false);
        grid.setPadding(Ui.dp(c, 16), Ui.dp(c, 6), Ui.dp(c, 16), Ui.dp(c, 16));
        grid.setAdapter(adapter);
        grid.setOnItemClickListener((p, v, pos, id) -> play(files.get(pos)));
        grid.setOnItemLongClickListener((p, v, pos, id) -> {
            confirmDelete(files.get(pos));
            return true;
        });
        body.addView(grid, new FrameLayout.LayoutParams(-1, -1));
        empty = Ui.text(c, "No captures here yet.", 15, Ui.TEXT2);
        empty.setGravity(Gravity.CENTER);
        body.addView(empty, new FrameLayout.LayoutParams(-1, -1));
        list.addView(body, new LinearLayout.LayoutParams(-1, 0, 1));
        addView(list, new FrameLayout.LayoutParams(-1, -1));

        // the player
        player = new FrameLayout(c);
        player.setBackgroundColor(Color.BLACK);
        player.setClickable(true);
        video = new VideoView(c);
        player.addView(video, new FrameLayout.LayoutParams(-1, -1, Gravity.CENTER));
        playerError = Ui.text(c, "", 15, 0xffe8e8ec);
        playerError.setGravity(Gravity.CENTER);
        playerError.setPadding(Ui.dp(c, 32), 0, Ui.dp(c, 32), 0);
        playerError.setVisibility(GONE);
        player.addView(playerError, new FrameLayout.LayoutParams(-1, -1));
        playerBar = Ui.row(c);
        playerBar.setBackgroundColor(0x99000000);
        Glyphs.Button back = new Glyphs.Button(c, Glyphs.BACK, 44);
        back.setColors(0x66000000, 0xffffffff);
        back.setContentDescription("Back to the library");
        back.setOnClickListener(v -> stopPlayer());
        playerBar.addView(back);
        playerTitle = Ui.text(c, "", 15, 0xffffffff);
        playerTitle.setSingleLine(true);
        playerTitle.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        playerTitle.setPadding(Ui.dp(c, 12), 0, Ui.dp(c, 12), 0);
        playerBar.addView(playerTitle, Ui.weight(1));
        player.addView(playerBar, new FrameLayout.LayoutParams(-1, -2, Gravity.TOP));
        controller = new MediaController(c);
        video.setMediaController(controller);
        video.setOnPreparedListener(mp -> {
            mp.setLooping(false);
            video.start();
            controller.show(3000);
        });
        video.setOnErrorListener((mp, what, extra) -> {
            video.stopPlayback();
            video.setVisibility(INVISIBLE);
            playerError.setText("This phone cannot play this file.\n\nAndroid's player handles H.264 and "
                    + "HEVC; DV, HDV, FFV1 and uncompressed AVI captures play on a computer.");
            playerError.setVisibility(VISIBLE);
            return true;
        });
        player.setVisibility(GONE);
        addView(player, new FrameLayout.LayoutParams(-1, -1));
        setVisibility(GONE);
    }

    /** The system bar and cutout insets (pixels). */
    void setInsets(int l, int t, int r, int b) {
        insL = l; insT = t; insR = r; insB = b;
        list.setPadding(l, t, r, b);
        playerBar.setPadding(l, t, r, 0);
        video.setPadding(0, 0, 0, 0);
    }

    /** Slides up over the screen (and down again), like the options: the system's animation scale applies. */
    void open() {
        reload();
        shown = true;
        animate().cancel();
        setVisibility(VISIBLE);
        setTranslationY((getHeight() > 0 ? getHeight() : Ui.dp(getContext(), 600)) * 0.4f);
        setAlpha(0f);
        animate().translationY(0).alpha(1f).setDuration(260)
                .setInterpolator(new android.view.animation.DecelerateInterpolator());
    }

    void close() {
        stopPlayer();
        if (!shown) return;
        shown = false;
        animate().cancel();
        animate().translationY(getHeight() * 0.4f).alpha(0f).setDuration(200)
                .setInterpolator(new android.view.animation.AccelerateInterpolator())
                .withEndAction(() -> {
                    if (shown) return;   // opened again meanwhile
                    setVisibility(GONE);
                    setTranslationY(0);
                    setAlpha(1f);
                });
    }

    void pause() {
        if (player.getVisibility() == VISIBLE) video.pause();
    }

    /** Open or opening (not while it slides away). */
    boolean isOpen() { return shown; }

    /** Back: ends the player first, then the library. */
    void back() {
        if (player.getVisibility() == VISIBLE) stopPlayer();
        else host.closeLibrary();
    }

    void reload() {
        Storage.Volume v = Storage.current();
        subtitle.setText(v.label + " · " + v.where());
        files.clear();
        files.addAll(Storage.captures(v.dir));
        adapter.notifyDataSetChanged();
        empty.setVisibility(files.isEmpty() ? VISIBLE : GONE);
    }

    private void play(File f) {
        playerTitle.setText(f.getName());
        playerError.setVisibility(GONE);
        video.setVisibility(VISIBLE);
        player.setVisibility(VISIBLE);
        controller.setAnchorView(video);
        video.setVideoPath(f.getPath());
    }

    private void stopPlayer() {
        if (player.getVisibility() != VISIBLE) return;
        controller.hide();
        video.stopPlayback();
        player.setVisibility(GONE);
    }

    private void confirmDelete(File f) {
        new AlertDialog.Builder(getContext()).setTitle("Delete this capture?")
                .setMessage(f.getName() + "\n" + Core.formatBytes(f.length()) + "\n\nThe file is removed from the storage.")
                .setPositiveButton("Delete", (d, w) -> {
                    if (!f.delete()) {
                        new AlertDialog.Builder(getContext()).setMessage("Could not delete " + f.getName())
                                .setPositiveButton("OK", null).show();
                    }
                    reload();
                })
                .setNegativeButton("Cancel", null).show();
    }

    // ---- thumbnails ----------------------------------------------------------------------------

    private static String key(File f) {
        return f.getPath() + "@" + f.lastModified() + "/" + f.length();
    }

    private void load(File f, ImageView image, TextView info, String k) {
        pool.execute(() -> {
            Bitmap bmp = null;
            long durMs = -1;
            MediaMetadataRetriever r = new MediaMetadataRetriever();
            try {
                r.setDataSource(f.getPath());
                String d = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION);
                if (d != null) durMs = Long.parseLong(d);
                bmp = r.getScaledFrameAtTime(Math.min(1_000_000L, Math.max(0, durMs) * 500L),
                        MediaMetadataRetriever.OPTION_CLOSEST_SYNC, Ui.dp(getContext(), 240), Ui.dp(getContext(), 135));
            } catch (Exception e) {
                // unsupported container or codec: the placeholder stays
            } finally {
                try { r.release(); } catch (Exception ignored) {}
            }
            Bitmap b = bmp;
            String text = describe(f, durMs);
            ui.post(() -> {
                if (b != null) thumbs.put(k, b);
                infos.put(k, text);
                if (k.equals(image.getTag())) {
                    if (b != null) {
                        image.setImageBitmap(b);
                        image.setScaleType(ImageView.ScaleType.CENTER_CROP);
                    }
                    info.setText(text);
                }
            });
        });
    }

    private static String describe(File f, long durMs) {
        StringBuilder sb = new StringBuilder(Core.formatBytes(f.length()));
        if (durMs >= 0) {
            long s = durMs / 1000;
            sb.append(" · ").append(s >= 3600 ? String.format(Locale.ROOT, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60)
                    : String.format(Locale.ROOT, "%d:%02d", s / 60, s % 60));
        }
        sb.append("\n").append(DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(new Date(f.lastModified())));
        return sb.toString();
    }

    private final class Adapter extends BaseAdapter {
        @Override public int getCount() { return files.size(); }
        @Override public Object getItem(int i) { return files.get(i); }
        @Override public long getItemId(int i) { return i; }

        @Override
        public View getView(int pos, View convert, ViewGroup parent) {
            Context c = parent.getContext();
            LinearLayout cell;
            ImageView image;
            TextView name, info;
            if (convert == null) {
                cell = Ui.column(c);
                cell.setBackgroundColor(Ui.CARD);
                cell.setTag(new View[3]);
                image = new ImageView(c);
                                image.setBackgroundColor(Color.BLACK);
                cell.addView(image, new LinearLayout.LayoutParams(-1, Ui.dp(c, 100)));
                ((View[]) cell.getTag())[0] = image;
                name = Ui.text(c, "", 13, Ui.TEXT);
                                name.setSingleLine(true);
                name.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
                name.setTypeface(Typeface.DEFAULT_BOLD);
                name.setPadding(Ui.dp(c, 8), Ui.dp(c, 6), Ui.dp(c, 8), 0);
                cell.addView(name, Ui.fillWidth());
                ((View[]) cell.getTag())[1] = name;
                info = Ui.text(c, "", 12, Ui.TEXT2);
                                info.setMinLines(2);
                info.setPadding(Ui.dp(c, 8), 0, Ui.dp(c, 8), Ui.dp(c, 8));
                cell.addView(info, Ui.fillWidth());
                ((View[]) cell.getTag())[2] = info;
            } else {
                cell = (LinearLayout) convert;
            }
            View[] parts = (View[]) cell.getTag();
            image = (ImageView) parts[0];
            name = (TextView) parts[1];
            info = (TextView) parts[2];
            File f = files.get(pos);
            String k = key(f);
            name.setText(f.getName());
            image.setTag(k);
            Bitmap b = thumbs.get(k);
            String text = infos.get(k);
            if (b != null) {
                image.setImageBitmap(b);
                image.setScaleType(ImageView.ScaleType.CENTER_CROP);
                info.setText(text);
            } else {
                image.setImageDrawable(new Placeholder(f.getName()));
                image.setScaleType(ImageView.ScaleType.FIT_XY);
                info.setText(text != null ? text : Core.formatBytes(f.length()) + "\n");
                if (text == null) load(f, image, info, k);
            }
            return cell;
        }
    }

    /** The extension on dark grey: for files without a thumbnail. */
    private static final class Placeholder extends android.graphics.drawable.Drawable {
        private final String text;
        private final android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);

        Placeholder(String name) {
            int dot = name.lastIndexOf('.');
            text = dot >= 0 ? name.substring(dot + 1).toUpperCase(Locale.ROOT) : "FILE";
            paint.setTextAlign(android.graphics.Paint.Align.CENTER);
            paint.setTypeface(Typeface.DEFAULT_BOLD);
        }

        @Override
        public void draw(android.graphics.Canvas c) {
            android.graphics.Rect r = getBounds();
            paint.setColor(0xff22252b);
            c.drawRect(r, paint);
            paint.setColor(0xff8a8d93);
            paint.setTextSize(r.height() * 0.28f);
            c.drawText(text, r.exactCenterX(), r.exactCenterY() + paint.getTextSize() * 0.35f, paint);
        }

        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter cf) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.OPAQUE; }
    }
}
