// sasrt-nx (2026-10-20): the EDRAM depth restore. A game restores a depth for a later pass by drawing the resolved depth words
// (an 8_8_8_8 view of its depth resolve) as colour into the EDRAM where that pass's depth buffer then sits, and binding the same
// EDRAM as depth: on the console the bits are the depth. Sonic Transformed's copy shader swaps the two 40-pixel halves of every
// 80-pixel EDRAM tile column on the way, as a depth tile keeps its halves the other way round from a colour tile; the words then
// land where the depth tile reads them. Renut keeps a host target per surface, so this pass turns the colour target's words into
// the depth target's depth (render/draw.cpp, RestoreDepthFromColorLocked): depth pixel x takes the colour texel at x with its
// halves swapped back. present_vs draws the fullscreen triangle.
//   g_Push.textureIndex : the colour target's slot (its words, least significant byte first: depth_resolve.hlsli)
//   g_Push.param0       : 1 for D24FS8 (20e4 float depth), 0 for D24S8
//   g_Push.param1       : > 0: that depth everywhere (a diagnostic)
#include "utility.hlsli"

float DecodeFloat20e4(uint value)
{
    const uint exponent = value >> 20;
    const uint mantissa = value & 0xFFFFF;
    if (exponent == 0)
        return float(mantissa) * (1.0 / 17179869184.0); // 2^-34 steps, as EncodeFloat20e4 writes them
    return asfloat(((exponent + 112) << 23) | (mantissa << 3));
}

float main(float4 position : SV_Position) : SV_Depth
{
    if (g_Push.param1 > 0.0)
        return g_Push.param1;
    int2 texel = int2(position.xy) + int2(g_Push.translate);
    // The other 40-pixel half of the texel's 80-pixel EDRAM tile column.
    texel.x += (texel.x % 80) < 40 ? 40 : -40;
    const float4 c = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0));
    const uint4 b = uint4(round(saturate(c) * 255.0));
    const uint word = b.x | (b.y << 8) | (b.z << 16) | (b.w << 24);
    const uint depth24 = word >> 8;
    return g_Push.param0 != 0.0 ? DecodeFloat20e4(depth24) : float(depth24) * (1.0 / 16777215.0);
}
