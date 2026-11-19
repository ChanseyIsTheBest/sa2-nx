// blur-nx (2026-11-10, port 76; its key 216: blur_renut_sample_grid_resolve): the resolve of a multisampled view, read
// from the single-sampled target over the same EDRAM (render/draw.cpp DrawSampleGridResolveLocked). In the EDRAM a 4x
// view's pixel is a 2x2 block of samples and a 2x view's a 1x2 block, so the single-sampled target of the same sample
// grid holds each of the view's samples as one of its pixels; the resolve averages a pixel's samples, as here.
//   translate: the view pixel that destination pixel (0, 0) resolves (the source rect's corner less the destination's)
//   param0 / param1: the samples across / down of a view pixel (4x: 2 / 2; 2x: 1 / 2)
//   scale.x: the colour scale (the resolve's exponent bias as 2^bias)
#include "utility.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    Texture2D<float4> source = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)];
    uint width, height;
    source.GetDimensions(width, height);
    const int2 samples = int2(g_Push.param0, g_Push.param1);
    const int2 origin = (int2(position.xy) + int2(g_Push.translate)) * samples;
    const int2 last = int2(width, height) - 1;
    float4 sum = 0.0;
    for (int y = 0; y < samples.y; ++y)
    {
        for (int x = 0; x < samples.x; ++x)
            sum += source.Load(int3(min(origin + int2(x, y), last), 0));
    }
    // scale.x: the resolve's colour scale, 2^exponent bias (blur_renut_resolve_exp_bias, port 76's key 219; 1 without).
    return sum / float(samples.x * samples.y) * g_Push.scale.x;
}
