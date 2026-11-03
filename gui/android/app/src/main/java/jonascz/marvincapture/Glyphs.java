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
import android.graphics.Path;
import android.graphics.RectF;
import android.view.View;

/**
 * The app's few icons, drawn on a 24-unit grid (no icon font, no image assets), and the round
 * buttons that carry them: the record button (a "save to device" arrow into a tray, a stop square
 * while recording) and the small gear / speaker buttons.
 */
final class Glyphs {
    private Glyphs() {}

    static final int DOWNLOAD = 0, GEAR = 1, SPEAKER_ON = 2, SPEAKER_OFF = 3, CLOSE = 4,
            LIBRARY = 5, BACK = 6;

    /** Draws glyph g in white-ish paint p, scaled from 24 units to size px, centred on (cx, cy). */
    static void draw(Canvas c, int g, float cx, float cy, float size, Paint p) {
        float u = size / 24f;
        c.save();
        c.translate(cx - size / 2, cy - size / 2);
        c.scale(u, u);
        Paint.Style style = p.getStyle();
        float sw = p.getStrokeWidth();
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setStrokeJoin(Paint.Join.ROUND);
        Path path = new Path();
        switch (g) {
            case DOWNLOAD:
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(2.4f);
                path.moveTo(12, 3.5f); path.lineTo(12, 14.5f);
                path.moveTo(7, 9.5f); path.lineTo(12, 14.5f); path.lineTo(17, 9.5f);
                path.moveTo(4, 14); path.lineTo(4, 19.5f); path.lineTo(20, 19.5f); path.lineTo(20, 14);
                c.drawPath(path, p);
                break;
            case GEAR: {
                p.setStyle(Paint.Style.FILL);
                int n = 8;
                double per = 2 * Math.PI / n;
                double[] off = { -0.20, -0.09, 0.09, 0.20 };
                float[] rad = { 7.6f, 10.2f, 10.2f, 7.6f };
                for (int i = 0; i < n; i++) {
                    for (int k = 0; k < 4; k++) {
                        double a = (i + off[k]) * per;
                        float x = 12 + (float) (rad[k] * Math.cos(a)), y = 12 + (float) (rad[k] * Math.sin(a));
                        if (i == 0 && k == 0) path.moveTo(x, y); else path.lineTo(x, y);
                    }
                }
                path.close();
                path.addCircle(12, 12, 3.4f, Path.Direction.CW);
                path.setFillType(Path.FillType.EVEN_ODD);
                c.drawPath(path, p);
                break;
            }
            case SPEAKER_ON:
            case SPEAKER_OFF:
                p.setStyle(Paint.Style.FILL);
                path.moveTo(3, 9); path.lineTo(7, 9); path.lineTo(12, 4.5f); path.lineTo(12, 19.5f);
                path.lineTo(7, 15); path.lineTo(3, 15); path.close();
                c.drawPath(path, p);
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(2f);
                if (g == SPEAKER_ON) {
                    c.drawArc(new RectF(8.5f, 8.5f, 15.5f, 15.5f), -50, 100, false, p);
                    c.drawArc(new RectF(5, 5, 19, 19), -50, 100, false, p);
                } else {
                    c.drawLine(15.5f, 9, 21, 14.5f, p);
                    c.drawLine(21, 9, 15.5f, 14.5f, p);
                }
                break;
            case CLOSE:
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(2.2f);
                c.drawLine(6, 6, 18, 18, p);
                c.drawLine(18, 6, 6, 18, p);
                break;
            case LIBRARY:   // a screen with a play triangle
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(2.2f);
                c.drawRoundRect(new RectF(3.5f, 5, 20.5f, 19), 2.5f, 2.5f, p);
                p.setStyle(Paint.Style.FILL);
                path.moveTo(10, 9); path.lineTo(15, 12); path.lineTo(10, 15); path.close();
                c.drawPath(path, p);
                break;
            case BACK:
                p.setStyle(Paint.Style.STROKE);
                p.setStrokeWidth(2.4f);
                path.moveTo(19, 12); path.lineTo(5, 12);
                path.moveTo(11, 6); path.lineTo(5, 12); path.lineTo(11, 18);
                c.drawPath(path, p);
                break;
            default:
                break;
        }
        p.setStyle(style);
        p.setStrokeWidth(sw);
        c.restore();
    }

    /** A round button with one glyph (gear, speaker, close). */
    static final class Button extends View {
        private final Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG), fg = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int glyph;
        private final float dp;
        private final int sizeDp;

        Button(Context c, int glyph, int sizeDp) {
            super(c);
            this.glyph = glyph;
            this.sizeDp = sizeDp;
            dp = getResources().getDisplayMetrics().density;
            bg.setColor(Ui.BUTTON);
            fg.setColor(Ui.TEXT);
            setClickable(true);
            setFocusable(true);
            setBackground(c.getDrawable(R.drawable.ripple_round));
        }

        void setGlyph(int g) {
            if (g != glyph) { glyph = g; invalidate(); }
        }

        /** Own colours (over a picture, where the theme's would vanish). */
        void setColors(int background, int foreground) {
            bg.setColor(background);
            fg.setColor(foreground);
            invalidate();
        }

        @Override
        protected void onMeasure(int w, int h) {
            int s = (int) (sizeDp * dp);
            setMeasuredDimension(resolveSize(s, w), resolveSize(s, h));
        }

        @Override
        public void setEnabled(boolean e) {
            super.setEnabled(e);
            setAlpha(e ? 1f : 0.4f);
        }

        @Override
        protected void onDraw(Canvas c) {
            float cx = getWidth() / 2f, cy = getHeight() / 2f, r = Math.min(cx, cy);
            c.drawCircle(cx, cy, r, bg);
            Glyphs.draw(c, glyph, cx, cy, r * 0.95f, fg);
        }
    }

    /** The big record button: a ring around a red disc with the "save to device" glyph; while
     *  recording a red stop square; finalising, a grey square. Disabled = dimmed. */
    static final class RecordButton extends View {
        private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG), fill = new Paint(Paint.ANTI_ALIAS_FLAG),
                glyph = new Paint(Paint.ANTI_ALIAS_FLAG),
                scrim = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final float dp;
        private boolean recording, finishing;
        private final RectF r = new RectF();

        RecordButton(Context c) {
            super(c);
            dp = getResources().getDisplayMetrics().density;
            ring.setStyle(Paint.Style.STROKE);
            ring.setColor(Ui.TEXT);
            glyph.setColor(0xffffffff);   // on the red disc
            setClickable(true);
            setFocusable(true);
            setBackground(c.getDrawable(R.drawable.ripple_round));
            setContentDescription("Start capture");
        }

        void setMode(boolean recording, boolean finishing) {
            if (recording == this.recording && finishing == this.finishing) return;
            this.recording = recording;
            this.finishing = finishing;
            setContentDescription(finishing ? "Finalizing" : recording ? "Stop capture" : "Start capture");
            invalidate();
        }

        @Override
        protected void onMeasure(int w, int h) {
            int s = (int) (76 * dp);
            setMeasuredDimension(resolveSize(s, w), resolveSize(s, h));
        }

        @Override
        public void setEnabled(boolean e) {
            super.setEnabled(e);
            invalidate();
        }

        @Override
        protected void onDraw(Canvas c) {
            float cx = getWidth() / 2f, cy = getHeight() / 2f, rad = Math.min(cx, cy);
            boolean on = isEnabled();
            scrim.setColor(Ui.PANEL);
            scrim.setAlpha(0xCC);   // legible over the picture when the panel around it is invisible
            c.drawCircle(cx, cy, rad, scrim);
            float stroke = 3.5f * dp;
            ring.setStrokeWidth(stroke);
            ring.setAlpha(on ? 255 : 110);
            c.drawCircle(cx, cy, rad - stroke / 2, ring);
            int red = 0xffe5393b;
            if (recording || finishing) {
                fill.setColor(finishing || !on ? 0xff8a8d93 : red);
                float h = rad * 0.42f;
                r.set(cx - h, cy - h, cx + h, cy + h);
                c.drawRoundRect(r, 6 * dp, 6 * dp, fill);
            } else {
                fill.setColor(on ? red : 0xff5b5e66);
                c.drawCircle(cx, cy, rad - stroke - 3 * dp, fill);
                glyph.setAlpha(on ? 255 : 150);
                Glyphs.draw(c, DOWNLOAD, cx, cy, rad * 0.95f, glyph);
            }
        }
    }
}
