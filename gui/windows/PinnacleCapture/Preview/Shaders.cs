namespace PinnacleCapture.Preview;

/// <summary>
/// Embedded HLSL for the preview: a fullscreen-triangle vertex shader (no
/// vertex buffer needed -- SV_VertexID drives it) and a pixel shader that
/// samples the Y/Cb/Cr planes and applies the 3x4 YUV->RGB matrix the core
/// hands back from pin_yuv_to_rgb_matrix (row-major, 4th column is the
/// offset, expects 0..1 normalised samples -- which is exactly what an
/// R8_UNorm SRV already gives us).
/// </summary>
internal static class Shaders
{
    public const string VertexShaderSource = @"
struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VsOut VSMain(uint id : SV_VertexID)
{
    // Fullscreen triangle covering the viewport; the viewport itself is set
    // to the letterboxed rect computed via pin_fit_rect, so this triangle
    // only ever needs to cover that viewport, not the whole render target.
    VsOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
";

    public const string PixelShaderSource = @"
Texture2D texY : register(t0);
Texture2D texU : register(t1);
Texture2D texV : register(t2);
SamplerState samp : register(s0);

cbuffer YuvMatrix : register(b0)
{
    float4 row0;
    float4 row1;
    float4 row2;
};

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float4 PSMain(VsOut i) : SV_Target
{
    float y = texY.Sample(samp, i.uv).r;
    float u = texU.Sample(samp, i.uv).r;
    float v = texV.Sample(samp, i.uv).r;
    float4 yuv1 = float4(y, u, v, 1.0);
    float r = dot(row0, yuv1);
    float g = dot(row1, yuv1);
    float b = dot(row2, yuv1);
    return float4(saturate(r), saturate(g), saturate(b), 1.0);
}
";
}
