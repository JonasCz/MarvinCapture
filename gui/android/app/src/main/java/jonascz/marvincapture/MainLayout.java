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
import android.view.View;
import android.view.ViewGroup;

/**
 * The screen's two parts, camera-app style: the preview (child 0, a {@link PreviewFrame}) and the
 * side panel (child 1: status, meters, buttons, or the options). Landscape (wider than tall): the
 * preview on the left, as wide as the picture's aspect allows at full height, the panel on the
 * right. Portrait: the preview on top, as tall as the aspect allows at full width, the panel below.
 * The panel keeps at least a minimum size; the preview then letterboxes inside its part (the
 * renderer's pin_fit_rect).
 *
 * Edge to edge: both parts reach the screen's edges. The preview frame extends behind the status
 * bar and the display cutout (its background shows there; the picture stays inside the insets),
 * the panel takes the insets of the sides it touches as padding.
 */
final class MainLayout extends ViewGroup {
    private final float dp;
    private int darNum = 4, darDen = 3;
    private int insL, insT, insR, insB;
    private boolean land;

    MainLayout(Context c) {
        super(c);
        dp = getResources().getDisplayMetrics().density;
    }

    /** The display aspect of the picture (4:3 until a frame says otherwise). */
    void setAspect(int num, int den) {
        if (num <= 0 || den <= 0) { num = 4; den = 3; }
        if (num * darDen == den * darNum) return;
        darNum = num;
        darDen = den;
        requestLayout();
    }

    /** The system bar and cutout insets in pixels. */
    void setInsets(int l, int t, int r, int b) {
        if (l == insL && t == insT && r == insR && b == insB) return;
        insL = l; insT = t; insR = r; insB = b;
        requestLayout();
    }

    boolean isLandscape() {
        return land;
    }

    @Override
    protected void onMeasure(int wSpec, int hSpec) {
        int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
        int iw = Math.max(0, w - insL - insR);
        int ih = Math.max(0, h - insT - insB);
        land = w > h;
        PreviewFrame preview = (PreviewFrame) getChildAt(0);
        View panel = getChildAt(1);
        int pw, ph, sw, sh;
        if (land) {
            int minPanel = (int) Math.min(w * 0.45f, 260 * dp);
            pw = Math.min(insL + Math.round((float) ih * darNum / darDen), w - minPanel);
            ph = h;
            sw = w - pw;
            sh = h;
            preview.setInsets(insL, insT, 0, insB);
            panel.setPadding(0, insT, insR, insB);
        } else {
            int minPanel = (int) Math.min(h * 0.55f, 330 * dp);
            pw = w;
            ph = Math.min(insT + Math.round((float) iw * darDen / darNum), h - minPanel);
            sw = w;
            sh = h - ph;
            preview.setInsets(insL, insT, insR, 0);
            panel.setPadding(insL, 0, insR, insB);
        }
        preview.measure(MeasureSpec.makeMeasureSpec(pw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(ph, MeasureSpec.EXACTLY));
        panel.measure(MeasureSpec.makeMeasureSpec(sw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(sh, MeasureSpec.EXACTLY));
        setMeasuredDimension(w, h);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        View preview = getChildAt(0), panel = getChildAt(1);
        int pw = preview.getMeasuredWidth(), ph = preview.getMeasuredHeight();
        preview.layout(0, 0, pw, ph);
        if (land) panel.layout(pw, 0, pw + panel.getMeasuredWidth(), panel.getMeasuredHeight());
        else panel.layout(0, ph, panel.getMeasuredWidth(), ph + panel.getMeasuredHeight());
    }

    /**
     * The preview part: children are the picture surface (child 0), the no-video card (child 1)
     * and a badge (child 2, top left). The card fills the whole frame, under the bars and the
     * cutout, with its content kept inside the insets; the surface and the badge stay inside the
     * insets, so the picture is never covered by the notch.
     */
    static final class PreviewFrame extends ViewGroup {
        private int l, t, r, b;

        PreviewFrame(Context c) {
            super(c);
        }

        void setInsets(int l, int t, int r, int b) {
            if (l == this.l && t == this.t && r == this.r && b == this.b) return;
            this.l = l; this.t = t; this.r = r; this.b = b;
            View card = getChildAt(1);
            if (card != null) {   // the card's content stays inside the insets
                int pad = (int) (20 * getResources().getDisplayMetrics().density);
                card.setPadding(l + pad, t + pad, r + pad, b + pad);
            }
            requestLayout();
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            int iw = Math.max(0, w - l - r), ih = Math.max(0, h - t - b);
            getChildAt(0).measure(MeasureSpec.makeMeasureSpec(iw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(ih, MeasureSpec.EXACTLY));
            getChildAt(1).measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY));
            getChildAt(2).measure(MeasureSpec.makeMeasureSpec(iw, MeasureSpec.AT_MOST), MeasureSpec.makeMeasureSpec(ih, MeasureSpec.AT_MOST));
            setMeasuredDimension(w, h);
        }

        @Override
        protected void onLayout(boolean changed, int x0, int y0, int x1, int y1) {
            int w = x1 - x0, h = y1 - y0;
            getChildAt(0).layout(l, t, w - r, h - b);
            getChildAt(1).layout(0, 0, w, h);
            View badge = getChildAt(2);
            int m = (int) (12 * getResources().getDisplayMetrics().density);
            badge.layout(l + m, t + m, l + m + badge.getMeasuredWidth(), t + m + badge.getMeasuredHeight());
        }
    }
}
