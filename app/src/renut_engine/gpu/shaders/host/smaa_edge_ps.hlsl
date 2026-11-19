// banjo-nx (2026-10-21, banjo_renut_smaa): SMAA pass 1. The game's target (textureIndex) read once a pixel, written out
// three ways, for every pixel:
//   SV_Target0  the luma edges (R8G8): SMAALumaEdgeDetectionPS's arithmetic, without its discard (0 where it would
//               discard, as the cleared target the reference leaves there);
//   SV_Target1  the pixel itself: the copy the blend pass reads while it writes the target;
//   SV_Depth    a mask for the next passes' early depth test (LESS): 1 where the pixel has an edge (the weights pass
//               shades only those: its result is 0 everywhere else), 0.5 where it has none but the blend pass can still
//               change it (the right or bottom neighbour's left or top edge needs that luma step from this pixel; the
//               local contrast adaptation only removes edges, so this is a superset), 0 elsewhere (the blend pass would
//               write the pixel unchanged).
// 36w wrote only the masked pixels (cleared targets, a discard, a copy pass more): the console said the pass is bound by
// its samples, not its writes (edges 1.46 ms either way), and the copy pass cost 0.27 ms: back to this form (36y).
// Not taken: the lumas shared within each 2x2 quad through fine derivatives (3 samples instead of 5): in Ryujinx the
// result differed from SMAA's own (emulator run 141).
// param0 = the threshold; samplerIndex = the point clamp sampler (SMAA's point samples: texel centres).
#define SMAA_THRESHOLD g_Push.param0
#define SMAA_POINT_FROM_PUSH 1
#include "smaa.hlsli"

struct EdgeOutput
{
    float4 edges : SV_Target0;
    float4 color : SV_Target1;
    float depth : SV_Depth;
};

EdgeOutput main(SmaaPSInput input)
{
    float4 offset[3];
    SMAAEdgeDetectionVS(input.uv, offset);
    EdgeOutput output;
    output.color = SMAASamplePoint(SmaaTexture(g_Push.textureIndex), input.uv);

    const float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);
    const float3 weights = float3(0.2126, 0.7152, 0.0722);
    const float L = dot(output.color.rgb, weights);
    const float Lleft = dot(SMAASamplePoint(SmaaTexture(g_Push.textureIndex), offset[0].xy).rgb, weights);
    const float Ltop = dot(SMAASamplePoint(SmaaTexture(g_Push.textureIndex), offset[0].zw).rgb, weights);
    float4 delta;
    delta.xy = abs(L - float2(Lleft, Ltop));
    float2 edges = step(threshold, delta.xy);
    const float Lright = dot(SMAASamplePoint(SmaaTexture(g_Push.textureIndex), offset[1].xy).rgb, weights);
    const float Lbottom = dot(SMAASamplePoint(SmaaTexture(g_Push.textureIndex), offset[1].zw).rgb, weights);
    delta.zw = abs(L - float2(Lright, Lbottom));
    const bool neighbour = dot(step(threshold, delta.zw), float2(1.0, 1.0)) != 0.0;

    [branch]
    if (dot(edges, float2(1.0, 1.0)) != 0.0)
    {
        float2 maxDelta = max(delta.xy, delta.zw);
        const float Lleftleft = dot(SMAASamplePoint(SmaaTexture(g_Push.textureIndex), offset[2].xy).rgb, weights);
        const float Ltoptop = dot(SMAASamplePoint(SmaaTexture(g_Push.textureIndex), offset[2].zw).rgb, weights);
        delta.zw = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));
        maxDelta = max(maxDelta.xy, delta.zw);
        const float finalDelta = max(maxDelta.x, maxDelta.y);
        edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    }
    const bool edge = dot(edges, float2(1.0, 1.0)) != 0.0;
    output.edges = float4(edges, 0.0, 0.0);
    output.depth = edge ? 1.0 : (neighbour ? 0.5 : 0.0);
    return output;
}
