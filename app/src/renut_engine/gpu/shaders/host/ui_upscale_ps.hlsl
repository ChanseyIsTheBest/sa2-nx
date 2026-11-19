// banjo-nx (2026-10-21, internal resolution, the user's request): the game's 3D image, drawn at the internal resolution
// (banjo_renut_render_height_handheld / _docked: 648p, 900p), scaled to the display's (720p, 1080p docked) right after the
// frame's final composite and SMAA, so the HUD the game draws next is at full resolution (render/ui_scale.cpp).
// Bilinear, the user's choice (banjo_renut_upscale_filter 0, every config): one filtered sample a pixel, nothing else, so
// the pass costs what reading the image once and writing it once does. ui_upscale_bicubic_ps.hlsl is the other filter.
// smaa_vs.hlsl's triangle: uv 0..1 over the destination, the same guest area as the source's 0..1.
//   textureIndex the source (the composite's target at the internal resolution)
//   samplerIndex the linear clamp sampler
// Both from the push block: uniform, so no NonUniformResourceIndex.
#include "utility.hlsli"

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    return g_Textures[g_Push.textureIndex].SampleLevel(g_Samplers[g_Push.samplerIndex], uv, 0);
}
