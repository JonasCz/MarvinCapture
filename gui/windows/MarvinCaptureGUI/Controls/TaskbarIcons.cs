using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using PinnacleCapture.Interop;

namespace PinnacleCapture.Controls;

/// <summary>
/// Draws the small taskbar icons (overlay dot, thumbnail-toolbar glyphs) at run time, so no
/// asset files are needed. Every icon is a 32-bit straight-alpha HICON the caller destroys
/// with <see cref="Win32.DestroyIcon"/>.
/// </summary>
internal static class TaskbarIcons
{
    /// <summary>A red dot with a thin white ring (readable on light and dark taskbars), size x size pixels.</summary>
    public static nint CreateCaptureDot(int size)
    {
        var px = new int[size * size];
        double c = size / 2.0, red = size * 0.375, ring = size / 2.0 - 0.25;
        const int N = 4; // 4 x 4 samples per pixel
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                double sa = 0, sr = 0, sg = 0, sb = 0;
                for (int j = 0; j < N; j++)
                {
                    for (int i = 0; i < N; i++)
                    {
                        double dx = x + (i + 0.5) / N - c, dy = y + (j + 0.5) / N - c;
                        double d = Math.Sqrt(dx * dx + dy * dy);
                        if (d <= red)
                        {
                            sa += 1; sr += 0xE8; sg += 0x11; sb += 0x23; // Windows red
                        }
                        else if (d <= ring)
                        {
                            sa += 1; sr += 255; sg += 255; sb += 255;
                        }
                    }
                }
                if (sa > 0)
                {
                    int a = (int)Math.Round(255 * sa / (N * N));
                    px[y * size + x] = Pack(a, (int)(sr / sa), (int)(sg / sa), (int)(sb / sa));
                }
            }
        }
        return FromPixels(size, px);
    }

    private static string? _iconFont;

    /// <summary>
    /// One glyph of Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10), in a colour
    /// that contrasts with the taskbar theme. Grey-scale antialiased text is drawn white on black into
    /// a DIB; the coverage then becomes the alpha channel.
    /// </summary>
    public static nint CreateGlyph(string glyph, int size)
    {
        int fg = TaskbarIsLight() ? 0x000000 : 0xFFFFFF;
        nint dc = Win32.CreateCompatibleDC(0);
        nint dib = 0, font = 0, oldBmp = 0, oldFont = 0;
        var cov = new int[size * size];
        try
        {
            var bmi = Header(size);
            dib = Win32.CreateDIBSection(dc, in bmi, 0, out nint bits, 0, 0);
            if (dib == 0 || bits == 0)
            {
                return 0;
            }
            oldBmp = Win32.SelectObject(dc, dib);
            font = MakeFont(dc, size * 7 / 8);
            oldFont = Win32.SelectObject(dc, font);
            Win32.SetBkMode(dc, 1); // TRANSPARENT
            Win32.SetTextColor(dc, 0xFFFFFF);
            Win32.GetTextExtentPoint32W(dc, glyph, glyph.Length, out var ext);
            Win32.TextOutW(dc, (size - ext.Cx) / 2, (size - ext.Cy) / 2, glyph, glyph.Length);
            Win32.GdiFlush();
            Marshal.Copy(bits, cov, 0, cov.Length);
        }
        finally
        {
            if (oldFont != 0) Win32.SelectObject(dc, oldFont);
            if (oldBmp != 0) Win32.SelectObject(dc, oldBmp);
            if (font != 0) Win32.DeleteObject(font);
            if (dib != 0) Win32.DeleteObject(dib);
            Win32.DeleteDC(dc);
        }
        var px = new int[cov.Length];
        for (int i = 0; i < px.Length; i++)
        {
            int a = Math.Max(Math.Max((cov[i] >> 16) & 0xFF, (cov[i] >> 8) & 0xFF), cov[i] & 0xFF);
            px[i] = Pack(a, (fg >> 16) & 0xFF, (fg >> 8) & 0xFF, fg & 0xFF);
        }
        return FromPixels(size, px);
    }

    private static unsafe nint MakeFont(nint dc, int height)
    {
        if (_iconFont is not null)
        {
            return Create(_iconFont);
        }
        // GDI silently substitutes a missing face: ask what it picked.
        foreach (var name in new[] { "Segoe Fluent Icons", "Segoe MDL2 Assets" })
        {
            nint f = Create(name);
            nint old = Win32.SelectObject(dc, f);
            Span<char> face = stackalloc char[64];
            int n;
            fixed (char* pf = face)
            {
                n = Win32.GetTextFaceW(dc, face.Length, pf);
            }
            Win32.SelectObject(dc, old);
            if (new string(face.Slice(0, Math.Max(0, n - 1))).Equals(name, StringComparison.OrdinalIgnoreCase))
            {
                _iconFont = name;
                return f;
            }
            Win32.DeleteObject(f);
        }
        _iconFont = "Segoe MDL2 Assets";
        return Create(_iconFont);

        nint Create(string face) =>
            Win32.CreateFontW(-height, 0, 0, 0, 400, 0, 0, 0, 1 /*DEFAULT_CHARSET*/, 0, 0, 4 /*ANTIALIASED_QUALITY*/, 0, face);
    }

    /// <summary>True when the taskbar uses the light theme (then white glyphs would vanish).</summary>
    public static bool TaskbarIsLight()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            return key?.GetValue("SystemUsesLightTheme") is int v && v != 0;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private static int Pack(int a, int r, int g, int b) => (a << 24) | (r << 16) | (g << 8) | b;

    private static Win32.BITMAPINFOHEADER Header(int size) => new()
    {
        Size = (uint)Marshal.SizeOf<Win32.BITMAPINFOHEADER>(),
        Width = size,
        Height = -size, // top-down
        Planes = 1,
        BitCount = 32,
    };

    private static nint FromPixels(int size, int[] px)
    {
        nint screen = Win32.GetDC(0);
        nint color = 0, mask = 0, zeros = 0;
        try
        {
            var bmi = Header(size);
            color = Win32.CreateDIBSection(screen, in bmi, 0, out nint bits, 0, 0);
            if (color == 0 || bits == 0)
            {
                return 0;
            }
            Marshal.Copy(px, 0, bits, px.Length);
            // The AND mask is ignored for 32-bit alpha icons but must exist: all zero.
            int maskBytes = (size + 15) / 16 * 2 * size;
            zeros = Marshal.AllocHGlobal(maskBytes);
            for (int i = 0; i < maskBytes; i++)
            {
                Marshal.WriteByte(zeros, i, 0);
            }
            mask = Win32.CreateBitmap(size, size, 1, 1, zeros);
            var info = new Win32.ICONINFO { IsIcon = 1, Mask = mask, Color = color };
            return Win32.CreateIconIndirect(in info);
        }
        finally
        {
            if (zeros != 0) Marshal.FreeHGlobal(zeros);
            if (mask != 0) Win32.DeleteObject(mask);
            if (color != 0) Win32.DeleteObject(color);
            Win32.ReleaseDC(0, screen);
        }
    }
}
