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
        float iconUnit = ICON_UNITS_DP * icon.getWidth() / ICON_DP;   // px per logo unit in the splash icon

        ValueAnimator v = ValueAnimator.ofFloat(0, 1);
        v.setDuration(FLIGHT_MS);
        v.setInterpolator(new LinearInterpolator());
        v.addUpdateListener(an -> {
            float f = (float) an.getAnimatedValue();
            float e = 1 - (1 - f) * (1 - f) * (1 - f);   // decelerating flight
            // the target is read every frame: the card's text can still change the size of Marvin
            marvin.getLocationOnScreen(b);
            float u = MarvinIdleView.unit(marvin.getWidth(), marvin.getHeight());
            // the icon's picture is 53 units high; the live view reserves more below it (MarvinIdleView.SPAN_H)
            float tx = b[0] + marvin.getWidth() / 2f - iconCx,
                    ty = b[1] + marvin.getHeight() / 2f - (MarvinIdleView.SPAN_H - 53) / 2f * u - iconCy;
            float ts = u / iconUnit;
            icon.setTranslationX(tx * e);
            icon.setTranslationY(ty * e);
            float sc = 1 + (ts - 1) * e;
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
