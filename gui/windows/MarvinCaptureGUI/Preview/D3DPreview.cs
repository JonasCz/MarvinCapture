using System;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.UI.Xaml.Controls;
using PinnacleCapture.Interop;
using Vortice.Direct3D;
using Vortice.Direct3D11;
using Vortice.DXGI;
using Vortice.D3DCompiler;
using Vortice.Mathematics;

namespace PinnacleCapture.Preview;

/// <summary>
/// Owns the Direct3D11 swap chain bound to the right-pane SwapChainPanel and
/// a dedicated render thread that blocks in pin_preview_wait() and only
/// presents on a genuinely new frame -- there is no timer, no polling loop,
/// so idle CPU stays near zero exactly as the spec asks for.
///
/// Threading contract: every D3D call (device context use, ResizeBuffers,
/// Present) happens either on the render thread or under `_gate`. The UI
/// thread only ever touches `_gate`-guarded state through
/// OnPanelSizeChanged/OnCompositionScaleChanged/SetPaused, never the D3D
/// context directly, so a resize can never race a frame upload.
/// </summary>
public sealed class D3DPreview : IDisposable
{
    [ComImport]
    [Guid("63AAD0B8-7C24-40FF-85A8-640D944CC325")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface ISwapChainPanelNative
    {
        void SetSwapChain(nint swapChain);
    }

    private readonly SwapChainPanel _panel;
    private readonly Func<PinSessionHandle?> _sessionProvider;
    private readonly object _gate = new();

    private ID3D11Device? _device;
    private ID3D11DeviceContext? _context;
    private IDXGISwapChain1? _swapChain;
    private ID3D11RenderTargetView? _rtv;
    private ID3D11VertexShader? _vs;
    private ID3D11PixelShader? _ps;
    private ID3D11SamplerState? _sampler;
    private ID3D11Buffer? _matrixBuffer;

    private ID3D11Texture2D? _texY, _texU, _texV;
    private ID3D11ShaderResourceView? _srvY, _srvU, _srvV;
    private int _planeWidth, _planeHeight, _chromaShiftX, _chromaShiftY;
    private PinFrame _lastFrame; // geometry only; plane pointers are stale after unlock and never used by Draw
    private bool _hasLastFrame;

    private int _pixelWidth = 1, _pixelHeight = 1;
    private double _compositionScaleX = 1, _compositionScaleY = 1;
    private double _appliedScaleX, _appliedScaleY;

    private Thread? _renderThread;
    private volatile bool _running;
    private volatile bool _paused;
    private readonly ManualResetEventSlim _wake = new(false);

    /// <summary>Tell the render thread a session was opened (or anything else worth re-checking).</summary>
    public void Wake() => _wake.Set();
    private ulong _lastSeq;

    public bool HasSignal { get; private set; }

    /// <summary>Display aspect of the last presented frame (override applied), 0 if none yet.</summary>
    public int FrameDarNum => Volatile.Read(ref _frameDarNum);
    public int FrameDarDen => Volatile.Read(ref _frameDarDen);
    private int _frameDarNum, _frameDarDen;

    /// <summary>Environment.TickCount64 of the last presented frame (0 = never).</summary>
    public long LastFrameTick => Interlocked.Read(ref _lastFrameTick);
    private long _lastFrameTick;

    /// <summary>Forget the last frame (e.g. after the session closed) so the overlay comes back.</summary>
    public void ResetFrameState()
    {
        Interlocked.Exchange(ref _lastFrameTick, 0);
        _lastSeq = 0;
        _hasLastFrame = false;
    }

    public D3DPreview(SwapChainPanel panel, Func<PinSessionHandle?> sessionProvider)
    {
        _panel = panel;
        _sessionProvider = sessionProvider;
    }

    public void Initialize()
    {
        CreateDeviceAndSwapChain();
        CreateShaders();

        _panel.SizeChanged += (_, __) => OnPanelSizeChanged();
        _panel.CompositionScaleChanged += (_, __) => OnPanelSizeChanged();

        OnPanelSizeChanged();

        _running = true;
        _renderThread = new Thread(RenderLoop) { IsBackground = true, Name = "PinnaclePreviewRender" };
        _renderThread.Start();
    }

    private void CreateDeviceAndSwapChain()
    {
        FeatureLevel[] levels = { FeatureLevel.Level_11_1, FeatureLevel.Level_11_0, FeatureLevel.Level_10_1, FeatureLevel.Level_10_0 };
        DeviceCreationFlags flags = DeviceCreationFlags.BgraSupport;
#if DEBUG
        // Only enabled if the D3D debug layer happens to be installed; harmless otherwise since
        // D3D11CreateDevice below falls back automatically when Debug creation fails.
#endif
        ID3D11Device device;
        ID3D11DeviceContext context;
        var result = D3D11.D3D11CreateDevice(null, DriverType.Hardware, flags, levels, out device, out context);
        if (result.Failure)
        {
            // No usable GPU (e.g. a VM / RDP session): fall back to WARP so the preview still works.
            result = D3D11.D3D11CreateDevice(null, DriverType.Warp, flags, levels, out device, out context);
            result.CheckError();
        }
        _device = device;
        _context = context;

        using var dxgiDevice = _device!.QueryInterface<IDXGIDevice>();
        using var adapter = dxgiDevice.GetAdapter();
        using var factory = adapter.GetParent<IDXGIFactory2>();

        var desc = new SwapChainDescription1
        {
            Width = 1,
            Height = 1,
            Format = Format.B8G8R8A8_UNorm,
            Stereo = false,
            SampleDescription = new SampleDescription(1, 0),
            BufferUsage = Usage.RenderTargetOutput,
            BufferCount = 2,
            Scaling = Vortice.DXGI.Scaling.Stretch,
            SwapEffect = SwapEffect.FlipSequential,
            AlphaMode = Vortice.DXGI.AlphaMode.Premultiplied,
        };

        _swapChain = factory.CreateSwapChainForComposition(_device, desc);

        var panelNative = WinRT.CastExtensions.As<ISwapChainPanelNative>(_panel);
        panelNative.SetSwapChain(_swapChain.NativePointer);

        CreateRenderTargetView();
    }

    private void CreateRenderTargetView()
    {
        _rtv?.Dispose();
        using var backBuffer = _swapChain!.GetBuffer<ID3D11Texture2D>(0);
        _rtv = _device!.CreateRenderTargetView(backBuffer);
    }

    private void CreateShaders()
    {
        ReadOnlyMemory<byte> vsBlob = Compiler.Compile(Shaders.VertexShaderSource, "VSMain", "preview_vs", "vs_4_0");
        ReadOnlyMemory<byte> psBlob = Compiler.Compile(Shaders.PixelShaderSource, "PSMain", "preview_ps", "ps_4_0");
        _vs = _device!.CreateVertexShader(vsBlob.Span);
        _ps = _device!.CreatePixelShader(psBlob.Span);

        // Bilinear, clamped: handles DAR scaling and the chroma upsample for 4:2:2 / 4:1:1 / 4:2:0.
        _sampler = _device.CreateSamplerState(SamplerDescription.LinearClamp);

        _matrixBuffer = _device.CreateBuffer(new BufferDescription
        {
            ByteWidth = 12 * sizeof(float), // 3 rows of float4 (row-major 3x4, padded to vec4 per HLSL cbuffer rules)
            Usage = ResourceUsage.Dynamic,
            BindFlags = BindFlags.ConstantBuffer,
            CPUAccessFlags = CpuAccessFlags.Write,
        });
    }

    // ---- resize / visibility -------------------------------------------------------------

    private void OnPanelSizeChanged()
    {
        lock (_gate)
        {
            _compositionScaleX = _panel.CompositionScaleX <= 0 ? 1 : _panel.CompositionScaleX;
            _compositionScaleY = _panel.CompositionScaleY <= 0 ? 1 : _panel.CompositionScaleY;
            int w = Math.Max(1, (int)(_panel.ActualWidth * _compositionScaleX));
            int h = Math.Max(1, (int)(_panel.ActualHeight * _compositionScaleY));
            if (w == _pixelWidth && h == _pixelHeight && _appliedScaleX == _compositionScaleX && _appliedScaleY == _compositionScaleY)
            {
                return;
            }
            _appliedScaleX = _compositionScaleX;
            _appliedScaleY = _compositionScaleY;
            _pixelWidth = w;
            _pixelHeight = h;

            _rtv?.Dispose();
            _rtv = null;
            _swapChain!.ResizeBuffers(2, (uint)w, (uint)h, Format.B8G8R8A8_UNorm, SwapChainFlags.None);
            CreateRenderTargetView();

            // The buffers are in physical pixels, but a SwapChainPanel maps one
            // swap-chain pixel to one DIP. Scale back down by the composition
            // scale, or at 150 % DPI the picture is shown 1.5x too big and cropped.
            using var sc2 = _swapChain.QueryInterfaceOrNull<IDXGISwapChain2>();
            if (sc2 is not null)
            {
                sc2.MatrixTransform = System.Numerics.Matrix3x2.CreateScale(
                    (float)(1.0 / _compositionScaleX), (float)(1.0 / _compositionScaleY));
            }

            // Re-present the last frame at the new size so a paused / still
            // source doesn't leave a stretched or blank surface after a resize.
            if (_hasLastFrame && !_paused)
            {
                Draw(in _lastFrame);
            }
        }
    }

    public void SetPaused(bool paused)
    {
        if (_paused == paused)
        {
            return;
        }
        _paused = paused;
        _wake.Set();
        if (paused)
        {
            var s = _sessionProvider();
            if (s is not null && !s.IsInvalid)
            {
                Native.PreviewEnable(s, false);
            }
        }
        else
        {
            var s = _sessionProvider();
            if (s is not null && !s.IsInvalid)
            {
                Native.PreviewEnable(s, true);
            }
        }
    }

    // ---- render thread --------------------------------------------------------------------

    private void RenderLoop()
    {
        while (_running)
        {
            if (_paused)
            {
                // Parked, not polling: SetPaused(false) / Wake() signals us.
                _wake.Wait(1000);
                _wake.Reset();
                continue;
            }

            var session = _sessionProvider();
            if (session is null || session.IsInvalid)
            {
                HasSignal = false;
                _wake.Wait(1000); // Wake() is called when a session opens
                _wake.Reset();
                continue;
            }

            // Hold a reference on the session for the whole wait/lock/upload/unlock
            // sequence: if the UI disposes the handle meanwhile, pin_close is deferred
            // until we let go, so a locked frame is never outlived by its session.
            bool added = false;
            try
            {
                session.DangerousAddRef(ref added);
            }
            catch (ObjectDisposedException)
            {
                continue;
            }
            try
            {
                RenderOne(session);
            }
            catch (Exception ex) when (ex is not OutOfMemoryException)
            {
                // A lost device or a driver hiccup must not kill the render thread.
                System.Diagnostics.Debug.WriteLine($"Preview render failed: {ex.Message}");
                Thread.Sleep(100);
            }
            finally
            {
                if (added)
                {
                    session.DangerousRelease();
                }
            }
        }
    }

    private void RenderOne(PinSessionHandle session)
    {
        int waited = Native.PreviewWait(session, _lastSeq, 100);
        if (waited <= 0)
        {
            // Timeout (0) or session closing (-1).
            if (waited < 0)
            {
                HasSignal = false;
                Thread.Sleep(50);
            }
            return;
        }

        if (_paused || Native.PreviewLock(session, out var frame) != PinStatus.Ok)
        {
            return;
        }
        try
        {
            _lastSeq = frame.Seq;
            lock (_gate)
            {
                UploadFrame(in frame);
            }
        }
        finally
        {
            // Unlock as soon as the planes are copied; drawing needs only our textures.
            Native.PreviewUnlock(session);
        }

        lock (_gate)
        {
            _lastFrame = frame;
            _hasLastFrame = true;
            Volatile.Write(ref _frameDarNum, frame.DarNum);
            Volatile.Write(ref _frameDarDen, frame.DarDen);
            Draw(in frame);
        }
        HasSignal = true;
        Interlocked.Exchange(ref _lastFrameTick, Environment.TickCount64);
    }

    private unsafe void UploadFrame(in PinFrame frame)
    {
        EnsureTextures(frame.Width, frame.Height, frame.ChromaShiftX, frame.ChromaShiftY);

        UploadPlane(_texY!, frame.Plane0, frame.Stride0, frame.Width, frame.Height);
        int cw = Math.Max(1, frame.Width >> frame.ChromaShiftX);
        int ch = Math.Max(1, frame.Height >> frame.ChromaShiftY);
        UploadPlane(_texU!, frame.Plane1, frame.Stride1, cw, ch);
        UploadPlane(_texV!, frame.Plane2, frame.Stride2, cw, ch);

        // Constant buffer: 3x4 row-major matrix from the core, packed as
        // three float4 rows (HLSL cbuffer packing already aligns float4 to
        // 16 bytes, matching pin_yuv_to_rgb_matrix's 12-float layout).
        float[] m = Native.YuvToRgbMatrix(frame.Matrix, frame.FullRange != 0);
        var mapped = _context!.Map(_matrixBuffer!, 0, MapMode.WriteDiscard);
        new Span<float>(mapped.DataPointer.ToPointer(), 12).Clear();
        var dst = new Span<float>(mapped.DataPointer.ToPointer(), 12);
        m.AsSpan(0, 12).CopyTo(dst);
        _context.Unmap(_matrixBuffer!, 0);
    }

    private unsafe void UploadPlane(ID3D11Texture2D tex, nint srcPlane, int srcStride, int width, int height)
    {
        if (srcPlane == 0)
        {
            return;
        }
        var mapped = _context!.Map(tex, 0, MapMode.WriteDiscard);
        byte* src = (byte*)srcPlane;
        byte* dst = (byte*)mapped.DataPointer;
        int copyBytes = Math.Min(width, srcStride);
        for (int row = 0; row < height; row++)
        {
            Buffer.MemoryCopy(src + (long)row * srcStride, dst + (long)row * mapped.RowPitch, mapped.RowPitch, copyBytes);
        }
        _context.Unmap(tex, 0);
    }

    private void EnsureTextures(int width, int height, int shiftX, int shiftY)
    {
        if (_texY is not null && _planeWidth == width && _planeHeight == height &&
            _chromaShiftX == shiftX && _chromaShiftY == shiftY)
        {
            return;
        }

        _srvY?.Dispose(); _srvU?.Dispose(); _srvV?.Dispose();
        _texY?.Dispose(); _texU?.Dispose(); _texV?.Dispose();

        _planeWidth = width; _planeHeight = height; _chromaShiftX = shiftX; _chromaShiftY = shiftY;
        int cw = Math.Max(1, width >> shiftX);
        int ch = Math.Max(1, height >> shiftY);

        _texY = CreatePlaneTexture(width, height);
        _texU = CreatePlaneTexture(cw, ch);
        _texV = CreatePlaneTexture(cw, ch);
        _srvY = _device!.CreateShaderResourceView(_texY);
        _srvU = _device.CreateShaderResourceView(_texU);
        _srvV = _device.CreateShaderResourceView(_texV);
    }

    private ID3D11Texture2D CreatePlaneTexture(int width, int height) => _device!.CreateTexture2D(new Texture2DDescription
    {
        Width = (uint)Math.Max(1, width),
        Height = (uint)Math.Max(1, height),
        MipLevels = 1,
        ArraySize = 1,
        Format = Format.R8_UNorm,
        SampleDescription = new SampleDescription(1, 0),
        Usage = ResourceUsage.Dynamic,
        BindFlags = BindFlags.ShaderResource,
        CPUAccessFlags = CpuAccessFlags.Write,
    });

    private void Draw(in PinFrame frame)
    {
        if (_context is null || _rtv is null)
        {
            return;
        }

        _context.OMSetRenderTargets(_rtv);
        _context.ClearRenderTargetView(_rtv, new Color4(0, 0, 0, 1));

        Native.FitRect(frame.DarNum, frame.DarDen, _pixelWidth, _pixelHeight, out int x, out int y, out int rw, out int rh);
        _context.RSSetViewport(new Viewport(x, y, Math.Max(1, rw), Math.Max(1, rh)));

        _context.IASetPrimitiveTopology(PrimitiveTopology.TriangleList);
        _context.VSSetShader(_vs);
        _context.PSSetShader(_ps);
        _context.PSSetShaderResources(0, new[] { _srvY!, _srvU!, _srvV! });
        _context.PSSetSampler(0, _sampler);
        _context.VSSetConstantBuffer(0, _matrixBuffer);
        _context.PSSetConstantBuffer(0, _matrixBuffer);
        _context.Draw(3, 0);

        _swapChain!.Present(1, PresentFlags.None);
    }

    public void Dispose()
    {
        _running = false;
        _wake.Set();
        _renderThread?.Join(500);

        _srvY?.Dispose(); _srvU?.Dispose(); _srvV?.Dispose();
        _texY?.Dispose(); _texU?.Dispose(); _texV?.Dispose();
        _matrixBuffer?.Dispose();
        _sampler?.Dispose();
        _vs?.Dispose();
        _ps?.Dispose();
        _rtv?.Dispose();
        _swapChain?.Dispose();
        _context?.Dispose();
        _device?.Dispose();
    }
}
