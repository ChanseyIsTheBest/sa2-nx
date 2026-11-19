//
// plume deko3d back end (banjo-nx, 2026-10-17): device, memory, resources, shaders and pipelines. The command lists,
// the queue and the swapchain are in plume_deko3d_commands.cpp. docs/deko3d-renderer.md is the design; the facts about
// deko3d 0.5.0 used here (and its bugs) are listed there with where they were found.
//
#include "plume_deko3d.h"
#include "plume_deko3d_region.h"  // banjo-nx (2026-10-28, h10): the GPU region's ranges

#include <switch.h>
#include <pthread.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>

#include <uam_api.h>

// blur-nx (2026-11-13): libnx's nvservices transfer memory size (weak, 8 MB by default), logged at device creation.
extern "C" u32 __nx_nv_transfermem_size;

namespace plume {
    // ---- helpers

    static void (*g_logCallback)(const char *message) = nullptr;
    void SetDeko3DLogCallback(void (*callback)(const char *message)) { g_logCallback = callback; }

    void deko3dLog(const char *fmt, ...) {
        char line[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);
        if (g_logCallback)
            g_logCallback(line);
        else
            fprintf(stderr, "[deko3d] %s\n", line);
    }
#define logf deko3dLog

    // Free memory, for the log lines that may be about running out of it: the heap's free bytes (newlib) and the
    // process's unused memory (the system's figures).
    extern "C" char *fake_heap_end; // libnx: the heap's end (sbrk grows towards it)
    // Port 161 (from fnxf_xiii, its note of 2026-11-05): newlib's malloc bins (devkitA64 libc.a mallocr.c; the third word
    // is the top chunk) and lock, for the heap's room without mallinfo in the logs made while the game runs. mallinfo
    // takes the malloc lock and walks every free chunk (newlib's __malloc_update_mallinfo; fnxf_xiii: 13-20 ms with the
    // game's heap): the "memory block" line of the first 24 blocks of 8 MB or more (the upload ring's chunks, the stream
    // buffers) cost the render thread about 20 ms each, and every thread's malloc waited meanwhile. The room: newlib's
    // top chunk (__malloc_av_[2], its size word masked, what mallinfo calls keepcost) and what sbrk can still give; free
    // chunks inside the heap are not counted.
    extern "C" void *__malloc_av_[];  // (__malloc_lock / __malloc_unlock: malloc.h, as regionMoveHeapEnd uses them)
    static void memoryFiguresQuick(uint64_t &heapRoom, uint64_t &processFree) {
        __malloc_lock(_REENT);
        const size_t *top = static_cast<const size_t *>(__malloc_av_[2]);
        const size_t keepcost = top ? top[1] & ~size_t(3) : 0;
        __malloc_unlock(_REENT);
        char *brk = static_cast<char *>(sbrk(0));
        heapRoom = uint64_t(keepcost) + (fake_heap_end > brk ? uint64_t(fake_heap_end - brk) : 0);
        uint64_t total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        processFree = total > used ? total - used : 0;
    }

    static void memoryFigures(uint64_t &heapFree, uint64_t &processFree) {
        // Free chunks inside what malloc has taken, and what sbrk can still give.
        struct mallinfo mi = mallinfo();
        char *brk = static_cast<char *>(sbrk(0));
        heapFree = uint64_t(mi.fordblks) + (fake_heap_end > brk ? uint64_t(fake_heap_end - brk) : 0);
        uint64_t total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        processFree = total > used ? total - used : 0;
    }

    static constexpr uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

    // blur-nx (2026-11-13, the console crash of 21:16: deko3d aborted the process when dkMemBlockCreate failed to map a
    // GPU-region block during a race load, with 1.8 GB of the heap free): the blocks live and their bytes, and before
    // every dkMemBlockCreate a breadcrumb in this thread's TLS (the IPC buffer below 0x100 is what deko3d's own calls
    // overwrite), which Atmosphère's crash report dumps with the thread: the request that failed, the blocks live then,
    // the GPU region's size and the process's system resource (the kernel memory its mappings use).
    // renut-ultimate (2026-11-17, port 153): at 0x138-0x17B. libnx's thread-local slots (threadTlsGet / Set, which
    // pthread keys and libstdc++'s thread-exit destructors use) are 0x180-0x1DF (its threadTlsGet: tpidrro_el0 + 0x180
    // + 8 * slot, twelve slots), and the kernel's preemption words 0x100-0x107: Blur's 0x140 put the crumb's last four
    // bytes over slot 0.
    static std::atomic<uint32_t> g_blocksLive{0}, g_blocksMade{0}, g_blockCrumbSeq{0};
    static std::atomic<uint64_t> g_blocksLiveBytes{0};
    static std::atomic<uint64_t> g_regionBytesNow{0};  // set where the region changes (makeBlock)
    struct BlockCrumb {
        char magic[8];  // "DKBLOCK1"
        uint32_t size, flags, path;  // path: 0 the heap, 1 the GPU region, 2 the guarded storage
        uint32_t live, liveMb, regionMb;
        uint32_t sysUsedKb, sysTotalKb, seq;
        char what[24];
    };
    static constexpr size_t kBlockCrumbAt = 0x138;  // renut-ultimate (2026-11-17): see above
    static_assert(kBlockCrumbAt >= 0x108 && kBlockCrumbAt + sizeof(BlockCrumb) <= 0x180,
                  "the crumb stays between the kernel's words and libnx's TLS slots");
    static void systemResource(uint64_t &used, uint64_t &total) {
        used = total = 0;
        svcGetInfo(&total, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0);
    }
    static void leaveBlockCrumb(uint32_t size, uint32_t flags, uint32_t path, const char *what) {
        BlockCrumb c{};
        std::memcpy(c.magic, "DKBLOCK1", 8);
        c.size = size;
        c.flags = flags;
        c.path = path;
        c.live = g_blocksLive.load(std::memory_order_relaxed);
        c.liveMb = uint32_t(g_blocksLiveBytes.load(std::memory_order_relaxed) >> 20);
        c.regionMb = uint32_t(g_regionBytesNow.load(std::memory_order_relaxed) >> 20);
        uint64_t used = 0, total = 0;
        systemResource(used, total);
        c.sysUsedKb = uint32_t(used >> 10);
        c.sysTotalKb = uint32_t(total >> 10);
        c.seq = g_blockCrumbSeq.fetch_add(1, std::memory_order_relaxed);
        std::strncpy(c.what, what ? what : "", sizeof(c.what) - 1);
        auto *tls = static_cast<volatile uint8_t *>(armGetTls()) + kBlockCrumbAt;
        const auto *from = reinterpret_cast<const uint8_t *>(&c);
        for (size_t i = 0; i < sizeof(c); ++i)
            tls[i] = from[i];
    }
    // A block made: counted, and every 32nd (and the first 8) logged with the live figures.
    static void blockMade(DkMemBlock block, const char *what) {
        if (!block)
            return;
        const uint64_t bytes = dkMemBlockGetSize(block);
        const uint32_t live = g_blocksLive.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t liveBytes = g_blocksLiveBytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        const uint32_t made = g_blocksMade.fetch_add(1, std::memory_order_relaxed) + 1;
        if (made <= 8 || (made & 31) == 0) {
            uint64_t used = 0, total = 0;
            systemResource(used, total);
            logf("blocks: %u made, %u live (%" PRIu64 " MB; this one a %s of %" PRIu64 " KiB); GPU region %" PRIu64
                 " MB; system resource %" PRIu64 " of %" PRIu64 " KiB used",
                 made, live, liveBytes >> 20, what ? what : "?", bytes >> 10,
                 g_regionBytesNow.load(std::memory_order_relaxed) >> 20, used >> 10, total >> 10);
        }
    }

    // sasrt-nx (2026-11-09; port 58): the device for Deko3DReleaseEmptyImageChunks (one device), and
    // Deko3DSetDedicatedTargets.
    static Deko3DDevice *g_imageDevice = nullptr;
    static std::atomic<bool> g_dedicatedTargets{true};
    uint64_t Deko3DHeapFreeBytes() {  // plume_deko3d.h
        uint64_t heapFree = 0, processFree = 0;
        memoryFigures(heapFree, processFree);
        return heapFree;
    }
    uint64_t Deko3DHeapRoomBytes() {  // plume_deko3d.h
        char *brk = static_cast<char *>(sbrk(0));
        return fake_heap_end > brk ? uint64_t(fake_heap_end - brk) : 0;
    }
    static std::atomic<bool> g_memoryEmergency{false};  // Deko3DTakeMemoryEmergency
    bool Deko3DTakeMemoryEmergency() { return g_memoryEmergency.exchange(false, std::memory_order_relaxed); }

    // banjo-nx (2026-10-28, the console's OOM): see makeBlock. Cold paths only (a block that did not fit, a new chunk).
    static thread_local bool t_inImageAlloc = false;
    static void (*g_memoryPressure)() = nullptr;
    void Deko3DSetMemoryPressureCallback(void (*callback)()) { g_memoryPressure = callback; }
    static constexpr uint64_t alignUp64(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

    // The GPU region (banjo-nx, 2026-10-28, h10, after the night_gpu2 console freeze; battd_nx's bp_gpuarena.c and its
    // notes for the lessons). Memory blocks of kRegionMinBlock and more come from a region of their own at the heap's
    // top, not from newlib among every small allocation: a freed block was cut into small pieces there, and the console
    // had 577 MB free in 170,000 pieces with no 32 MB one. The region grows down from the heap's end by lowering
    // fake_heap_end (newlib's _sbrk_r reads it at every call; moved under malloc's lock, and never closer than
    // kRegionNewlibSlack above newlib's break), so no size is fixed in advance (battd's fixed arena was wrong by 2x at
    // one point). Blocks pack at its top; free space at its bottom beyond kRegionKeep goes back to newlib once
    // kRegionGiveBackAt of it is free. Storage given to deko3d is zeroed and flushed here: deko3d zeroes only what it
    // allocates itself, and the zero vertex buffer and command memory rely on it. A block that does not fit takes the
    // old path (newlib). No logging under g_regionMutex (battd: a log line that allocates deadlocked its arena lock).
    static bool g_regionOn = true;
    void Deko3DSetGpuRegion(bool on) { g_regionOn = on; }
    static constexpr uint64_t kRegionMinBlock = 64ull << 10, kRegionGrain = 64ull << 10, kRegionStep = 2ull << 20;
    // sasrt-nx (2026-11-09; port 58): back from 32 MB free at the bottom, 16 kept (128 and 64 before: SASRT's docked race
    // never gave any back, and its CPU side ran out beside 500-700 MB free in the region)
    static constexpr uint64_t kRegionNewlibSlack = 32ull << 20, kRegionGiveBackAt = 32ull << 20, kRegionKeep = 16ull << 20;
    static std::mutex g_regionMutex;
    static GpuRegionRanges g_region;
    static std::unordered_map<DkMemBlock, std::pair<uintptr_t, uint64_t>> g_regionBlocks;
    static uint64_t g_regionRefused = 0;

    // newlib's end moved to 'end' under malloc's lock; lowering keeps kRegionNewlibSlack above its break.
    static bool regionMoveHeapEnd(uintptr_t end, bool lowering) {
        __malloc_lock(_REENT);
        const uintptr_t brk = reinterpret_cast<uintptr_t>(sbrk(0));
        const bool ok = !lowering || end >= brk + kRegionNewlibSlack;
        if (ok)
            fake_heap_end = reinterpret_cast<char *>(end);
        __malloc_unlock(_REENT);
        return ok;
    }

    uint64_t Deko3DRegionGiveBack() {  // sasrt-nx (2026-11-09; port 58): plume_deko3d.h
        std::lock_guard<std::mutex> lock(g_regionMutex);
        if (!g_regionOn || !g_region.high)
            return 0;
        return g_region.giveBackBottom(kRegionStep, [](uintptr_t low) { return regionMoveHeapEnd(low, false); });
    }

    static std::atomic<bool> g_imageBestFit{true};  // sasrt-nx (2026-11-09 night; port 58): Deko3DSetImageBestFit
    void Deko3DSetImageBestFit(bool on) { g_imageBestFit.store(on, std::memory_order_relaxed); }
    static std::atomic<bool> g_imageSpare{true};  // blur-nx (2026-11-18): Deko3DSetImageSpareChunk
    void Deko3DSetImageSpareChunk(bool on) { g_imageSpare.store(on, std::memory_order_relaxed); }

    void Deko3DSetRegionBestFit(bool on) {  // sasrt-nx (2026-11-09 night; port 58): plume_deko3d.h
        std::lock_guard<std::mutex> lock(g_regionMutex);
        g_region.bestFit = on;
    }

    void Deko3DGpuRegionFigures(Deko3DGpuRegionInfo &out) {
        std::lock_guard<std::mutex> lock(g_regionMutex);
        out.on = g_regionOn;
        out.size = g_region.high - g_region.low;
        out.used = g_region.used;
        out.peak = g_region.peak;
        out.freeBytes = g_region.freeBytes();
        out.largestFree = g_region.largestFree();
        out.freeRanges = uint32_t(g_region.free.size());
        out.blocks = uint32_t(g_regionBlocks.size());
        out.growths = g_region.growths;
        out.givebacks = g_region.givebacks;
        out.givenBack = g_region.givenBack;
        out.refused = g_regionRefused;
    }

    // FNV-1a 64: the variant cache key (tools/deko3d_shaders.py computes the same over the same text).
    static uint64_t fnv1a64(const void *data, size_t size) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < size; i++) {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }

    static uint64_t mixHash(uint64_t h, const void *data, size_t size) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; i++) {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }

    Deko3DStats g_deko3dStats;
    // PLUME_DEKO3D_GUARDS: bytes after each block's storage, and their value.
    static constexpr uint32_t GuardBytes = 64u << 10;
    static constexpr uint8_t GuardByte = 0xA5;


    Deko3DOptions g_deko3dOptions;
    // sasrt-nx (2026-11-11, port 82; its GPU list's row 2; UNTESTED): preciseStencilWrite's count.
    std::atomic<uint64_t> g_deko3dPreciseStencilSaved{0};
    void Deko3DSetOptions(const Deko3DOptions &options) { g_deko3dOptions = options; }

    static Deko3DShaderCacheLookup g_cacheLookup;
    static Deko3DShaderCacheStore g_cacheStore;
    static Deko3DShaderCacheContains g_cacheContains;  // banjo-nx (2026-10-23)
    void SetDeko3DShaderCacheContains(Deko3DShaderCacheContains contains) { g_cacheContains = std::move(contains); }
    void SetDeko3DShaderCache(Deko3DShaderCacheLookup lookup, Deko3DShaderCacheStore store) {
        g_cacheLookup = std::move(lookup);
        g_cacheStore = std::move(store);
    }

    // ---- formats

    DkImageFormat toDkFormat(RenderFormat format) {
        switch (format) {
        case RenderFormat::R32G32B32A32_TYPELESS:
        case RenderFormat::R32G32B32A32_FLOAT: return DkImageFormat_RGBA32_Float;
        case RenderFormat::R32G32B32A32_UINT: return DkImageFormat_RGBA32_Uint;
        case RenderFormat::R32G32B32A32_SINT: return DkImageFormat_RGBA32_Sint;
        case RenderFormat::R32G32B32_TYPELESS:
        case RenderFormat::R32G32B32_FLOAT: return DkImageFormat_RGB32_Float;
        case RenderFormat::R32G32B32_UINT: return DkImageFormat_RGB32_Uint;
        case RenderFormat::R32G32B32_SINT: return DkImageFormat_RGB32_Sint;
        case RenderFormat::R16G16B16A16_TYPELESS:
        case RenderFormat::R16G16B16A16_FLOAT: return DkImageFormat_RGBA16_Float;
        case RenderFormat::R16G16B16A16_UNORM: return DkImageFormat_RGBA16_Unorm;
        case RenderFormat::R16G16B16A16_UINT: return DkImageFormat_RGBA16_Uint;
        case RenderFormat::R16G16B16A16_SNORM: return DkImageFormat_RGBA16_Snorm;
        case RenderFormat::R16G16B16A16_SINT: return DkImageFormat_RGBA16_Sint;
        case RenderFormat::R32G32_TYPELESS:
        case RenderFormat::R32G32_FLOAT: return DkImageFormat_RG32_Float;
        case RenderFormat::R32G32_UINT: return DkImageFormat_RG32_Uint;
        case RenderFormat::R32G32_SINT: return DkImageFormat_RG32_Sint;
        case RenderFormat::R8G8B8A8_TYPELESS:
        case RenderFormat::R8G8B8A8_UNORM: return DkImageFormat_RGBA8_Unorm;
        case RenderFormat::R8G8B8A8_UINT: return DkImageFormat_RGBA8_Uint;
        case RenderFormat::R8G8B8A8_SNORM: return DkImageFormat_RGBA8_Snorm;
        case RenderFormat::R8G8B8A8_SINT: return DkImageFormat_RGBA8_Sint;
        case RenderFormat::B8G8R8A8_UNORM: return DkImageFormat_BGRA8_Unorm;
        case RenderFormat::R16G16_TYPELESS:
        case RenderFormat::R16G16_FLOAT: return DkImageFormat_RG16_Float;
        case RenderFormat::R16G16_UNORM: return DkImageFormat_RG16_Unorm;
        case RenderFormat::R16G16_UINT: return DkImageFormat_RG16_Uint;
        case RenderFormat::R16G16_SNORM: return DkImageFormat_RG16_Snorm;
        case RenderFormat::R16G16_SINT: return DkImageFormat_RG16_Sint;
        case RenderFormat::R32_TYPELESS:
        case RenderFormat::R32_FLOAT: return DkImageFormat_R32_Float;
        case RenderFormat::D32_FLOAT: return DkImageFormat_ZF32;
        case RenderFormat::D32_FLOAT_S8_UINT: return DkImageFormat_ZF32_X24S8;
        case RenderFormat::D24_UNORM_S8_UINT: return DkImageFormat_Z24S8;  // banjo-nx (2026-10-19)
        // sasrt-nx (2026-11-11, port 75; its formats batch, rows 6, 8, 10, 74, 93 and 7, 66, 73, 95, 96)
        case RenderFormat::R10G10B10A2_UNORM: return DkImageFormat_RGB10A2_Unorm;
        case RenderFormat::R11G11B10_FLOAT: return DkImageFormat_RG11B10_Float;
        // renut-ultimate (2026-11-16, port 145; tw2de_nx's of 2026-10-25): banjo_deko3d_native_16bit.
        case RenderFormat::R5G6B5_UNORM: return DkImageFormat_RGB565_Unorm;
        case RenderFormat::R5G5B5A1_UNORM: return DkImageFormat_RGB5A1_Unorm;
        case RenderFormat::R4G4B4A4_UNORM: return DkImageFormat_RGBA4_Unorm;
        case RenderFormat::R32_UINT: return DkImageFormat_R32_Uint;
        case RenderFormat::R32_SINT: return DkImageFormat_R32_Sint;
        case RenderFormat::R8G8_TYPELESS:
        case RenderFormat::R8G8_UNORM: return DkImageFormat_RG8_Unorm;
        case RenderFormat::R8G8_UINT: return DkImageFormat_RG8_Uint;
        case RenderFormat::R8G8_SNORM: return DkImageFormat_RG8_Snorm;
        case RenderFormat::R8G8_SINT: return DkImageFormat_RG8_Sint;
        case RenderFormat::R16_TYPELESS:
        case RenderFormat::R16_FLOAT: return DkImageFormat_R16_Float;
        case RenderFormat::D16_UNORM: return DkImageFormat_Z16;
        case RenderFormat::R16_UNORM: return DkImageFormat_R16_Unorm;
        case RenderFormat::R16_UINT: return DkImageFormat_R16_Uint;
        case RenderFormat::R16_SNORM: return DkImageFormat_R16_Snorm;
        case RenderFormat::R16_SINT: return DkImageFormat_R16_Sint;
        case RenderFormat::R8_TYPELESS:
        case RenderFormat::R8_UNORM: return DkImageFormat_R8_Unorm;
        case RenderFormat::R8_UINT: return DkImageFormat_R8_Uint;
        case RenderFormat::R8_SNORM: return DkImageFormat_R8_Snorm;
        case RenderFormat::R8_SINT: return DkImageFormat_R8_Sint;
        case RenderFormat::BC1_TYPELESS:
        case RenderFormat::BC1_UNORM: return DkImageFormat_RGBA_BC1;
        case RenderFormat::BC1_UNORM_SRGB: return DkImageFormat_RGBA_BC1_sRGB;
        case RenderFormat::BC2_TYPELESS:
        case RenderFormat::BC2_UNORM: return DkImageFormat_RGBA_BC2;
        case RenderFormat::BC2_UNORM_SRGB: return DkImageFormat_RGBA_BC2_sRGB;
        case RenderFormat::BC3_TYPELESS:
        case RenderFormat::BC3_UNORM: return DkImageFormat_RGBA_BC3;
        case RenderFormat::BC3_UNORM_SRGB: return DkImageFormat_RGBA_BC3_sRGB;
        case RenderFormat::BC4_TYPELESS:
        case RenderFormat::BC4_UNORM: return DkImageFormat_R_BC4_Unorm;
        case RenderFormat::BC4_SNORM: return DkImageFormat_R_BC4_Snorm;
        case RenderFormat::BC5_TYPELESS:
        case RenderFormat::BC5_UNORM: return DkImageFormat_RG_BC5_Unorm;
        case RenderFormat::BC5_SNORM: return DkImageFormat_RG_BC5_Snorm;
        case RenderFormat::BC6H_TYPELESS:
        case RenderFormat::BC6H_UF16: return DkImageFormat_RGBA_BC6H_UF16_Float;
        case RenderFormat::BC6H_SF16: return DkImageFormat_RGBA_BC6H_SF16_Float;
        case RenderFormat::BC7_TYPELESS:
        case RenderFormat::BC7_UNORM: return DkImageFormat_RGBA_BC7_Unorm;
        case RenderFormat::BC7_UNORM_SRGB: return DkImageFormat_RGBA_BC7_Unorm_sRGB;
        case RenderFormat::S8_UINT: return DkImageFormat_S8;
        default: return DkImageFormat_None;
        }
    }

    static bool isDepthFormat(DkImageFormat f) {
        return f == DkImageFormat_Z16 || f == DkImageFormat_Z24X8 || f == DkImageFormat_ZF32 ||
               f == DkImageFormat_Z24S8 || f == DkImageFormat_ZF32_X24S8 || f == DkImageFormat_S8;
    }

    static bool isCompressedFormat(DkImageFormat f) {
        return f >= DkImageFormat_RGB_BC1 && f <= DkImageFormat_RGBA_ASTC_12x12_sRGB;
    }

    static bool isSrgbFormat(DkImageFormat f) {
        return f == DkImageFormat_RGBA8_Unorm_sRGB || f == DkImageFormat_RGBX8_Unorm_sRGB ||
               f == DkImageFormat_BGRA8_Unorm_sRGB || f == DkImageFormat_BGRX8_Unorm_sRGB;
    }

    // The 2D engine (blits, MSAA resolves) takes uncompressed colour formats.
    static bool canUse2DEngine(DkImageFormat f) {
        return !isDepthFormat(f) && !isCompressedFormat(f) && f != DkImageFormat_RGB32_Float &&
               f != DkImageFormat_RGB32_Uint && f != DkImageFormat_RGB32_Sint;
    }

    // What a texture of this plume format is made of when it is a depth target (R32_TYPELESS and friends).
    static DkImageFormat depthTargetFormat(RenderFormat format) {
        switch (format) {
        case RenderFormat::R32_TYPELESS:
        case RenderFormat::R32_FLOAT:
        case RenderFormat::D32_FLOAT: return DkImageFormat_ZF32;
        case RenderFormat::R16_TYPELESS:
        case RenderFormat::R16_UNORM:
        case RenderFormat::D16_UNORM: return DkImageFormat_Z16;
        case RenderFormat::D32_FLOAT_S8_UINT: return DkImageFormat_ZF32_X24S8;
        case RenderFormat::D24_UNORM_S8_UINT: return DkImageFormat_Z24S8;
        default: return toDkFormat(format);
        }
    }

    static bool vertexFormat(RenderFormat format, DkVtxAttribSize &size, DkVtxAttribType &type, bool &bgra) {
        bgra = false;
        switch (format) {
        case RenderFormat::R32G32B32A32_FLOAT: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R32G32B32A32_UINT: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R32G32B32A32_SINT: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R32G32B32_FLOAT: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R32G32B32_UINT: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R32G32B32_SINT: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R32G32_FLOAT: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R32G32_UINT: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R32G32_SINT: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R32_FLOAT: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R32_UINT: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R32_SINT: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R16G16B16A16_FLOAT: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R16G16B16A16_UNORM: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R16G16B16A16_SNORM: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R16G16B16A16_UINT: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R16G16B16A16_SINT: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R16G16_FLOAT: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R16G16_UNORM: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R16G16_SNORM: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R16G16_UINT: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R16G16_SINT: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R16_FLOAT: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Float; return true;
        case RenderFormat::R16_UNORM: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R16_SNORM: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R16_UINT: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R16_SINT: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R8G8B8A8_UNORM: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R8G8B8A8_SNORM: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R8G8B8A8_UINT: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R8G8B8A8_SINT: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::B8G8R8A8_UNORM: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Unorm; bgra = true; return true;
        case RenderFormat::R8G8_UNORM: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R8G8_SNORM: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R8G8_UINT: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R8G8_SINT: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Sint; return true;
        case RenderFormat::R8_UNORM: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Unorm; return true;
        case RenderFormat::R8_SNORM: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Snorm; return true;
        case RenderFormat::R8_UINT: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Uint; return true;
        case RenderFormat::R8_SINT: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Sint; return true;
        default: return false;
        }
    }

    static DkCompareOp toDk(RenderComparisonFunction f) {
        switch (f) {
        case RenderComparisonFunction::NEVER: return DkCompareOp_Never;
        case RenderComparisonFunction::LESS: return DkCompareOp_Less;
        case RenderComparisonFunction::EQUAL: return DkCompareOp_Equal;
        case RenderComparisonFunction::LESS_EQUAL: return DkCompareOp_Lequal;
        case RenderComparisonFunction::GREATER: return DkCompareOp_Greater;
        case RenderComparisonFunction::NOT_EQUAL: return DkCompareOp_NotEqual;
        case RenderComparisonFunction::GREATER_EQUAL: return DkCompareOp_Gequal;
        case RenderComparisonFunction::ALWAYS:
        default: return DkCompareOp_Always;
        }
    }

    static DkStencilOp toDk(RenderStencilOp op) {
        switch (op) {
        case RenderStencilOp::ZERO: return DkStencilOp_Zero;
        case RenderStencilOp::REPLACE: return DkStencilOp_Replace;
        case RenderStencilOp::INCREMENT_AND_CLAMP: return DkStencilOp_Incr;
        case RenderStencilOp::DECREMENT_AND_CLAMP: return DkStencilOp_Decr;
        case RenderStencilOp::INVERT: return DkStencilOp_Invert;
        case RenderStencilOp::INCREMENT_AND_WRAP: return DkStencilOp_IncrWrap;
        case RenderStencilOp::DECREMENT_AND_WRAP: return DkStencilOp_DecrWrap;
        case RenderStencilOp::KEEP:
        default: return DkStencilOp_Keep;
        }
    }

    static DkBlendFactor toDk(RenderBlend b) {
        switch (b) {
        case RenderBlend::ZERO: return DkBlendFactor_Zero;
        case RenderBlend::ONE: return DkBlendFactor_One;
        case RenderBlend::SRC_COLOR: return DkBlendFactor_SrcColor;
        case RenderBlend::INV_SRC_COLOR: return DkBlendFactor_InvSrcColor;
        case RenderBlend::SRC_ALPHA: return DkBlendFactor_SrcAlpha;
        case RenderBlend::INV_SRC_ALPHA: return DkBlendFactor_InvSrcAlpha;
        case RenderBlend::DEST_ALPHA: return DkBlendFactor_DstAlpha;
        case RenderBlend::INV_DEST_ALPHA: return DkBlendFactor_InvDstAlpha;
        case RenderBlend::DEST_COLOR: return DkBlendFactor_DstColor;
        case RenderBlend::INV_DEST_COLOR: return DkBlendFactor_InvDstColor;
        case RenderBlend::SRC_ALPHA_SAT: return DkBlendFactor_SrcAlphaSaturate;
        case RenderBlend::BLEND_FACTOR: return DkBlendFactor_ConstColor;
        case RenderBlend::INV_BLEND_FACTOR: return DkBlendFactor_InvConstColor;
        case RenderBlend::SRC1_COLOR: return DkBlendFactor_Src1Color;
        case RenderBlend::INV_SRC1_COLOR: return DkBlendFactor_InvSrc1Color;
        case RenderBlend::SRC1_ALPHA: return DkBlendFactor_Src1Alpha;
        case RenderBlend::INV_SRC1_ALPHA: return DkBlendFactor_InvSrc1Alpha;
        default: return DkBlendFactor_One;
        }
    }

    // In the alpha factors, the constant's colour factors mean its alpha (D3D's BLEND_FACTOR on alpha).
    static DkBlendFactor toDkAlpha(RenderBlend b) {
        switch (b) {
        case RenderBlend::BLEND_FACTOR: return DkBlendFactor_ConstAlpha;
        case RenderBlend::INV_BLEND_FACTOR: return DkBlendFactor_InvConstAlpha;
        case RenderBlend::SRC_COLOR: return DkBlendFactor_SrcAlpha;
        case RenderBlend::INV_SRC_COLOR: return DkBlendFactor_InvSrcAlpha;
        case RenderBlend::DEST_COLOR: return DkBlendFactor_DstAlpha;
        case RenderBlend::INV_DEST_COLOR: return DkBlendFactor_InvDstAlpha;
        case RenderBlend::SRC1_COLOR: return DkBlendFactor_Src1Alpha;
        case RenderBlend::INV_SRC1_COLOR: return DkBlendFactor_InvSrc1Alpha;
        default: return toDk(b);
        }
    }

    static DkBlendOp toDk(RenderBlendOperation op) {
        switch (op) {
        case RenderBlendOperation::SUBTRACT: return DkBlendOp_Sub;
        case RenderBlendOperation::REV_SUBTRACT: return DkBlendOp_RevSub;
        case RenderBlendOperation::MIN: return DkBlendOp_Min;
        case RenderBlendOperation::MAX: return DkBlendOp_Max;
        case RenderBlendOperation::ADD:
        default: return DkBlendOp_Add;
        }
    }

    static DkLogicOp toDk(RenderLogicOperation op) {
        switch (op) {
        case RenderLogicOperation::CLEAR: return DkLogicOp_Clear;
        case RenderLogicOperation::SET: return DkLogicOp_Set;
        case RenderLogicOperation::COPY: return DkLogicOp_Copy;
        case RenderLogicOperation::COPY_INVERTED: return DkLogicOp_CopyInverted;
        case RenderLogicOperation::NOOP: return DkLogicOp_NoOp;
        case RenderLogicOperation::INVERT: return DkLogicOp_Invert;
        case RenderLogicOperation::AND: return DkLogicOp_And;
        case RenderLogicOperation::NAND: return DkLogicOp_Nand;
        case RenderLogicOperation::OR: return DkLogicOp_Or;
        case RenderLogicOperation::NOR: return DkLogicOp_Nor;
        case RenderLogicOperation::XOR: return DkLogicOp_Xor;
        case RenderLogicOperation::EQUIV: return DkLogicOp_Equivalent;
        case RenderLogicOperation::AND_REVERSE: return DkLogicOp_AndReverse;
        case RenderLogicOperation::AND_INVERTED: return DkLogicOp_AndInverted;
        case RenderLogicOperation::OR_REVERSE: return DkLogicOp_OrReverse;
        case RenderLogicOperation::OR_INVERTED: return DkLogicOp_OrInverted;
        default: return DkLogicOp_Copy;
        }
    }

    static DkWrapMode toDk(RenderTextureAddressMode m) {
        switch (m) {
        case RenderTextureAddressMode::MIRROR: return DkWrapMode_MirroredRepeat;
        case RenderTextureAddressMode::CLAMP: return DkWrapMode_ClampToEdge;
        case RenderTextureAddressMode::BORDER: return DkWrapMode_ClampToBorder;
        case RenderTextureAddressMode::MIRROR_ONCE: return DkWrapMode_MirrorClampToEdge;
        case RenderTextureAddressMode::WRAP:
        default: return DkWrapMode_Repeat;
        }
    }

    static DkImageSwizzle toDk(RenderSwizzle s, DkImageSwizzle identity) {
        switch (s) {
        case RenderSwizzle::ZERO: return DkImageSwizzle_Zero;
        case RenderSwizzle::ONE: return DkImageSwizzle_One;
        case RenderSwizzle::R: return DkImageSwizzle_Red;
        case RenderSwizzle::G: return DkImageSwizzle_Green;
        case RenderSwizzle::B: return DkImageSwizzle_Blue;
        case RenderSwizzle::A: return DkImageSwizzle_Alpha;
        case RenderSwizzle::IDENTITY:
        default: return identity;
        }
    }

    static DkMsMode toDkMsMode(RenderSampleCounts count) {
        switch (count) {
        case RenderSampleCount::COUNT_2: return DkMsMode_2x;
        case RenderSampleCount::COUNT_4: return DkMsMode_4x;
        case RenderSampleCount::COUNT_8: return DkMsMode_8x;
        default: return DkMsMode_1x;
        }
    }

    // deko3d 0.5.0's tile height bug (SwitchWakerHD, memory.cpp image_tile_size_fix): dkImageLayoutInitialize picks the
    // tile from 1.5 x the height and its layout shrinks it for level 0, but the engines and the descriptor use the
    // unshrunk one. When the two differ, the shrunk tile is given as a custom tile size.
    static void tileSizeFix(DkImageLayoutMaker &m, uint32_t rows) {
        if (m.flags & (DkImageFlags_PitchLinear | DkImageFlags_CustomTileSize | DkImageFlags_UsageVideo))
            return;
        if (m.type == DkImageType_1D || m.type == DkImageType_1DArray || m.type == DkImageType_3D || m.type == DkImageType_Buffer)
            return;
        if ((m.type == DkImageType_2DMS || m.type == DkImageType_2DMSArray) && (m.msMode == DkMsMode_4x || m.msMode == DkMsMode_8x))
            rows *= 2;
        const uint32_t gobs = (rows + rows / 2 + 7) / 8;
        uint32_t tile = gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0;
        uint32_t shrunk = tile;
        while (shrunk && (8u << (shrunk - 1)) >= rows)
            shrunk--;
        if (shrunk == tile)
            return;
        m.flags |= DkImageFlags_CustomTileSize;
        m.tileSize = DkTileSize(shrunk);
    }

    // ---- Deko3DBuffer

    Deko3DBuffer::Deko3DBuffer(Deko3DDevice *device, const RenderBufferDesc &desc) : device(device), desc(desc) {
        const uint64_t size = std::max<uint64_t>(alignUp64(desc.size, DK_MEMBLOCK_ALIGNMENT), DK_MEMBLOCK_ALIGNMENT);
        if (size > 0xFFFFFFFFull) {
            logf("buffer of %" PRIu64 " bytes is too large", desc.size);
            return;
        }
        uint32_t flags = DkMemBlockFlags_GpuCached;
        // Uploads written at cache speed and published by unmap(writtenRange) (HOST_CACHED, as the Vulkan back end);
        // everything else uncached for the CPU, so the GPU sees the CPU's writes and the CPU the GPU's after a fence.
        // banjo-nx (2026-10-23, batch 6): readback buffers that ask for HOST_CACHED too (the memexport readback): the CPU
        // copied the GPU's results out of uncached memory a load at a time (2.8 % of the render thread in the city);
        // cached, map(readRange) drops the range's lines first, so the reads see what the GPU wrote.
        cpuCached = (desc.heapType == RenderHeapType::UPLOAD || desc.heapType == RenderHeapType::READBACK) &&
                    (desc.flags & RenderBufferFlag::HOST_CACHED);
        flags |= cpuCached ? DkMemBlockFlags_CpuCached : DkMemBlockFlags_CpuUncached;
        block = device->makeBlock(uint32_t(size), flags, "buffer");
        if (!block)
            return;
        gpu = dkMemBlockGetGpuAddr(block);
        cpu = static_cast<uint8_t *>(dkMemBlockGetCpuAddr(block));
        // No line of the memory (from whatever used it before) may be dirty in the CPU's caches: one written back over the
        // GPU's results later would lose them. The CPU never writes a readback buffer after this.
        // sasrt-nx (2026-11-09, sasrt_rt_cached_export_seeds; port 54): a cached upload buffer the GPU writes too (the
        // memexport windows, which the CPU seeds) needs the same: every cached buffer gets it (once, at creation).
        if (cpuCached)
            armDCacheFlush(cpu, size_t(size));
        g_deko3dStats.bufferBytes += size;
        {
            std::lock_guard<std::mutex> lock(device->bufferMutex);
            device->bufferRanges[gpu] = {uint32_t(size), cpu};
        }
    }

    Deko3DBuffer::~Deko3DBuffer() {
        if (block) {
            {
                std::lock_guard<std::mutex> lock(device->bufferMutex);
                device->bufferRanges.erase(gpu);
            }
            g_deko3dStats.bufferBytes -= dkMemBlockGetSize(block);
            device->retire({}, block);
        }
    }

    void *Deko3DBuffer::map(uint32_t, const RenderRange *readRange) {
        // banjo-nx (2026-10-23, batch 6): a CPU-cached readback buffer: the lines to read dropped (dc civac; clean ones,
        // nothing is written back), so they come from memory, where the GPU's writes are once its fence passed. A line
        // the prefetcher brought in while the GPU was still writing goes with them.
        if (cpuCached && cpu && desc.heapType == RenderHeapType::READBACK) {
            uint64_t begin = 0, end = desc.size;
            if (readRange != nullptr) {
                begin = readRange->begin;
                end = std::min<uint64_t>(readRange->end, desc.size);
            }
            if (end > begin)
                armDCacheFlush(cpu + begin, size_t(end - begin));
        }
        return cpu;
    }

    void Deko3DBuffer::unmap(uint32_t, const RenderRange *writtenRange) {
        if (!cpuCached || !block || desc.heapType == RenderHeapType::READBACK)
            return;
        uint64_t begin = 0, end = desc.size;
        if (writtenRange != nullptr) {
            begin = writtenRange->begin;
            end = writtenRange->end;
        }
        // banjo-nx (2026-10-19, CPU proposal 14): a clean (dc cvac) of the written lines, not deko3d's clean and
        // invalidate (dc civac): only upload buffers are CPU-cached (written by the CPU, read by the GPU, never read
        // back), so the lines may stay valid for the ring's next pass.
        if (end > begin)
            armDCacheClean(static_cast<uint8_t *>(cpu) + begin, size_t(end - begin));
    }

    std::unique_ptr<RenderBufferFormattedView> Deko3DBuffer::createBufferFormattedView(RenderFormat format) {
        auto view = std::make_unique<Deko3DBufferFormattedView>();
        view->format = format;
        view->buffer = this;
        return view;
    }

    void Deko3DBuffer::setName(const std::string &) {}

    uint64_t Deko3DBuffer::getDeviceAddress() const { return gpu; }

    // ---- Deko3DTexture

    Deko3DTexture::Deko3DTexture(Deko3DDevice *device, const RenderTextureDesc &desc) : device(device), desc(desc) {
        const bool depth = (desc.flags & RenderTextureFlag::DEPTH_TARGET) != 0;
        format = depth ? depthTargetFormat(desc.format) : toDkFormat(desc.format);
        if (format == DkImageFormat_None) {
            logf("texture format %d has no deko3d format", int(desc.format));
            return;
        }
        const uint32_t layers = std::max<uint32_t>(desc.arraySize, 1);
        const bool multisampled = desc.multisampling.sampleCount > RenderSampleCount::COUNT_1;
        switch (desc.dimension) {
        case RenderTextureDimension::TEXTURE_1D:
            type = layers > 1 ? DkImageType_1DArray : DkImageType_1D;
            break;
        case RenderTextureDimension::TEXTURE_3D:
            type = DkImageType_3D;
            break;
        case RenderTextureDimension::TEXTURE_2D:
        default:
            if (multisampled)
                type = layers > 1 ? DkImageType_2DMSArray : DkImageType_2DMS;
            else if ((desc.flags & RenderTextureFlag::CUBE) && layers % 6 == 0)
                type = layers > 6 ? DkImageType_CubemapArray : DkImageType_Cubemap;
            else
                type = layers > 1 ? DkImageType_2DArray : DkImageType_2D;
            break;
        }

        DkImageLayoutMaker m;
        dkImageLayoutMakerDefaults(&m, device->dk);
        m.type = type;
        m.format = format;
        m.msMode = toDkMsMode(desc.multisampling.sampleCount);
        m.dimensions[0] = std::max<uint32_t>(desc.width, 1);
        m.dimensions[1] = std::max<uint32_t>(desc.height, 1);
        // deko3d's layer counts (dk_image.cpp): a cube map's six faces are implied, a cube map array gives cubes (it
        // multiplies by six), a 1D array gives its layers as the height.
        m.dimensions[2] = type == DkImageType_3D ? std::max<uint32_t>(desc.depth, 1)
                          : type == DkImageType_Cubemap ? 6
                          : type == DkImageType_CubemapArray ? layers / 6 : layers;
        if (type == DkImageType_1D)
            m.dimensions[1] = m.dimensions[2] = 0;
        else if (type == DkImageType_1DArray) {
            m.dimensions[1] = layers;
            m.dimensions[2] = 0;
        }
        else if (type == DkImageType_2D || type == DkImageType_2DMS)
            m.dimensions[2] = 0;
        m.mipLevels = std::max<uint32_t>(desc.mipLevels, 1);
        uint32_t flags = 0;
        const bool render = (desc.flags & (RenderTextureFlag::RENDER_TARGET | RenderTextureFlag::DEPTH_TARGET)) != 0;
        if (render && type != DkImageType_3D) {
            flags |= DkImageFlags_UsageRender;
            // Colour targets compressed; depth too with Deko3DOptions::depthCompression (SwitchWakerHD's defaults left it
            // off as not verified).
            if (!depth || g_deko3dOptions.depthCompression) {
                flags |= DkImageFlags_HwCompression;
                compressed = true;  // sasrt-nx (2026-10-21): Deko3DOptions::depthResolve
            }
        }
        if (desc.flags & (RenderTextureFlag::UNORDERED_ACCESS | RenderTextureFlag::STORAGE))
            flags |= DkImageFlags_UsageLoadStore;
        if (type != DkImageType_3D && canUse2DEngine(format))
            flags |= DkImageFlags_Usage2DEngine;
        m.flags = flags;
        uint32_t rows = m.dimensions[1] ? m.dimensions[1] : 1;
        if (isCompressedFormat(format))
            rows = (rows + 3) / 4;
        tileSizeFix(m, rows);
        dkImageLayoutInitialize(&layout, &m);
        const uint64_t size = dkImageLayoutGetSize(&layout);
        const uint32_t alignment = dkImageLayoutGetAlignment(&layout);
        // sasrt-nx (2026-11-09, Deko3DSetDedicatedTargets; port 58): a render or depth target of 1 MB or more in its own chunk.
        const bool dedicated = render && size >= (uint64_t(1) << 20) && g_dedicatedTargets.load(std::memory_order_relaxed);
        if (size > 0xFFFFFFFFull || !device->imageAlloc(uint32_t(size), alignment, alloc, dedicated)) {
            g_memoryEmergency.store(true, std::memory_order_relaxed);  // sasrt-nx (2026-11-09 night; port 58)
            logf("texture %ux%ux%u format %d: no memory for %" PRIu64 " bytes", desc.width, desc.height, desc.depth,
                 int(desc.format), size);
            return;
        }
        dkImageInitialize(&image, &layout, alloc.block, alloc.offset);
        g_deko3dStats.imageBytes += alloc.size;
        valid = true;
    }

    Deko3DTexture::~Deko3DTexture() {
        if (valid && ownsMemory) {
            g_deko3dStats.imageBytes -= alloc.size;
            device->retire(alloc, nullptr);
        }
    }

    DkImageView Deko3DTexture::defaultView() const {
        DkImageView v;
        dkImageViewDefaults(&v, &image);
        return v;
    }

    std::unique_ptr<RenderTextureView> Deko3DTexture::createTextureView(const RenderTextureViewDesc &viewDesc) const {
        auto view = std::make_unique<Deko3DTextureView>();
        view->texture = this;
        view->desc = viewDesc;
        dkImageViewDefaults(&view->view, &image);
        // A different format of the same size (sRGB, typeless, integer views). A depth image keeps its own: deko3d
        // samples depth formats as depth (the red channel), which is what Renut's R32 views of depth read.
        if (viewDesc.format != RenderFormat::UNKNOWN && !isDepthFormat(format)) {
            const DkImageFormat f = toDkFormat(viewDesc.format);
            if (f != DkImageFormat_None && f != format)
                view->view.format = f;
        }
        // banjo-nx (2026-10-18): an S8_UINT view of a combined depth/stencil image samples its stencil (an unsigned
        // integer in the red channel), for Renut's depth resolve words.
        if (viewDesc.format == RenderFormat::S8_UINT && (format == DkImageFormat_ZF32_X24S8 || format == DkImageFormat_Z24S8))
            view->view.dsSource = DkDsSource_Stencil;
        switch (viewDesc.dimension) {
        case RenderTextureViewDimension::TEXTURE_1D:
            view->view.type = DkImageType_1D;
            break;
        case RenderTextureViewDimension::TEXTURE_2D:
            // banjo-nx (2026-10-20, from SASRT's port): a 2D view of a layered 2D image that does not pick one layer
            // stays a 2D array, as plume's Vulkan back end makes it (VK_IMAGE_VIEW_TYPE_2D_ARRAY when arraySize > 1).
            // Renut's stacked textures are such views, sampled through sampler2DArray; a single-layer 2D view read
            // layer 0 only.
            if (type == DkImageType_2DArray && desc.arraySize > 1 && viewDesc.arrayIndex == 0 &&
                (viewDesc.arraySize == UINT32_MAX || viewDesc.arraySize > 1))
                view->view.type = DkImageType_2DArray;
            else if (type == DkImageType_2DArray || type == DkImageType_Cubemap || type == DkImageType_CubemapArray)
                view->view.type = DkImageType_2D;
            break;
        case RenderTextureViewDimension::TEXTURE_3D:
            view->view.type = DkImageType_3D;
            break;
        case RenderTextureViewDimension::TEXTURE_CUBE:
            view->view.type = DkImageType_Cubemap;
            break;
        default:
            break;
        }
        if (viewDesc.arrayIndex != 0 || (viewDesc.arraySize != UINT32_MAX && viewDesc.arraySize != 0)) {
            view->view.layerOffset = uint16_t(viewDesc.arrayIndex);
            if (viewDesc.arraySize != UINT32_MAX)
                view->view.layerCount = uint16_t(viewDesc.arraySize);
        }
        if (viewDesc.mipSlice != 0 || viewDesc.mipLevels != UINT32_MAX) {
            view->view.mipLevelOffset = uint8_t(viewDesc.mipSlice);
            if (viewDesc.mipLevels != UINT32_MAX)
                view->view.mipLevelCount = uint8_t(viewDesc.mipLevels);
        }
        const RenderComponentMapping &c = viewDesc.componentMapping;
        view->view.swizzle[0] = toDk(c.r, DkImageSwizzle_Red);
        view->view.swizzle[1] = toDk(c.g, DkImageSwizzle_Green);
        view->view.swizzle[2] = toDk(c.b, DkImageSwizzle_Blue);
        view->view.swizzle[3] = toDk(c.a, DkImageSwizzle_Alpha);
        return view;
    }

    void Deko3DTexture::setName(const std::string &) {}

    // ---- Deko3DDescriptorSet

    Deko3DDescriptorSet::Deko3DDescriptorSet(Deko3DDevice *device, const RenderDescriptorSetDesc &desc) : device(device) {
        uint32_t index = 0;
        uint32_t images = 0, samplers = 0, buffers = 0;
        bool imageStarted = false, samplerStarted = false;
        for (uint32_t i = 0; i < desc.descriptorRangesCount; i++) {
            const RenderDescriptorRange &r = desc.descriptorRanges[i];
            uint32_t count = r.count;
            if (desc.lastRangeIsBoundless && i + 1 == desc.descriptorRangesCount)
                count = std::max<uint32_t>(count, desc.boundlessRangeSize);
            Range range;
            range.type = r.type;
            range.binding = r.binding;
            range.first = index;
            range.count = count;
            ranges.push_back(range);
            switch (r.type) {
            case RenderDescriptorRangeType::TEXTURE:
            case RenderDescriptorRangeType::READ_WRITE_TEXTURE:
                if (!imageStarted) {
                    imageFirst = index;
                    imageStarted = true;
                }
                images = index + count - imageFirst;
                break;
            case RenderDescriptorRangeType::SAMPLER:
                if (!samplerStarted) {
                    samplerFirst = index;
                    samplerStarted = true;
                }
                samplers = index + count - samplerFirst;
                break;
            default:
                buffers = index + count;
                break;
            }
            index += count;
        }
        if (images) {
            imageCount = images;
            imageBlock = device->makeBlock(alignUp(images * 32, DK_MEMBLOCK_ALIGNMENT), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "image descriptors");
            if (imageBlock)
                memset(dkMemBlockGetCpuAddr(imageBlock), 0, images * 32);
        }
        if (samplers) {
            samplerCount = samplers;
            samplerBlock = device->makeBlock(alignUp(samplers * 32, DK_MEMBLOCK_ALIGNMENT), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "sampler descriptors");
            if (samplerBlock)
                memset(dkMemBlockGetCpuAddr(samplerBlock), 0, samplers * 32);
        }
        this->buffers.resize(buffers ? index : 0);
    }

    Deko3DDescriptorSet::~Deko3DDescriptorSet() {
        if (imageBlock)
            device->retire({}, imageBlock);
        if (samplerBlock)
            device->retire({}, samplerBlock);
    }

    const Deko3DDescriptorSet::Range *Deko3DDescriptorSet::rangeOf(uint32_t descriptorIndex) const {
        for (const Range &r : ranges) {
            if (descriptorIndex >= r.first && descriptorIndex < r.first + r.count)
                return &r;
        }
        return nullptr;
    }

    void Deko3DDescriptorSet::setBuffer(uint32_t descriptorIndex, const RenderBuffer *buffer, uint64_t bufferSize, const RenderBufferStructuredView *structuredView, const RenderBufferFormattedView *) {
        if (descriptorIndex >= buffers.size())
            return;
        const Deko3DBuffer *b = static_cast<const Deko3DBuffer *>(buffer);
        DkBufExtents &e = buffers[descriptorIndex];
        if (b == nullptr || b->block == nullptr) {
            e = {};
            return;
        }
        uint64_t offset = 0;
        if (structuredView != nullptr)
            offset = uint64_t(structuredView->firstElement) * structuredView->structureByteStride;
        e.addr = b->gpu + offset;
        uint64_t size = bufferSize ? bufferSize : (b->desc.size - offset);
        const Range *r = rangeOf(descriptorIndex);
        if (r && (r->type == RenderDescriptorRangeType::CONSTANT_BUFFER || r->type == RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC))
            size = std::min<uint64_t>(size, DK_UNIFORM_BUF_MAX_SIZE);
        e.size = uint32_t(std::min<uint64_t>(size, 0xFFFFFFFFull));
    }

    // renut-ultimate (2026-11-10, port 59, from LostOdysseyRecomp ff013164; Deko3DOptions::relaxedDescriptorCounters): the
    // count and the epoch of a descriptor write. Relaxed, nothing waits for the uncached descriptor store to drain.
    static void noteDescriptorWrite(Deko3DDevice *device) {
        if (g_deko3dOptions.relaxedDescriptorCounters) {
            g_deko3dStats.descriptorWrites.fetch_add(1, std::memory_order_relaxed);
            device->descriptorEpoch.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_deko3dStats.descriptorWrites++;
        device->descriptorEpoch.fetch_add(1, std::memory_order_release);
    }

    void Deko3DDescriptorSet::setTexture(uint32_t descriptorIndex, const RenderTexture *texture, RenderTextureLayout, const RenderTextureView *textureView) {
        if (!imageBlock || descriptorIndex < imageFirst || descriptorIndex - imageFirst >= imageCount)
            return;
        DkImageDescriptor *d = static_cast<DkImageDescriptor *>(dkMemBlockGetCpuAddr(imageBlock)) + (descriptorIndex - imageFirst);
        const Deko3DTexture *t = static_cast<const Deko3DTexture *>(texture);
        if (t == nullptr || !t->valid) {
            memset(d, 0, sizeof(*d));
        }
        else {
            const Range *r = rangeOf(descriptorIndex);
            const bool loadStore = r && r->type == RenderDescriptorRangeType::READ_WRITE_TEXTURE;
            DkImageView view = textureView ? static_cast<const Deko3DTextureView *>(textureView)->view : t->defaultView();
            // Written into a local copy first: the GPU may read the descriptor block (other slots) while the CPU
            // writes it, so a slot is written whole, with one copy.
            DkImageDescriptor local;
            // renut-ultimate (2026-11-10, port 59, from LostOdysseyRecomp; Deko3DOptions::descriptorCache): a sampled
            // descriptor made once per view (or per texture for its default view), then copied.
            if (g_deko3dOptions.descriptorCache && !loadStore) {
                const Deko3DTextureView *tv = static_cast<const Deko3DTextureView *>(textureView);
                DkImageDescriptor &cached = tv ? tv->sampledDescriptor : t->sampledDescriptor;
                std::atomic<bool> &ready = tv ? tv->sampledDescriptorReady : t->sampledDescriptorReady;
                if (ready.load(std::memory_order_acquire)) {
                    g_deko3dStats.descriptorCacheHits.fetch_add(1, std::memory_order_relaxed);
                } else {
                    dkImageDescriptorInitialize(&cached, &view, false, false);
                    ready.store(true, std::memory_order_release);
                }
                local = cached;
            } else {
                dkImageDescriptorInitialize(&local, &view, loadStore, false);
            }
            memcpy(d, &local, sizeof(local));
            // banjo-nx (2026-10-19, GPU, txq): the size a texture query of this view returns at level 0: the image's
            // size at the view's first level; the depth of a 3D image there, else the view's layers (all from its first
            // when it names no count). tools/deko3d_glsl_opt.py's txq verify (constant 908 = 2) checks it on the GPU.
            const uint32_t id = descriptorIndex - imageFirst;
            if (imageSizes.size() <= id)
                imageSizes.resize(id + 1);
            const uint32_t m = view.mipLevelOffset;
            const uint32_t w = std::max(1u, t->desc.width >> m), h = std::max(1u, t->desc.height >> m);
            const DkImageType type = view.type != DkImageType_None ? view.type : t->type;
            uint32_t z;
            if (type == DkImageType_3D)
                z = std::max(1u, t->desc.depth >> m);
            else
                z = view.layerCount ? view.layerCount
                                    : (t->desc.arraySize > view.layerOffset ? t->desc.arraySize - view.layerOffset : 1u);
            imageSizes[id] = {w, h, z};
            if (imageTextures.size() <= id)  // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate)
                imageTextures.resize(id + 1, nullptr);
            imageTextures[id] = t;
        }
        if (t == nullptr || !t->valid) {  // sasrt-nx (2026-11-08): an emptied slot names no texture
            const uint32_t id = descriptorIndex - imageFirst;
            if (id < imageTextures.size())
                imageTextures[id] = nullptr;
        }
        noteDescriptorWrite(device);
    }

    void Deko3DDescriptorSet::setSampler(uint32_t descriptorIndex, const RenderSampler *sampler) {
        if (!samplerBlock || descriptorIndex < samplerFirst || descriptorIndex - samplerFirst >= samplerCount)
            return;
        DkSamplerDescriptor *d = static_cast<DkSamplerDescriptor *>(dkMemBlockGetCpuAddr(samplerBlock)) + (descriptorIndex - samplerFirst);
        if (sampler == nullptr)
            memset(d, 0, sizeof(*d));
        else
            memcpy(d, &static_cast<const Deko3DSampler *>(sampler)->descriptor, sizeof(*d));
        noteDescriptorWrite(device);
    }

    void Deko3DDescriptorSet::setAccelerationStructure(uint32_t, const RenderAccelerationStructure *) {}

    // ---- Deko3DQueryPool

    Deko3DQueryPool::Deko3DQueryPool(Deko3DDevice *device, uint32_t count) : device(device) {
        results.resize(count);
        // A counter report is 16 bytes: the counter, then the GPU timestamp.
        block = device->makeBlock(alignUp(std::max<uint32_t>(count, 1) * 16, DK_MEMBLOCK_ALIGNMENT), DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "query pool");
        if (block)
            memset(dkMemBlockGetCpuAddr(block), 0, count * 16);
    }

    Deko3DQueryPool::~Deko3DQueryPool() {
        if (block)
            device->retire({}, block);
    }

    void Deko3DQueryPool::queryResults() { queryResults(uint32_t(results.size())); }

    bool Deko3DQueryPool::queryResults(uint32_t count) {
        count = std::min<uint32_t>(count, uint32_t(results.size()));
        if (!block)
            return true;
        const volatile uint64_t *reports = static_cast<const volatile uint64_t *>(dkMemBlockGetCpuAddr(block));
        for (uint32_t i = 0; i < count; i++)
            results[i] = dkTimestampToNs(reports[i * 2 + 1]);
        return true;
    }

    // ---- Deko3DDevice

    static void debugCallback(void *, const char *context, DkResult result, const char *message) {
        logf("deko3d says (%s, result %d): %s", context ? context : "", int(result), message ? message : "");
    }

    void Deko3DSetDedicatedTargets(bool on) { g_dedicatedTargets.store(on, std::memory_order_relaxed); }

    // sasrt-nx (2026-11-11, Deko3DSetTargetBlockCacheMb)
    static std::atomic<uint64_t> g_targetCacheCap{0};
    static std::atomic<uint64_t> g_targetCacheBytesNow{0}, g_targetCacheHits{0}, g_targetCacheMisses{0},
        g_targetCacheKept{0}, g_targetCacheEvicted{0}, g_targetCacheReleased{0};
    static std::atomic<uint32_t> g_targetCacheBlocks{0};
    void Deko3DSetTargetBlockCacheMb(uint32_t mb) { g_targetCacheCap.store(uint64_t(mb) << 20, std::memory_order_relaxed); }
    void Deko3DTargetCacheFigures(Deko3DTargetCacheInfo &out) {
        out.bytes = g_targetCacheBytesNow.load(std::memory_order_relaxed);
        out.blocks = g_targetCacheBlocks.load(std::memory_order_relaxed);
        out.hits = g_targetCacheHits.load(std::memory_order_relaxed);
        out.misses = g_targetCacheMisses.load(std::memory_order_relaxed);
        out.kept = g_targetCacheKept.load(std::memory_order_relaxed);
        out.evicted = g_targetCacheEvicted.load(std::memory_order_relaxed);
        out.released = g_targetCacheReleased.load(std::memory_order_relaxed);
    }

    void Deko3DDevice::cacheTargetBlockLocked(DkMemBlock block, uint32_t size) {
        const uint64_t cap = g_targetCacheCap.load(std::memory_order_relaxed);
        if (!cap || size > cap) {
            destroyBlock(block);
            return;
        }
        targetCache.push_back({block, size});
        targetCacheBytes += size;
        g_targetCacheKept.fetch_add(1, std::memory_order_relaxed);
        // over the cap: the oldest go
        size_t drop = 0;
        while (targetCacheBytes > cap && drop < targetCache.size()) {
            destroyBlock(targetCache[drop].block);
            targetCacheBytes -= targetCache[drop].size;
            ++drop;
        }
        if (drop) {
            targetCache.erase(targetCache.begin(), targetCache.begin() + drop);
            g_targetCacheEvicted.fetch_add(drop, std::memory_order_relaxed);
        }
        g_targetCacheBytesNow.store(targetCacheBytes, std::memory_order_relaxed);
        g_targetCacheBlocks.store(uint32_t(targetCache.size()), std::memory_order_relaxed);
    }

    DkMemBlock Deko3DDevice::takeCachedTargetBlockLocked(uint32_t size, uint32_t &blockSize) {
        if (targetCache.empty()) {
            g_targetCacheMisses.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        // the smallest block of the size or up to an eighth bigger
        const uint64_t most = uint64_t(size) + size / 8;
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < targetCache.size(); i++) {
            const uint32_t s = targetCache[i].size;
            if (s >= size && s <= most && (best == SIZE_MAX || s < targetCache[best].size))
                best = i;
        }
        if (best == SIZE_MAX) {
            g_targetCacheMisses.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        const CachedBlock taken = targetCache[best];
        targetCache.erase(targetCache.begin() + best);
        targetCacheBytes -= taken.size;
        g_targetCacheHits.fetch_add(1, std::memory_order_relaxed);
        g_targetCacheBytesNow.store(targetCacheBytes, std::memory_order_relaxed);
        g_targetCacheBlocks.store(uint32_t(targetCache.size()), std::memory_order_relaxed);
        blockSize = taken.size;
        return taken.block;
    }

    uint64_t Deko3DDevice::releaseTargetCacheLocked() {
        uint64_t freed = 0;
        for (const CachedBlock &c : targetCache) {
            destroyBlock(c.block);
            freed += c.size;
        }
        if (!targetCache.empty())
            g_targetCacheReleased.fetch_add(targetCache.size(), std::memory_order_relaxed);
        targetCache.clear();
        targetCacheBytes = 0;
        g_targetCacheBytesNow.store(0, std::memory_order_relaxed);
        g_targetCacheBlocks.store(0, std::memory_order_relaxed);
        return freed;
    }

    Deko3DDevice::Deko3DDevice() { g_imageDevice = this; }

    uint64_t Deko3DReleaseEmptyImageChunks() {  // sasrt-nx (2026-11-09; port 58): plume_deko3d.h
        Deko3DDevice *d = g_imageDevice;
        if (!d)
            return 0;
        std::lock_guard<std::mutex> heap(d->heapMutex);
        return d->releaseEmptyChunksLocked();
    }

    static void *compileThreadEntry(void *arg) {
        static_cast<Deko3DDevice *>(arg)->compileWorker();
        return nullptr;
    }

    bool Deko3DDevice::initialize() {
        {
            uint64_t heapFree = 0, processFree = 0;
            memoryFigures(heapFree, processFree);
            logf("device: creating (heap %" PRIu64 " MB free, process %" PRIu64 " MB unused)", heapFree >> 20, processFree >> 20);
            {   // blur-nx (2026-11-13): nvservices' transfer memory (libnx __nx_nv_transfermem_size) and the system resource
                uint64_t used = 0, total = 0;
                systemResource(used, total);
                logf("device: nvservices transfer memory %u KiB; system resource %" PRIu64 " of %" PRIu64 " KiB used",
                     __nx_nv_transfermem_size >> 10, used >> 10, total >> 10);
            }
        }
        DkDeviceMaker dm;
        dkDeviceMakerDefaults(&dm);
        dm.cbDebug = debugCallback;
        // Upper-left origin and depth from 0 to 1 (Vulkan's, D3D's); with the upper-left origin deko3d 0.5.0 maps clip
        // space y = +1 to the top row, D3D's convention: the deko3d shaders are compiled without -fvk-invert-y.
        dm.flags = DkDeviceFlags_OriginUpperLeft | DkDeviceFlags_DepthZeroToOne;
        dk = dkDeviceCreate(&dm);
        if (!dk) {
            logf("dkDeviceCreate failed");
            return false;
        }
        DkQueueMaker qm;
        dkQueueMakerDefaults(&qm, dk);
        // Zcull off unless Deko3DOptions::zcull (SwitchWakerHD: with it deko3d dropped depth-biased decals in whole 4x8
        // tiles).
        qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_MediumPrio |
                   (g_deko3dOptions.zcull ? 0u : uint32_t(DkQueueFlags_DisableZcull));
        qm.commandMemorySize = 1u << 20;
        qm.flushThreshold = qm.commandMemorySize / 8;
        // Scratch (local) memory per warp: Renut's biggest vertex shaders need 32 KB (uam's DKSH headers; 81 of the 4,881
        // need some), variants maybe more: 64 KB a warp (about 8 MB for the GPU's warps).
        qm.perWarpScratchMemorySize = 64u << 10;
        perWarpScratch = qm.perWarpScratchMemorySize;
        queue = dkQueueCreate(&qm);
        if (!queue) {
            logf("dkQueueCreate failed");
            return false;
        }
        pushBlock = makeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "push constants");
        if (!pushBlock)
            return false;
        memset(dkMemBlockGetCpuAddr(pushBlock), 0, DK_MEMBLOCK_ALIGNMENT);
        pushAddr = dkMemBlockGetGpuAddr(pushBlock);
        texSizeBlock = makeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "texture sizes");
        if (!texSizeBlock)
            return false;
        memset(dkMemBlockGetCpuAddr(texSizeBlock), 0, DK_MEMBLOCK_ALIGNMENT);
        texSizeAddr = dkMemBlockGetGpuAddr(texSizeBlock);
        zeroBlock = makeBlock(ZeroSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "zero vertex buffer");
        if (!zeroBlock)
            return false;
        memset(dkMemBlockGetCpuAddr(zeroBlock), 0, ZeroSize);
        zeroAddr = dkMemBlockGetGpuAddr(zeroBlock);

        capabilities.geometryShader = true;
        capabilities.descriptorIndexing = true;
        capabilities.scalarBlockLayout = false;
        capabilities.bufferDeviceAddress = true;
        capabilities.samplerMirrorClampToEdge = true;
        capabilities.maxTextureSize = 16384;
        capabilities.triangleFan = true;
        capabilities.dynamicDepthBias = true;
        capabilities.uma = true;
        capabilities.queryPools = true;
        capabilities.resolveRegion = false;
        description.name = "deko3d (Tegra X1, Maxwell GM20B)";
        description.type = RenderDeviceType::INTEGRATED;
        description.vendor = RenderDeviceVendor::NVIDIA;
        description.driverVersion = 0x000500;

        // The uam worker: one thread (uam is not reentrant), 8 MB of stack (Mesa's parser and nv50_ir recurse deeply).
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);
        pthread_t thread;
        if (pthread_create(&thread, &attr, compileThreadEntry, this) != 0) {
            logf("the shader compile thread did not start");
            pthread_attr_destroy(&attr);
            return false;
        }
        pthread_attr_destroy(&attr);
        pthread_detach(thread);
        logf("device ready: queue with zcull %s, depth targets %scompressed, vertex shader outputs %s, 1 MB of queue "
             "command memory; uam compile thread started", g_deko3dOptions.zcull ? "on" : "off",
             g_deko3dOptions.depthCompression ? "" : "not ", g_deko3dOptions.vsOutputs ? "pruned per pipeline" : "all written");
        return true;
    }

    // banjo-nx (2026-10-23): boost mode while a draw waits for a compile (boostBegin / boostEnd, by compile()).
    static std::mutex g_boostMutex;
    static int g_boostWaiters = 0;
    static bool g_boostActive = false, g_boostRefused = false;
    static uint64_t g_boostSince = 0;

    Deko3DDevice::~Deko3DDevice() {
        {  // blur-nx (2026-11-18): the spare-chunk thread stopped first (it makes blocks and adds chunks)
            std::unique_lock<std::mutex> lock(spareMutex);
            spareStop = true;
            spareWake.notify_all();
            spareDone.wait_for(lock, std::chrono::seconds(2), [this] { return !spareRunning; });
        }
        {
            std::lock_guard<std::mutex> lock(g_boostMutex);  // banjo-nx (2026-10-23): never left in boost mode
            if (g_boostActive)
                appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
            g_boostActive = false;
        }
        if (queue)
            dkQueueWaitIdle(queue);
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            compileStop = true;
        }
        compileWake.notify_all();
        {
            std::unique_lock<std::mutex> lock(compileMutex);
            compileDone.wait_for(lock, std::chrono::seconds(2), [this] { return !compilerReady; });
        }
        collectRetired();
        for (Retired &r : retired) {
            if (r.block)
                destroyBlock(r.block);
        }
        retired.clear();
        for (auto &c : chunks)
            destroyBlock(c.block);
        for (auto &c : codeBlocks)
            destroyBlock(c.first);
        if (pushBlock)
            destroyBlock(pushBlock);
        if (zeroBlock)
            destroyBlock(zeroBlock);
        if (queue)
            dkQueueDestroy(queue);
        if (dk)
            dkDeviceDestroy(dk);
    }

    // blur-nx (2026-11-18, for 1 % lows, every port): a block that took 4 ms or more to make is logged with its time (64
    // lines at most): the storage zeroed and flushed, the GPU mapping, the heap's figures, on the thread that asked.
    DkMemBlock Deko3DDevice::makeBlock(uint32_t size, uint32_t flags, const char *what) {
        const uint64_t t0 = armGetSystemTick();
        DkMemBlock block = makeBlockUntimed(size, flags, what);
        const double ms = double(armTicksToNs(armGetSystemTick() - t0)) / 1e6;
        static std::atomic<uint32_t> slowLogged{0};
        if (ms >= 4.0 && slowLogged.fetch_add(1, std::memory_order_relaxed) < 64) {
            s32 priority = -1;
            svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
            logf("memory block: %s, %u KiB took %.1f ms (a thread of priority 0x%X, core %d)%s", what, size >> 10, ms,
                 uint32_t(priority), svcGetCurrentProcessorNumber(), block ? "" : ": NONE MADE");
        }
        return block;
    }

    DkMemBlock Deko3DDevice::makeBlockUntimed(uint32_t size, uint32_t flags, const char *what) {
        DkMemBlockMaker m;
        dkMemBlockMakerDefaults(&m, dk, alignUp(size, DK_MEMBLOCK_ALIGNMENT));
        m.flags = flags;
        // deko3d takes the block's memory from the heap (aligned_alloc) and aborts the process when a creation fails:
        // big ones, and every 64 MB of them, are logged with the heap's free bytes.
        static std::atomic<uint64_t> total{0};
        static std::atomic<uint32_t> logged{0};
        const uint64_t before = total.fetch_add(m.size);
        // banjo-nx (2026-10-19, render-thread proposal 18): 24 lines, not 96 (each walks the heap and writes the SD card,
        // on the thread making the block: the render thread for the upload ring's chunks).
        if ((size >= (8u << 20) || (before >> 26) != ((before + m.size) >> 26)) && logged.fetch_add(1) < 24) {
            uint64_t heapFree = 0, processFree = 0;
            memoryFiguresQuick(heapFree, processFree);  // port 161: no heap walk (the heap's room)
            logf("memory block: %s, %u KiB (deko3d blocks %" PRIu64 " MB in all; heap %" PRIu64 " MB free, process %" PRIu64 " MB unused)",
                 what, size >> 10, (before + m.size) >> 20, heapFree >> 20, processFree >> 20);
        }
#if defined(PLUME_DEKO3D_GUARDS)
        // The block's storage with guard bytes after it (deko3d takes storage given to it as it is).
        uint8_t *storage = static_cast<uint8_t *>(memalign(DK_MEMBLOCK_ALIGNMENT, m.size + GuardBytes));
        if (storage) {
            memset(storage + m.size, GuardByte, GuardBytes);
            m.storage = storage;
        }
        leaveBlockCrumb(m.size, m.flags, 2, what);  // blur-nx (2026-11-13)
        DkMemBlock block = dkMemBlockCreate(&m);
        blockMade(block, what);
        if (storage) {
            std::lock_guard<std::mutex> lock(guardMutex);
            guarded.push_back({block, storage, m.size, what});
        }
        return block;
#else
        // banjo-nx (2026-10-28, the console's OOM, 05:43): deko3d takes the block's memory from the heap itself (and
        // zeroes it) and aborts the process when it cannot. So the room is tried first: an allocation of the same size
        // and alignment, freed at once. None: the image heap's empty chunks (unless this thread holds the image heap,
        // making a chunk) and the application's spares go back and it is tried again; still none: a log line and no
        // block (each caller's failure path) instead of the abort. (Giving deko3d storage of our own hung the emulator:
        // such a block is not zeroed, and the zero vertex buffer and command memory rely on it.)
        // banjo-nx (2026-10-28, h10): the GPU region first (see g_region); this storage is zeroed here.
        if (g_regionOn && m.size >= kRegionMinBlock) {
            const uint64_t span = alignUp64(m.size, kRegionGrain);
            uintptr_t at = 0, startedAt = 0;
            uint32_t growths = 0;
            bool grew = false;
            uint64_t regionSize = 0;
            {
                std::lock_guard<std::mutex> lock(g_regionMutex);
                if (!g_region.high) {
                    // The region starts at the heap's end, on the grain.
                    const uintptr_t end = reinterpret_cast<uintptr_t>(fake_heap_end) & ~uintptr_t(kRegionGrain - 1);
                    if (regionMoveHeapEnd(end, true)) {
                        g_region.low = g_region.high = end;
                        startedAt = end;
                    }
                }
                if (g_region.high) {
                    const uint32_t before = g_region.growths;
                    at = g_region.alloc(span, kRegionStep, [](uintptr_t low) { return regionMoveHeapEnd(low, true); });
                    grew = g_region.growths != before;
                    growths = g_region.growths;
                    regionSize = g_region.high - g_region.low;
                    if (!at)
                        ++g_regionRefused;
                }
            }
            if (startedAt)
                logf("GPU region: at the heap's top, %p (banjo_deko3d_gpu_region)", reinterpret_cast<void *>(startedAt));
            if (grew && growths <= 48)
                logf("GPU region: grown to %" PRIu64 " MB for a %s of %u KiB (growth %u)", regionSize >> 20, what,
                     size >> 10, growths);
            if (at) {
                void *storage = reinterpret_cast<void *>(at);
                memset(storage, 0, m.size);
                armDCacheFlush(storage, m.size);
                m.storage = storage;
                g_regionBytesNow.store(regionSize, std::memory_order_relaxed);  // blur-nx (2026-11-13)
                leaveBlockCrumb(m.size, m.flags, 1, what);
                DkMemBlock block = dkMemBlockCreate(&m);
                blockMade(block, what);
                std::lock_guard<std::mutex> lock(g_regionMutex);
                if (block)
                    g_regionBlocks.emplace(block, std::make_pair(at, span));
                else
                    g_region.release(at, span, kRegionGiveBackAt, kRegionKeep, kRegionStep,
                                     [](uintptr_t low) { return regionMoveHeapEnd(low, false); });
                return block;
            }
            // No room above newlib's break: newlib's heap, as before.
        }
        void *probe = memalign(DK_MEMBLOCK_ALIGNMENT, m.size);
        if (!probe) {
            // banjo-nx (2026-10-28, night_gpu2 console freeze): a request at least as big as one that failed in the last
            // 250 ms fails at once, and only the first 8 failures take the heap's figures for their log line: mallinfo
            // walked 170,000 free pieces under malloc's lock (the audio threads waited in free), and with the SD card
            // write each failure took 22 ms; the stream cache asked 3,812 times in 95 s (0.1 fps).
            static std::atomic<uint64_t> failedSize{0}, failedTick{0};
            static std::atomic<uint32_t> failures{0}, quick{0};
            const uint64_t tick = armGetSystemTick();
            if (m.size >= failedSize.load(std::memory_order_relaxed) &&
                tick - failedTick.load(std::memory_order_relaxed) < armNsToTicks(250000000ull)) {
                quick.fetch_add(1, std::memory_order_relaxed);
                total.fetch_sub(m.size);
                g_memoryEmergency.store(true, std::memory_order_relaxed);  // sasrt-nx (2026-11-09 night; port 58)
                return nullptr;
            }
            uint64_t freed = 0;
            if (!t_inImageAlloc) {
                std::lock_guard<std::mutex> heap(heapMutex);
                freed = releaseEmptyChunksLocked();
            }
            if (g_memoryPressure)
                g_memoryPressure();
            probe = memalign(DK_MEMBLOCK_ALIGNMENT, m.size);
            const uint32_t n = failures.fetch_add(1, std::memory_order_relaxed);
            if (n < 8) {
                uint64_t heapFree = 0, processFree = 0;
                memoryFigures(heapFree, processFree);
                logf("memory block: %s, %u KiB not had from the heap at first (%" PRIu64 " MB of image chunks given "
                     "back, the application's spares released): %s (heap %" PRIu64 " MB free, process %" PRIu64
                     " MB unused)", what, size >> 10, freed >> 20, probe ? "had on the second try" : "NONE MADE",
                     heapFree >> 20, processFree >> 20);
            } else if ((n & 255) == 0) {
                logf("memory block: %s, %u KiB: %s; %u blocks not had at first so far, %u more refused at once",
                     what, size >> 10, probe ? "had on the second try" : "NONE MADE", n + 1,
                     quick.load(std::memory_order_relaxed));
            }
            if (!probe) {
                g_memoryEmergency.store(true, std::memory_order_relaxed);  // sasrt-nx (2026-11-09 night; port 58)
                failedSize.store(m.size, std::memory_order_relaxed);
                failedTick.store(armGetSystemTick(), std::memory_order_relaxed);
                total.fetch_sub(m.size);
                return nullptr;
            }
            failedTick.store(0, std::memory_order_relaxed);
        }
        free(probe);
        leaveBlockCrumb(m.size, m.flags, 0, what);  // blur-nx (2026-11-13)
        DkMemBlock block = dkMemBlockCreate(&m);
        blockMade(block, what);
        return block;
#endif
    }

    void Deko3DDevice::destroyBlock(DkMemBlock block) {
        if (!block)
            return;
        // banjo-nx (2026-10-28, h10): a GPU region block's range goes back after deko3d has let it go; taken out of the
        // table first (the block's handle may be made again by another thread once destroyed).
        std::pair<uintptr_t, uint64_t> range{0, 0};
        {
            std::lock_guard<std::mutex> lock(g_regionMutex);
            if (auto it = g_regionBlocks.find(block); it != g_regionBlocks.end()) {
                range = it->second;
                g_regionBlocks.erase(it);
            }
        }
        const uint64_t bytes = dkMemBlockGetSize(block);  // blur-nx (2026-11-13): the live figures
        dkMemBlockDestroy(block);
        g_blocksLive.fetch_sub(1, std::memory_order_relaxed);
        g_blocksLiveBytes.fetch_sub(bytes, std::memory_order_relaxed);
        if (range.second) {
            std::lock_guard<std::mutex> lock(g_regionMutex);
            g_region.release(range.first, range.second, kRegionGiveBackAt, kRegionKeep, kRegionStep,
                             [](uintptr_t low) { return regionMoveHeapEnd(low, false); });
        }
#if defined(PLUME_DEKO3D_GUARDS)
        std::lock_guard<std::mutex> lock(guardMutex);
        for (size_t i = 0; i < guarded.size(); i++) {
            if (guarded[i].block == block) {
                free(guarded[i].storage);
                guarded[i] = guarded.back();
                guarded.pop_back();
                break;
            }
        }
#endif
    }

    void Deko3DDevice::checkGuards() {
#if defined(PLUME_DEKO3D_GUARDS)
        std::lock_guard<std::mutex> lock(guardMutex);
        for (GuardedBlock &g : guarded) {
            const uint8_t *guard = g.storage + g.size;
            for (uint32_t i = 0; i < GuardBytes; i++) {
                if (guard[i] != GuardByte) {
                    uint32_t last = i;
                    for (uint32_t j = i; j < GuardBytes; j++)
                        if (guard[j] != GuardByte)
                            last = j;
                    uint32_t word = 0;
                    memcpy(&word, guard + (i & ~3u), 4);
                    logf("GUARD: written past the end of %s (%u bytes at GPU 0x%" PRIx64 "): guard bytes %u..%u changed "
                         "(first word 0x%08X)", g.what, g.size, dkMemBlockGetGpuAddr(g.block), i, last, word);
                    memset(const_cast<uint8_t *>(guard), GuardByte, GuardBytes);
                    break;
                }
            }
        }
#endif
    }

    bool Deko3DDevice::findBuffer(DkGpuAddr address, DkGpuAddr &base, uint32_t &size, const uint8_t *&cpu) {
        std::lock_guard<std::mutex> lock(bufferMutex);
        auto it = bufferRanges.upper_bound(address);
        if (it == bufferRanges.begin())
            return false;
        --it;
        if (address >= it->first + it->second.first)
            return false;
        base = it->first;
        size = it->second.first;
        cpu = it->second.second;
        return true;
    }

    // Image heap: 32 MB chunks (bigger for a bigger image), first fit. sasrt-nx (2026-11-09; port 58): 64 MB before; a
    // chunk goes back to the GPU region only when empty, and a few textures left in a 64 MB one held it all (render and
    // depth targets have chunks of their own: Deko3DSetDedicatedTargets).
    static constexpr uint32_t ImageChunkSize = 32u << 20;

    // banjo-nx (2026-10-28, the console's OOM): what the heap's room is short of: the image heap's empty chunks go back
    // below it (the application's rule for its own spares, f7b), and kept above it (a chunk given back is made again by
    // the next texture: a 64 MB allocation and a GPU mapping).
    static bool heapRoomShort() {
        char *brk = static_cast<char *>(sbrk(0));
        const uint64_t room = (brk && fake_heap_end > brk) ? uint64_t(fake_heap_end - brk) : 0;
        return room < (uint64_t(256) << 20);
    }

    uint64_t Deko3DDevice::releaseEmptyChunksLocked() {
        uint64_t freed = releaseTargetCacheLocked();  // sasrt-nx (2026-11-11, Deko3DSetTargetBlockCacheMb)
        for (Chunk &c : chunks) {
            if (c.block && c.freeList.size() == 1 && c.freeList[0].first == 0 && c.freeList[0].second == c.size) {
                destroyBlock(c.block);
                freed += c.size;
                c.block = nullptr;
                c.size = 0;
                c.freeList.clear();
            }
        }
        if (freed)
            logf("image heap: %" PRIu64 " MiB of empty chunks given back to the heap", freed >> 20);
        return freed;
    }

    static void chunkFree(Deko3DDevice::Chunk &c, uint32_t offset, uint32_t size) {
        auto it = std::lower_bound(c.freeList.begin(), c.freeList.end(), std::make_pair(offset, 0u));
        it = c.freeList.insert(it, {offset, size});
        if (it + 1 != c.freeList.end() && it->first + it->second == (it + 1)->first) {
            it->second += (it + 1)->second;
            c.freeList.erase(it + 1);
        }
        if (it != c.freeList.begin() && (it - 1)->first + (it - 1)->second == it->first) {
            (it - 1)->second += it->second;
            c.freeList.erase(it);
        }
    }

    // blur-nx (2026-11-18, for 1 % lows, every port: Deko3DSetImageSpareChunk). The image heap's spare chunk: asked for
    // (heapMutex held) after an image took room in the shared chunks, when none of them is empty; made on a thread of
    // its own, outside every lock, and added under heapMutex. One at a time (spareMaking), so at most one is made ahead.
    static void *spareThreadEntry(void *arg) {
        static_cast<Deko3DDevice *>(arg)->spareWorker();
        return nullptr;
    }

    void Deko3DDevice::wantSpareLocked() {
        if (!g_imageSpare.load(std::memory_order_relaxed))
            return;
        for (const Chunk &c : chunks)
            if (c.block && !c.dedicated && c.freeList.size() == 1 && c.freeList[0].first == 0 && c.freeList[0].second == c.size)
                return;  // an empty shared chunk: the spare is there
        std::lock_guard<std::mutex> lock(spareMutex);
        if (spareStop || spareWanted || spareMaking)
            return;
        spareWanted = true;
        if (!spareStarted) {
            spareStarted = true;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 256u << 10);
            pthread_t thread;
            spareRunning = pthread_create(&thread, &attr, spareThreadEntry, this) == 0;
            pthread_attr_destroy(&attr);
            if (spareRunning)
                pthread_detach(thread);
            else
                logf("image heap: the spare-chunk thread did not start");
        }
        spareWake.notify_one();
    }

    void Deko3DDevice::spareWorker() {
        // The uam thread's place: off the render thread's core, below the game's threads (the priority the process
        // allows at or above the asked one, as compileWorker takes it).
        if (g_deko3dOptions.compileCore >= 0)
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_deko3dOptions.compileCore, 1u << g_deko3dOptions.compileCore);
        s32 asked = g_deko3dOptions.compilePriority, priority = -1;
        if (asked > 0) {
            u64 allowed = 0;
            if (R_SUCCEEDED(svcGetInfo(&allowed, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0)) && allowed != 0) {
                s32 p = std::min<s32>(asked, 63);
                while (p > 0 && !(allowed & (1ull << p)))
                    p--;
                asked = p > 0 ? p : asked;
            }
            svcSetThreadPriority(CUR_THREAD_HANDLE, u32(asked));
        }
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        logf("image heap: the spare-chunk thread started (core %d, priority 0x%X; Deko3DSetImageSpareChunk)",
             g_deko3dOptions.compileCore, uint32_t(priority));
        std::unique_lock<std::mutex> lock(spareMutex);
        for (;;) {
            spareWake.wait(lock, [this] { return spareStop || spareWanted; });
            if (spareStop)
                break;
            spareWanted = false;
            spareMaking = true;
            lock.unlock();
            DkMemBlock block = nullptr;
            const uint64_t t0 = armGetSystemTick();
            if (!heapRoomShort() && g_imageSpare.load(std::memory_order_relaxed)) {
                t_inImageAlloc = true;  // no empty chunks given back to make a spare one
                block = makeBlock(ImageChunkSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "image heap chunk (ahead)");
                t_inImageAlloc = false;
            }
            const double ms = double(armTicksToNs(armGetSystemTick() - t0)) / 1e6;
            if (block) {
                size_t n = 0;
                {
                    std::lock_guard<std::mutex> heap(heapMutex);
                    Chunk c;
                    c.block = block;
                    c.size = ImageChunkSize;
                    c.freeList.push_back({0, ImageChunkSize});
                    chunks.push_back(std::move(c));
                    n = chunks.size();
                }
                logf("image heap: chunk %zu of %u MiB made ahead in %.1f ms (Deko3DSetImageSpareChunk)", n,
                     ImageChunkSize >> 20, ms);
            }
            lock.lock();
            spareMaking = false;
        }
        spareRunning = false;
        spareDone.notify_all();
    }

    bool Deko3DDevice::imageAlloc(uint32_t size, uint32_t alignment, Deko3DImageAlloc &out, bool dedicated) {
        size = alignUp(size, 256);
        alignment = std::max<uint32_t>(alignment, 256);
        collectRetired();
        std::lock_guard<std::mutex> lock(heapMutex);
        if (dedicated) {
            // sasrt-nx (2026-11-09, Deko3DSetDedicatedTargets; port 58): a chunk of the image's own size (in a slot a
            // freed one left, if any), placed as the first fit places an image at a chunk's start.
            uint32_t own = alignUp(size + alignment, DK_MEMBLOCK_ALIGNMENT);
            // sasrt-nx (2026-11-11, Deko3DSetTargetBlockCacheMb): a kept block of the size (or a little bigger) first
            uint32_t cachedSize = 0;
            DkMemBlock block = takeCachedTargetBlockLocked(own, cachedSize);
            if (block)
                own = cachedSize;
            t_inImageAlloc = true;
            if (!block)
                block = makeBlock(own, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "render target (own chunk)");
            if (!block) {
                releaseEmptyChunksLocked();
                block = makeBlock(own, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "render target (own chunk)");
            }
            t_inImageAlloc = false;
            if (block) {
                size_t ci = chunks.size();
                for (size_t i = 0; i < chunks.size(); i++)
                    if (!chunks[i].block && chunks[i].size == 0) {
                        ci = i;
                        break;
                    }
                if (ci == chunks.size())
                    chunks.emplace_back();
                Chunk &c = chunks[ci];
                c.block = block;
                c.size = own;
                c.dedicated = true;
                c.freeList.clear();
                if (size < own)
                    c.freeList.push_back({size, own - size});
                out.block = block;
                out.offset = 0;
                out.size = size;
                out.chunk = int32_t(ci);
                return true;
            }
            // no block of its own: the shared chunks as before
        }
        for (int pass = 0; pass < 2; pass++) {
            if (g_imageBestFit.load(std::memory_order_relaxed)) {
                // sasrt-nx (2026-11-09 night, Deko3DSetImageBestFit; port 58): the smallest free range that fits, in any
                // shared chunk
                size_t bc = SIZE_MAX, bi = 0;
                uint32_t blen = UINT32_MAX;
                for (size_t ci = 0; ci < chunks.size(); ci++) {
                    const Chunk &c = chunks[ci];
                    if (c.dedicated || !c.block)
                        continue;
                    for (size_t i = 0; i < c.freeList.size(); i++) {
                        const auto [off, len] = c.freeList[i];
                        const uint32_t at = alignUp(off, alignment);
                        if (at - off + uint64_t(size) > len || len >= blen)
                            continue;
                        bc = ci, bi = i, blen = len;
                    }
                }
                if (bc != SIZE_MAX) {
                    Chunk &c = chunks[bc];
                    const auto [off, len] = c.freeList[bi];
                    const uint32_t at = alignUp(off, alignment);
                    c.freeList.erase(c.freeList.begin() + long(bi));
                    if (at > off)
                        chunkFree(c, off, at - off);
                    if (at + size < off + len)
                        chunkFree(c, at + size, off + len - at - size);
                    out.block = c.block;
                    out.offset = at;
                    out.size = size;
                    out.chunk = int32_t(bc);
                    wantSpareLocked();  // blur-nx (2026-11-18): the next chunk made ahead if none is empty now
                    return true;
                }
            }
            for (size_t ci = 0; ci < chunks.size(); ci++) {
                Chunk &c = chunks[ci];
                if (c.dedicated)
                    continue;  // sasrt-nx (2026-11-09; port 58): one target's
                for (size_t i = 0; i < c.freeList.size(); i++) {
                    const auto [off, len] = c.freeList[i];
                    const uint32_t at = alignUp(off, alignment);
                    if (at - off + uint64_t(size) > len)
                        continue;
                    c.freeList.erase(c.freeList.begin() + long(i));
                    if (at > off)
                        chunkFree(c, off, at - off);
                    if (at + size < off + len)
                        chunkFree(c, at + size, off + len - at - size);
                    out.block = c.block;
                    out.offset = at;
                    out.size = size;
                    out.chunk = int32_t(ci);
                    wantSpareLocked();  // blur-nx (2026-11-18): the next chunk made ahead if none is empty now
                    return true;
                }
            }
            if (pass == 0) {
                uint32_t chunkSize = std::max(ImageChunkSize, alignUp(size + alignment, DK_MEMBLOCK_ALIGNMENT));
                Chunk c;
                const uint64_t made_at = armGetSystemTick();  // blur-nx (2026-11-18): how long this thread waited
                t_inImageAlloc = true;  // banjo-nx (2026-10-28): makeBlock leaves the image heap to this thread
                c.block = makeBlock(chunkSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "image heap chunk");
                // banjo-nx (2026-10-28, the console's OOM): the empty chunks back, then the image's own size.
                if (!c.block) {
                    releaseEmptyChunksLocked();
                    const uint32_t own = alignUp(size + alignment, DK_MEMBLOCK_ALIGNMENT);
                    if (own < chunkSize) {
                        chunkSize = own;
                        c.block = makeBlock(chunkSize, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "image (own block)");
                    }
                }
                t_inImageAlloc = false;
                if (!c.block)
                    return false;
                c.size = chunkSize;
                c.freeList.push_back({0, chunkSize});
                chunks.push_back(std::move(c));
                logf("image heap: chunk %zu of %u MiB, made at once in %.1f ms (the heap's lock held)", chunks.size(),
                     chunkSize >> 20, double(armTicksToNs(armGetSystemTick() - made_at)) / 1e6);
            }
        }
        return false;
    }

    void Deko3DDevice::retire(const Deko3DImageAlloc &image, DkMemBlock block) {
        std::lock_guard<std::mutex> lock(retireMutex);
        retired.push_back({submitSerial.load(std::memory_order_acquire), image, block});
    }

    void Deko3DDevice::noteSubmission() {
        // A fence per submission serial (16 in a ring): what was retired before a submission is freed once its fence
        // signalled.
        std::lock_guard<std::mutex> lock(retireMutex);
        const uint64_t serial = submitSerial.load(std::memory_order_relaxed);
        DkFence &fence = serialFences[serial % SerialFences];
        if (serialFenceValue[serial % SerialFences] != 0) {
            // The ring wrapped: that older submission must be finished before its fence is reused.
            dkFenceWait(&fence, -1);
            completedSerial.store(std::max(completedSerial.load(), serialFenceValue[serial % SerialFences]));
        }
        dkQueueSignalFence(queue, &fence, false);
        serialFenceValue[serial % SerialFences] = serial;
        submitSerial.store(serial + 1, std::memory_order_release);
    }

    void Deko3DDevice::collectRetired() {
        std::lock_guard<std::mutex> lock(retireMutex);
        if (retired.empty())
            return;
        uint64_t done = completedSerial.load();
        for (uint32_t i = 0; i < SerialFences; i++) {
            if (serialFenceValue[i] > done && dkFenceWait(&serialFences[i], 0) == DkResult_Success)
                done = std::max(done, serialFenceValue[i]);
        }
        completedSerial.store(done);
        size_t kept = 0;
        for (size_t i = 0; i < retired.size(); i++) {
            Retired &r = retired[i];
            // Retired while submission r.serial was being recorded: free once that one has completed.
            if (r.serial <= done) {
                if (r.block)
                    destroyBlock(r.block);
                if (r.image.chunk >= 0) {
                    std::lock_guard<std::mutex> heap(heapMutex);
                    Chunk &c = chunks[size_t(r.image.chunk)];
                    chunkFree(c, r.image.offset, r.image.size);
                    // sasrt-nx (2026-11-09, Deko3DSetDedicatedTargets; port 58): a target's own chunk goes back at once
                    // (its slot stays, empty, for the next one).
                    if (c.dedicated && c.block && c.freeList.size() == 1 && c.freeList[0].first == 0 &&
                        c.freeList[0].second == c.size) {
                        cacheTargetBlockLocked(c.block, c.size);  // sasrt-nx (2026-11-11): kept for the next target
                        c.block = nullptr;
                        c.size = 0;
                        c.freeList.clear();
                        c.dedicated = false;
                    }
                }
            }
            else {
                retired[kept++] = r;
            }
        }
        const bool freed_images = kept != retired.size();
        retired.resize(kept);
        // banjo-nx (2026-10-28, the console's OOM): chunks emptied by those, back to the heap while its room is short.
        if (freed_images && heapRoomShort()) {
            std::lock_guard<std::mutex> heap(heapMutex);
            releaseEmptyChunksLocked();
        }
    }

    // ---- shaders

    static Deko3DShaderUsage parseUsage(const std::string &glsl) {
        Deko3DShaderUsage usage;
        const size_t at = glsl.find("// banjo-dk ");
        if (at == std::string::npos)
            return usage;
        const size_t end = glsl.find('\n', at);
        const std::string line = glsl.substr(at, end == std::string::npos ? std::string::npos : end - at);
        auto field = [&](const char *key) -> std::string {
            const size_t k = line.find(key);
            if (k == std::string::npos)
                return {};
            const size_t v = k + strlen(key);
            const size_t e = line.find(' ', v);
            return line.substr(v, e == std::string::npos ? std::string::npos : e - v);
        };
        usage.uboMask = uint32_t(strtoul(field("ubo=").c_str(), nullptr, 16));
        usage.ssboMask = uint32_t(strtoul(field("ssbo=").c_str(), nullptr, 16));
        usage.textures = field("tex=") == "1";
        usage.txqMask = uint32_t(strtoul(field("txq=").c_str(), nullptr, 16));  // banjo-nx (2026-10-19)
        // texb=k:textureWord:samplerWord,... in binding order
        const std::string texb = field("texb=");
        size_t q = 0;
        while (q < texb.size()) {
            const size_t comma = texb.find(',', q);
            const std::string item = texb.substr(q, comma == std::string::npos ? std::string::npos : comma - q);
            unsigned k = 0, t = 0, s = 0, v = 0;  // vivapinata-nx: v, the sampler's variant (TexBinding::samplerVariant)
            if (sscanf(item.c_str(), "%u:%u:%u:%u", &k, &t, &s, &v) >= 3 && k == usage.texBindings.size())
                usage.texBindings.push_back({uint16_t(t), uint16_t(s), uint32_t(v) << 20});
            if (comma == std::string::npos)
                break;
            q = comma + 1;
        }
        const std::string vin = field("vin=");
        size_t p = 0;
        while (p < vin.size()) {
            const size_t comma = vin.find(',', p);
            usage.vertexInputs.push_back(uint32_t(strtoul(vin.substr(p, comma - p).c_str(), nullptr, 10)));
            if (comma == std::string::npos)
                break;
            p = comma + 1;
        }
        return usage;
    }

    struct SpecDefault {
        uint32_t id;
        uint32_t value;
    };

    // The specialization constants the GLSL defines: "#define SPIRV_CROSS_CONSTANT_ID_<id> <value>u".
    static std::vector<SpecDefault> parseSpecDefaults(const std::string &glsl) {
        std::vector<SpecDefault> out;
        const char *key = "#define SPIRV_CROSS_CONSTANT_ID_";
        size_t p = 0;
        while ((p = glsl.find(key, p)) != std::string::npos) {
            p += strlen(key);
            char *end = nullptr;
            const uint32_t id = uint32_t(strtoul(glsl.c_str() + p, &end, 10));
            const uint32_t value = uint32_t(strtoul(end, nullptr, 0));
            out.push_back({id, value});
        }
        std::sort(out.begin(), out.end(), [](const SpecDefault &a, const SpecDefault &b) { return a.id < b.id; });
        return out;
    }

    struct Deko3DShaderImpl : Deko3DShader {
        std::vector<SpecDefault> specDefaults;
        // Variants already looked up (spec values -> variant), so a pipeline does not hash the text again.
        std::mutex mutex;
        std::unordered_map<uint64_t, const DkShader *> known;
    };

    std::unique_ptr<RenderShader> Deko3DDevice::createShader(const void *data, uint64_t size, const char *, RenderShaderFormat format) {
        if (format != RenderShaderFormat::DEKO3D || data == nullptr || size < sizeof(Deko3DShaderBlob)) {
            logf("createShader: not a deko3d shader (format %d, %" PRIu64 " bytes)", int(format), size);
            return nullptr;
        }
        Deko3DShaderBlob header;
        memcpy(&header, data, sizeof(header));
        if (header.magic != Deko3DShaderBlob::Magic || sizeof(header) + uint64_t(header.glslSize) + header.dkshSize > size) {
            logf("createShader: bad deko3d shader blob");
            return nullptr;
        }
        auto shader = std::make_unique<Deko3DShaderImpl>();
        shader->device = this;
        shader->stage = DkStage(header.stage);
        const char *glsl = static_cast<const char *>(data) + sizeof(header);
        shader->glsl = std::make_shared<const std::string>(glsl, header.glslSize);
        if (header.dkshSize)
            shader->defaultDksh.assign(reinterpret_cast<const uint8_t *>(glsl) + header.glslSize,
                                       reinterpret_cast<const uint8_t *>(glsl) + header.glslSize + header.dkshSize);
        shader->usage = parseUsage(*shader->glsl);
        // banjo-nx (2026-10-27): a vertex shader whose text never names gl_VertexID cannot read it, in any variant (a
        // variant's defines only remove code): its draws may give their base vertex to the draw (Renut's whole-buffer
        // draws), the vertex index it does not read being the only thing that differs.
        shader->usage.readsVertexId = shader->stage != DkStage_Vertex || shader->glsl->find("gl_VertexID") != std::string::npos;
        // vivapinata-nx (2026-11-03, tools/deko3d_glsl_opt.py vskill): a vertex shader with the kill's constant 914.
        shader->usage.writesVertexKill = shader->stage == DkStage_Vertex && shader->glsl->find("SPIRV_CROSS_CONSTANT_ID_914") != std::string::npos;
        // blur-nx (2026-11-12): see Deko3DShaderUsage::writesClipDistance.
        shader->usage.writesClipDistance = (shader->stage != DkStage_Vertex && shader->stage != DkStage_Geometry) ||
                                           shader->glsl->find("gl_ClipDistance") != std::string::npos;
        // banjo-nx (2026-10-18): the varying locations it declares ("layout(location = N) in / out" lines).
        // katamari-nx (2026-11-01, rc10's black screen): an interpolation qualifier may come first ("layout(location = N)
        // centroid in", XenosRecomp's inputs the shader's header samples at the centroid); without it, those inputs were
        // left out of the mask, the vertex shader's constant 905 dropped the outputs feeding them and they read nothing.
        {
            const char *key = "\nlayout(location = ";
            size_t p = 0;
            while ((p = shader->glsl->find(key, p)) != std::string::npos) {
                p += strlen(key);
                char *end = nullptr;
                const unsigned long loc = strtoul(shader->glsl->c_str() + p, &end, 10);
                if (loc >= 32 || !end || strncmp(end, ") ", 2) != 0)
                    continue;
                const char *q = end + 2;
                for (const char *qualifier : {"centroid ", "sample ", "noperspective ", "flat "}) {
                    if (strncmp(q, qualifier, strlen(qualifier)) == 0) {
                        q += strlen(qualifier);
                        break;
                    }
                }
                if (strncmp(q, "in ", 3) == 0)
                    shader->usage.inputMask |= 1u << loc;
                else if (strncmp(q, "out ", 4) == 0)
                    shader->usage.outputMask |= 1u << loc;
            }
        }
        {
            const size_t z = shader->glsl->find("\n// banjo-vszero=0x");
            if (z != std::string::npos)
                shader->usage.zeroOutputMask = uint32_t(strtoul(shader->glsl->c_str() + z + 19, nullptr, 16));  // after "0x"
        }
        shader->specDefaults = parseSpecDefaults(*shader->glsl);
        for (const SpecDefault &d : shader->specDefaults)
            shader->specIds.push_back(d.id);
        return shader;
    }

    void Deko3DDevice::compileWorker() {
        uint64_t heapFree = 0, processFree = 0;
        memoryFigures(heapFree, processFree);
        logf("uam thread: starting (heap %" PRIu64 " MB free, process %" PRIu64 " MB unused)", heapFree >> 20, processFree >> 20);
        const uint64_t i0 = armTicksToNs(armGetSystemTick());
        uam::setBanjoFlags(g_deko3dOptions.uamFlags);  // banjo-nx (2026-10-18)
        // banjo-nx (2026-10-18): off the render thread's core and below the game's threads (package 33's first run
        // compiled 231 variants during play: a core busy for minutes, every thread slower).
        {
            std::lock_guard<std::mutex> lock(compileMutex);  // compile() reads it under the lock
            compileThreadHandle = threadGetCurHandle();
        }
        if (g_deko3dOptions.compileCore >= 0)
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_deko3dOptions.compileCore, 1u << g_deko3dOptions.compileCore);
        // banjo-nx (2026-10-23): the priority the process may give (the console's profile showed this thread at 0x3B in
        // every report: 0x3E is below an application's lowest, the game threads' 0x3B, and the kernel refused it while
        // the log said 0x3E). The asked one, or the lowest allowed above it; what the kernel took is logged.
        s32 asked = g_deko3dOptions.compilePriority, priority = -1;
        u64 allowed = 0;
        Result setResult = 0;
        if (asked > 0) {
            if (R_SUCCEEDED(svcGetInfo(&allowed, InfoType_PriorityMask, CUR_PROCESS_HANDLE, 0)) && allowed != 0) {
                s32 p = std::min<s32>(asked, 63);
                while (p > 0 && !(allowed & (1ull << p)))
                    p--;
                asked = p > 0 ? p : asked;
            }
            setResult = svcSetThreadPriority(CUR_THREAD_HANDLE, u32(asked));
        }
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            compileBasePriority = priority;
            compileAppliedPriority = priority;
        }
        logf("uam thread: core %d, priority 0x%X (0x%X asked, 0x%X given: result 0x%X, allowed mask 0x%016" PRIx64
             "; a waiting thread lends its own); background compiles use at most %d%% of the core",
             g_deko3dOptions.compileCore, uint32_t(priority), uint32_t(g_deko3dOptions.compilePriority), uint32_t(asked),
             uint32_t(setResult), allowed, g_deko3dOptions.backgroundCompileShare);
        uam::init(true);
        // Self-test (SwitchWakerHD's): a small shader now, which also builds the resident frontend's tables before the
        // game's first variant.
        static const char test[] = "#version 460\nlayout(location = 0) in vec4 pos;\nlayout(std140, binding = 0) uniform B { vec4 s; };\n"
                                   "void main() { gl_Position = pos * s; }\n";
        const uint64_t i1 = armTicksToNs(armGetSystemTick());
        uam::Result selfTest = uam::compile(uam::Stage::Vertex, test);
        const uint64_t i2 = armTicksToNs(armGetSystemTick());
        memoryFigures(heapFree, processFree);
        logf("uam thread: %s (init %.0f ms, self-test %zu bytes of DKSH in %.0f ms; heap %" PRIu64 " MB free, process %" PRIu64 " MB unused)%s%s",
             selfTest.ok ? "ready" : "SELF-TEST FAILED", double(i1 - i0) / 1e6, selfTest.dksh.size(), double(i2 - i1) / 1e6,
             heapFree >> 20, processFree >> 20, selfTest.ok ? "" : ": ", selfTest.ok ? "" : selfTest.log.c_str());
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            compilerReady = true;
        }
        for (;;) {
            CompileJob *job = nullptr;
            {
                std::unique_lock<std::mutex> lock(compileMutex);
                compileWake.wait(lock, [this] { return compileStop || !compileQueue.empty(); });
                if (compileStop)
                    break;
                job = compileQueue.front();
                compileQueue.erase(compileQueue.begin());
            }
            // banjo-nx (2026-10-23, asyncCacheReads): a variant the cache file holds, read here (the drawing thread
            // went on with the generic variant). No rest after it: it is a read, not a compile.
            if (job->async && job->cached) {
                if (g_cacheLookup && g_cacheLookup(job->request.key, job->ownDksh)) {
                    g_deko3dStats.shaderCacheHits++;
                    g_deko3dStats.variantCacheReadsBackground++;
                    job->ok = true;
                    finishAsync(job);
                    delete job;
                    continue;
                }
                job->ownDksh.clear();  // gone from the file (a failed write): compiled as any other
            }
            // banjo-nx (2026-10-21): a background job's text, made now from the shared one (one at a time).
            if (job->async) {
                const std::string &glsl = *job->request.glsl;
                const size_t nl = job->request.defines.empty() ? std::string::npos : glsl.find('\n');
                if (nl == std::string::npos) {
                    job->ownSource = glsl;
                }
                else {
                    job->ownSource.reserve(glsl.size() + job->request.defines.size());
                    job->ownSource.append(glsl, 0, nl + 1);
                    job->ownSource += job->request.defines;
                    job->ownSource.append(glsl, nl + 1, std::string::npos);
                }
            }
            static const uam::Stage stages[] = {uam::Stage::Vertex, uam::Stage::TessCtrl, uam::Stage::TessEval,
                                                uam::Stage::Geometry, uam::Stage::Fragment, uam::Stage::Compute};
            const uint64_t t0 = armTicksToNs(armGetSystemTick());
            {
                static uint32_t logged = 0;
                if (logged < 4) {
                    logged++;
                    uint64_t hf = 0, pf = 0;
                    memoryFiguresQuick(hf, pf);  // port 161: no heap walk
                    logf("uam: compile %u (stage %u, %zu bytes of GLSL; heap %" PRIu64 " MB free, process %" PRIu64 " MB unused)",
                         logged, uint32_t(job->stage), job->source->size(), hf >> 20, pf >> 20);
                }
                // banjo-nx (2026-10-20): every run-time compile's specialization (its constants), so the variants
                // a cache lacks can be found (tools/deko3d_variant_cache.py rebuilds their keys from these).
                // 2026-10-23: its kind ahead of "run-time variant" (tools/deko3d_variant_cache.py --runtime-log
                // searches from there); every compile a draw waits for is named (up to 512), the others the first 64.
                static uint32_t named = 0, namedBlocking = 0;
                const bool drawBlocked = !job->async && job->drawWaits;
                if (drawBlocked ? namedBlocking < 512 : named < 64) {
                    (drawBlocked ? namedBlocking : named)++;
                    // The variant's own defines: the lines plume inserted after the first one (2026-10-20: only those;
                    // the shader's defaults further down are not logged). And which shader: the text without them,
                    // its size and FNV-1a 64, which tools/deko3d_variant_cache.py --runtime-log matches to a GLSL file
                    // (so a console log alone completes the next cache).
                    std::string defines;
                    const std::string &src = *job->source;
                    const char *key = "#define SPIRV_CROSS_CONSTANT_ID_";
                    size_t body = src.find('\n');
                    body = body == std::string::npos ? src.size() : body + 1;
                    size_t rest = body;
                    while (src.compare(rest, strlen(key), key) == 0) {
                        const size_t e = src.find('\n', rest);
                        const size_t next = e == std::string::npos ? src.size() : e + 1;
                        if (defines.size() < 900) {
                            defines += src.substr(rest + strlen(key), (e == std::string::npos ? src.size() : e) - rest - strlen(key));
                            defines += ';';
                        }
                        rest = next;
                    }
                    uint64_t base = 0xcbf29ce484222325ull;
                    auto mix = [&](size_t from, size_t to) {
                        for (size_t i = from; i < to; ++i)
                            base = (base ^ uint8_t(src[i])) * 0x100000001b3ull;
                    };
                    mix(0, body);
                    mix(rest, src.size());
                    logf("uam: %s run-time variant (stage %u, %zu bytes, base %zu bytes %016" PRIx64 "): %s",
                         job->async ? "background" : job->drawWaits ? "BLOCKING (a draw waits)" : "blocking (prebuild waits)",
                         uint32_t(job->stage), src.size(), body + (src.size() - rest), base, defines.c_str());
                }
            }
            // port 56 (mk9-nx, fastFirstCompile): one pass for a compile a drawing thread waits for.
            const bool fast = !job->async && job->drawWaits && g_deko3dOptions.fastFirstCompile;
            uam::Result r = fast ? uam::compileFast(stages[std::min<uint32_t>(job->stage, 5)], job->source->c_str())
                                 : uam::compile(stages[std::min<uint32_t>(job->stage, 5)], job->source->c_str());
            if (fast)
                g_deko3dStats.fastCompiles++;
            g_deko3dStats.shaderCompileNs += armTicksToNs(armGetSystemTick()) - t0;
            g_deko3dStats.shaderCompiles++;
            if (job->async) {
                job->ownSource.clear();
                job->ownSource.shrink_to_fit();
                job->ok = r.ok;
                job->ownDksh = std::move(r.dksh);
                job->ownLog = std::move(r.log);
                finishAsync(job);
                delete job;
                // banjo-nx (2026-10-23): at most backgroundCompileShare % of the core for background compiles (the
                // thread cannot go below the game's threads, see compileWorker): a rest in proportion to the compile,
                // at most 30 s (2026-10-28: was 8 s, which held the share at 20 % or more for 2 s compiles), ended by a
                // compile a thread waits for (those go ahead of the background ones).
                const int share = g_deko3dOptions.backgroundCompileShare;
                if (share > 0 && share < 100) {
                    const uint64_t t1 = armTicksToNs(armGetSystemTick());
                    const uint64_t rest = std::min<uint64_t>((t1 - t0) * uint64_t(100 - share) / uint64_t(share),
                                                             30000000000ull);
                    std::unique_lock<std::mutex> lock(compileMutex);
                    compileWake.wait_for(lock, std::chrono::nanoseconds(rest), [this] {
                        return compileStop || (!compileQueue.empty() && !compileQueue.front()->async);
                    });
                    g_deko3dStats.backgroundRestNs += armTicksToNs(armGetSystemTick()) - t1;
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(compileMutex);
                job->ok = r.ok;
                *job->dksh = std::move(r.dksh);
                *job->log = std::move(r.log);
                job->done = true;
            }
            compileDone.notify_all();
        }
        uam::shutdown();
        std::lock_guard<std::mutex> lock(compileMutex);
        compilerReady = false;
        compileDone.notify_all();
    }

    // banjo-nx (2026-10-21): set on threads that do not draw (Deko3DSetBackgroundThread): the pipeline prebuild.
    static thread_local bool t_backgroundThread = false;
    // banjo-nx (2026-11-01, Deko3DSetUrgentVariants): the pipeline this thread builds draws much slower with the generic
    // variant (banjo_gpu_fetch_spec): its variants compiled now, on this thread (a compile it waits for: ahead of the
    // background ones), not queued in the background.
    static thread_local bool t_urgentVariants = false;

    // banjo-nx (2026-10-23, the user's request, Deko3DOptions::compileWaitBoost): while a drawing thread waits for a
    // compile (the game is stopped), the console in boost mode: CPU 1785 MHz (the system drops the GPU to 76.8 MHz,
    // which has no new work while the frame waits), back to normal the moment the last such compile is done. Compiles
    // in the background, and those a background thread waits for, never boost. A refusal (an applet, not an
    // application) is logged once and not tried again.
    static void boostBegin() {
        std::lock_guard<std::mutex> lock(g_boostMutex);
        if (g_boostWaiters++ > 0 || g_boostRefused)
            return;
        const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
        static bool logged = false;
        if (!logged || R_FAILED(rc)) {
            logged = true;
            logf("boost mode for a draw waiting on a compile: appletSetCpuBoostMode(FastLoad) -> 0x%X%s", uint32_t(rc),
                 R_FAILED(rc) ? " (refused: not tried again)" : "");
        }
        if (R_FAILED(rc)) {
            g_boostRefused = true;
            return;
        }
        g_boostActive = true;
        g_boostSince = armTicksToNs(armGetSystemTick());
        g_deko3dStats.compileBoosts++;
    }

    static void boostEnd() {
        std::lock_guard<std::mutex> lock(g_boostMutex);
        if (--g_boostWaiters > 0 || !g_boostActive)
            return;
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        g_boostActive = false;
        g_deko3dStats.compileBoostNs += armTicksToNs(armGetSystemTick()) - g_boostSince;
    }

    bool Deko3DDevice::compile(DkStage stage, const std::string &source, std::vector<uint8_t> &dksh, std::string &log) {
        CompileJob job;
        job.stage = stage;
        job.source = &source;
        job.dksh = &dksh;
        job.log = &log;
        // banjo-nx (2026-10-23): a blocking compile: a drawing thread (the game stops) or a background one (prebuild).
        job.drawWaits = !t_backgroundThread;
        // banjo-nx (2026-10-18): the waiting thread's priority, lent to the uam thread while it waits (the render
        // thread must not wait behind the game's threads for a low-priority compile).
        s32 waiterPriority = 0;
        bool lend = g_deko3dOptions.compilePriority > 0 && R_SUCCEEDED(svcGetThreadPriority(&waiterPriority, CUR_THREAD_HANDLE));
        const uint64_t waitStart = armTicksToNs(armGetSystemTick());
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            // banjo-nx (2026-10-21): ahead of the background compiles (a draw waits for this one).
            auto at = std::find_if(compileQueue.begin(), compileQueue.end(), [](const CompileJob *j) { return j->async; });
            compileQueue.insert(at, &job);
            // 2026-10-23: against the priority the uam thread has (the one asked may have been refused).
            lend = lend && compileThreadHandle != 0 && waiterPriority < compileBasePriority;
            if (lend) {
                compileWaiters.push_back(waiterPriority);
                applyCompilePriority();
            }
        }
        compileWake.notify_one();
        const bool boost = job.drawWaits && g_deko3dOptions.compileWaitBoost;
        if (boost)
            boostBegin();
        std::unique_lock<std::mutex> lock(compileMutex);
        compileDone.wait(lock, [&] { return job.done; });
        if (lend) {
            compileWaiters.erase(std::find(compileWaiters.begin(), compileWaiters.end(), waiterPriority));
            applyCompilePriority();
        }
        lock.unlock();
        if (boost)
            boostEnd();
        if (job.drawWaits) {
            g_deko3dStats.compileWaits++;
            g_deko3dStats.compileWaitNs += armTicksToNs(armGetSystemTick()) - waitStart;
        }
        else {
            g_deko3dStats.compileBackgroundWaits++;
        }
        return job.ok;
    }

    // banjo-nx (2026-10-21, asyncVariants): a background compile done (uam thread): into the cache file and the device's
    // variants (an invalid one when it failed: its draws keep the generic variant), then the epoch that provisional
    // pipelines look at. No shader object: the next loadVariant of the variant finds it in 'variants'.
    void Deko3DDevice::finishAsync(CompileJob *job) {
        auto fresh = std::make_unique<Deko3DShaderVariant>();
        const uint64_t key = job->request.key;
        if (job->ok) {
            if (g_cacheStore)
                g_cacheStore(key, job->ownDksh);
            fresh->valid = loadCode(job->ownDksh, fresh->shader, job->request.name.c_str(), &fresh->vsInputs);
        }
        else {
            g_deko3dStats.shaderCompileFailures++;
            static std::atomic<uint32_t> logged{0};
            if (logged.fetch_add(1) < 32)
                logf("uam failed for %s (variant %016" PRIx64 ", in the background: the generic variant stays): %s",
                     job->request.name.empty() ? "a shader" : job->request.name.c_str(), key, job->ownLog.c_str());
        }
        {
            std::lock_guard<std::mutex> lock(variantMutex);
            auto &slot = variants[key];
            if (!slot)
                slot = std::move(fresh);
        }
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            asyncQueued.erase(key);
            if (job->background && !job->cached)
                asyncBackgroundQueued--;
        }
        g_deko3dStats.variantBackground++;
        asyncEpoch.fetch_add(1, std::memory_order_release);
    }

    bool Deko3DDevice::refreshPipeline(Deko3DGraphicsPipeline *p, bool &changed) {
        changed = false;
        const uint64_t epoch = asyncEpoch.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> refresh(refreshMutex);
        if (!p->provisional.load(std::memory_order_relaxed))
            return false;
        if (epoch == p->pendingEpoch)
            return true;
        p->pendingEpoch = epoch;
        bool waiting = false;
        for (int i = 0; i < 3; i++) {
            Deko3DVariantRequest &req = p->pending[i];
            const uint64_t key = req.key;
            if (key == 0)
                continue;
            {
                std::lock_guard<std::mutex> lock(compileMutex);
                if (asyncQueued.count(key)) {
                    waiting = true;
                    continue;
                }
            }
            // Done: its variant (invalid when it failed: the generic one stays). Neither queued nor done: a background
            // thread's request the queue had no room for, queued now as a draw's.
            const DkShader *s = nullptr;
            bool done = false;
            {
                std::lock_guard<std::mutex> lock(variantMutex);
                auto it = variants.find(key);
                if (it != variants.end() && it->second) {
                    done = true;
                    if (it->second->valid)
                        s = &it->second->shader;
                }
            }
            if (!done) {
                // banjo-nx (2026-10-23, batch 5's console run): as a read when the cache file holds it. Queued as a
                // compile (as before asyncCacheReads), thousands of the prebuild's variants waited behind compiles and
                // their rests, and their pipelines drew with the generic variants all session (GPU +6 ms in the city).
                queueAsync(req, false, g_deko3dOptions.asyncCacheReads && g_cacheContains && g_cacheContains(key));
                waiting = true;
                continue;
            }
            const DkShader *old = p->stageShaders[i];
            if (s != nullptr && s != old) {
                // Each pointer at once (a recording thread may bind the pipeline now): any mix of generic and own
                // stages draws the same pixels.
                for (uint32_t k = 0; k < p->shaderCount; k++) {
                    if (p->shaders[k] == old) {
                        __atomic_store_n(&p->shaders[k], s, __ATOMIC_RELEASE);
                        break;
                    }
                }
                __atomic_store_n(&p->stageShaders[i], s, __ATOMIC_RELEASE);
                g_deko3dStats.variantUpgrades++;
                changed = true;
            }
            req = Deko3DVariantRequest();
        }
        if (!waiting) {
            p->provisional.store(false, std::memory_order_release);
            g_deko3dProvisionalPipelines.fetch_sub(1, std::memory_order_relaxed);  // banjo-nx (2026-10-28)
        }
        return waiting;
    }

    bool Deko3DShaderWritesVertexKill(const RenderShader *shader) {
        return shader != nullptr && static_cast<const Deko3DShader *>(shader)->usage.writesVertexKill;
    }

    bool Deko3DShaderReadsVertexId(const RenderShader *shader) {
        return shader == nullptr || static_cast<const Deko3DShader *>(shader)->usage.readsVertexId;
    }

    // banjo-nx (2026-10-28, render-thread round): see plume_deko3d.h.
    std::atomic<int> g_deko3dProvisionalPipelines{0};
    bool g_deko3dProvisionalCount = true;
    void Deko3DSetProvisionalCount(bool on) { g_deko3dProvisionalCount = on; }

    Deko3DGraphicsPipeline::~Deko3DGraphicsPipeline() {
        if (provisional.load(std::memory_order_relaxed))
            g_deko3dProvisionalPipelines.fetch_sub(1, std::memory_order_relaxed);
    }

    bool Deko3DRefreshPipelineSlow(RenderPipeline *pipeline) {
        auto *p = static_cast<Deko3DGraphicsPipeline *>(pipeline);
        bool changed = false;
        if (p->device)
            p->device->refreshPipeline(p, changed);
        return changed;
    }

    // compileMutex held: the uam thread at its own priority, or the highest a waiting thread has.
    void Deko3DDevice::applyCompilePriority() {
        s32 priority = compileBasePriority > 0 ? compileBasePriority : g_deko3dOptions.compilePriority;
        for (s32 p : compileWaiters)
            priority = std::min(priority, p);
        if (priority != compileAppliedPriority && compileThreadHandle != 0 &&
            R_SUCCEEDED(svcSetThreadPriority(compileThreadHandle, u32(priority))))
            compileAppliedPriority = priority;
    }

    // Shader code: 16 MB blocks, bump allocated (deko3d maps every code block into its one code segment).
    static constexpr uint32_t CodeBlockSize = 16u << 20;

    bool Deko3DDevice::loadCode(const std::vector<uint8_t> &dksh, DkShader &shader, const char *what, uint32_t *vsInputs) {
        if (dksh.size() < 64)
            return false;
        // sasrt-nx (2026-11-08, Deko3DOptions::vsInputMask): a vertex program's input map. The DKSH header has control_sz
        // at byte 8 and programs_off at 16; the program header its type (0 vertex) and entrypoint; the program starts with
        // its shader program header (SPH type 1, shader type 1 for a vertex shader), whose words 6-9 map the 32 generic
        // attributes, 4 bits (x y z w) each: an attribute with none is never read.
        if (vsInputs) {
            *vsInputs = ~0u;
            uint32_t controlSz = 0, programsOff = 0, type = 0, entry = 0;
            memcpy(&controlSz, dksh.data() + 8, 4);
            memcpy(&programsOff, dksh.data() + 16, 4);
            if (uint64_t(programsOff) + 8 <= dksh.size()) {
                memcpy(&type, dksh.data() + programsOff, 4);
                memcpy(&entry, dksh.data() + programsOff + 4, 4);
                const uint64_t sph = uint64_t(controlSz) + entry;
                uint32_t w[10];
                if (type == 0 && sph + sizeof(w) <= dksh.size()) {
                    memcpy(w, dksh.data() + sph, sizeof(w));
                    if ((w[0] & 0x1F) == 1 && ((w[0] >> 10) & 0xF) == 1) {
                        uint32_t used = 0;
                        for (uint32_t a = 0; a < 32; a++)
                            used |= uint32_t(((w[6 + a / 8] >> ((a % 8) * 4)) & 0xF) != 0) << a;
                        *vsInputs = used;
                    }
                }
            }
        }
        {
            // The DKSH header (uam's dksh.h): programs_off at byte 16; the program's per_warp_scratch_sz at byte 20.
            uint32_t programsOff = 0, scratch = 0;
            memcpy(&programsOff, dksh.data() + 16, 4);
            if (uint64_t(programsOff) + 24 <= dksh.size())
                memcpy(&scratch, dksh.data() + programsOff + 20, 4);
            if (scratch > perWarpScratch) {
                logf("shader %s needs %u bytes of scratch a warp, the queue has %u: not loaded", what, scratch, perWarpScratch);
                return false;
            }
        }
        std::lock_guard<std::mutex> lock(codeMutex);
        const uint32_t size = uint32_t(dksh.size());
        if (codeBlocks.empty() || alignUp(codeBlocks.back().second, DK_SHADER_CODE_ALIGNMENT) + size > CodeBlockSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
            if (size > CodeBlockSize - DK_SHADER_CODE_UNUSABLE_SIZE) {
                logf("shader %s: %u bytes of DKSH do not fit a code block", what, size);
                return false;
            }
            DkMemBlock block = makeBlock(CodeBlockSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, "shader code");
            if (!block)
                return false;
            codeBlocks.push_back({block, 0});
            logf("shader code: block %zu (16 MiB)", codeBlocks.size());
        }
        auto &cb = codeBlocks.back();
        const uint32_t at = alignUp(cb.second, DK_SHADER_CODE_ALIGNMENT);
        memcpy(static_cast<uint8_t *>(dkMemBlockGetCpuAddr(cb.first)) + at, dksh.data(), size);
        // sasrt-nx (2026-11-09, Deko3DOptions::expSphStoreBits): an EXPERIMENT on a vertex program's header bits
        if (g_deko3dOptions.expSphStoreBits) {
            uint32_t controlSz = 0, programsOff = 0, type = 1, entry = 0, w0 = 0;
            memcpy(&controlSz, dksh.data() + 8, 4);
            memcpy(&programsOff, dksh.data() + 16, 4);
            if (uint64_t(programsOff) + 8 <= size) {
                memcpy(&type, dksh.data() + programsOff, 4);
                memcpy(&entry, dksh.data() + programsOff + 4, 4);
            }
            const uint64_t sph = uint64_t(controlSz) + entry;
            if (type == 0 && sph + 4 <= size) {
                uint8_t *word = static_cast<uint8_t *>(dkMemBlockGetCpuAddr(cb.first)) + at + sph;
                memcpy(&w0, word, 4);
                if ((w0 & 0x1F) == 1 && ((w0 >> 10) & 0xF) == 1 && (w0 & (1u << 16))) {
                    const uint32_t before = w0;
                    w0 &= ~(1u << 16);
                    if (g_deko3dOptions.expSphStoreBits & 2u)
                        w0 &= ~(1u << 26);
                    memcpy(word, &w0, 4);
                    static uint32_t logged = 0;
                    if (logged++ < 8)
                        logf("[sasrt] EXPERIMENT: vertex program %s header word 0 %08X -> %08X (sasrt_gpu_exp_sph_no_global_store)",
                             what, before, w0);
                }
            }
        }
        DkShaderMaker m;
        dkShaderMakerDefaults(&m, cb.first, at);
        dkShaderInitialize(&shader, &m);
        if (!dkShaderIsValid(&shader)) {
            logf("shader %s: %u bytes of DKSH are not valid", what, size);
            return false;
        }
        cb.second = at + size;
        g_deko3dStats.codeBytes += size;
        // Command lists invalidate the GPU's shader cache before their next draw (deko3d does it only at a queue
        // flush).
        codeEpoch.fetch_add(1, std::memory_order_release);
        return true;
    }

    // banjo-nx (2026-10-21, asyncVariants): the specializations a generic variant drops. Each only folds what the shader
    // reads at run time when its constant has the default (tools/deko3d_glsl_opt.py): the game's bools (1000-1255, read
    // from SharedConstants.booleans), blend skip (900: no discard), output trim (901: every channel written, the write
    // mask drops them), the alpha function (903: SharedConstants.alpha_test_func), the tessellation kind (904), the
    // vertex shader's outputs (905: all written) and the pixel shader's zero inputs (906: read, zero), early depth (909:
    // after the shader). The configuration's (907, 908, 910-913) stay: skin-once (913) writes rows other draws read.
    static bool genericDrops(uint32_t id) {
        // banjo-nx (2026-10-31): 920-923, the tessellated draws' fetch types (banjo_gpu_fetch_spec): the generic variant
        // reads them at run time, the same values.
        // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec): 929, the user clip distance (written by default; the draws that set
        // the clip slot off ignore it).
        return (id >= 1000 && id <= 1255) || id == 900 || id == 901 || id == 903 || id == 904 || id == 905 || id == 906 ||
               id == 909 || (id >= 920 && id <= 923) || id == 929;
    }

    void Deko3DSetBackgroundThread(bool background) { t_backgroundThread = background; }
    void Deko3DSetUrgentVariants(bool urgent) { t_urgentVariants = urgent; }

    // At most this many background threads' requests wait at a time (each a few hundred bytes, but the uam thread does
    // about one a second on the console: a pipeline-list prebuild over a cache that lacks them would queue thousands).
    static constexpr uint32_t kAsyncBackgroundCap = 256;

    bool Deko3DDevice::queueAsync(const Deko3DVariantRequest &request, bool background, bool cached) {
        {
            std::lock_guard<std::mutex> lock(compileMutex);
            if (asyncQueued.count(request.key))
                return true;
            // The cap bounds the background compiles' text copies; a read carries no text (2026-10-23).
            if (background && !cached && asyncBackgroundQueued >= kAsyncBackgroundCap)
                return false;
            asyncQueued.insert(request.key);
            auto *job = new CompileJob();
            job->async = true;
            job->cached = cached;
            job->background = background;
            job->stage = request.stage;
            job->request = request;
            job->source = &job->ownSource;
            job->dksh = &job->ownDksh;
            job->log = &job->ownLog;
            if (cached) {
                // banjo-nx (2026-10-23): a read, milliseconds: ahead of every compile (after the waited-for ones and
                // the reads before it), so a compile of a second does not hold it back.
                // Not counted against the background cap (that bounds the compiles' text copies).
                auto at = std::find_if(compileQueue.begin(), compileQueue.end(),
                                       [](const CompileJob *j) { return j->async && !j->cached; });
                compileQueue.insert(at, job);
            }
            else if (background) {
                asyncBackgroundQueued++;
                compileQueue.push_back(job);
            }
            else {
                // a draw's: ahead of the background threads' ones (after the waited-for and the other draws' ones)
                auto at = std::find_if(compileQueue.begin(), compileQueue.end(),
                                       [](const CompileJob *j) { return j->async && j->background; });
                compileQueue.insert(at, job);
            }
        }
        compileWake.notify_one();
        return true;
    }

    const DkShader *Deko3DDevice::loadVariant(const Deko3DShader *base, const RenderSpecConstant *specConstants, uint32_t specConstantsCount,
                                              Deko3DVariantRequest *pending, bool noCompile) {
        if (pending)
            pending->key = 0;
        uint64_t *pendingKey = pending ? &pending->key : nullptr;
        Deko3DShaderImpl *shader = const_cast<Deko3DShaderImpl *>(static_cast<const Deko3DShaderImpl *>(base));
        // The constants this shader has whose value differs from its default, by id.
        std::string defines;
        uint64_t specKey = 0xcbf29ce484222325ull;
        for (const SpecDefault &d : shader->specDefaults) {
            for (uint32_t i = 0; i < specConstantsCount; i++) {
                if (specConstants[i].index == d.id && specConstants[i].value != d.value) {
                    char line[64];
                    snprintf(line, sizeof(line), "#define SPIRV_CROSS_CONSTANT_ID_%u %uu\n", d.id, specConstants[i].value);
                    defines += line;
                    specKey = mixHash(specKey, &d.id, sizeof(d.id));
                    specKey = mixHash(specKey, &specConstants[i].value, sizeof(uint32_t));
                    break;
                }
            }
        }
        // banjo-nx (2026-10-21): with pendingKey, the generic variant instead (the constants genericDrops drops left
        // out), when it is another one: for a variant that is compiling in the background, or that failed.
        bool droppable = false;
        if (pendingKey) {
            for (const SpecDefault &d : shader->specDefaults) {
                for (uint32_t i = 0; i < specConstantsCount && !droppable; i++)
                    droppable = specConstants[i].index == d.id && specConstants[i].value != d.value && genericDrops(d.id);
            }
        }
        auto generic = [&]() -> const DkShader * {
            std::vector<RenderSpecConstant> kept;
            for (uint32_t i = 0; i < specConstantsCount; i++) {
                if (!genericDrops(specConstants[i].index))
                    kept.push_back(specConstants[i]);
            }
            g_deko3dStats.variantFallbacks++;
            return loadVariant(base, kept.data(), uint32_t(kept.size()), nullptr);
        };
        bool failed = false;
        {
            std::lock_guard<std::mutex> lock(shader->mutex);
            auto it = shader->known.find(specKey);
            if (it != shader->known.end()) {
                if (it->second || !droppable)
                    return it->second;
                failed = true;  // failed (in the background): the generic variant from now on
            }
        }
        if (failed)
            return generic();
        // The variant's text: the GLSL with the constants defined after its first line (#version).
        std::string source;
        const std::string &glsl = *shader->glsl;
        const size_t nl = glsl.find('\n');
        if (defines.empty() || nl == std::string::npos) {
            source = glsl;
        }
        else {
            source.reserve(glsl.size() + defines.size());
            source.append(glsl, 0, nl + 1);
            source += defines;
            source.append(glsl, nl + 1, std::string::npos);
        }
        const uint64_t key = fnv1a64(source.data(), source.size());
        Deko3DShaderVariant *variant = nullptr;
        {
            std::lock_guard<std::mutex> lock(variantMutex);
            auto it = variants.find(key);
            if (it != variants.end())
                variant = it->second.get();
        }
        if (variant == nullptr) {
            std::vector<uint8_t> dksh;
            bool have = false;
            if (defines.empty() && !shader->defaultDksh.empty()) {
                dksh = shader->defaultDksh;
                have = true;
            }
            // banjo-nx (2026-10-23, asyncCacheReads): in the cache file, read in the background; the generic variant
            // until then, but only when that one is at hand (loaded, or in the cache file itself): one that had to
            // compile here would stop the draw for the compile, where reading this variant now costs one card read
            // (the first batch-5 emulator run: a missing generic variant compiled on the render thread for minutes).
            if (!have && droppable && !t_urgentVariants && g_deko3dOptions.asyncVariants && g_deko3dOptions.asyncCacheReads && g_cacheContains &&
                g_cacheContains(key)) {
                std::vector<RenderSpecConstant> kept;
                for (uint32_t i = 0; i < specConstantsCount; i++) {
                    if (!genericDrops(specConstants[i].index))
                        kept.push_back(specConstants[i]);
                }
                if (const DkShader *fallback = loadVariant(base, kept.data(), uint32_t(kept.size()), nullptr, true)) {
                    pending->key = key;
                    pending->glsl = shader->glsl;
                    pending->defines = defines;
                    pending->stage = shader->stage;
                    pending->name = shader->name;
                    queueAsync(*pending, t_backgroundThread, true);
                    g_deko3dStats.variantFallbacks++;
                    return fallback;
                }
            }
            if (!have && g_cacheLookup && g_cacheLookup(key, dksh)) {
                g_deko3dStats.shaderCacheHits++;
                have = true;
            }
            // banjo-nx (2026-10-21, asyncVariants): compiled on the uam thread in the background (once), the generic
            // variant until then.
            // banjo-nx (2026-11-01): not for an urgent one (t_urgentVariants): compiled below, on this thread.
            if (!have && droppable && !t_urgentVariants && g_deko3dOptions.asyncVariants) {
                pending->key = key;
                pending->glsl = shader->glsl;
                pending->defines = defines;
                pending->stage = shader->stage;
                pending->name = shader->name;
                queueAsync(*pending, t_backgroundThread);  // no room: the draw that needs it queues it (refresh)
                return generic();
            }
            if (!have && noCompile)
                return nullptr;  // banjo-nx (2026-10-23): the caller does without it
            if (!have) {
                std::string log;
                if (!compile(shader->stage, source, dksh, log)) {
                    g_deko3dStats.shaderCompileFailures++;
                    static std::atomic<uint32_t> logged{0};
                    if (logged.fetch_add(1) < 32)
                        logf("uam failed for %s (variant %016" PRIx64 "): %s", shader->name.empty() ? "a shader" : shader->name.c_str(), key, log.c_str());
                }
                else {
                    have = true;
                    if (g_cacheStore)
                        g_cacheStore(key, dksh);
                }
            }
            auto fresh = std::make_unique<Deko3DShaderVariant>();
            if (have)
                fresh->valid = loadCode(dksh, fresh->shader, shader->name.c_str(), &fresh->vsInputs);
            std::lock_guard<std::mutex> lock(variantMutex);
            auto &slot = variants[key];
            if (!slot)
                slot = std::move(fresh);
            variant = slot.get();
        }
        const DkShader *out = variant->valid ? &variant->shader : nullptr;
        {
            std::lock_guard<std::mutex> lock(shader->mutex);
            shader->known[specKey] = out;
        }
        // banjo-nx (2026-10-21): a failed variant (in the background or not): its draws take the generic one.
        if (out == nullptr && droppable)
            return generic();
        return out;
    }

    // ---- samplers

    std::unique_ptr<RenderSampler> Deko3DDevice::createSampler(const RenderSamplerDesc &desc) {
        auto sampler = std::make_unique<Deko3DSampler>();
        sampler->desc = desc;
        DkSampler s;
        dkSamplerDefaults(&s);
        s.minFilter = desc.minFilter == RenderFilter::NEAREST ? DkFilter_Nearest : DkFilter_Linear;
        s.magFilter = desc.magFilter == RenderFilter::NEAREST ? DkFilter_Nearest : DkFilter_Linear;
        s.mipFilter = desc.mipmapMode == RenderMipmapMode::NEAREST ? DkMipFilter_Nearest : DkMipFilter_Linear;
        s.wrapMode[0] = toDk(desc.addressU);
        s.wrapMode[1] = toDk(desc.addressV);
        s.wrapMode[2] = toDk(desc.addressW);
        s.lodClampMin = desc.minLOD;
        s.lodClampMax = std::min(desc.maxLOD, 1000.0f);
        s.lodBias = desc.mipLODBias;
        s.compareEnable = desc.comparisonEnabled;
        s.compareOp = toDk(desc.comparisonFunc);
        const float border = desc.borderColor == RenderBorderColor::OPAQUE_WHITE ? 1.0f : 0.0f;
        const float borderAlpha = desc.borderColor == RenderBorderColor::TRANSPARENT_BLACK ? 0.0f : 1.0f;
        s.borderColor[0].value_f = border;
        s.borderColor[1].value_f = border;
        s.borderColor[2].value_f = border;
        s.borderColor[3].value_f = borderAlpha;
        s.maxAnisotropy = desc.anisotropyEnabled ? float(std::max<uint32_t>(desc.maxAnisotropy, 1)) : 1.0f;
        dkSamplerDescriptorInitialize(&sampler->descriptor, &s);
        return sampler;
    }

    // ---- pipeline layouts

    std::unique_ptr<RenderPipelineLayout> Deko3DDevice::createPipelineLayout(const RenderPipelineLayoutDesc &desc) {
        auto layout = std::make_unique<Deko3DPipelineLayout>();
        // Constant buffers: the k-th one of the layout (set order, binding order) is UBO binding k; storage buffers
        // likewise (an array takes one binding per element). tools/deko3d_glsl.py numbers the shaders' the same way.
        uint32_t ubo = 0, ssbo = 0;
        for (uint32_t s = 0; s < desc.descriptorSetDescsCount; s++) {
            const RenderDescriptorSetDesc &sd = desc.descriptorSetDescs[s];
            Deko3DPipelineLayout::Set set;
            uint32_t index = 0;
            for (uint32_t r = 0; r < sd.descriptorRangesCount; r++) {
                const RenderDescriptorRange &dr = sd.descriptorRanges[r];
                Deko3DPipelineLayout::Range range;
                range.type = dr.type;
                range.binding = dr.binding;
                range.count = dr.count;
                if (sd.lastRangeIsBoundless && r + 1 == sd.descriptorRangesCount)
                    range.count = std::max<uint32_t>(range.count, sd.boundlessRangeSize);
                range.descriptorIndex = index;
                switch (dr.type) {
                case RenderDescriptorRangeType::CONSTANT_BUFFER:
                case RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC:
                    range.slot = ubo;
                    ubo += range.count;
                    break;
                case RenderDescriptorRangeType::STRUCTURED_BUFFER:
                case RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER:
                case RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER:
                case RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER:
                case RenderDescriptorRangeType::FORMATTED_BUFFER:
                case RenderDescriptorRangeType::READ_WRITE_FORMATTED_BUFFER:
                    range.slot = ssbo;
                    ssbo += range.count;
                    break;
                default:
                    break;
                }
                index += range.count;
                set.ranges.push_back(range);
            }
            layout->sets.push_back(std::move(set));
        }
        if (ubo > 5)
            logf("pipeline layout with %u constant buffers: the push constants' UBO binding 5 overlaps them", ubo);
        for (uint32_t i = 0; i < desc.pushConstantRangesCount; i++) {
            layout->pushRanges.push_back(desc.pushConstantRanges[i]);
            layout->pushSize = std::max(layout->pushSize, desc.pushConstantRanges[i].offset + desc.pushConstantRanges[i].size);
        }
        if (layout->pushSize > PushSize)
            logf("push constants of %u bytes (more than %u)", layout->pushSize, PushSize);
        return layout;
    }

    // ---- pipelines

    static DkPrimitive toDk(RenderPrimitiveTopology t) {
        switch (t) {
        case RenderPrimitiveTopology::POINT_LIST: return DkPrimitive_Points;
        case RenderPrimitiveTopology::LINE_LIST: return DkPrimitive_Lines;
        case RenderPrimitiveTopology::LINE_STRIP: return DkPrimitive_LineStrip;
        case RenderPrimitiveTopology::TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
        case RenderPrimitiveTopology::TRIANGLE_FAN: return DkPrimitive_TriangleFan;
        case RenderPrimitiveTopology::TRIANGLE_LIST:
        default: return DkPrimitive_Triangles;
        }
    }

    // deko3d 0.5.0 writes the colour destination factor into FuncAlphaDst (SwitchWakerHD's deko3d patch 1, Tenkaichi-3-NX's
    // deko_blend_state.h): the target's IndependentBlend registers (method 0x781 + 8 * target, six of them) are
    // written again with the right alpha factor after dkCmdBufBindBlendStates.
    static uint32_t blendFactorWord(DkBlendFactor f) { return (uint32_t(f) & 0x1Fu) | ((uint32_t(f) & 0x20u) ? 0xC000u : 0u); }

    static void blendWords(uint32_t words[7], uint32_t target, const DkBlendState &s) {
        words[0] = (1u << 29) | (6u << 16) | (0u << 13) | (0x781u + 8u * target); // increasing, 6 words, 3D subchannel
        words[1] = s.colorBlendOp;
        words[2] = blendFactorWord(s.srcColorBlendFactor);
        words[3] = blendFactorWord(s.dstColorBlendFactor);
        words[4] = s.alphaBlendOp;
        words[5] = blendFactorWord(s.srcAlphaBlendFactor);
        words[6] = blendFactorWord(s.dstAlphaBlendFactor);
    }

    std::unique_ptr<RenderPipeline> Deko3DDevice::createGraphicsPipeline(const RenderGraphicsPipelineDesc &desc) {
        auto p = std::make_unique<Deko3DGraphicsPipeline>();
        p->type = Deko3DPipeline::Type::Graphics;
        p->device = this;
        p->layout = static_cast<const Deko3DPipelineLayout *>(desc.pipelineLayout);
        // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec): constant 929 = 0 for a vertex shader that has it (pass clipspec).
        for (uint32_t i = 0; i < desc.specConstantsCount; i++) {
            const Deko3DShader *vs = static_cast<const Deko3DShader *>(desc.vertexShader);
            if (desc.specConstants[i].index == 929 && desc.specConstants[i].value == 0 && vs &&
                std::find(vs->specIds.begin(), vs->specIds.end(), 929u) != vs->specIds.end())
                p->noUserClip = true;
        }
        // blur-nx (2026-11-12, Blur's black band on the console; the user: "it's probably some clipping issue / seems
        // like deko3d only"): the clip slot off as well when the last stage before the rasterizer (the geometry shader,
        // or else the vertex shader) writes no clip distance. Deko3DOptions::userClipEnable turns slot 0 on for every
        // other draw (rr6-nx's fix for deko3d leaving every slot on), and Renut's host passes draw with present_vs, which
        // writes none: on the console the hardware then clipped their full-screen triangle on whatever that output held,
        // so a colour resolve (drawn on deko3d alone: DrawResolveCopy) could fill only a band of the target and the passes
        // reading it showed the rest black (Blur's race intro). Ryujinx clips only with what a shader writes, which is why
        // neither it nor the PC ever showed it. A shader that writes no clip distance asks for no clipping: exact.
        {
            const Deko3DShader *last = static_cast<const Deko3DShader *>(desc.geometryShader ? desc.geometryShader
                                                                                             : desc.vertexShader);
            if (last && !last->usage.writesClipDistance)
                p->noUserClip = true;
        }
        // banjo-nx (2026-10-21, asyncVariants): the epoch before any of its variants is queued, so one done meanwhile
        // still moves it.
        p->pendingEpoch = asyncEpoch.load(std::memory_order_acquire);
        const Deko3DShader *stages[3] = {static_cast<const Deko3DShader *>(desc.vertexShader),
                                         static_cast<const Deko3DShader *>(desc.geometryShader),
                                         static_cast<const Deko3DShader *>(desc.pixelShader)};
        static const DkStage dkStages[3] = {DkStage_Vertex, DkStage_Geometry, DkStage_Fragment};
        bool ok = stages[0] != nullptr;
        // banjo-nx (2026-10-18, Deko3DOptions::vsOutputs): the vertex shader's constant 905, the locations the pixel
        // shader reads of the ones it writes, when that is not all of them (tools/deko3d_variant_cache.py's stage_spec
        // gives the DKSH cache the same).
        std::vector<RenderSpecConstant> vsSpec;
        if (g_deko3dOptions.vsOutputs && stages[0] && !stages[1] &&
            std::find(stages[0]->specIds.begin(), stages[0]->specIds.end(), 905u) != stages[0]->specIds.end()) {
            const uint32_t outs = stages[0]->usage.outputMask;
            const uint32_t mask = (stages[2] ? stages[2]->usage.inputMask : 0u) & outs;
            if (mask != outs) {
                vsSpec.assign(desc.specConstants, desc.specConstants + desc.specConstantsCount);
                vsSpec.push_back(RenderSpecConstant(905, mask));
            }
        }
        // banjo-nx (2026-10-18): the pixel shader's constant 906, its inputs the vertex shader writes as zero (not with a
        // geometry shader, which writes them).
        std::vector<RenderSpecConstant> psSpec;
        if (g_deko3dOptions.vsOutputs && stages[0] && stages[2] && !stages[1] &&
            std::find(stages[2]->specIds.begin(), stages[2]->specIds.end(), 906u) != stages[2]->specIds.end()) {
            const uint32_t zero = stages[0]->usage.zeroOutputMask & stages[2]->usage.inputMask;
            if (zero) {
                psSpec.assign(desc.specConstants, desc.specConstants + desc.specConstantsCount);
                psSpec.push_back(RenderSpecConstant(906, zero));
            }
        }
        for (int i = 0; i < 3; i++) {
            if (!stages[i])
                continue;
            const bool own = i == 0 && !vsSpec.empty();
            const bool ownPs = i == 2 && !psSpec.empty();
            const RenderSpecConstant *stageSpec = own ? vsSpec.data() : ownPs ? psSpec.data() : desc.specConstants;
            const uint32_t stageSpecCount = own ? uint32_t(vsSpec.size()) : ownPs ? uint32_t(psSpec.size()) : desc.specConstantsCount;
            Deko3DVariantRequest request;
            const DkShader *s = loadVariant(stages[i], stageSpec, stageSpecCount,
                                            g_deko3dOptions.asyncVariants ? &request : nullptr);
            if (request.key != 0) {
                p->pending[i] = std::move(request);
                p->provisional.store(true, std::memory_order_relaxed);
            }
            if (!s) {
                ok = false;
                continue;
            }
            p->shaders[p->shaderCount++] = s;
            p->stageShaders[i] = s;  // banjo-nx (2026-10-19)
            p->stageMask |= 1u << dkStages[i];
            p->uboMask[i] = stages[i]->usage.uboMask;
            p->ssboMask[i] = stages[i]->usage.ssboMask;
            p->texBindings[i] = stages[i]->usage.texBindings;
            p->txqMask[i] = stages[i]->usage.txqMask;
            p->boundTextures = p->boundTextures || !p->texBindings[i].empty();
            for (const Deko3DShaderUsage::TexBinding &b : p->texBindings[i]) {  // banjo-nx (2026-10-27)
                p->texLines |= b.textureWord / 64u < 32u ? 1u << (b.textureWord / 64u) : ~0u;
                if (b.samplerWord != Deko3DShaderUsage::SamplerNone)
                    p->texLines |= b.samplerWord / 64u < 32u ? 1u << (b.samplerWord / 64u) : ~0u;
            }
            p->textures = p->textures || stages[i]->usage.textures;
        }
        p->valid = ok;
        // banjo-nx (2026-10-28): counted once, before anyone has the pipeline (the destructor or the refresh that finds
        // its variants takes it off again).
        if (p->provisional.load(std::memory_order_relaxed))
            g_deko3dProvisionalPipelines.fetch_add(1, std::memory_order_relaxed);
        p->primitive = toDk(desc.primitiveTopology);

        dkRasterizerStateDefaults(&p->rasterizer);
        p->rasterizer.cullMode = desc.cullMode == RenderCullMode::FRONT ? DkFace_Front : desc.cullMode == RenderCullMode::BACK ? DkFace_Back
                              : desc.cullMode == RenderCullMode::FRONT_AND_BACK ? DkFace_FrontAndBack : DkFace_None;  // kameo-nx (2026-11-04)
        p->rasterizer.frontFace = desc.frontFace == RenderFrontFace::COUNTER_CLOCKWISE ? DkFrontFace_CCW : DkFrontFace_CW;
        // D3D's (and Vulkan's) provoking vertex is the first one.
        p->rasterizer.provokingVertex = DkProvokingVertex_First;
        p->rasterizer.depthClampEnable = desc.depthClipEnabled ? 0 : 1;
        p->depthBias = float(desc.depthBias);
        p->depthBiasClamp = desc.depthBiasClamp;
        p->slopeScaledDepthBias = desc.slopeScaledDepthBias;
        p->dynamicDepthBias = desc.dynamicDepthBiasEnabled;
        if (desc.dynamicDepthBiasEnabled || desc.depthBias != 0 || desc.slopeScaledDepthBias != 0.0f)
            p->rasterizer.depthBiasEnableMask = DkPolygonFlag_All;

        dkMultisampleStateDefaults(&p->multisample);
        p->multisample.mode = toDkMsMode(desc.multisampling.sampleCount);
        p->multisample.rasterizerMode = p->multisample.mode;
        p->multisample.alphaToCoverageEnable = desc.alphaToCoverageEnabled;
        // sasrt-nx (2026-10-30, Deko3DOptions::alphaToCoverageDither and msaaPositions).
        p->multisample.alphaToCoverageDither = g_deko3dOptions.alphaToCoverageDither;
        if (p->multisample.mode != DkMsMode_1x) {
            g_deko3dStats.msaaPipelines.fetch_add(1, std::memory_order_relaxed);
            if (desc.alphaToCoverageEnabled)
                g_deko3dStats.msaaAlphaToCoverage.fetch_add(1, std::memory_order_relaxed);
            if (g_deko3dOptions.msaaPositions) {
                dkMultisampleStateSetLocations(&p->multisample, nullptr, 0);
                g_deko3dStats.msaaPositioned.fetch_add(1, std::memory_order_relaxed);
            }
        }

        dkColorStateDefaults(&p->color);
        if (desc.logicOpEnabled)
            p->color.logicOp = toDk(desc.logicOp);
        dkColorWriteStateDefaults(&p->colorWrite);
        p->blendCount = std::min<uint32_t>(desc.renderTargetCount, 8);
        for (uint32_t i = 0; i < p->blendCount; i++) {
            const RenderBlendDesc &b = desc.renderTargetBlend[i];
            dkColorStateSetBlendEnable(&p->color, i, b.blendEnabled);
            dkColorWriteStateSetMask(&p->colorWrite, i, b.renderTargetWriteMask & 0xF);
            DkBlendState &s = p->blend[i];
            dkBlendStateDefaults(&s);
            if (b.blendEnabled) {
                s.colorBlendOp = toDk(b.blendOp);
                s.srcColorBlendFactor = toDk(b.srcBlend);
                s.dstColorBlendFactor = toDk(b.dstBlend);
                s.alphaBlendOp = toDk(b.blendOpAlpha);
                s.srcAlphaBlendFactor = toDkAlpha(b.srcBlendAlpha);
                s.dstAlphaBlendFactor = toDkAlpha(b.dstBlendAlpha);
                for (RenderBlend f : {b.srcBlend, b.dstBlend, b.srcBlendAlpha, b.dstBlendAlpha})
                    p->usesBlendConstant = p->usesBlendConstant || f == RenderBlend::BLEND_FACTOR || f == RenderBlend::INV_BLEND_FACTOR;
            }
            blendWords(p->blendWords[i], i, s);
        }
        for (uint32_t i = p->blendCount; i < 8; i++)
            dkColorWriteStateSetMask(&p->colorWrite, i, 0);

        dkDepthStencilStateDefaults(&p->depthStencil);
        p->depthStencil.depthTestEnable = desc.depthEnabled;
        p->depthStencil.depthWriteEnable = desc.depthWriteEnabled;
        p->depthStencil.depthCompareOp = toDk(desc.depthFunction);
        p->depthStencil.stencilTestEnable = desc.stencilEnabled;
        p->depthStencil.stencilFrontFailOp = toDk(desc.stencilFrontFace.failOp);
        p->depthStencil.stencilFrontPassOp = toDk(desc.stencilFrontFace.passOp);
        p->depthStencil.stencilFrontDepthFailOp = toDk(desc.stencilFrontFace.depthFailOp);
        p->depthStencil.stencilFrontCompareOp = toDk(desc.stencilFrontFace.compareFunction);
        p->depthStencil.stencilBackFailOp = toDk(desc.stencilBackFace.failOp);
        p->depthStencil.stencilBackPassOp = toDk(desc.stencilBackFace.passOp);
        p->depthStencil.stencilBackDepthFailOp = toDk(desc.stencilBackFace.depthFailOp);
        p->depthStencil.stencilBackCompareOp = toDk(desc.stencilBackFace.compareFunction);
        p->stencilEnabled = desc.stencilEnabled;
        // sasrt-nx (2026-11-08, Deko3DOptions::preciseDepthResolve): conservative (the write mask and the depth test
        // are not looked at).
        {
            const auto keeps = [](const RenderStencilFaceDesc &f) {
                return f.failOp == RenderStencilOp::KEEP && f.passOp == RenderStencilOp::KEEP &&
                       f.depthFailOp == RenderStencilOp::KEEP;
            };
            p->writesDepthStencil = desc.depthWriteEnabled ||
                                    (desc.stencilEnabled && (!keeps(desc.stencilFrontFace) || !keeps(desc.stencilBackFace)));
            // sasrt-nx (2026-11-11, port 82; its GPU list's row 2; UNTESTED), Deko3DOptions::preciseStencilWrite:
            // the stencil plane changes only through an outcome that can happen (pass always can; fail only when the
            // test can fail; depth-fail only when the depth test can fail) whose operation is not keep, and only with
            // write mask bits. Depth writes as before (not looked at).
            if (g_deko3dOptions.preciseStencilWrite && p->writesDepthStencil && !desc.depthWriteEnabled) {
                const bool depthCanFail = desc.depthEnabled && desc.depthFunction != RenderComparisonFunction::ALWAYS;
                const auto writes = [&](const RenderStencilFaceDesc &f) {
                    return f.passOp != RenderStencilOp::KEEP ||
                           (f.compareFunction != RenderComparisonFunction::ALWAYS &&
                            f.failOp != RenderStencilOp::KEEP) ||
                           (depthCanFail && f.depthFailOp != RenderStencilOp::KEEP);
                };
                const bool stencilWrites = desc.stencilEnabled && (desc.stencilWriteMask & 0xFF) != 0 &&
                                           (writes(desc.stencilFrontFace) || writes(desc.stencilBackFace));
                if (!stencilWrites) {
                    p->writesDepthStencil = false;
                    g_deko3dPreciseStencilSaved.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        p->stencilReadMask = uint8_t(desc.stencilReadMask);
        p->stencilWriteMask = uint8_t(desc.stencilWriteMask);
        p->stencilReference = uint8_t(desc.stencilReference);
        p->dynamicStencilReference = desc.dynamicStencilReferenceEnabled;

        // Vertex input: the vertex shader's attribute i reads the element at its original location vin[i]
        // (tools/deko3d_glsl.py renumbered them to fit uam's 16).
        const std::vector<uint32_t> &vin = stages[0] ? stages[0]->usage.vertexInputs : std::vector<uint32_t>();
        p->attribCount = uint32_t(vin.size());
        for (uint32_t a = 0; a < p->attribCount; a++) {
            DkVtxAttribState &st = p->attribs[a];
            memset(&st, 0, sizeof(st));
            st.isFixed = 1; // a constant zero unless an element feeds it
            st.size = DkVtxAttribSize_1x32;
            st.type = DkVtxAttribType_Float;
            for (uint32_t e = 0; e < desc.inputElementsCount; e++) {
                const RenderInputElement &el = desc.inputElements[e];
                if (el.location != vin[a])
                    continue;
                DkVtxAttribSize size;
                DkVtxAttribType type;
                bool bgra;
                if (!vertexFormat(el.format, size, type, bgra)) {
                    logf("vertex element format %d has no deko3d form", int(el.format));
                    break;
                }
                st.isFixed = 0;
                st.bufferId = el.slotIndex;
                st.offset = el.alignedByteOffset;
                st.size = size;
                st.type = type;
                st.isBgra = bgra ? 1 : 0;
                break;
            }
        }
        for (uint32_t a = 0; a < p->attribCount; a++)  // sasrt-nx (2026-11-08, Deko3DOptions::vsInputMask)
            p->fetchedAttribs |= uint32_t(p->attribs[a].isFixed == 0) << a;
        for (uint32_t s = 0; s < desc.inputSlotsCount; s++) {
            const RenderInputSlot &slot = desc.inputSlots[s];
            if (slot.index >= 16)
                continue;
            p->vtxBuffers[slot.index].stride = slot.stride;
            p->vtxBuffers[slot.index].divisor = slot.classification == RenderInputSlotClassification::PER_INSTANCE_DATA ? 1 : 0;
            p->vtxBufferCount = std::max(p->vtxBufferCount, slot.index + 1);
        }

        uint64_t h = 0xcbf29ce484222325ull;
        h = mixHash(h, &p->primitive, sizeof(p->primitive));
        h = mixHash(h, &p->rasterizer, sizeof(p->rasterizer));
        h = mixHash(h, &p->multisample, sizeof(p->multisample));
        h = mixHash(h, &p->color, sizeof(p->color));
        h = mixHash(h, &p->colorWrite, sizeof(p->colorWrite));
        h = mixHash(h, p->blendWords, sizeof(p->blendWords));
        h = mixHash(h, &p->depthStencil, sizeof(p->depthStencil));
        h = mixHash(h, &p->stencilReadMask, 3);
        h = mixHash(h, &p->depthBias, sizeof(float) * 3);
        h = mixHash(h, p->attribs, sizeof(DkVtxAttribState) * p->attribCount);
        h = mixHash(h, &p->attribCount, sizeof(p->attribCount));
        h = mixHash(h, p->vtxBuffers, sizeof(DkVtxBufferState) * p->vtxBufferCount);
        h = mixHash(h, &p->vtxBufferCount, sizeof(p->vtxBufferCount));
        p->stateHash = h | 1;
        // banjo-nx (2026-10-19, CPU proposal 13): each group of setPipeline's state binds, hashed apart (never 0).
        auto group = [](std::initializer_list<std::pair<const void *, size_t>> parts) {
            uint64_t g = 0xcbf29ce484222325ull;
            for (const auto &[data, size] : parts)
                g = mixHash(g, data, size);
            return g | 1;
        };
        p->groupHash[0] = group({{&p->rasterizer, sizeof(p->rasterizer)}});
        p->groupHash[1] = group({{&p->multisample, sizeof(p->multisample)}});
        p->groupHash[2] = group({{&p->color, sizeof(p->color)}});
        p->groupHash[3] = group({{&p->colorWrite, sizeof(p->colorWrite)}});
        p->groupHash[4] = group({{&p->blendCount, sizeof(p->blendCount)},
                                 {p->blend, sizeof(DkBlendState) * p->blendCount},
                                 {p->blendWords, sizeof(p->blendWords[0]) * p->blendCount}});
        p->groupHash[5] = group({{&p->depthStencil, sizeof(p->depthStencil)}});
        p->groupHash[6] = group({{&p->stencilReadMask, 1}, {&p->stencilWriteMask, 1}, {&p->stencilReference, 1}});
        p->groupHash[7] = group({{&p->depthBias, sizeof(float)}, {&p->depthBiasClamp, sizeof(float)},
                                 {&p->slopeScaledDepthBias, sizeof(float)}});
        p->groupHash[8] = group({{&p->attribCount, sizeof(p->attribCount)},
                                 {p->attribs, sizeof(DkVtxAttribState) * p->attribCount},
                                 {&p->vtxBufferCount, sizeof(p->vtxBufferCount)},
                                 {p->vtxBuffers, sizeof(DkVtxBufferState) * p->vtxBufferCount}});
        return p;
    }

    std::unique_ptr<RenderPipeline> Deko3DDevice::createComputePipeline(const RenderComputePipelineDesc &desc) {
        auto p = std::make_unique<Deko3DComputePipeline>();
        p->type = Deko3DPipeline::Type::Compute;
        p->device = this;
        p->layout = static_cast<const Deko3DPipelineLayout *>(desc.pipelineLayout);
        const Deko3DShader *shader = static_cast<const Deko3DShader *>(desc.computeShader);
        if (shader) {
            p->shader = loadVariant(shader, desc.specConstants, desc.specConstantsCount);
            p->uboMask = shader->usage.uboMask;
            p->ssboMask = shader->usage.ssboMask;
        }
        p->valid = p->shader != nullptr;
        return p;
    }

    std::unique_ptr<RenderPipeline> Deko3DDevice::createRaytracingPipeline(const RenderRaytracingPipelineDesc &, const RenderPipeline *) { return nullptr; }

    // ---- the rest of the device

    std::unique_ptr<RenderDescriptorSet> Deko3DDevice::createDescriptorSet(const RenderDescriptorSetDesc &desc) {
        return std::make_unique<Deko3DDescriptorSet>(this, desc);
    }

    std::unique_ptr<RenderCommandQueue> Deko3DDevice::createCommandQueue(RenderCommandListType type) {
        return std::make_unique<Deko3DCommandQueue>(this, type);
    }

    std::unique_ptr<RenderBuffer> Deko3DDevice::createBuffer(const RenderBufferDesc &desc) {
        auto buffer = std::make_unique<Deko3DBuffer>(this, desc);
        if (!buffer->block)
            return nullptr;
        return buffer;
    }

    std::unique_ptr<RenderTexture> Deko3DDevice::createTexture(const RenderTextureDesc &desc) {
        return std::make_unique<Deko3DTexture>(this, desc);
    }

    std::unique_ptr<RenderAccelerationStructure> Deko3DDevice::createAccelerationStructure(const RenderAccelerationStructureDesc &) { return nullptr; }

    std::unique_ptr<RenderPool> Deko3DDevice::createPool(const RenderPoolDesc &) { return nullptr; }

    std::unique_ptr<RenderCommandFence> Deko3DDevice::createCommandFence() { return std::make_unique<Deko3DCommandFence>(); }

    std::unique_ptr<RenderCommandSemaphore> Deko3DDevice::createCommandSemaphore() { return std::make_unique<Deko3DCommandSemaphore>(); }

    std::unique_ptr<RenderFramebuffer> Deko3DDevice::createFramebuffer(const RenderFramebufferDesc &desc) {
        auto fb = std::make_unique<Deko3DFramebuffer>();
        static std::atomic<uint64_t> serials{0};  // sasrt-nx (2026-11-08, sameTargetsKept)
        fb->serial = serials.fetch_add(1, std::memory_order_relaxed) + 1;
        fb->colorCount = std::min<uint32_t>(desc.colorAttachmentsCount, 8);
        for (uint32_t i = 0; i < fb->colorCount; i++) {
            const Deko3DTexture *t = static_cast<const Deko3DTexture *>(desc.colorAttachments[i]);
            const Deko3DTextureView *v = desc.colorAttachmentViews ? static_cast<const Deko3DTextureView *>(desc.colorAttachmentViews[i]) : nullptr;
            fb->colors[i] = v ? v->view : t->defaultView();
            fb->colorTextures[i] = t;  // sasrt-nx (2026-11-08, trackedReadWait)
            fb->srgb[i] = isSrgbFormat(fb->colors[i].format != DkImageFormat_None ? fb->colors[i].format : t->format);
            if (i == 0) {
                // The view's level (mip chains rendered level by level).
                fb->width = std::max<uint32_t>(t->desc.width >> fb->colors[i].mipLevelOffset, 1);
                fb->height = std::max<uint32_t>(t->desc.height >> fb->colors[i].mipLevelOffset, 1);
            }
        }
        if (desc.depthAttachment) {
            const Deko3DTexture *t = static_cast<const Deko3DTexture *>(desc.depthAttachment);
            const Deko3DTextureView *v = static_cast<const Deko3DTextureView *>(desc.depthAttachmentView);
            fb->depth = v ? v->view : t->defaultView();
            fb->hasDepth = true;
            fb->depthCompressed = t->compressed;  // sasrt-nx (2026-10-21)
            fb->depthTexture = t;  // renut-ultimate (2026-11-08, depthResolveLazy)
            if (fb->colorCount == 0) {
                fb->width = t->desc.width;
                fb->height = t->desc.height;
            }
        }
        return fb;
    }

    std::unique_ptr<RenderQueryPool> Deko3DDevice::createQueryPool(uint32_t queryCount) {
        return std::make_unique<Deko3DQueryPool>(this, queryCount);
    }

    void Deko3DDevice::setBottomLevelASBuildInfo(RenderBottomLevelASBuildInfo &, const RenderBottomLevelASMesh *, uint32_t, bool, bool) {}
    void Deko3DDevice::setTopLevelASBuildInfo(RenderTopLevelASBuildInfo &, const RenderTopLevelASInstance *, uint32_t, bool, bool) {}
    void Deko3DDevice::setShaderBindingTableInfo(RenderShaderBindingTableInfo &, const RenderShaderBindingGroups &, const RenderPipeline *, RenderDescriptorSet **, uint32_t) {}

    RenderSampleCounts Deko3DDevice::getSampleCountsSupported(RenderFormat) const {
        return RenderSampleCount::COUNT_1 | RenderSampleCount::COUNT_2 | RenderSampleCount::COUNT_4 | RenderSampleCount::COUNT_8;
    }

    // ---- Deko3DInterface

    Deko3DInterface::Deko3DInterface() {
        capabilities.shaderFormat = RenderShaderFormat::DEKO3D;
        deviceNames.push_back("deko3d");
    }

    std::unique_ptr<RenderDevice> Deko3DInterface::createDevice(const std::string &) {
        auto device = std::make_unique<Deko3DDevice>();
        if (!device->initialize())
            return nullptr;
        return device;
    }

    std::unique_ptr<RenderInterface> CreateDeko3DInterface() {
        return std::make_unique<Deko3DInterface>();
    }

    Deko3DFenceCopy Deko3DCopyFence(const RenderCommandFence *fence) {
        Deko3DFenceCopy copy;
        if (const auto *f = static_cast<const Deko3DCommandFence *>(fence)) {
            if (f->pending) {
                copy.fence = f->fence;
                copy.valid = true;
            } else {
                copy.done = true;
            }
        }
        return copy;
    }

    bool Deko3DFenceCopyDone(Deko3DFenceCopy &copy) {
        if (!copy.done && copy.valid && dkFenceWait(&copy.fence, 0) == DkResult_Success)
            copy.done = true;  // signalled once is signalled for good: no second wait
        return copy.done;
    }

    bool Deko3DFenceCopyWait(Deko3DFenceCopy &copy, RenderDevice *device) {
        if (copy.done || !copy.valid)
            return copy.done;
        Deko3DDevice *d = static_cast<Deko3DDevice *>(device);
        for (;;) {
            if (dkFenceWait(&copy.fence, 2'000'000'000ll) == DkResult_Success) {
                copy.done = true;
                return true;
            }
            if (!d || !d->queue || dkQueueIsInErrorState(d->queue))
                return false;
        }
    }

    bool Deko3DDeviceLost(RenderDevice *device) {
        Deko3DDevice *d = static_cast<Deko3DDevice *>(device);
        return d && d->queue && dkQueueIsInErrorState(d->queue);
    }
};

// blur-nx (2026-11-18, the map-load crashes: Video::KickLocked -> dkQueueSubmitCommands or dkQueueFlush ->
// dk::detail::RaiseError(DkResult_Fail) -> diagAbortWithResult, Atmosphère's 2359-0001). deko3d 0.5.0 raises that error
// when libnx's gpfifo kickoff fails and the channel holds no error notification: Queue::appendGpfifoEntries and
// Queue::flush then ask Queue::checkError(), which returns false. So it is not a GPU fault: nvservices refused the
// submission. libnx's nvGpuChannelKickoff retries nvservices' Busy every 100 ms and returns any other refusal. The
// ioctl is wrapped here (-Wl,--wrap=nvioctlChannel_KickoffPb, plume's CMakeLists.txt). The largest submissions are
// logged. A refusal is logged with its result, and the submission is sent again in two halves (and so on down), with
// the gpfifo entries in their order, so the GPU runs the same commands. The first half carries the submission's
// syncpoint increments (flag 8: nvservices raises the syncpoint's expected value by fence.value; the increment
// commands are in deko3d's entries). The expected value then runs ahead of the GPU until the second half's
// increments run, the state of any submission in flight. The second half is a plain one (libnx's flags without
// increments).
extern "C" Result __real_nvioctlChannel_KickoffPb(u32 fd, nvioctl_gpfifo_entry *entries, u32 num_entries, u32 flags,
                                                  nvioctl_fence *fence_inout);
extern "C" Result __wrap_nvioctlChannel_KickoffPb(u32 fd, nvioctl_gpfifo_entry *entries, u32 num_entries, u32 flags,
                                                  nvioctl_fence *fence_inout);

namespace plume {
    static std::atomic<uint32_t> g_kickoffMost{0}, g_kickoffLogged{0}, g_kickoffRefusalLines{0};
    static std::atomic<uint64_t> g_kickoffRefusals{0}, g_kickoffResent{0};
    static constexpr uint32_t kKickoffIncrementFlag = 1u << 8;  // nvservices: the syncpoint raised by fence.value
    static constexpr uint32_t kKickoffRefusalLines = 48;
    static constexpr Result kKickoffBusy = MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Busy);

    static void kickoffRefused(Result r, const nvioctl_gpfifo_entry *entries, uint32_t count, uint32_t flags,
                               const nvioctl_fence &fence, uint32_t depth) {
        const uint64_t n = g_kickoffRefusals.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_kickoffRefusalLines.fetch_add(1, std::memory_order_relaxed) >= kKickoffRefusalLines)
            return;
        uint64_t words = 0;
        uint32_t biggest = 0;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t w = (entries[i].desc32[1] >> 10) & 0x1FFFFFu;  // the entry's length in words
            words += w;
            biggest = std::max(biggest, w);
        }
        uint64_t heapFree = 0, processFree = 0, used = 0, total = 0;
        memoryFigures(heapFree, processFree);
        systemResource(used, total);
        logf("nvservices REFUSED a GPU submission (refusal %" PRIu64 ", split depth %u): result 0x%X (module %u, "
             "description %u); %u gpfifo entries (%" PRIu64 " command words, the largest entry %u), flags 0x%X, "
             "syncpoint %u +%u; blocks %u live (%" PRIu64 " MB), GPU region %" PRIu64 " MB, heap %" PRIu64
             " MB free, process %" PRIu64 " MB unused, system resource %" PRIu64 " of %" PRIu64 " KiB; %s",
             n, depth, unsigned(r), unsigned(R_MODULE(r)), unsigned(R_DESCRIPTION(r)), count, words, biggest, flags,
             fence.id, fence.value, g_blocksLive.load(std::memory_order_relaxed),
             g_blocksLiveBytes.load(std::memory_order_relaxed) >> 20,
             g_regionBytesNow.load(std::memory_order_relaxed) >> 20, heapFree >> 20, processFree >> 20, used >> 10,
             total >> 10, count > 1 ? "sent again in halves" : "one entry: nothing to split (deko3d aborts)");
    }

    static Result kickoffHalves(u32 fd, nvioctl_gpfifo_entry *entries, u32 count, u32 flags, nvioctl_fence *fence,
                                u32 depth);

    // One part. nvservices' Busy is waited out here, as libnx does (libnx's own retry would send the whole submission
    // again). A refusal is split again.
    static Result kickoffPart(u32 fd, nvioctl_gpfifo_entry *entries, u32 count, u32 flags, nvioctl_fence *fence,
                              u32 depth) {
        for (;;) {
            nvioctl_fence f = *fence;
            const Result r = __real_nvioctlChannel_KickoffPb(fd, entries, count, flags, &f);
            if (r == kKickoffBusy) {
                svcSleepThread(100'000'000);
                continue;
            }
            if (R_SUCCEEDED(r)) {
                *fence = f;
                return r;
            }
            kickoffRefused(r, entries, count, flags, *fence, depth);
            return count > 1 ? kickoffHalves(fd, entries, count, flags, fence, depth + 1) : r;
        }
    }

    static Result kickoffHalves(u32 fd, nvioctl_gpfifo_entry *entries, u32 count, u32 flags, nvioctl_fence *fence,
                                u32 depth) {
        const u32 first = count / 2;
        nvioctl_fence a = *fence;  // the first half: the increments
        Result r = kickoffPart(fd, entries, first, flags, &a, depth);
        if (R_FAILED(r))
            return r;
        nvioctl_fence b{fence->id, 0};
        r = kickoffPart(fd, entries + first, count - first, flags & ~kKickoffIncrementFlag, &b, depth);
        if (R_FAILED(r))
            return r;
        *fence = a;
        return r;
    }

}  // namespace plume

extern "C" __attribute__((used)) Result __wrap_nvioctlChannel_KickoffPb(u32 fd, nvioctl_gpfifo_entry *entries,
                                                                        u32 num_entries, u32 flags,
                                                                        nvioctl_fence *fence_inout) {
    using namespace plume;
    if (num_entries > g_kickoffMost.load(std::memory_order_relaxed)) {
        g_kickoffMost.store(num_entries, std::memory_order_relaxed);
        if (num_entries >= 64 && (num_entries >= 2 * g_kickoffLogged.load(std::memory_order_relaxed) || num_entries >= 1024)) {
            g_kickoffLogged.store(num_entries, std::memory_order_relaxed);
            logf("nvservices: a GPU submission of %u gpfifo entries, the most so far (libnx sends at most 2048)",
                 num_entries);
        }
    }
    const nvioctl_fence in = *fence_inout;
    const Result r = __real_nvioctlChannel_KickoffPb(fd, entries, num_entries, flags, fence_inout);
    if (R_SUCCEEDED(r) || r == kKickoffBusy)
        return r;  // (Busy: nothing was sent; libnx waits 100 ms and sends it again, as before)
    kickoffRefused(r, entries, num_entries, flags, in, 0);
    if (num_entries < 2)
        return r;
    nvioctl_fence f = in;
    const Result s = kickoffHalves(fd, entries, num_entries, flags, &f, 1);
    if (R_SUCCEEDED(s)) {
        *fence_inout = f;
        const uint64_t resent = g_kickoffResent.fetch_add(1, std::memory_order_relaxed) + 1;
        if (resent <= 16 || (resent & 63) == 0)
            logf("nvservices: the refused submission went in as parts (%" PRIu64 " so far)", resent);
    } else {
        logf("nvservices: the refused submission could not be sent in parts either: result 0x%X (deko3d aborts)",
             unsigned(s));
    }
    return s;
}
