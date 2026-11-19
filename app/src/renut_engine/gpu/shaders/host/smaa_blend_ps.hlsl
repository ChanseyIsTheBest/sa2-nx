// banjo-nx (2026-10-21, banjo_renut_smaa): SMAA pass 3, neighbourhood blending of the colour copy (textureIndex) with the
// weights (param0's bits), written into the game's target (its alpha kept: the pipeline writes red, green, blue).
#include "smaa.hlsli"

// banjo-nx: only the pixels it can change pass the depth mask (smaa_edge_ps.hlsl): tested before the shader.
[earlydepthstencil]
float4 main(SmaaPSInput input) : SV_Target0
{
    float4 offset;
    SMAANeighborhoodBlendingVS(input.uv, offset);
    return SMAANeighborhoodBlendingPS(input.uv, offset, SmaaTexture(g_Push.textureIndex),
                                      SmaaTexture(asuint(g_Push.param0)));
}
