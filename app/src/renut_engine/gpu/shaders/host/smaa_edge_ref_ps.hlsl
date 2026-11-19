// banjo-nx (2026-10-21, banjo_renut_diag_smaa_compare_s only): SMAA's own luma edge detection, unchanged
// (SMAALumaEdgeDetectionPS with its discard, into a target cleared to 0), the start of the reference the comparison
// frames check the optimised passes against (render/smaa.cpp).
#define SMAA_THRESHOLD g_Push.param0
#define SMAA_POINT_FROM_PUSH 1
#include "smaa.hlsli"

float4 main(SmaaPSInput input) : SV_Target0
{
    float4 offset[3];
    SMAAEdgeDetectionVS(input.uv, offset);
    return float4(SMAALumaEdgeDetectionPS(input.uv, offset, SmaaTexture(g_Push.textureIndex)), 0.0, 0.0);
}
