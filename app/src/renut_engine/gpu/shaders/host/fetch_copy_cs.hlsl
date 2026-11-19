// sasrt-nx (2026-11-08, sasrt_gpu_fetch_copy_compute; the GPU round after the move to the ultimate base): the vertex
// fetch data of a draw that reads exported vertices (UploadVertexFetchData in draw.cpp) copied by the 3D engine's
// compute, as raw words, instead of by the copy engine: the copy engine needs a full barrier (the whole GPU idle) on
// each side, the 3D engine a wait for its own work. One thread per word.
//   set 0 : RWByteAddressBuffer[4] (u0, space0): the two slots' export buffers, the upload chunk, the fetch data buffer
//   push  : FetchCopyPush       (b0, space1)
// Must match BuildFetchCopyPipeline in video.cpp and FetchCopyPush in video.h.
struct FetchCopyPush
{
    uint source;        // 0 / 1 the slots' export buffers, 2 the upload chunk
    uint sourceOffset;  // bytes
    uint destOffset;    // bytes, in the fetch data buffer
    uint words;
};

#ifdef __spirv__
[[vk::push_constant]] ConstantBuffer<FetchCopyPush> g_Push;
#else
ConstantBuffer<FetchCopyPush> g_Push : register(b0, space1);
#endif

RWByteAddressBuffer g_Buffers[4] : register(u0, space0);

uint LoadSource(uint address)
{
    // Constant indices (as stream_convert_cs): Ryujinx cannot trace a buffer picked at run time.
    [branch] if (g_Push.source == 0)
        return g_Buffers[0].Load(address);
    [branch] if (g_Push.source == 1)
        return g_Buffers[1].Load(address);
    return g_Buffers[2].Load(address);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Push.words)
        return;
    g_Buffers[3].Store(g_Push.destOffset + id.x * 4, LoadSource(g_Push.sourceOffset + id.x * 4));
}
