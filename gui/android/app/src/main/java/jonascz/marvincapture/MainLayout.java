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
 * The screen's parts, camera-app style: the preview (child 0, a {@link PreviewFrame}), the controls
 * (child 1: input, status, meters, buttons) and the options holder (child 2, the options panel when
 * it is open, or always on a tablet). The picture is always left aligned (landscape) or top aligned
 * (portrait), at its own aspect.
 *
 * <ul>
 * <li>Portrait: the preview on top, as tall as the aspect allows at full width, the controls below.</li>
 * <li>Landscape phone: the controls dock in the free space right of the picture while that is at least
 * {@link #PANEL_MIN_DP} wide. On a wider phone they would get too narrow: then they keep that width
 * and overlay the right end of the picture ("overlay" mode; the activity makes the panel
 * translucent, and invisible but for the buttons while capturing). The no-video card is centred in
 * what the panel leaves free.</li>
 * <li>Tablet (landscape 840 dp wide and 600 dp tall, or portrait 600 x 900 dp): the controls and the
 * options are both on screen all the time ("dual" mode): in landscape one column right of the
 * picture, in portrait two columns under it.</li>
 * </ul>
 *
 * Edge to edge: all parts reach the screen's edges; the insets of the system bars and the cutout are
 * padding of the parts they touch, and the picture and the card's content stay inside them.
 */
final class MainLayout extends ViewGroup {
    /** The narrowest the controls get before they overlay the picture instead of docking. */
    static final int PANEL_MIN_DP = 300;
    private static final int TABLET_SIDE_DP = 380;

    interface ModeListener {
        void onModeChanged(boolean overlay, boolean dual);
    }

    private float dp;   // read again on every measure: the density can change (display size setting)
    private int darNum = 4, darDen = 3;
    private int insL, insT, insR, insB;
    private boolean land, overlay, dual;
    private boolean bleed;   // the picture goes behind the status and navigation bars (not under a tablet's taskbar)
    private boolean reportedOverlay, reportedDual;
    private ModeListener listener;
    // rects computed by onMeasure: x, y, w, h
    private final int[] rPreview = new int[4], rControls = new int[4], rOptions = new int[4], rBox = new int[4];
    private int overlayW;

    MainLayout(Context c) {
        super(c);
        dp = getResources().getDisplayMetrics().density;
    }

    void setModeListener(ModeListener l) {
        listener = l;
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

    boolean isLandscape() { return land; }
    /** The controls float over the picture (wide phone in landscape). */
    boolean isOverlay() { return overlay; }
    /** Tablet: controls and options both stay on screen. */
    boolean isDual() { return dual; }

    /** Fits the picture's aspect into the area (aw x ah at ax, ay) and stores it in rBox. */
    private void fit(int ax, int ay, int aw, int ah, boolean centerX, boolean centerY) {
        int bw = aw, bh = ah;
        if ((long) aw * darDen > (long) ah * darNum) bw = Math.round((float) ah * darNum / darDen);
        else bh = Math.round((float) aw * darDen / darNum);
        rBox[0] = ax + (centerX ? (aw - bw) / 2 : 0);
        rBox[1] = ay + (centerY ? (ah - bh) / 2 : 0);
        rBox[2] = bw;
        rBox[3] = bh;
    }

    private static void set(int[] r, int x, int y, int w, int h) {
        r[0] = x; r[1] = y; r[2] = Math.max(0, w); r[3] = Math.max(0, h);
    }

    @Override
    protected void onMeasure(int wSpec, int hSpec) {
        dp = getResources().getDisplayMetrics().density;
        int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
        int iw = Math.max(0, w - insL - insR), ih = Math.max(0, h - insT - insB);
        land = w > h;
        float wdp = w / dp, hdp = h / dp;
        dual = land ? wdp >= 840 && hdp >= 600 : wdp >= 600 && hdp >= 900;
        bleed = getResources().getConfiguration().smallestScreenWidthDp < 600;
        // the picture's own vertical insets: none when it may go behind the bars
        int pt = bleed ? 0 : insT, pb = bleed ? 0 : insB;
        int pih = Math.max(0, h - pt - pb);
        overlay = false;
        PreviewFrame preview = (PreviewFrame) getChildAt(0);
        View controls = getChildAt(1), options = getChildAt(2);
        int cardRight = 0;   // the part of the preview frame the panel covers (card content avoids it)

        if (land && dual) {
            int sideW = Math.round(TABLET_SIDE_DP * dp) + insR;
            int ctlH = Math.round(Math.max(360 * dp, Math.min(hdp * 0.5f * dp, 440 * dp)));
            set(rPreview, 0, 0, w - sideW, h);
            fit(insL, insT, w - sideW - insL, ih, false, true);
            set(rControls, w - sideW, 0, sideW, insT + ctlH);
            set(rOptions, w - sideW, insT + ctlH, sideW, h - insT - ctlH);
            padControls(controls, 0, insT, insR, 0);
            options.setPadding(0, 0, insR, insB);
        } else if (land) {
            int picW = Math.round((float) pih * darNum / darDen);
            if (picW > iw) picW = iw;
            int free = w - insL - picW;
            int minPanel = Math.round(PANEL_MIN_DP * dp);
            if (free >= minPanel + insR) {
                set(rPreview, 0, 0, insL + picW, h);
                fit(insL, pt, picW, pih, false, false);
                set(rControls, insL + picW, 0, w - insL - picW, h);
            } else {
                overlay = true;
                overlayW = minPanel + insR;
                set(rPreview, 0, 0, w, h);
                fit(insL, pt, iw, pih, false, true);
                set(rControls, w - overlayW, 0, overlayW, h);
                cardRight = overlayW - insR;
            }
            set(rOptions, rControls[0], 0, rControls[2], h);
            padControls(controls, 0, insT, insR, insB);
            options.setPadding(0, insT, insR, insB);
        } else if (dual) {
            int ph = Math.min(insT + Math.round((float) iw * darDen / darNum), Math.round(h * 0.55f));
            set(rPreview, 0, 0, w, ph);
            fit(insL, insT, iw, ph - insT, true, false);
            int colW = Math.min(Math.round(w * 0.45f), Math.round(440 * dp));
            set(rControls, 0, ph, colW, h - ph);
            set(rOptions, colW, ph, w - colW, h - ph);
            padControls(controls, insL, 0, 0, insB);
            options.setPadding(0, 0, insR, insB);
        } else {
            int minPanel = (int) Math.min(h * 0.55f, 330 * dp);
            int ph = Math.min(pt + Math.round((float) iw * darDen / darNum), h - minPanel);
            set(rPreview, 0, 0, w, ph);
            fit(insL, pt, iw, ph - pt, true, false);
            set(rControls, 0, ph, w, h - ph);
            set(rOptions, 0, ph, w, h - ph);
            padControls(controls, insL, 0, insR, insB);
            options.setPadding(insL, 0, insR, insB);
        }
        preview.setGeometry(rBox[0], rBox[1], rBox[2], rBox[3], insL, insT, insR + cardRight, insB);
        measureExact(preview, rPreview);
        measureExact(controls, rControls);
        measureExact(options, rOptions);
        setMeasuredDimension(w, h);
        if (listener != null && (overlay != reportedOverlay || dual != reportedDual)) {
            reportedOverlay = overlay;
            reportedDual = dual;
            post(() -> listener.onModeChanged(reportedOverlay, reportedDual));
        }
    }

    /** The controls' own padding (16 dp at the sides, 8 dp above and below) plus the insets. */
    private void padControls(View v, int l, int t, int r, int b) {
        int h = Math.round(16 * dp), vv = Math.round(8 * dp);
        v.setPadding(l + h, t + vv, r + h, b + vv);
    }

    private static void measureExact(View v, int[] r) {
        v.measure(MeasureSpec.makeMeasureSpec(r[2], MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(r[3], MeasureSpec.EXACTLY));
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        place(getChildAt(0), rPreview);
        place(getChildAt(1), rControls);
        place(getChildAt(2), rOptions);
    }

    private static void place(View v, int[] r) {
        v.layout(r[0], r[1], r[0] + r[2], r[1] + r[3]);
    }

    /**
     * The preview part: children are the picture surface (child 0), the no-video card (child 1) and a
     * badge (child 2, top left of the picture). The surface sits in the box MainLayout computed; the
     * card fills the whole frame, under the bars and the cutout (and the overlay panel), with its
     * content kept inside the insets and out of the panel's way.
     */
    static final class PreviewFrame extends ViewGroup {
        private int bx, by, bw, bh;
        private int l, t, r, b;
        private int topScrim, bottomScrim;

        PreviewFrame(Context c) {
            super(c);
        }

        void setGeometry(int bx, int by, int bw, int bh, int l, int t, int r, int b) {
            if (bx == this.bx && by == this.by && bw == this.bw && bh == this.bh
                    && l == this.l && t == this.t && r == this.r && b == this.b) return;
            this.bx = bx; this.by = by; this.bw = bw; this.bh = bh;
            this.l = l; this.t = t; this.r = r; this.b = b;
            View card = getChildAt(1);
            if (card != null) {   // the card's content stays inside the insets, clear of the panel
                int pad = (int) (20 * getResources().getDisplayMetrics().density);
                card.setPadding(l + pad, t + pad, r + pad, b + pad);
            }
            requestLayout();
        }

        @Override
        protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
            getChildAt(0).measure(MeasureSpec.makeMeasureSpec(bw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(bh, MeasureSpec.EXACTLY));
            getChildAt(1).measure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY));
            getChildAt(2).measure(MeasureSpec.makeMeasureSpec(bw, MeasureSpec.AT_MOST), MeasureSpec.makeMeasureSpec(bh, MeasureSpec.AT_MOST));
            // scrims (children 3 and 4) where the picture goes behind the status / navigation bar
            float d = getResources().getDisplayMetrics().density;
            topScrim = by < t ? Math.round(t - by + 24 * d) : 0;
            int under = by + bh - (h - b);
            bottomScrim = under > 0 ? Math.round(under + 24 * d) : 0;
            if (getChildCount() > 4) {
                getChildAt(3).measure(MeasureSpec.makeMeasureSpec(bw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(topScrim, MeasureSpec.EXACTLY));
                getChildAt(4).measure(MeasureSpec.makeMeasureSpec(bw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(bottomScrim, MeasureSpec.EXACTLY));
            }
            setMeasuredDimension(w, h);
        }

        @Override
        protected void onLayout(boolean changed, int x0, int y0, int x1, int y1) {
            int w = x1 - x0, h = y1 - y0;
            getChildAt(0).layout(bx, by, bx + bw, by + bh);
            getChildAt(1).layout(0, 0, w, h);
            View badge = getChildAt(2);
            int m = (int) (12 * getResources().getDisplayMetrics().density);
            int top = Math.max(by, t) + m;   // never under the status bar
            badge.layout(bx + m, top, bx + m + badge.getMeasuredWidth(), top + badge.getMeasuredHeight());
            if (getChildCount() > 4) {
                getChildAt(3).layout(bx, by, bx + bw, by + topScrim);
                getChildAt(4).layout(bx, by + bh - bottomScrim, bx + bw, by + bh);
            }
        }
    }
}
