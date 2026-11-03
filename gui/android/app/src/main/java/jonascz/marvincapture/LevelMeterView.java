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
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.util.AttributeSet;
import android.view.View;

import java.util.ArrayDeque;

/**
 * Two thin horizontal audio level meters, L and R (port of the macOS LevelMeterView): a bar for the
 * peak (dBFS) coloured by zone (green up to -18, yellow up to -6, red above), falling at 30 dB/s,
 * and a tick for the highest peak of the last 10 s, red when that came within 1 dB of clipping.
 * The core reports the loudest peak since the previous status read; the service reads it once per
 * tick and the activity pushes it here.
 */
public final class LevelMeterView extends View {
    static final float FLOOR_DB = -60f;
    private static final float YELLOW_FROM = -18f, RED_FROM = -6f, DECAY_DB_PER_S = 30f;
    private static final long HOLD_MS = 10_000;

    private final float dp;
    private final Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint[] zones = { new Paint(Paint.ANTI_ALIAS_FLAG), new Paint(Paint.ANTI_ALIAS_FLAG), new Paint(Paint.ANTI_ALIAS_FLAG) };
    private final Paint holdPaint = new Paint();
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float[] level = { FLOOR_DB, FLOOR_DB };
    private final float[] hold = { FLOOR_DB, FLOOR_DB };
    @SuppressWarnings("unchecked")
    private final ArrayDeque<float[]>[] history = new ArrayDeque[] { new ArrayDeque<float[]>(), new ArrayDeque<float[]>() };
    private long lastPush;
    private final RectF r = new RectF();

    public LevelMeterView(Context c, AttributeSet a) {
        super(c, a);
        dp = getResources().getDisplayMetrics().density;
        track.setColor(0x33ffffff);
        zones[0].setColor(0xff34c759);
        zones[1].setColor(0xffffcc00);
        zones[2].setColor(0xffff3b30);
        label.setColor(0xb3ffffff);
        label.setTextSize(10 * getResources().getDisplayMetrics().scaledDensity);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        setContentDescription("Audio level meters: silent");
    }

    /** New peaks (dBFS, -144 for silence) from one status read. */
    void push(double peakL, double peakR) {
        long now = SystemClock.uptimeMillis();
        float dt = lastPush == 0 ? 0 : Math.min(0.25f, (now - lastPush) / 1000f);
        lastPush = now;
        double[] in = { peakL, peakR };
        boolean changed = false;
        for (int ch = 0; ch < 2; ch++) {
            float v = (float) Math.max(FLOOR_DB, Math.min(0, in[ch]));
            float shown = Math.max(v, level[ch] - DECAY_DB_PER_S * dt);
            ArrayDeque<float[]> h = history[ch];
            h.addLast(new float[] { now, v });
            while (!h.isEmpty() && now - h.peekFirst()[0] > HOLD_MS) h.removeFirst();
            float max = FLOOR_DB;
            for (float[] e : h) max = Math.max(max, e[1]);
            if (Math.abs(shown - level[ch]) >= 0.25f || max != hold[ch]) changed = true;
            level[ch] = shown;
            hold[ch] = max;
        }
        if (changed) {
            setContentDescription("Audio level meters: left " + text(level[0]) + ", right " + text(level[1]));
            invalidate();
        }
    }

    private static String text(float db) { return db <= FLOOR_DB ? "silent" : Math.round(db) + " dBFS"; }

    private static float frac(float db) { return Math.max(0, Math.min(1, (db - FLOOR_DB) / -FLOOR_DB)); }

    @Override
    protected void onMeasure(int w, int h) {
        setMeasuredDimension(getDefaultSize(getSuggestedMinimumWidth(), w), resolveSize((int) (18 * dp), h));
    }

    @Override
    protected void onDraw(Canvas c) {
        float barH = 4 * dp, gap = 6 * dp;
        float labelW = 14 * dp;
        float x0 = getPaddingLeft() + labelW, x1 = getWidth() - getPaddingRight();
        float w = x1 - x0;
        float top = (getHeight() - (2 * barH + gap)) / 2f;
        String[] names = { "L", "R" };
        for (int ch = 0; ch < 2; ch++) {
            float y = top + ch * (barH + gap);
            c.drawText(names[ch], getPaddingLeft(), y + barH / 2 + label.getTextSize() * 0.35f, label);
            r.set(x0, y, x1, y + barH);
            c.drawRoundRect(r, barH / 2, barH / 2, track);
            float bar = frac(level[ch]) * w;
            float[][] z = { { FLOOR_DB, YELLOW_FROM }, { YELLOW_FROM, RED_FROM }, { RED_FROM, 0 } };
            c.save();
            c.clipRect(x0, y, x0 + bar, y + barH);
            for (int i = 0; i < 3; i++) {
                r.set(x0 + frac(z[i][0]) * w, y, x0 + frac(z[i][1]) * w, y + barH);
                c.drawRoundRect(r, i == 0 ? barH / 2 : 0, i == 0 ? barH / 2 : 0, zones[i]);
            }
            c.restore();
            if (hold[ch] > FLOOR_DB) {
                holdPaint.setColor(hold[ch] > -1 ? 0xffff3b30 : 0xffffffff);
                float hx = x0 + frac(hold[ch]) * w;
                c.drawRect(Math.max(x0, hx - 2 * dp), y, Math.max(x0 + 2 * dp, hx), y + barH, holdPaint);
            }
        }
    }
}
