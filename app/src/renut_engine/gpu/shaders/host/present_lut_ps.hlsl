// vivapinata-nx (2026-11-12, vivapinata_present_lut; A/B 89): present_ps.hlsl's scan-out with the game's gamma ramp, for
// the case it is drawn in at 720p: the frame's texel loaded at one to one (no filter), no pow, an 8-bit unorm frame. Then
// each channel's ramp entry depends on its 8-bit value n alone (present_ps: round(n / 255 * 1023), never within 0.002 of a
// tie), so the three ramp fetches become two of a 256 x 256 table made from the same texels (present.cpp): texel (i, j)
// holds red's entry for i, green's for j and blue's for i. The same values, so the same output.
#include "utility.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    const float4 color = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(int2(input.position.xy), 0));
    const int3 n = int3(round(saturate(color.rgb) * 255.0));  // the 8-bit values themselves
    const uint lut = asuint(g_Push.param1);
    const float2 rg = g_Textures[NonUniformResourceIndex(lut)].Load(int3(n.r, n.g, 0)).rg;
    const float b = g_Textures[NonUniformResourceIndex(lut)].Load(int3(n.b, 0, 0)).b;
    return float4(rg, b, 1.0);
}
