// sasrt-nx (2026-11-08, sasrt_gpu_texel_readback_compute; the GPU round after the move to the ultimate base): a 1x1
// 32-bit float resolve's texel read back for the CPU (QueueResolveReadbackLocked in draw.cpp; the race's average
// luminance, every frame) by the 3D engine's compute: its bits stored as they are, as the copy engine's copy stored
// them, without the copy layout's full barriers.
//   set 0 : bindless Texture2D table (t0, space0)
//   set 1 : RWByteAddressBuffer      (u0, space1), the slot's readback buffer
//   push  : TexelReadbackPush        (b0, space2), in target_readback_cs's layout (BuildTexelReadbackPipeline)

struct TexelReadbackPush
{
    uint textureIndex;
    uint destOffset;  // bytes
    uint pad0;
    uint pad1;
    uint pad2;
    uint pad3;
    uint pad4;
    uint pad5;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<TexelReadbackPush> g_Push;
#else
ConstantBuffer<TexelReadbackPush> g_Push : register(b0, space2);
#endif

Texture2D<float4> g_Textures[] : register(t0, space0);
RWByteAddressBuffer g_Words : register(u0, space1);

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const float4 texel = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(0, 0, 0));
    g_Words.Store(g_Push.destOffset, asuint(texel.r));
}
