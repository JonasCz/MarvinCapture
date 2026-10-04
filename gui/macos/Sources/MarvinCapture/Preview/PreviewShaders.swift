// Pinnacle Studio 500-USB open driver
// Copyright (C) 2026 Jonas Cz.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
// for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

/// Metal Shading Language source of the preview, port of Windows' Shaders.cs. It is compiled at run
/// time (`MTLDevice.makeLibrary(source:options:)`) because the Command Line Tools ship no `metal`
/// compiler, so there is no build-time .metallib.
///
/// A fullscreen-triangle vertex shader (vertex_id drives it, no vertex buffer) and a fragment shader
/// that samples the Y / Cb / Cr planes (r8Unorm textures, so samples are already 0..1) and applies the
/// core's 3x4 matrix from pin_yuv_to_rgb_matrix (row-major, fourth column = offset). The viewport is
/// set to the letterboxed rect, so the triangle only has to cover that.
enum PreviewShaders {
    static let source = """
    #include <metal_stdlib>
    using namespace metal;

    struct VsOut {
        float4 pos [[position]];
        float2 uv;
    };

    vertex VsOut previewVertex(uint id [[vertex_id]]) {
        VsOut o;
        float2 uv = float2(float((id << 1) & 2u), float(id & 2u));
        o.uv = uv;
        o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
        return o;
    }

    // Three float4 rows = the 12 floats of pin_yuv_to_rgb_matrix, in order.
    struct YuvMatrix {
        float4 row0;
        float4 row1;
        float4 row2;
    };

    fragment float4 previewFragment(VsOut in [[stage_in]],
                                    texture2d<float> texY [[texture(0)]],
                                    texture2d<float> texU [[texture(1)]],
                                    texture2d<float> texV [[texture(2)]],
                                    constant YuvMatrix &m [[buffer(0)]]) {
        // Bilinear + clamped: the DAR scaling and the chroma upsample (4:2:2 / 4:1:1 / 4:2:0).
        constexpr sampler samp(coord::normalized, address::clamp_to_edge, filter::linear);
        float y = texY.sample(samp, in.uv).r;
        float u = texU.sample(samp, in.uv).r;
        float v = texV.sample(samp, in.uv).r;
        float4 yuv1 = float4(y, u, v, 1.0);
        return float4(saturate(float3(dot(m.row0, yuv1), dot(m.row1, yuv1), dot(m.row2, yuv1))), 1.0);
    }
    """
}
