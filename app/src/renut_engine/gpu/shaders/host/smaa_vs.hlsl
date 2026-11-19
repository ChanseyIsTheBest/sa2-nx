// banjo-nx (2026-10-21, banjo_renut_smaa): the fullscreen triangle of the SMAA passes (smaa.hlsli), texture
// coordinates 0..1 over the target (present_vs.hlsl's triangle without its scale and translate, which the SMAA passes
// use for the target's size).
#include "utility.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    VSOutput output;
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = uv;
    return output;
}
