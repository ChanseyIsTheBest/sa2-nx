// banjo-nx (2026-10-21, banjo_renut_smaa): SMAA pass 2, blending weights from the edges (textureIndex), the area texture
// (param0's bits) and the search texture (param1's bits), SMAA 1x. SMAA's medium preset: 8 search steps, no diagonal or
// corner detection (smaa_weights_high_ps.hlsl is the high preset).
#ifndef SMAA_PRESET_HIGH
#define SMAA_PRESET_MEDIUM
#endif
#include "smaa.hlsli"

// banjo-nx: only the pixels with an edge pass the depth mask (smaa_edge_ps.hlsl): tested before the shader.
[earlydepthstencil]
float4 main(SmaaPSInput input) : SV_Target0
{
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(input.uv, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(input.uv, pixcoord, offset, SmaaTexture(g_Push.textureIndex),
                                           SmaaTexture(asuint(g_Push.param0)), SmaaTexture(asuint(g_Push.param1)),
                                           float4(0.0, 0.0, 0.0, 0.0));
}
