// banjo-nx (2026-10-21, internal resolution): the scene's depth target, at the internal resolution, into the UI depth
// target at the display's (render/ui_scale.cpp, UiDepthAfterCompositeLocked): made the first time in a frame a draw after
// the final composite tests depth (the 3D icons of the menus' item bars), so it tests against the scene's depth at its
// own resolution. Nearest texel (depth values are not filtered); the values are copied as they are (D24 and D32F
// round-trip through float). smaa_vs.hlsl's triangle: uv 0..1 over the destination.
//   textureIndex the scene depth target's depth-plane slot (SurfaceSampleSlotLocked)
//   translate    its host size
#define UTILITY_OWN_TEXTURE_TABLE
#include "utility.hlsli"

Texture2D<float> g_Depth[] : register(t0, space0);

float main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Depth
{
    const int2 size = int2(g_Push.translate);
    const int2 texel = min(int2(uv * g_Push.translate), size - 1);
    return g_Depth[g_Push.textureIndex].Load(int3(texel, 0));
}
