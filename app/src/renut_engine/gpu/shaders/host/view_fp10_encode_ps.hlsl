// halo3-nx (2026-11-03): one EDRAM view read through another (draw.cpp, SyncColorViewLocked). Halo 3 draws its last pass to
// a 2_10_10_10_FLOAT surface with a colour exponent bias and resolves the same EDRAM through a 2_10_10_10 surface: on the
// console that reads the 7e3 floats' bits as 10-bit unorm, which makes a log-like curve for the display out of the float
// format. Here the views are separate host targets, the float one RGBA16F, so its values are encoded as the console would
// have stored them. g_Push.param0 is 2^bias (a host target holds the shader's output, without the bias); 1 with
// blur_renut_target_bias_rgb (blur-nx, 2026-11-17: the target holds the biased colour, as the EDRAM does). The pixel plus
// g_Push.translate is the source texel; present_vs draws the fullscreen triangle.
#include "utility.hlsli"

// The 10 bits of a 7e3 float: 3 bits of exponent, 7 of mantissa; 0 to 31.875, denormal below 0.25.
float Encode7e3(float value)
{
    const float v = clamp(value, 0.0, 31.875);
    if (!(v >= 0.25))
        return floor(v * 512.0 + 0.5);
    const float exponent = floor(log2(v));
    return (exponent + 3.0) * 128.0 + floor((v / pow(2.0, exponent) - 1.0) * 128.0 + 0.5);
}

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    const float4 c = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0));
    return float4(Encode7e3(c.r * g_Push.param0), Encode7e3(c.g * g_Push.param0), Encode7e3(c.b * g_Push.param0), 0.0) / 1023.0 +
           float4(0.0, 0.0, 0.0, c.a);
}
