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

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.animation.LinearInterpolator;
import android.window.SplashScreenView;

/**
 * The cold start's exit from the system splash screen (Android 12+; older versions just show the
 * card's colour, see the theme). The splash shows the whole Marvin (drawable/ic_splash_marvin.xml,
 * laid out like MarvinIdleView draws him) in the middle of the window; once the first frame is
 * ready the system hands over the splash view, and here its icon flies and scales onto the live
 * {@link MarvinIdleView} (top of the screen in portrait, left in landscape) while the splash's
 * background fades to the layout under it. In the last moment the two Marvins cross-fade, and the
 * live one takes over with his idle animation.
 *
 * The live view stays invisible until then; every way out (no splash view, animations off, an OEM
 * that never calls back) ends with it visible.
 */
final class SplashExit {
    private static final long FLIGHT_MS = 520, SAFETY_MS = 3000;
    private static final float ICON_DP = 288, ICON_UNITS_DP = 2.5f;   // ic_splash_marvin: size, dp per logo unit

    private final MarvinIdleView marvin;
    private boolean done;

    private SplashExit(MarvinIdleView marvin) {
        this.marvin = marvin;
    }

    static void install(Activity a, MarvinIdleView marvin) {
        SplashExit x = new SplashExit(marvin);
        marvin.setAlpha(0);
        // no callback (a launcher or skin that skips the splash): never leave Marvin invisible
        new Handler(Looper.getMainLooper()).postDelayed(x::finish, SAFETY_MS);
        a.getSplashScreen().setOnExitAnimationListener(x::run);
    }

    private void finish() {
        done = true;
        marvin.setAlpha(1);
    }

    /** The box {left, top, right, bottom} (view pixels) of what the view draws, or null when nothing. */
    private static float[] drawnBox(View v) {
        int w = v.getWidth(), h = v.getHeight();
        if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return null;
        try {
            Bitmap bm = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            v.draw(new Canvas(bm));
            int[] px = new int[w * h];
            bm.getPixels(px, 0, w, 0, 0, w, h);
            bm.recycle();
            int x0 = w, y0 = h, x1 = -1, y1 = -1;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    if ((px[y * w + x] >>> 24) > 40) {
                        if (x < x0) x0 = x;
                        if (x > x1) x1 = x;
                        if (y < y0) y0 = y;
                        if (y > y1) y1 = y;
                    }
            return x1 - x0 < w / 8 ? null : new float[] { x0, y0, x1 + 1, y1 + 1 };
        } catch (RuntimeException | OutOfMemoryError e) {
            return null;
        }
    }

    private void run(SplashScreenView splash) {
        View icon = splash.getIconView();
        if (done || icon == null || icon.getWidth() == 0 || !ValueAnimator.areAnimatorsEnabled()
                || !marvin.isShown() || marvin.getWidth() == 0) {
            finish();
            splash.remove();
            return;
        }
        Drawable bg = splash.getBackground() != null ? splash.getBackground().mutate() : null;
        int bgAlpha = bg != null ? bg.getAlpha() : 255;
        int[] a = new int[2], b = new int[2];
        icon.getLocationOnScreen(a);
        float iconCx = a[0] + icon.getWidth() / 2f, iconCy = a[1] + icon.getHeight() / 2f;
        // Where Marvin really is in the splash icon, measured on what the view draws (the system may
        // size or scale the icon differently from its 288 dp): his body's outer box is logo x 4..60,
        // its top is logo y 4 (+ the drop). k = px per logo unit; (px0, py0) = screen position of the
        // logo point (32, 28), which stays put while the body bobs and which the flight pins down.
        float kk = icon.getWidth() / ICON_DP, k = ICON_UNITS_DP * kk;
        float left = a[0] + (64 + 4 * ICON_UNITS_DP) * kk, top = a[1] + (71.5f + 6.5f * ICON_UNITS_DP) * kk;
        float[] box = drawnBox(icon);
        if (box != null) {
            k = (box[2] - box[0]) / 56f;
            left = a[0] + box[0];
            top = a[1] + box[1];
        }
        final float iconUnit = k, px0 = left + 28 * k, py0 = top + 24 * k;

        ValueAnimator v = ValueAnimator.ofFloat(0, 1);
        v.setDuration(FLIGHT_MS);
        v.setInterpolator(new LinearInterpolator());
        v.addUpdateListener(an -> {
            float f = (float) an.getAnimatedValue();
            float e = 1 - (1 - f) * (1 - f) * (1 - f);   // decelerating flight
            // the target is read every frame: the card's text can still change the size of Marvin
            marvin.getLocationOnScreen(b);
            float u = MarvinIdleView.unit(marvin.getWidth(), marvin.getHeight());
            // where the live view draws logo point (32, 28): see MarvinIdleView.onDraw
            float lx = b[0] + marvin.getWidth() / 2f, ly = b[1] + marvin.getHeight() / 2f - 5 * u;
            float sc = 1 + (u / iconUnit - 1) * e;
            // the icon scales about its centre; translate so that the point lands on the path A -> live
            float cx = px0 + (lx - px0) * e, cy = py0 + (ly - py0) * e;
            icon.setTranslationX(cx - iconCx - sc * (px0 - iconCx));
            icon.setTranslationY(cy - iconCy - sc * (py0 - iconCy));
            icon.setScaleX(sc);
            icon.setScaleY(sc);
            if (bg != null) bg.setAlpha(Math.round(bgAlpha * (1 - e)));
            float swap = f < 0.8f ? 0 : (f - 0.8f) / 0.2f;   // the hand-over in the last fifth
            icon.setAlpha(1 - swap);
            marvin.setAlpha(swap);
        });
        v.addListener(new AnimatorListenerAdapter() {
            private boolean ended;
            private void end() {
                if (ended) return;
                ended = true;
                finish();
                splash.remove();
            }
            @Override public void onAnimationEnd(Animator an) { end(); }
            @Override public void onAnimationCancel(Animator an) { end(); }
        });
        v.start();
    }
}
