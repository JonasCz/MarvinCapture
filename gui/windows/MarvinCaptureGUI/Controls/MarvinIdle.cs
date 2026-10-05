using System;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace PinnacleCapture.Controls;

/// <summary>
/// The animated Marvin of the no-signal screen, drawn with XAML shapes: a port of the drawing and the idle
/// animation in docs/logo.html (the source of truth for the look, motion and timings; keep them in step).
/// Everything is in the logo's 64 x 64 grid, scaled to the control. Decorative only: no hit testing, no
/// tab stop, hidden from accessibility. It animates only while <see cref="Active"/>: the frame callback is
/// attached then and detached otherwise, so a live picture costs nothing. With "animations" switched off
/// in Windows it shows the still pose. If the shapes cannot be built it shows the static logo.
/// </summary>
public sealed class MarvinIdle : Grid
{
    // palette, as in docs/logo.html
    private const uint Ink = 0x1a1d42, Body = 0x2d3180, Cream = 0xfbf3dc, Coral = 0xff5f5a, CoralDk = 0xd43a48, Cheek = 0xff7a6e;
    private static readonly uint[] Bars = { 0xddd8c6, 0xdcc050, 0x62b9bf, 0x6fb57c, 0xb76fab, 0xd2604f, 0x5069b5 };

    private const double Lower = 2.5;            // centres body plus tongue in the 64 grid
    private const double ShaftHalf = 3.1, HeadHalf = 7.4, Shaft = 12.5, Head = 9;
    private const double Tau = Math.PI * 2;
    private const double Fps = 30;              // the motion is slow; half the display rate is plenty

    private readonly Random _rnd = new();
    private readonly CompositeTransform _fit = new();
    private Canvas? _art;
    private Image? _still;

    // the parts the animation drives
    private readonly RotateTransform _rot = new() { CenterX = 32, CenterY = 28 };
    private readonly TranslateTransform _move = new();
    private readonly TranslateTransform[] _pupil = { new(), new() };
    private readonly Rectangle[] _lid = new Rectangle[2];
    private readonly PathFigure _mouthFigure = new();
    private readonly QuadraticBezierSegment _mouthUp = new(), _mouthDown = new();
    private readonly Path _mouth = new();
    private readonly Polygon _shade = new();
    private readonly Canvas _tongueHost = new();
    private readonly Polygon _tongue = new();
    private readonly Polyline _groove = new();

    private bool _active;
    private bool _attached;
    private TimeSpan _last = TimeSpan.MinValue;

    // animation state (Idle in docs/logo.html)
    private enum Phase { Out, Retract, Close, Smile, Open, Extend }
    private double _clock, _since, _until, _popAt, _nextGaze, _nextBlink, _blinkStart;
    private Phase _phase;
    private (double X, double Y) _gaze, _target;

    public MarvinIdle()
    {
        IsHitTestVisible = false;
        AutomationProperties.SetAccessibilityView(this, AccessibilityView.Raw);
        try
        {
            _art = Build();
            Children.Add(_art);
            SizeChanged += (_, _) => Fit();
            Render(m: 1, len: 1, bend: _ => 0, y: 0, rot: 0, gaze: (0, 1), lid: 0);
        }
        catch (Exception)
        {
            // the shapes could not be built: the static logo instead, or nothing
            _art = null;
            Children.Clear();
            try
            {
                _still = new Image
                {
                    Stretch = Stretch.Uniform,
                    IsHitTestVisible = false,
                    Source = new SvgImageSource(new Uri(System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "logo.svg"))),
                };
                AutomationProperties.SetAccessibilityView(_still, AccessibilityView.Raw);
                Children.Add(_still);
            }
            catch (Exception)
            {
                // no image: the space stays empty
            }
        }
    }

    /// <summary>True while the control is on screen and should animate; false stops all work.</summary>
    public bool Active
    {
        get => _active;
        set
        {
            if (_active == value)
            {
                return;
            }
            _active = value;
            if (value && _art is not null && AnimationsEnabled())
            {
                Restart();
                Attach();
            }
            else
            {
                Detach();
                if (_art is not null)
                {
                    Render(m: 1, len: 1, bend: _ => 0, y: 0, rot: 0, gaze: (0, 1), lid: 0); // the still pose
                }
            }
        }
    }

    private static bool AnimationsEnabled()
    {
        try { return new Windows.UI.ViewManagement.UISettings().AnimationsEnabled; }
        catch (Exception) { return true; }
    }

    private void Attach()
    {
        if (!_attached)
        {
            _attached = true;
            _last = TimeSpan.MinValue;
            CompositionTarget.Rendering += OnRendering;
        }
    }

    private void Detach()
    {
        if (_attached)
        {
            _attached = false;
            CompositionTarget.Rendering -= OnRendering;
        }
    }

    // ================================================================== scene

    private static Windows.UI.Color Rgb(uint c) => Windows.UI.Color.FromArgb(255, (byte)(c >> 16), (byte)(c >> 8), (byte)c);
    private static SolidColorBrush Fill(uint c) => new(Rgb(c));

    private static T At<T>(T shape, double x, double y) where T : UIElement
    {
        Canvas.SetLeft(shape, x);
        Canvas.SetTop(shape, y);
        return shape;
    }

    private static Rectangle Box(double x, double y, double w, double h, double radius, uint color) =>
        At(new Rectangle { Width = w, Height = h, RadiusX = radius, RadiusY = radius, Fill = Fill(color) }, x, y);

    private static Ellipse Dot(double cx, double cy, double r, uint color) =>
        At(new Ellipse { Width = 2 * r, Height = 2 * r, Fill = Fill(color) }, cx - r, cy - r);

    private static Path Shape(string data, uint color)
    {
        // geometry parsed from path data (the bars at the label's two bottom corners follow its rounded corners)
        var p = (Path)Microsoft.UI.Xaml.Markup.XamlReader.Load(
            "<Path xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' Data='" + data + "'/>");
        p.Fill = Fill(color);
        return p;
    }

    private Canvas Build()
    {
        var root = new Canvas
        {
            Width = 64, Height = 64, IsHitTestVisible = false,
            HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top,
        };
        var move = new Canvas { RenderTransform = new TransformGroup { Children = { _rot, _move } } };
        root.Children.Add(move);
        void Add(UIElement e) => move.Children.Add(e);

        Add(Box(4, 4, 56, 40, 8, Body));
        Add(Box(9, 8.5, 46, 22, 5, Cream));
        for (int i = 0; i < 7; i++)
        {
            double x = 9 + i * 46.0 / 7, w = 46.0 / 7 + 0.1;
            string f(double v) => v.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture);
            Add(i switch
            {
                0 => Shape($"M9.05 26.2H{f(x + w)}V30.5H14A5 5 0 0 1 9.05 26.2Z", Bars[i]),
                6 => Shape($"M{f(x)} 26.2H54.95A5 5 0 0 1 50 30.5H{f(x)}Z", Bars[i]),
                _ => Box(x, 26.2, w, 4.3, 0, Bars[i]),
            });
        }
        Add(Box(14, 11.5, 36, 13, 6.5, Ink));

        // eyes: the pupil is not clipped to the eyeball (clips are rectangles only), a dark ring hides what pokes out
        for (int k = 0; k < 2; k++)
        {
            double cx = k == 0 ? 23 : 41;
            Add(Dot(cx, 18, 4.4, 0xffffff));
            var pupil = Dot(cx, 18, 2.1, Ink);
            pupil.RenderTransform = _pupil[k];
            Add(pupil);
            Add(new Path
            {
                Fill = Fill(Ink),
                Data = new GeometryGroup
                {
                    FillRule = FillRule.EvenOdd,
                    Children =
                    {
                        new EllipseGeometry { Center = new Point(cx, 18), RadiusX = 6.4, RadiusY = 6.4 },
                        new EllipseGeometry { Center = new Point(cx, 18), RadiusX = 4.4, RadiusY = 4.4 },
                    },
                },
            });
            _lid[k] = Box(cx - 5, 13.2, 10, 0, 0, Ink); // the lid stays inside the dark window
            Add(_lid[k]);
        }
        Add(Dot(15.5, 37.5, 2.6, Cheek));
        Add(Dot(48.5, 37.5, 2.6, Cheek));

        _mouthFigure.Segments.Add(_mouthUp);
        _mouthFigure.Segments.Add(_mouthDown);
        _mouthFigure.IsClosed = true;
        _mouth.Data = new PathGeometry { Figures = { _mouthFigure } };
        _mouth.Fill = Fill(0xffffff);
        _mouth.Stroke = Fill(Cream);
        _mouth.StrokeLineJoin = PenLineJoin.Round;
        Add(_mouth);
        _shade.Fill = Fill(Ink);
        Add(_shade);

        // the tongue shows only below the upper lip; it is out only while the mouth is fully open, when the lip
        // is the straight line y0, so a rectangle clip does it
        _tongue.Fill = Fill(Coral);
        _tongue.Stroke = Fill(Coral);
        _tongue.StrokeThickness = 1.4;
        _tongue.StrokeLineJoin = PenLineJoin.Round;
        _groove.Stroke = Fill(CoralDk);
        _groove.StrokeThickness = 1.3;
        _groove.StrokeLineJoin = PenLineJoin.Round;
        _groove.StrokeStartLineCap = _groove.StrokeEndLineCap = PenLineCap.Round;
        _tongueHost.Children.Add(_tongue);
        _tongueHost.Children.Add(_groove);
        Add(_tongueHost);

        root.RenderTransform = _fit;
        return root;
    }

    /// <summary>Scale the 64 grid into the control: the logo's x 0..64 and y 5..58 (body top to tongue tip).</summary>
    private void Fit()
    {
        double w = ActualWidth, h = ActualHeight;
        if (w <= 0 || h <= 0)
        {
            return;
        }
        double s = Math.Min(w / 64, h / 53);
        _fit.ScaleX = _fit.ScaleY = s;
        _fit.TranslateX = (w - 64 * s) / 2;
        _fit.TranslateY = (h - 53 * s) / 2 - 5 * s;
    }

    // ================================================================== geometry (mouthGeom / tongueGeom)

    private static double Clamp01(double v) => Math.Max(0, Math.Min(1, v));

    private void Render(double m, double len, Func<double, double> bend, double y, double rot, (double X, double Y) gaze, double lid)
    {
        _rot.Angle = rot;
        _move.Y = Lower + y;
        _pupil[0].X = _pupil[1].X = gaze.X * 2;
        _pupil[0].Y = _pupil[1].Y = gaze.Y * 1.7;
        foreach (var l in _lid)
        {
            l.Height = lid * 9.2;
        }

        // mouth, by openness m: 0 the closed smile (a stroked curve), 1 fully open
        double xl = 26 - 1.5 * m, xr = 38 + 1.5 * m, y0 = 35.5 - 1.5 * m, top = 40.5 - 6.5 * m, bot = 40.5 + 8.5 * m;
        _mouthFigure.StartPoint = new Point(xl, y0);
        _mouthUp.Point1 = new Point(32, top);
        _mouthUp.Point2 = new Point(xr, y0);
        _mouthDown.Point1 = new Point(32, bot);
        _mouthDown.Point2 = new Point(xl, y0);
        _mouth.StrokeThickness = 2.6 * (1 - m);

        // the shade under the upper lip: the strip y0..y0+1.8 of the mouth (where the lower curve crosses it)
        double tt = (1 - Math.Sqrt(1 - 4 * (1.8 / (2 * (bot - y0))))) / 2;   // t with y(t) = y0 + 1.8 on the lower curve
        double qx(double a, double b) => a * (1 - tt) * (1 - tt) + 64 * tt * (1 - tt) + b * tt * tt;
        _shade.Points = new PointCollection
        {
            new Point(xl, y0), new Point(xr, y0), new Point(qx(xr, xl), y0 + 1.8), new Point(qx(xl, xr), y0 + 1.8),
        };
        _shade.Opacity = 0.2 * m;

        _tongueHost.Clip = new RectangleGeometry { Rect = new Rect(-40, y0, 144, 86) };
        RenderTongue((y0 + top) / 2 - 1, len, bend);
    }

    private void RenderTongue(double rootY, double len, Func<double, double> bend)
    {
        if (len <= 0.01)
        {
            _tongue.Visibility = _groove.Visibility = Visibility.Collapsed;
            return;
        }
        _tongue.Visibility = _groove.Visibility = Visibility.Visible;
        double full = Shaft + Head;
        double k = Clamp01(len / 0.35), sl = Shaft * Math.Max(0, (len - 0.35) / 0.65), hl = Head * k;
        double wS = ShaftHalf * k, wH = HeadHalf * k;

        // the spine: 7 stops along the shaft, then the head's middle and tip
        var stops = new double[9];
        for (int i = 0; i < 7; i++)
        {
            stops[i] = sl * i / 6;
        }
        stops[7] = sl + hl * 0.5;
        stops[8] = sl + hl;
        double x = 32, y = rootY, s = 0;
        var p = new (double X, double Y, double Nx, double Ny)[9];
        for (int j = 0; j < 9; j++)
        {
            double ds = (stops[j] - s) / 4;
            for (int i = 0; i < 4; i++)
            {
                double a0 = bend((s + ds / 2) / full);
                x += Math.Sin(a0) * ds;
                y += Math.Cos(a0) * ds;
                s += ds;
            }
            double a = bend(s / full);
            p[j] = (x, y, Math.Cos(a), -Math.Sin(a));
        }
        Point At(int i, double w) => new(p[i].X + p[i].Nx * w, p[i].Y + p[i].Ny * w);

        var outline = new PointCollection { new Point(32 - wS, rootY - 2), new Point(32 + wS, rootY - 2) };
        for (int i = 0; i < 7; i++) outline.Add(At(i, wS));
        outline.Add(At(6, wH));
        outline.Add(At(7, wH / 2));
        outline.Add(At(8, 0));
        outline.Add(At(7, -wH / 2));
        outline.Add(At(6, -wH));
        for (int i = 6; i >= 0; i--) outline.Add(At(i, -wS));
        _tongue.Points = outline;

        var groove = new PointCollection();
        for (int i = 2; i < 7; i++) groove.Add(At(i, 0));
        groove.Add(new Point((p[6].X * 2 + p[7].X) / 3, (p[6].Y * 2 + p[7].Y) / 3));
        _groove.Points = groove;
        _groove.Opacity = 0.8 * Clamp01((len - 0.5) / 0.3);
    }

    // ================================================================== idle animation (class Idle)

    private double Rnd(double a, double b) => a + _rnd.NextDouble() * (b - a);
    private static double Smooth(double k) => k * k * (3 - 2 * k);
    private static double BackOut(double k) => 1 + 2.70158 * Math.Pow(k - 1, 3) + 1.70158 * Math.Pow(k - 1, 2); // overshoots, then settles
    private static double Ease(double rate, double dt) => 1 - Math.Exp(-rate * dt);

    private void Restart()
    {
        _clock = 0;
        _gaze = (0, 1);
        _target = (0, 0.2);
        _nextGaze = Rnd(0.8, 2);
        _nextBlink = Rnd(1.5, 3);
        _blinkStart = -1;
        _phase = Phase.Out;
        _since = 0;
        _until = Rnd(4, 7);
        _popAt = -10;
    }

    private void Go(Phase phase, double t)
    {
        _phase = phase;
        _since = t;
    }

    private void OnRendering(object? sender, object e)
    {
        try
        {
            var now = ((RenderingEventArgs)e).RenderingTime;
            if (_last != TimeSpan.MinValue && (now - _last).TotalSeconds < 1 / Fps - 0.004)
            {
                return;
            }
            double dt = _last == TimeSpan.MinValue ? 0 : Math.Min(0.05, (now - _last).TotalSeconds);
            _last = now;
            _clock += dt;
            Step(_clock, dt);
        }
        catch (Exception)
        {
            Detach(); // never let a drawing problem reach the app
        }
    }

    private void Step(double t, double dt)
    {
        // tongue and mouth: out -> retract -> close -> smile -> open -> extend -> out
        double len = 0, m = 1;
        double K(double dur) => Math.Min(1, (t - _since) / dur);
        switch (_phase)
        {
            case Phase.Out:
                len = 1;
                if (t > _until) Go(Phase.Retract, t);
                break;
            case Phase.Retract:
            {
                double p = K(0.32);
                len = 1 - p * p * p;
                if (p >= 1) Go(Phase.Close, t);
                break;
            }
            case Phase.Close:
            {
                double p = K(0.2);
                m = 1 - Smooth(p);
                if (p >= 1)
                {
                    Go(Phase.Smile, t);
                    _until = t + Rnd(3, 6);
                    _nextBlink = Math.Min(_nextBlink, t + 0.05);
                }
                break;
            }
            case Phase.Smile:
                m = 0;
                if (t > _until) Go(Phase.Open, t);
                break;
            case Phase.Open:
            {
                double p = K(0.2);
                m = Smooth(p);
                if (p >= 1)
                {
                    Go(Phase.Extend, t);
                    _popAt = t;
                }
                break;
            }
            case Phase.Extend:
            {
                double p = K(0.45);
                len = BackOut(p);
                if (p >= 1)
                {
                    Go(Phase.Out, t);
                    _until = t + Rnd(5, 9);
                }
                break;
            }
        }

        // body
        double y = 1.3 * Math.Sin(Tau * t / 3.2), rot = 1.4 * Math.Sin(Tau * t / 5.3);

        // tongue bend: hangs with gravity as the body tilts, sways slowly, carries a ripple that travels from
        // root to tip, and wobbles for a moment after popping out
        double sincePop = t - _popAt, pop = 24 * Math.Exp(-3.2 * sincePop);
        double Bend(double u) => (rot * u + 6 * Math.Sin(Tau * t / 2.6) * Math.Pow(u, 1.2) + 10 * Math.Sin(Tau * t / 1.5 - 2.4 * u) * u
                                  + pop * Math.Sin(15 * sincePop - 2 * u) * u) * Math.PI / 180;

        // gaze: watch the tongue come out, otherwise a random glance every second or three
        if (_phase == Phase.Extend || (_phase == Phase.Out && sincePop < 1.1))
        {
            _target = (0, 1);
            _nextGaze = t + 0.3;
        }
        else if (t > _nextGaze)
        {
            double c = _rnd.NextDouble();
            _target = c < 0.35 ? (0, 0.2) : c < 0.5 ? (0, 1) : (Rnd(-1, 1), Rnd(-0.7, 0.8));
            _nextGaze = t + Rnd(0.8, 3);
        }
        _gaze = (_gaze.X + (_target.X - _gaze.X) * Ease(14, dt), _gaze.Y + (_target.Y - _gaze.Y) * Ease(14, dt));

        // blinks, occasionally double; a closed eye keeps a thin sliver at the bottom
        double lid = 0;
        if (_blinkStart < 0 && t > _nextBlink) _blinkStart = t;
        if (_blinkStart >= 0)
        {
            double b = (t - _blinkStart) / 0.17;
            if (b >= 1)
            {
                _blinkStart = -1;
                _nextBlink = t + (_rnd.NextDouble() < 0.2 ? 0.12 : Rnd(2.2, 6));
            }
            else
            {
                lid = 0.86 * Math.Pow(Math.Sin(Math.PI * b), 0.6);
            }
        }

        Render(m, len, Bend, y, rot, _gaze, lid);
    }
}
