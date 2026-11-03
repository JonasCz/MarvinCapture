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
 * The screen's two parts, camera-app style: the preview (child 0) and the side panel (child 1:
 * status, meters, buttons, or the options). Landscape (wider than tall): the preview on the left,
 * as wide as the picture's aspect allows at full height, the panel on the right. Portrait: the
 * preview on top, as tall as the aspect allows at full width, the panel below. The panel keeps at
 * least a minimum size; the preview then letterboxes inside its part (the renderer's
 * pin_fit_rect). Padding = the system bar and cutout insets.
 */
final class MainLayout extends ViewGroup {
    private final float dp;
    private int darNum = 4, darDen = 3;
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

    boolean isLandscape() {
        return land;
    }

    private int[] split(int w, int h) {
        if (w > h) {
            int minPanel = (int) Math.min(w * 0.5f, 300 * dp);
            int pw = Math.min(Math.round((float) h * darNum / darDen), w - minPanel);
            return new int[] { pw, h, w - pw, h };
        }
        int minPanel = (int) Math.min(h * 0.55f, 330 * dp);
        int ph = Math.min(Math.round((float) w * darDen / darNum), h - minPanel);
        return new int[] { w, ph, w, h - ph };
    }

    @Override
    protected void onMeasure(int wSpec, int hSpec) {
        int w = MeasureSpec.getSize(wSpec), h = MeasureSpec.getSize(hSpec);
        int iw = Math.max(0, w - getPaddingLeft() - getPaddingRight());
        int ih = Math.max(0, h - getPaddingTop() - getPaddingBottom());
        land = iw > ih;
        int[] s = split(iw, ih);
        View preview = getChildAt(0), panel = getChildAt(1);
        preview.measure(MeasureSpec.makeMeasureSpec(s[0], MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(s[1], MeasureSpec.EXACTLY));
        panel.measure(MeasureSpec.makeMeasureSpec(s[2], MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(s[3], MeasureSpec.EXACTLY));
        setMeasuredDimension(w, h);
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        int x = getPaddingLeft(), y = getPaddingTop();
        View preview = getChildAt(0), panel = getChildAt(1);
        int pw = preview.getMeasuredWidth(), ph = preview.getMeasuredHeight();
        preview.layout(x, y, x + pw, y + ph);
        if (land) panel.layout(x + pw, y, x + pw + panel.getMeasuredWidth(), y + panel.getMeasuredHeight());
        else panel.layout(x, y + ph, x + panel.getMeasuredWidth(), y + ph + panel.getMeasuredHeight());
    }
}
