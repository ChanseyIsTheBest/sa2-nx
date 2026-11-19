// banjo-nx (2026-10-21, banjo_renut_smaa): SMAA 1x (Jimenez, Echevarria, Masia, Navarro, Gutierrez: app/thirdparty/smaa,
// MIT licence) on the frame's final composite, before the game draws its HUD over it (render/smaa.cpp).
//
// The passes use utility.hlsli's bindless tables and push block:
//   scale        1 / target size      } SMAA_RT_METRICS
//   translate    target size          }
//   textureIndex the pass's first texture (edge detection, blending: the colour copy; weights: the edges)
//   samplerIndex the linear clamp sampler
//   param0       edge detection: the threshold; weights: the area texture; blending: the weights (slots as raw bits:
//                Ryujinx follows a bindless handle only back to a constant buffer load, as present_ps.hlsl's ramp)
//   param1       weights: the search texture
// SMAA's point samples are texel loads (the colour image is read at pixel centres only), clamped to the image as the
// reference's clamp sampler would; its other samples use the linear sampler at level 0 (every texture has one level).
#include "utility.hlsli"

#define SMAA_RT_METRICS float4(g_Push.scale, g_Push.translate)
#define SMAA_CUSTOM_SL 1
#define SMAA_LINEAR_SAMPLER g_Samplers[NonUniformResourceIndex(g_Push.samplerIndex)]

float4 SmaaLoadClamped(Texture2D<float4> tex, float2 coord)
{
    const int2 size = int2(SMAA_RT_METRICS.zw);
    const int2 texel = clamp(int2(floor(coord * SMAA_RT_METRICS.zw)), int2(0, 0), size - 1);
    return tex.Load(int3(texel, 0));
}

#define SMAATexture2D(tex) Texture2D<float4> tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(SMAA_LINEAR_SAMPLER, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(SMAA_LINEAR_SAMPLER, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(SMAA_LINEAR_SAMPLER, coord, 0)
#if defined(SMAA_POINT_FROM_PUSH)
// The edge pass: samplerIndex is the point clamp sampler (it takes no linear samples): the hardware clamps, the same
// texels as the loads below without their arithmetic.
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(SMAA_LINEAR_SAMPLER, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(SMAA_LINEAR_SAMPLER, coord, 0)
#else
#define SMAASampleLevelZeroPoint(tex, coord) SmaaLoadClamped(tex, coord)
#define SMAASamplePoint(tex, coord) SmaaLoadClamped(tex, coord)
#endif
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(SMAA_LINEAR_SAMPLER, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]

#define SmaaTexture(slot) g_Textures[NonUniformResourceIndex(slot)]

struct SmaaPSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

#include "../../../../../thirdparty/smaa/SMAA.hlsl"
