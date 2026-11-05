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

import android.animation.ValueAnimator;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.View;

import java.util.Random;

/**
 * The animated Marvin of the no-video card: a port of the drawing and the idle animation in
 * docs/logo.html (the source of truth for the look, motion and timings; keep them in step) and of
 * the macOS MarvinIdleView / MarvinRenderer, drawn with Canvas in the logo's 64 x 64 grid. The
 * clip paths of the SVG are path intersections here (Path.op), because a hardware canvas clips
 * without antialiasing. Decorative only (hidden from accessibility). It animates at 30 fps only
 * while it is shown, and with animations turned off (ValueAnimator.areAnimatorsEnabled(): the
 * "Remove animations" accessibility setting or an animator scale of 0) it draws the still pose.
 */
public final class MarvinIdleView extends View {
    private static final int INK = 0xff1a1d42, BODY = 0xff2d3180, CREAM = 0xfffbf3dc, CORAL = 0xffff5f5a,
            CORAL_DK = 0xffd43a48, CHEEK = 0xffff7a6e, WHITE = 0xffffffff;
    private static final int[] BARS = { 0xffddd8c6, 0xffdcc050, 0xff62b9bf, 0xff6fb57c, 0xffb76fab, 0xffd2604f, 0xff5069b5 };
    private static final float DROP = 2.5f;

    private final float dp;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Idle idle = new Idle();
    private final Path body = new Path(), tmp = new Path(), tmp2 = new Path();
    private final RectF rf = new RectF();

    public MarvinIdleView(Context c) {
        super(c);
        dp = getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
        body.addRoundRect(new RectF(4, 4, 60, 44), 8, 8, Path.Direction.CW);
        body.addRoundRect(new RectF(9, 8.5f, 55, 30.5f), 5, 5, Path.Direction.CW);
        body.setFillType(Path.FillType.EVEN_ODD);
    }

    @Override
    protected void onMeasure(int w, int h) {
        // 150 x 125 dp, smaller where the card is short (the picture of a 16:9 phone in portrait): the
        // animation gives way so that the sentence below it is never cut off
        int want = (int) (125 * dp);
        int height = MeasureSpec.getMode(h) == MeasureSpec.UNSPECIFIED ? want : Math.min(want, MeasureSpec.getSize(h));
        setMeasuredDimension(resolveSize((int) (150 * dp), w), height);
    }

    /** The size of one logo unit in pixels in a view of this size (onDraw, and the splash flight). */
    static float unit(float w, float h) {
        return Math.min(w / 64f, h / 53f);
    }

    private boolean animated() {
        return ValueAnimator.areAnimatorsEnabled();
    }

    private final Runnable frame = this::invalidate;

    @Override
    protected void onDraw(Canvas c) {
        Pose p = animated() ? idle.pose(SystemClock.uptimeMillis() / 1000.0) : Pose.STILL;
        float w = getWidth(), h = getHeight();
        // The logo's x 0..64 and y 5..58 (body top to tongue tip) scaled into the view, the body
        // bobbing and tilting about (32, 28).
        float s = unit(w, h);
        c.save();
        c.translate((w - 64 * s) / 2, (h - 53 * s) / 2 - 5 * s);
        c.scale(s, s);
        c.translate(0, DROP + (float) p.y);
        c.rotate((float) p.rot, 32, 28);
        drawMarvin(c, p);
        c.restore();
        if (animated() && isShown()) postOnAnimationDelayed(frame, 33);
    }

    private void rect(Canvas c, float x, float y, float rw, float rh, int color) {
        fill.setColor(color);
        c.drawRect(x, y, x + rw, y + rh, fill);
    }

    private void drawMarvin(Canvas c, Pose p) {
        // label and bars oversized, the body on top as a frame with the label cut out
        rect(c, 8, 7.5f, 48, 24, CREAM);
        for (int i = 0; i < 7; i++) rect(c, 9 + i * 46f / 7, 26.2f, 46f / 7 + 0.1f, 5.3f, BARS[i]);
        rect(c, 8, 26.2f, 1.1f, 5.3f, BARS[0]);
        rect(c, 54.9f, 26.2f, 1.1f, 5.3f, BARS[6]);
        fill.setColor(BODY);
        c.drawPath(body, fill);
        fill.setColor(INK);
        rf.set(14, 11.5f, 50, 24.5f);
        c.drawRoundRect(rf, 6.5f, 6.5f, fill);

        // eyes: white, the pupil intersected with the white, the lid
        for (int e = 0; e < 2; e++) {
            float cx = e == 0 ? 23 : 41;
            fill.setColor(WHITE);
            c.drawCircle(cx, 18, 4.4f, fill);
            tmp.reset();
            tmp.addCircle(cx + (float) p.gx * 2, 18 + (float) p.gy * 1.7f, 2.1f, Path.Direction.CW);
            tmp2.reset();
            tmp2.addCircle(cx, 18, 4.4f, Path.Direction.CW);
            tmp.op(tmp2, Path.Op.INTERSECT);
            fill.setColor(INK);
            c.drawPath(tmp, fill);
            if (p.lid > 0) rect(c, cx - 5, 13.2f, 10, (float) p.lid * 9.2f, INK);
        }
        fill.setColor(CHEEK);
        c.drawCircle(15.5f, 37.5f, 2.6f, fill);
        c.drawCircle(48.5f, 37.5f, 2.6f, fill);

        // mouth by openness m: 0 is the closed smile (a stroked curve), 1 fully open
        double m = p.mouth;
        float xl = (float) (26 - 1.5 * m), xr = (float) (38 + 1.5 * m), y0 = (float) (35.5 - 1.5 * m),
                top = (float) (40.5 - 6.5 * m), bot = (float) (40.5 + 8.5 * m);
        Path mouth = new Path();
        mouth.moveTo(xl, y0);
        mouth.quadTo(32, top, xr, y0);
        mouth.quadTo(32, bot, xl, y0);
        mouth.close();
        fill.setColor(WHITE);
        c.drawPath(mouth, fill);
        float sw = (float) (2.6 * (1 - m));
        if (sw > 0.01f) {
            stroke.setColor(CREAM);
            stroke.setStrokeWidth(sw);
            c.drawPath(mouth, stroke);
        }
        if (m > 0.01) {
            tmp.reset();
            tmp.addRect(22, y0, 42, y0 + 1.8f, Path.Direction.CW);
            tmp.op(mouth, Path.Op.INTERSECT);
            fill.setColor(INK);
            fill.setAlpha((int) (255 * 0.2 * m));
            c.drawPath(tmp, fill);
            fill.setAlpha(255);
        }

        // the tongue shows only below the upper lip
        Tongue t = Tongue.of(p, (y0 + top) / 2 - 1);
        if (t != null) {
            Path lip = new Path();
            lip.moveTo(-40, y0);
            lip.lineTo(xl, y0);
            lip.quadTo(32, top, xr, y0);
            lip.lineTo(104, y0);
            lip.lineTo(104, 120);
            lip.lineTo(-40, 120);
            lip.close();
            // fill + a 1.4 wide stroke in the same colour = the outline grown by 0.7
            stroke.setStrokeWidth(1.4f);
            Path grown = new Path();
            stroke.getFillPath(t.outline, grown);
            grown.op(t.outline, Path.Op.UNION);
            grown.op(lip, Path.Op.INTERSECT);
            fill.setColor(CORAL);
            c.drawPath(grown, fill);
            if (t.grooveOpacity > 0) {
                stroke.setStrokeWidth(1.3f);
                Path g = new Path();
                stroke.getFillPath(t.groove, g);
                g.op(lip, Path.Op.INTERSECT);
                fill.setColor(CORAL_DK);
                fill.setAlpha((int) (255 * t.grooveOpacity));
                c.drawPath(g, fill);
                fill.setAlpha(255);
            }
        }
    }

    // ---- the animation (class Idle in docs/logo.html) ---------------------------------------------

    /** One frame of Marvin. */
    static final class Pose {
        double mouth = 1, tongue = 1, y, rot, gx, gy = 1, lid;
        Bend bend = u -> 0;
        static final Pose STILL = new Pose();
    }

    interface Bend { double at(double u); }

    static final class Idle {
        private static final int OUT = 0, RETRACT = 1, CLOSE = 2, SMILE = 3, OPEN = 4, EXTEND = 5;
        private final Random rnd = new Random();
        private int phase = OUT;
        private double clock, since, until = r(4, 7), popAt = -10;
        private double nextGaze = r(0.8, 2), nextBlink = r(1.5, 3), blinkStart = -1;
        private double gx = 0, gy = 1, tx = 0, ty = 0.2;
        private double last = -1;

        private double r(double a, double b) { return a + rnd.nextDouble() * (b - a); }
        private static double smooth(double k) { return k * k * (3 - 2 * k); }
        private static double backOut(double k) { return 1 + 2.70158 * Math.pow(k - 1, 3) + 1.70158 * Math.pow(k - 1, 2); }

        private void go(int p, double t) { phase = p; since = t; }

        Pose pose(double now) {
            // a gap (the view was hidden) is not animated through
            double dt = last < 0 ? 0 : Math.min(0.05, Math.max(0, now - last));
            last = now;
            clock += dt;
            double t = clock;

            double len = 0, m = 1;
            switch (phase) {
                case OUT:
                    len = 1;
                    if (t > until) go(RETRACT, t);
                    break;
                case RETRACT: {
                    double p = Math.min(1, (t - since) / 0.32);
                    len = 1 - p * p * p;
                    if (p >= 1) go(CLOSE, t);
                    break;
                }
                case CLOSE: {
                    double p = Math.min(1, (t - since) / 0.2);
                    m = 1 - smooth(p);
                    if (p >= 1) { go(SMILE, t); until = t + r(3, 6); nextBlink = Math.min(nextBlink, t + 0.05); }
                    break;
                }
                case SMILE:
                    m = 0;
                    if (t > until) go(OPEN, t);
                    break;
                case OPEN: {
                    double p = Math.min(1, (t - since) / 0.2);
                    m = smooth(p);
                    if (p >= 1) { go(EXTEND, t); popAt = t; }
                    break;
                }
                default: {   // EXTEND
                    double p = Math.min(1, (t - since) / 0.45);
                    len = backOut(p);
                    if (p >= 1) { go(OUT, t); until = t + r(5, 9); }
                    break;
                }
            }

            double y = 1.3 * Math.sin(2 * Math.PI * t / 3.2), rot = 1.4 * Math.sin(2 * Math.PI * t / 5.3);
            // tongue bend: hangs with gravity as the body tilts, sways, carries a ripple from root to
            // tip, and wobbles for a moment after popping out
            double sincePop = t - popAt, pop = 24 * Math.exp(-3.2 * sincePop);
            Bend bend = u -> Math.toRadians(rot * u + 6 * Math.sin(2 * Math.PI * t / 2.6) * Math.pow(u, 1.2)
                    + 10 * Math.sin(2 * Math.PI * t / 1.5 - 2.4 * u) * u + pop * Math.sin(15 * sincePop - 2 * u) * u);

            // gaze: watch the tongue come out, otherwise a random glance every second or three
            if (phase == EXTEND || (phase == OUT && sincePop < 1.1)) {
                tx = 0; ty = 1;
                nextGaze = t + 0.3;
            } else if (t > nextGaze) {
                double ch = rnd.nextDouble();
                if (ch < 0.35) { tx = 0; ty = 0.2; }
                else if (ch < 0.5) { tx = 0; ty = 1; }
                else { tx = r(-1, 1); ty = r(-0.7, 0.8); }
                nextGaze = t + r(0.8, 3);
            }
            double e = 1 - Math.exp(-14 * dt);
            gx += (tx - gx) * e;
            gy += (ty - gy) * e;

            // blinks, occasionally double; a closed eye keeps a thin sliver at the bottom
            double lid = 0;
            if (blinkStart < 0 && t > nextBlink) blinkStart = t;
            if (blinkStart >= 0) {
                double b = (t - blinkStart) / 0.17;
                if (b >= 1) {
                    blinkStart = -1;
                    nextBlink = t + (rnd.nextDouble() < 0.2 ? 0.12 : r(2.2, 6));
                } else {
                    lid = 0.86 * Math.pow(Math.sin(Math.PI * b), 0.6);
                }
            }
            Pose p = new Pose();
            p.mouth = m; p.tongue = len; p.bend = bend; p.y = y; p.rot = rot; p.gx = gx; p.gy = gy; p.lid = lid;
            return p;
        }
    }

    /** The tongue (tongueGeom in docs/logo.html): an arrow laid along a bending spine. */
    static final class Tongue {
        final Path outline = new Path(), groove = new Path();
        double grooveOpacity;

        static Tongue of(Pose pose, double rootY) {
            double len = pose.tongue;
            if (len <= 0.01) return null;
            double shaft = 12.5, head = 9, full = shaft + head;
            double k = Math.min(1, Math.max(0, len / 0.35)), sl = shaft * Math.max(0, (len - 0.35) / 0.65), hl = head * k;
            double wS = 3.1 * k, wH = 7.4 * k;
            double[] stops = new double[9];
            for (int i = 0; i < 7; i++) stops[i] = sl * i / 6;
            stops[7] = sl + hl * 0.5;
            stops[8] = sl + hl;
            double x = 32, y = rootY, s = 0;
            double[][] pts = new double[9][];
            for (int j = 0; j < 9; j++) {
                double ds = (stops[j] - s) / 4;
                for (int i = 0; i < 4; i++) {
                    double a = pose.bend.at((s + ds / 2) / full);
                    x += Math.sin(a) * ds;
                    y += Math.cos(a) * ds;
                    s += ds;
                }
                double a = pose.bend.at(s / full);
                pts[j] = new double[] { x, y, Math.cos(a), -Math.sin(a) };
            }
            Tongue t = new Tongue();
            Path o = t.outline;
            o.moveTo((float) (32 - wS), (float) (rootY - 2));
            o.lineTo((float) (32 + wS), (float) (rootY - 2));
            for (int i = 0; i < 7; i++) at(o, pts[i], wS);
            at(o, pts[6], wH);
            at(o, pts[7], wH / 2);
            at(o, pts[8], 0);
            at(o, pts[7], -wH / 2);
            at(o, pts[6], -wH);
            for (int i = 6; i >= 0; i--) at(o, pts[i], -wS);
            o.close();
            Path g = t.groove;
            g.moveTo((float) pts[2][0], (float) pts[2][1]);
            for (int i = 3; i < 7; i++) g.lineTo((float) pts[i][0], (float) pts[i][1]);
            g.lineTo((float) ((pts[6][0] * 2 + pts[7][0]) / 3), (float) ((pts[6][1] * 2 + pts[7][1]) / 3));
            t.grooveOpacity = 0.8 * Math.min(1, Math.max(0, (len - 0.5) / 0.3));
            return t;
        }

        private static void at(Path p, double[] pt, double w) {
            p.lineTo((float) (pt[0] + pt[2] * w), (float) (pt[1] + pt[3] * w));
        }
    }
}
