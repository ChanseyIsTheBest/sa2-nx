// sasrt-nx (2026-10-20): a colour resolve of a 16-bit fixed-point target (Xenos k_16_16 / k_16_16_16_16, held as FP16:
// render/format.cpp), drawn with the console's range: those targets store -32..32, so every value a shader wrote past
// that reads back clamped on the console (render/draw.cpp, ResolveFixed16Locked). As resolve_copy_ps otherwise: the
// destination pixel plus g_Push.translate is the source texel.
#include "utility.hlsli"

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    return clamp(g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0)), -32.0, 32.0);
}
