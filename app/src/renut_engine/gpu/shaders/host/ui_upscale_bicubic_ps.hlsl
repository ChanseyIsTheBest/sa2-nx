// banjo-nx (2026-10-21, internal resolution): ui_upscale_ps.hlsl's pass with bicubic Catmull-Rom (banjo_renut_upscale_filter
// 1; bilinear is the default, the user's choice): sharper, five bilinear samples a pixel (render/ui_scale.cpp).
//   scale        1 / the source's host size
//   translate    the source's host size
//   textureIndex the source (the composite's target at the internal resolution)
//   samplerIndex the linear clamp sampler
// The usual five-tap form of the 4x4 Catmull-Rom filter: the middle 2x2 taps of each axis merged into one bilinear sample
// at the weighted position, and the four corner taps left out (their weights are the products of the two small outer
// ones), the rest normalised (as in Jimenez's Filmic SMAA and Unreal Engine's temporal upsampling).
#include "utility.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    Texture2D<float4> source = g_Textures[g_Push.textureIndex];
    SamplerState linearClamp = g_Samplers[g_Push.samplerIndex];
    const float2 samplePos = uv * g_Push.translate;
    const float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    const float2 f = samplePos - texPos1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 texPos0 = (texPos1 - 1.0) * g_Push.scale;
    const float2 texPos3 = (texPos1 + 2.0) * g_Push.scale;
    const float2 texPos12 = (texPos1 + w2 / w12) * g_Push.scale;

    float4 result = source.SampleLevel(linearClamp, float2(texPos12.x, texPos0.y), 0) * (w12.x * w0.y);
    result += source.SampleLevel(linearClamp, float2(texPos0.x, texPos12.y), 0) * (w0.x * w12.y);
    result += source.SampleLevel(linearClamp, texPos12, 0) * (w12.x * w12.y);
    result += source.SampleLevel(linearClamp, float2(texPos3.x, texPos12.y), 0) * (w3.x * w12.y);
    result += source.SampleLevel(linearClamp, float2(texPos12.x, texPos3.y), 0) * (w12.x * w3.y);
    const float weight = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return saturate(result / weight);
}
