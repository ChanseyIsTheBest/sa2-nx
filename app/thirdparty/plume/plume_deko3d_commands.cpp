//
// plume deko3d back end (banjo-nx, 2026-10-17): command lists, the queue and the swapchain (plume_deko3d.cpp has the
// device). docs/deko3d-renderer.md section 4 maps each plume call to deko3d.
//
#include "plume_deko3d.h"

#include <switch.h>

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace plume {
    namespace {
        uint32_t traceImage(const void *p);  // sasrt-nx (2026-11-08, the trace): below
    }
    // banjo-nx (2026-10-26): setVertexBuffers' filter of unchanged slots (banjo_deko3d_vtx_dedupe).
    bool g_vtxDedupe = true;
    DkImageFormat toDkFormat(RenderFormat format);

    void deko3dLog(const char *fmt, ...); // plume_deko3d.cpp
#define logf deko3dLog

    static constexpr uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

    // Port 167 (from fnxf_xiii, its note of 2026-10-27): why the GPU stopped, logged before deko3d aborts. A GPU fault
    // (an address the GPU may not read, work that ran too long) only puts the queue in an error state; deko3d's release
    // build then aborts at the next submission or flush with nothing but result 2359-0001. Here the channel's error
    // notification and info are read and logged first (deko3d.log writes such a line at once), with what the faulting
    // address is in. Logging only: what happens next is as before (deko3d's own abort, or the fence wait giving up).
    // deko3d 0.5.0's Queue holds its NvGpuChannel at +0x18 (dkQueueIsInErrorState and Queue::checkError, disassembled by
    // fnxf_xiii).
    static const char *notificationName(uint32_t type) {
        switch (type) {
        case 8: return "FifoErrorIdleTimeout (work ran too long)";
        case 13: return "GrErrorSwNotify";
        case 24: return "GrSemaphoreTimeout (a semaphore wait ran too long)";
        case 25: return "GrIllegalNotify";
        case 31: return "FifoErrorMmuErrFlt (the GPU used an address with no memory behind it)";
        case 32: return "PbdmaError (bad command data)";
        case 43: return "ResetChannelVerifError";
        case 80: return "PbdmaPushbufferCrcMismatch";
        default: return "?";
        }
    }

    static std::atomic<Deko3DDevice *> g_faultDevice{nullptr};

    // The channel's report, logged once: the notification, the error info, what a fault's address is in. 'rc': the
    // kickoff's result when a kickoff failed (0 otherwise).
    static void reportChannel(NvGpuChannel *channel, const char *where, uint32_t rc) {
        static std::atomic<bool> reported{false};
        if (!channel || reported.exchange(true))
            return;
        NvNotification n{};
        NvError e{};
        const Result rn = channel->base.has_init ? nvGpuChannelGetErrorNotification(channel, &n) : Result(-1);
        const Result re = channel->base.has_init ? nvGpuChannelGetErrorInfo(channel, &e) : Result(-1);
        logf("GPU FAULT (%s, kickoff result 0x%X): notification (0x%X): type %u = %s, info16 0x%X, status 0x%X, at "
             "%" PRIu64 "; error info (0x%X): type %u, info %08X %08X %08X %08X %08X %08X %08X %08X; %u gpfifo "
             "entries not yet given to the GPU",
             where, rc, rn, n.info32, notificationName(n.info32), n.info16, n.status, n.timestamp, re, e.type,
             e.info[0], e.info[1], e.info[2], e.info[3], e.info[4], e.info[5], e.info[6], e.info[7],
             channel->num_entries);
        if (rc && !n.status)
            logf("  the kickoff failed with no GPU error reported (0x%X: module %u, description %u%s)", rc, R_MODULE(rc),
                 R_DESCRIPTION(rc), rc == MAKERESULT(Module_LibnxNvidia, LibnxNvidiaError_Timeout) ? " = Timeout: the "
                 "driver gave up waiting" : "");
        if (Deko3DDevice *device = g_faultDevice.load()) {
            size_t retired = 0, guarded = 0;
            {
                std::lock_guard<std::mutex> lock(device->retireMutex);
                retired = device->retired.size();
            }
            {
                std::lock_guard<std::mutex> lock(device->guardMutex);
                guarded = device->guarded.size();
            }
            logf("  %zu blocks waiting for their frame to be freed, %zu guarded; submissions %" PRIu64 " made, %" PRIu64
                 " known finished", retired, guarded, device->submitSerial.load(), device->completedSerial.load());
            // A fault's address, among the info words: is it in a buffer the device still has?
            for (int i = 0; i + 1 < 8; i++) {
                const DkGpuAddr address = (DkGpuAddr(e.info[i + 1]) << 32) | e.info[i];
                if (address < 0x100000ull || (address >> 40))
                    continue;
                DkGpuAddr base = 0;
                uint32_t size = 0;
                const uint8_t *cpu = nullptr;
                if (device->findBuffer(address, base, size, cpu))
                    logf("  info %d-%d: GPU address 0x%" PRIX64 " is inside a live buffer 0x%" PRIX64 " (%u bytes, +0x%"
                         PRIX64 ")", i, i + 1, address, base, size, address - base);
                else
                    logf("  info %d-%d: GPU address 0x%" PRIX64 " is in no live buffer (freed, an image, or not an "
                         "address)", i, i + 1, address);
            }
        }
    }

    // The queue's state (a memory read, no ioctl): the channel's report when it is in error.
    static void checkQueue(Deko3DDevice *device, const char *where) {
        g_faultDevice.store(device, std::memory_order_relaxed);
        if (!device || !device->queue || !dkQueueIsInErrorState(device->queue))
            return;
        reportChannel(reinterpret_cast<NvGpuChannel *>(reinterpret_cast<uint8_t *>(device->queue) + 0x18), where, 0);
    }

    // The push constants' UBO binding in every stage, and the address window's storage binding (tools/deko3d_glsl.py).
    static constexpr uint32_t PushUboSlot = 5;
    static constexpr uint32_t TexSizeUboSlot = 3;  // banjo-nx (2026-10-19, txq)
    static constexpr uint32_t AddressWindowSlot = 15;
    static constexpr uint32_t CommandBlockSize = 1u << 20;
    static constexpr uint32_t TransientBlockSize = 256u << 10;
    static const DkStage GraphicsStages[3] = {DkStage_Vertex, DkStage_Geometry, DkStage_Fragment};
    // Debug builds: the first operations of each kind, logged (what the frame is made of).
    static std::atomic<uint32_t> g_traceClears{0}, g_traceCopies{0}, g_traceTargets{0};

    // ---- Deko3DCommandList

    static void addMemoryCallback(void *user, DkCmdBuf, size_t minimum) {
        static_cast<Deko3DCommandList *>(user)->addMemory(minimum);
    }

    Deko3DCommandList::Deko3DCommandList(Deko3DCommandQueue *queue) : queue(queue), device(queue->device) {
        DkCmdBufMaker m;
        dkCmdBufMakerDefaults(&m, device->dk);
        m.userData = this;
        m.cbAddMem = addMemoryCallback;
        cmd = dkCmdBufCreate(&m);
    }

    Deko3DCommandList::~Deko3DCommandList() {
        if (device->queue)
            dkQueueWaitIdle(device->queue);
        if (cmd)
            dkCmdBufDestroy(cmd);
        for (auto &m : memory)
            device->destroyBlock(m.first);
        for (auto &t : transient)
            device->destroyBlock(t.first);
    }

    void Deko3DCommandList::addMemory(size_t minimum) {
        const uint32_t want = alignUp(uint32_t(std::max<size_t>(minimum, CommandBlockSize)), DK_MEMBLOCK_ALIGNMENT);
        while (memoryUsed < memory.size() && memory[memoryUsed].second < want)
            memoryUsed++;
        if (memoryUsed == memory.size()) {
            DkMemBlock block = device->makeBlock(want, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "command memory");
            memory.push_back({block, want});
            GetDeko3DStats().commandBytes += want;
        }
        dkCmdBufAddMemory(cmd, memory[memoryUsed].first, 0, memory[memoryUsed].second);
        memoryUsed++;
    }

    bool Deko3DCommandList::transientAlloc(uint32_t size, uint32_t alignment, void **cpu, DkGpuAddr *gpu) {
        if (size > TransientBlockSize)
            return false;
        for (;;) {
            if (transientBlock < transient.size()) {
                const uint32_t at = alignUp(transientUsed, alignment);
                if (at + size <= transient[transientBlock].second) {
                    *cpu = static_cast<uint8_t *>(dkMemBlockGetCpuAddr(transient[transientBlock].first)) + at;
                    *gpu = dkMemBlockGetGpuAddr(transient[transientBlock].first) + at;
                    transientUsed = at + size;
                    return true;
                }
                transientBlock++;
                transientUsed = 0;
                continue;
            }
            DkMemBlock block = device->makeBlock(TransientBlockSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "transient data");
            transient.push_back({block, TransientBlockSize});
        }
    }

    void Deko3DCommandList::begin() {
        // The application waited for this list's last submission (as Vulkan requires before a reset): its command and
        // transient memory is free again. dkCmdBufClear rewinds into the memory fed last, so the first block is fed
        // explicitly (SwitchWakerHD: deko3d 0.5.0's clear keeps the old position).
        dkCmdBufClear(cmd);
        pendingBarrier = false;  // banjo-nx (2026-10-19): nothing held over from the list before
        pendingBarrierMode = DkBarrier_None;
        pendingBarrierInvalidate = 0;
        copyRunFenced = false;  // port 161
        sampleMaskSet = 0xFFFFFFFFu;  // port 166
        memoryUsed = 0;
        addMemory(0);
        // sasrt-nx (2026-11-08, Deko3DOptions::freshComputeConstants): InvalidateShaderCaches (0x369), Constant (bit 12).
        // sasrt-nx (2026-11-08): and the global-data cache (bit 4): the seeds read upload-ring addresses (CPU-written,
        // reused two frames on) with no barrier right before them (sasrt_gpu_seed_no_lead_wait).
        if (Deko3DGetOptions().freshComputeConstants) {
            const uint32_t word = (4u << 29) | (0x1010u << 16) | (0u << 13) | 0x369u;
            dkCmdBufReplayCmds(cmd, &word, 1);
        }
        transientBlock = 0;
        transientUsed = 0;
        open = true;
        list = 0;

        graphicsPipeline = nullptr;
        computePipeline = nullptr;
        boundTargetsSerial = 0;  // sasrt-nx (2026-11-08, sameTargetsKept)
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        discardNextDraw = false;
        dispatchSinceIdle = true;
        boundStateHash = 0;
        boundShaders[0] = boundShaders[1] = boundShaders[2] = kDeko3DShadersUnknown;  // banjo-nx (2026-10-28)
        std::fill(std::begin(boundGroups), std::end(boundGroups), 0);
        graphicsLayout = computeLayout = nullptr;
        boundImageSet = boundSamplerSet = 0;
        boundImageCount = boundSamplerCount = 0;
        graphicsUboValid = graphicsSsboValid = 0;
        computeUboValid = computeSsboValid = 0;
        computeUboDirty = computeSsboDirty = true;
        computePushSize = 0;
        memset(uboBoundStages, 0, sizeof(uboBoundStages));
        memset(ssboBoundStages, 0, sizeof(ssboBoundStages));
        graphicsBuffersDirty = true;
        boundVtxValid = 0;  // banjo-nx (2026-10-26)
        framebuffer = nullptr;
        depthDirty = false;  // sasrt-nx (2026-10-21)
        viewportCount = scissorCount = 0;
        indexAddress = 0;
        boundIndexFormat = DkIdxFormat_Uint16;
        boundRestart = 1;  // sasrt-nx (2026-10-30)
        boundTexCount[0] = boundTexCount[1] = boundTexCount[2] = 0;
        texPipeline = nullptr;  // banjo-nx (2026-10-27)
        texLinesWritten = ~0u;
        sharedBase = 0;
        sharedSize = 0;
        sharedCpu = nullptr;
        sharedWords = nullptr;  // banjo-nx (2026-10-19)
        sharedWordsSize = 0;
        descriptorEpoch = 0;
        // NaN: the first setBlendConstant of the list always writes it.
        blendConstant[0] = blendConstant[1] = blendConstant[2] = blendConstant[3] = __builtin_nanf("");

        // The push constants' UBO, bound once for every graphics stage. The vertex fetch data's binding (15) is each
        // draw's own range (Deko3DBindFetchData).
        graphicsUbo[PushUboSlot] = {device->pushAddr, Deko3DDevice::PushSize};
        graphicsUboValid |= 1u << PushUboSlot;
        // banjo-nx (2026-10-19, GPU, txq): the texture size buffer, for the shaders whose usage has UBO bit 3.
        graphicsUbo[TexSizeUboSlot] = {device->texSizeAddr, Deko3DDevice::TexSizeSize};
        graphicsUboValid |= 1u << TexSizeUboSlot;
        boundImageSetObj = nullptr;
        texSizeValid = 0;
        // Whatever the GPU holds of descriptors and shader code from before is dropped.
        dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader);
        // sasrt-nx (2026-11-08, Deko3DOptions::tiledCache): on (or off again once it has been on: the A/B halves).
        {
            static bool everOn = false;
            const bool on = Deko3DGetOptions().tiledCache;
            if (on || everOn) {
                everOn = everOn || on;
                dkCmdBufTiledCacheOp(cmd, on ? DkTiledCacheOp_Enable : DkTiledCacheOp_Disable);
            }
        }
        if (const int clip = Deko3DGetOptions().userClipEnable; clip >= 0) {  // rr6-nx (2026-11-05): see Deko3DOptions
            const uint32_t words[2] = {(1u << 29) | (1u << 16) | 0x544u, uint32_t(clip)};
            dkCmdBufReplayCmds(cmd, words, 2);
            boundUserClip = clip;  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
        } else {
            boundUserClip = -1;
        }
        descriptorEpoch = currentEpoch();
        seenDescriptors = device->descriptorEpoch.load(std::memory_order_relaxed);  // banjo-nx (2026-10-28)
        seenCode = device->codeEpoch.load(std::memory_order_relaxed);
    }

    // Descriptors written and shader code loaded so far, as one number (flushDescriptorInvalidate).
    uint64_t Deko3DCommandList::currentEpoch() const {
        // banjo-nx (2026-10-27, render thread night): relaxed loads (two LDARs at every draw before). A code load bumps
        // codeEpoch before the variant is handed over under a lock, and descriptors are written before their texture is
        // published (or by this thread): the thread that draws with them has acquired that, so it reads the new epoch.
        return device->descriptorEpoch.load(std::memory_order_relaxed) ^ (device->codeEpoch.load(std::memory_order_relaxed) << 40);
    }

    void Deko3DCommandList::end() {
        // sasrt-nx (2026-10-21): the next list may read the depth target still bound here.
        // renut-ultimate (2026-11-08, depthResolveLazy): then it is decompressed there, when it moves to a read layout.
        if (Deko3DGetOptions().depthResolveLazy)
            deferBoundDepth();
        // sasrt-nx (2026-11-08, lazyDepthResolve): or its read does, in whichever list.
        if (depthDirty && framebuffer && lazyResolvable(framebuffer->depthTexture) && framebuffer->depthTexture->depthUnresolved)
            depthDirty = false;
        else
            resolveBoundDepth(false);
        flushBarriers();
        list = dkCmdBufFinishList(cmd);
        open = false;
        if (Deko3DGetOptions().diagBarriers)
            bumpStat(GetDeko3DStats().diagListEnds);  // sasrt-nx (2026-11-08): the census
        if (g_deko3dTrace)
            Deko3DTraceToken("E");
        // banjo-nx (2026-10-19, render-thread proposal 13): this list's draws into the totals.
        Deko3DStats &st = GetDeko3DStats();
        st.draws.store(st.draws.load(std::memory_order_relaxed) + statDraws, std::memory_order_relaxed);
        st.windowDraws.store(st.windowDraws.load(std::memory_order_relaxed) + statWindowDraws, std::memory_order_relaxed);
        st.textureRuns.store(st.textureRuns.load(std::memory_order_relaxed) + statTexRuns, std::memory_order_relaxed);
        st.textureSkips.store(st.textureSkips.load(std::memory_order_relaxed) + statTexSkips, std::memory_order_relaxed);
        st.stripRestartDraws.store(st.stripRestartDraws.load(std::memory_order_relaxed) + statStripRestarts, std::memory_order_relaxed);
        statDraws = statWindowDraws = 0;
        statStripRestarts = 0;
        statTexRuns = statTexSkips = 0;
    }

    void Deko3DCommandList::flushDescriptorInvalidate() {
        // Descriptors the CPU wrote since this list last invalidated, or shader code loaded since: the GPU's caches of
        // them are dropped before the next draw or dispatch.
        // banjo-nx (2026-10-28, per-draw night): the two numbers read once (currentEpoch's form), and only the caches
        // whose contents changed dropped.
        const uint64_t d = device->descriptorEpoch.load(std::memory_order_relaxed);
        const uint64_t c = device->codeEpoch.load(std::memory_order_relaxed);
        const uint64_t epoch = d ^ (c << 40);
        if (epoch != descriptorEpoch) {
            uint32_t flags = (d != seenDescriptors ? uint32_t(DkInvalidateFlags_Descriptors) : 0u) |
                             (c != seenCode ? uint32_t(DkInvalidateFlags_Shader) : 0u);
            if (!flags)
                flags = DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader;
            flushBarriers();
            dkCmdBufBarrier(cmd, DkBarrier_None, flags);
            if (Deko3DGetOptions().diagBarriers) {  // sasrt-nx (2026-11-08): the census
                if (flags & DkInvalidateFlags_Descriptors)
                    bumpStat(GetDeko3DStats().diagDescInvalidates);
                if (flags & DkInvalidateFlags_Shader)
                    bumpStat(GetDeko3DStats().diagCodeInvalidates);
            }
            if (g_deko3dTrace)
                Deko3DTraceToken("I");
            descriptorEpoch = epoch;
            seenDescriptors = d;
            seenCode = c;
        }
    }

    void Deko3DCommandList::barriers(RenderBarrierStages stages, const RenderBufferBarrier *bufferBarriers, uint32_t bufferBarriersCount, const RenderTextureBarrier *textureBarriers, uint32_t textureBarriersCount) {
        // Image layouts do not exist in deko3d: a barrier is an ordering of the GPU's work and an invalidation of its
        // caches. Rendering and compute before sampling: Fragments / Primitives with the texture cache dropped
        // (SwitchWakerHD's choices); the copy engine involved: Full (it waits for the copy engine too).
        // sasrt-nx (2026-10-21, Deko3DOptions::depthResolve): the bound depth target moving to a read layout is
        // decompressed first; the barrier recorded here then orders the read after it.
        // Moving back to DEPTH_WRITE (written again while still bound) makes it dirty again.
        // renut-ultimate (2026-11-08, depthResolveLazy): a target left for later is decompressed through its own view
        // when it moves to a read layout; the barrier recorded below orders the read after it.
        if (Deko3DGetOptions().depthResolveLazy && Deko3DGetOptions().depthResolve) {
            for (uint32_t i = 0; i < textureBarriersCount; i++) {
                const RenderTextureLayout l = textureBarriers[i].layout;
                const Deko3DTexture *t = static_cast<const Deko3DTexture *>(textureBarriers[i].texture);
                if (t && t->depthPending &&
                    (l == RenderTextureLayout::SHADER_READ || l == RenderTextureLayout::DEPTH_READ ||
                     l == RenderTextureLayout::GENERAL || l == RenderTextureLayout::COPY_SOURCE ||
                     l == RenderTextureLayout::RESOLVE_SOURCE))
                    resolvePendingDepth(t, true);
            }
        }
        if (framebuffer && framebuffer->hasDepth && framebuffer->depthCompressed && Deko3DGetOptions().depthResolve) {
            for (uint32_t i = 0; i < textureBarriersCount; i++) {
                const RenderTextureLayout l = textureBarriers[i].layout;
                const Deko3DTexture *t = static_cast<const Deko3DTexture *>(textureBarriers[i].texture);
                if (!t || &t->image != framebuffer->depth.pImage)
                    continue;
                if (depthDirty &&
                    (l == RenderTextureLayout::SHADER_READ || l == RenderTextureLayout::DEPTH_READ ||
                     l == RenderTextureLayout::GENERAL || l == RenderTextureLayout::COPY_SOURCE ||
                     l == RenderTextureLayout::RESOLVE_SOURCE))
                    resolveBoundDepth(false);
                else if (l == RenderTextureLayout::DEPTH_WRITE && !Deko3DGetOptions().preciseDepthResolve)
                    depthDirty = true;  // sasrt-nx (2026-11-08): with preciseDepthResolve the draws that write mark it
            }
        }
        // sasrt-nx (2026-11-08, lazyDepthResolve): a written depth image not bound now, about to be read: decompressed
        // first (bound alone for it), before this barrier orders the read after it.
        if (Deko3DGetOptions().lazyDepthResolve && Deko3DGetOptions().depthResolve) {
            for (uint32_t i = 0; i < textureBarriersCount; i++) {
                const Deko3DTexture *t = static_cast<const Deko3DTexture *>(textureBarriers[i].texture);
                if (!t || !t->compressed || !t->depthUnresolved || !(t->desc.flags & RenderTextureFlag::DEPTH_TARGET) ||
                    !lazyResolvable(t))
                    continue;
                if (framebuffer && framebuffer->hasDepth && framebuffer->depthTexture == t)
                    continue;  // the bound one: the code below
                const RenderTextureLayout l = textureBarriers[i].layout;
                if (l == RenderTextureLayout::SHADER_READ || l == RenderTextureLayout::DEPTH_READ ||
                    l == RenderTextureLayout::GENERAL || l == RenderTextureLayout::COPY_SOURCE ||
                    l == RenderTextureLayout::RESOLVE_SOURCE)
                    resolveUnboundDepth(t);
            }
        }
        if (bufferBarriersCount)
            ++bufferWaits;  // sasrt-nx (2026-11-08, Deko3DBufferWaits)
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedTargetWait): copies, resolves and decompressions use their textures:
        // marked for the wait epoch (the next move into a target layout waits for them).
        {  // sasrt-nx (2026-11-08): kept whatever the options
            const uint64_t now = g_deko3dWaitEpoch.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < textureBarriersCount; i++) {
                const RenderTextureLayout l = textureBarriers[i].layout;
                if (l != RenderTextureLayout::COLOR_WRITE && l != RenderTextureLayout::DEPTH_WRITE && textureBarriers[i].texture)
                    static_cast<const Deko3DTexture *>(textureBarriers[i].texture)->usedWaitEpoch = now;
            }
        }
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedTargetWait; the third GPU round): only moves into target layouts, and
        // none of their textures used since the last fragment wait: nothing reads them while they are written.
        if (Deko3DGetOptions().trackedTargetWait && Deko3DGetOptions().leanBarriers &&
            bufferBarriersCount == 0 && textureBarriersCount > 0 &&
            !(stages & (RenderBarrierStage::COPY | RenderBarrierStage::COMPUTE))) {
            const uint64_t now = g_deko3dWaitEpoch.load(std::memory_order_relaxed);
            bool unused = true;
            for (uint32_t i = 0; i < textureBarriersCount && unused; i++) {
                const RenderTextureLayout l = textureBarriers[i].layout;
                const Deko3DTexture *t = static_cast<const Deko3DTexture *>(textureBarriers[i].texture);
                unused = (l == RenderTextureLayout::COLOR_WRITE || l == RenderTextureLayout::DEPTH_WRITE) && t &&
                         t->usedWaitEpoch != now;
            }
            if (unused) {
                bumpRound(43);  // sasrt::gpu_round::kTargetWaitTracked
                return;
            }
        }
        // sasrt-nx (2026-11-08, Deko3DOptions::leanBarrierSkip): only moves into write layouts, nothing to wait for.
        if (Deko3DGetOptions().leanBarrierSkip && Deko3DGetOptions().leanBarriers &&
            !workSinceFragmentWait && bufferBarriersCount == 0 && textureBarriersCount > 0 &&
            !(stages & (RenderBarrierStage::COPY | RenderBarrierStage::COMPUTE))) {
            bool writesOnly = true;
            for (uint32_t i = 0; i < textureBarriersCount; i++) {
                const RenderTextureLayout l = textureBarriers[i].layout;
                writesOnly = writesOnly && (l == RenderTextureLayout::COLOR_WRITE || l == RenderTextureLayout::DEPTH_WRITE);
            }
            if (writesOnly) {
                bumpRound(27);
                return;
            }
        }
        uint32_t invalidate = 0;
        DkBarrier mode = DkBarrier_None;
        bool copy = (stages & RenderBarrierStage::COPY) != 0;
        uint32_t readMovesFree = 0;  // sasrt-nx (2026-11-08, trackedReadWait): read moves needing neither wait nor drop
        // sasrt-nx (2026-11-08, Deko3DOptions::diagBarriers): why, for the counts.
        uint32_t why = copy ? uint32_t(Deko3DWhyCopy) : 0u;
        if (Deko3DGetOptions().diagBarriers) {
            for (uint32_t i = 0; i < textureBarriersCount; i++) {
                switch (textureBarriers[i].layout) {
                case RenderTextureLayout::SHADER_READ:
                case RenderTextureLayout::DEPTH_READ:
                case RenderTextureLayout::GENERAL: why |= Deko3DWhyRead; break;
                case RenderTextureLayout::COPY_SOURCE:
                case RenderTextureLayout::COPY_DEST:
                case RenderTextureLayout::RESOLVE_SOURCE:
                case RenderTextureLayout::RESOLVE_DEST: why |= Deko3DWhyCopy; break;
                case RenderTextureLayout::COLOR_WRITE: why |= Deko3DWhyColor; break;
                case RenderTextureLayout::DEPTH_WRITE: why |= Deko3DWhyDepth; break;
                default: why |= Deko3DWhyOther; break;
                }
            }
            if (bufferBarriersCount)
                why |= Deko3DWhyBuffer;
            if (stages & RenderBarrierStage::COMPUTE)
                why |= Deko3DWhyCompute;
        }
        for (uint32_t i = 0; i < textureBarriersCount; i++) {
            switch (textureBarriers[i].layout) {
            case RenderTextureLayout::SHADER_READ:
            case RenderTextureLayout::DEPTH_READ:
            case RenderTextureLayout::GENERAL: {
                // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate): only a texture sampled since the last
                // drop can have lines in the cache.
                const Deko3DTexture *t = static_cast<const Deko3DTexture *>(textureBarriers[i].texture);
                const bool drop = !Deko3DGetOptions().trackedImageInvalidate || !t ||
                                  t->sampledEpoch == g_deko3dTexDropEpoch.load(std::memory_order_relaxed);
                if (drop)
                    invalidate |= DkInvalidateFlags_Image;
                else
                    bumpRound(42);  // sasrt::gpu_round::kTexDropTracked: a read move without the drop
                // sasrt-nx (2026-11-08, Deko3DOptions::trackedReadWait): the wait only for a texture written since the last
                // wait, or with its drop due (which waits for the GPU's earlier work).
                const bool written = !Deko3DGetOptions().trackedReadWait || !t ||
                                     t->writtenWaitEpoch == g_deko3dWaitEpoch.load(std::memory_order_relaxed);
                if (drop || written) {
                    mode = std::max(mode, DkBarrier_Fragments);
                } else {
                    ++readMovesFree;
                }
                break;
                }
            case RenderTextureLayout::COPY_SOURCE:
            case RenderTextureLayout::COPY_DEST:
            case RenderTextureLayout::RESOLVE_SOURCE:
            case RenderTextureLayout::RESOLVE_DEST:
                copy = true;
                break;
            case RenderTextureLayout::PRESENT:
            case RenderTextureLayout::COLOR_WRITE:
            case RenderTextureLayout::DEPTH_WRITE:
                mode = std::max(mode, DkBarrier_Fragments);
                // banjo-nx (2026-10-19, Deko3DOptions::leanBarriers): written next, not read: no invalidation.
                if (!Deko3DGetOptions().leanBarriers)
                    invalidate |= DkInvalidateFlags_Image;
                break;
            default:
                mode = std::max(mode, DkBarrier_Fragments);
                invalidate |= DkInvalidateFlags_Image;
                break;
            }
        }
        if (bufferBarriersCount) {
            // sasrt-nx (2026-11-08, Deko3DOptions::dataOnlyInvalidate): the data caches, not the instruction cache.
            invalidate |= (Deko3DGetOptions().dataOnlyInvalidate ? Deko3DDataInvalidate : uint32_t(DkInvalidateFlags_Shader)) |
                          DkInvalidateFlags_Image;
            mode = std::max(mode, DkBarrier_Primitives);
        }
        if (stages & RenderBarrierStage::COMPUTE)
            mode = std::max(mode, DkBarrier_Primitives);
        if (copy)
            mode = DkBarrier_Full;
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedReadWait): every move was a read move with nothing to wait for or drop.
        if (readMovesFree && readMovesFree == textureBarriersCount && mode == DkBarrier_None && !invalidate &&
            bufferBarriersCount == 0) {
            bumpRound(44);  // sasrt::gpu_round::kReadWaitTracked
            return;
        }
        if (mode == DkBarrier_None && textureBarriersCount == 0 && bufferBarriersCount == 0)
            mode = DkBarrier_Primitives;
        if (Deko3DGetOptions().mergeBarriers) {
            // banjo-nx (2026-10-19): held until the next command that does work (flushBarriers), merged with any
            // barrier recorded before then.
            pendingBarrierMode = pendingBarrier ? std::max(pendingBarrierMode, mode) : mode;
            pendingBarrierInvalidate |= invalidate;
            pendingBarrierWhy = pendingBarrier ? (pendingBarrierWhy | why | Deko3DWhyMerged) : why;
            pendingBarrier = true;
            return;
        }
        pendingBarrierWhy = why;
        emitBarrier(mode, invalidate);
    }

    void Deko3DCommandList::emitBarrier(DkBarrier mode, uint32_t invalidate) {
        // sasrt-nx (2026-11-08, Deko3DOptions::tiledCache): the copy engine reads memory: the tiles flushed first.
        if (mode == DkBarrier_Full && Deko3DGetOptions().tiledCache)
            dkCmdBufTiledCacheOp(cmd, DkTiledCacheOp_Flush);
        {
            // banjo-nx (2026-10-19): counted for the renderer's report (a load and a store: diagnostics).
            Deko3DStats &st = GetDeko3DStats();
            auto bump = [](std::atomic<uint64_t> &c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); };
            bump(mode == DkBarrier_Full ? st.barriersFull : mode == DkBarrier_Fragments ? st.barriersFragments : st.barriersPrimitives);
            if (invalidate & DkInvalidateFlags_Image)
                bump(st.barriersImageInvalidate);
            if (Deko3DGetOptions().diagBarriers)  // sasrt-nx (2026-11-08)
                bump(st.barrierWhy[mode == DkBarrier_Full ? 2 : mode == DkBarrier_Fragments ? 1 : 0][pendingBarrierWhy & 255]);
        }
        // sasrt-nx (2026-11-08, Deko3DOptions::dataOnlyInvalidate): after the wait and deko3d's invalidations, the shaders'
        // data caches alone (an inline 3D method: InvalidateShaderCaches 0x369, GlobalData bit 4 | Constant bit 12);
        // nothing when the full shader invalidate is in the flags anyway.
        const bool dataOnly = (invalidate & Deko3DDataInvalidate) && !(invalidate & DkInvalidateFlags_Shader);
        // sasrt-nx (2026-11-08, Deko3DBufferWait): the global-data cache alone when nothing above covers it.
        const bool globalOnly = (invalidate & Deko3DGlobalDataInvalidate) && !dataOnly && !(invalidate & DkInvalidateFlags_Shader);
        if (g_deko3dTrace) {
            static const char kWhy[] = "rcCdobkm";
            char t[16];
            uint32_t n = 0;
            t[n++] = 'B';
            t[n++] = mode == DkBarrier_Full ? 'X' : mode == DkBarrier_Fragments ? 'F' : mode == DkBarrier_Primitives ? 'P' : 'N';
            for (uint32_t b = 0; b < 8; b++)
                if ((pendingBarrierWhy >> b) & 1)
                    t[n++] = kWhy[b];
            if (invalidate & DkInvalidateFlags_Image)
                t[n++] = 'i';
            t[n] = 0;
            Deko3DTraceToken(t);
        }
        dkCmdBufBarrier(cmd, mode, invalidate & ~(Deko3DDataInvalidate | Deko3DGlobalDataInvalidate));
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate): a drop after the GPU's earlier work (a fragment
        // barrier's drop waits for idle, a primitives barrier waited): a new epoch; what is bound is sampled in it too.
        {
            // sasrt-nx (2026-11-08): the epochs and marks are kept whatever the options (an A/B half switching one on
            // finds them current).
            const bool drop = (invalidate & DkInvalidateFlags_Image) && mode >= DkBarrier_Fragments;
            // sasrt-nx (2026-11-08, Deko3DOptions::trackedTargetWait): every wait for the fragments or more: a new wait epoch.
            const bool wait = mode >= DkBarrier_Fragments;
            if (drop)
                g_deko3dTexDropEpoch.store(g_deko3dTexDropEpoch.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (wait)
                g_deko3dWaitEpoch.store(g_deko3dWaitEpoch.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            if (drop || wait)
                markBoundSampled();
            // sasrt-nx (2026-11-08, Deko3DOptions::trackedReadWait): the bound targets are written in the new epoch too.
            if (mode >= DkBarrier_Fragments)
                markTargetsWritten();
        }
        if (dataOnly) {
            bumpRound(8);
            const uint32_t word = (4u << 29) | (0x1010u << 16) | (0u << 13) | 0x369u;
            dkCmdBufReplayCmds(cmd, &word, 1);
        } else if (globalOnly) {
            const uint32_t word = (4u << 29) | (0x0010u << 16) | (0u << 13) | 0x369u;
            dkCmdBufReplayCmds(cmd, &word, 1);
        }
        pendingBarrier = false;
        pendingBarrierMode = DkBarrier_None;
        pendingBarrierInvalidate = 0;
        pendingBarrierWhy = 0;
        copyRunFenced = mode == DkBarrier_Full;  // port 161: see beforeBufferCopy
        // sasrt-nx (2026-11-08, leanBarrierSkip): a fragment barrier, a wait for idle or a full barrier: the fragments
        // recorded before it are done before anything after it.
        if (mode == DkBarrier_Primitives || mode == DkBarrier_Full) {
            workSinceFragmentWait = false;
            dispatchSinceIdle = false;
        } else if (mode == DkBarrier_Fragments && !dispatchSinceIdle) {
            workSinceFragmentWait = false;
        }
    }

    void Deko3DCommandList::dispatch(uint32_t x, uint32_t y, uint32_t z) {
        if (!computePipeline || !computePipeline->valid)
            return;
        flushDescriptorInvalidate();
        // Compute jobs read their uniform data when they run, so the push constants go into fresh transient memory
        // per dispatch (not the graphics stages' pipelined UBO).
        if (computePushSize) {
            void *cpu;
            DkGpuAddr gpu;
            if (transientAlloc(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT, &cpu, &gpu)) {
                memset(cpu, 0, DK_UNIFORM_BUF_ALIGNMENT);
                memcpy(cpu, computePush, computePushSize);
                computeUbo[PushUboSlot] = {gpu, DK_UNIFORM_BUF_ALIGNMENT};
                computeUboValid |= 1u << PushUboSlot;
                computeUboDirty = true;
            }
        }
        if (computeUboDirty) {
            for (uint32_t s = 0; s < 16; s++) {
                if (((computePipeline->uboMask & computeUboValid) >> s) & 1u)
                    dkCmdBufBindUniformBuffer(cmd, DkStage_Compute, s, computeUbo[s].addr, computeUbo[s].size);
            }
            computeUboDirty = false;
        }
        if (computeSsboDirty) {
            for (uint32_t s = 0; s < 16; s++) {
                if (((computePipeline->ssboMask & computeSsboValid) >> s) & 1u)
                    dkCmdBufBindStorageBuffer(cmd, DkStage_Compute, s, computeSsbo[s].addr, computeSsbo[s].size);
            }
            computeSsboDirty = false;
        }
        flushBarriers();
        if (Deko3DGetOptions().diagBarriers)
            bumpStat(GetDeko3DStats().diagDispatches);  // sasrt-nx (2026-11-08): the census
        if (g_deko3dTrace)
            Deko3DTraceToken("X");
        dispatchSinceIdle = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        dkCmdBufDispatchCompute(cmd, x, y, z);
    }

    void Deko3DCommandList::traceRays(uint32_t, uint32_t, uint32_t, RenderBufferReference, const RenderShaderBindingGroupsInfo &) {}

    // The graphics stages' constant and storage buffers each shader reads, bound for the stages that read them when
    // they changed.
    void Deko3DCommandList::bindGraphicsBuffers() {
        const Deko3DGraphicsPipeline *p = graphicsPipeline;
        if ((p->ssboMask[0] | p->ssboMask[1] | p->ssboMask[2]) & (1u << AddressWindowSlot))
            ++statWindowDraws;
        if (!graphicsBuffersDirty)  // banjo-nx (2026-10-23, batch 6)
            return;
        graphicsBuffersDirty = false;
        for (int st = 0; st < 3; st++) {
            const uint32_t stageBit = 1u << GraphicsStages[st];
            uint32_t ubo = p->uboMask[st] & graphicsUboValid;
            while (ubo) {
                const uint32_t s = uint32_t(__builtin_ctz(ubo));
                ubo &= ubo - 1;
                if (!(uboBoundStages[s] & stageBit)) {
                    dkCmdBufBindUniformBuffer(cmd, GraphicsStages[st], s, graphicsUbo[s].addr, graphicsUbo[s].size);
                    uboBoundStages[s] |= stageBit;
                }
            }
            uint32_t ssbo = p->ssboMask[st] & graphicsSsboValid;
            while (ssbo) {
                const uint32_t s = uint32_t(__builtin_ctz(ssbo));
                ssbo &= ssbo - 1;
                if (!(ssboBoundStages[s] & stageBit)) {
                    dkCmdBufBindStorageBuffer(cmd, GraphicsStages[st], s, graphicsSsbo[s].addr, graphicsSsbo[s].size);
                    ssboBoundStages[s] |= stageBit;
                }
            }
        }
    }

    // Bound textures: each graphics stage's handles from the shared constants the draw has bound (UBO binding 2), read
    // where the CPU wrote them, bound when they differ from the last ones.
    static constexpr uint32_t SharedUboSlot = 2;

    static bool g_texSkip = true;  // banjo-nx (2026-10-27): Deko3DSetTextureSkip

    void Deko3DSetTextureSkip(bool on) {
        g_texSkip = on;
    }

    void Deko3DSharedLinesWritten(RenderCommandList *commandList, uint32_t lines) {
        if (commandList)
            static_cast<Deko3DCommandList *>(commandList)->texLinesWritten |= lines;
    }

    // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate): the texture-cache drops that waited for the GPU so far
    // (+ 1: epoch 0 is "never sampled"). Commands are recorded and submitted in one order on one queue.
    std::atomic<uint64_t> g_deko3dTexDropEpoch{1};
    // sasrt-nx (2026-11-08, Deko3DOptions::trackedTargetWait): the fragment waits (or stronger) recorded so far, + 1.
    std::atomic<uint64_t> g_deko3dWaitEpoch{1};

    void Deko3DCommandList::markSampled(uint32_t id) {
        const Deko3DDescriptorSet *set = boundImageSetObj;
        if (set && id < set->imageTextures.size()) {
            if (const Deko3DTexture *t = set->imageTextures[id]) {
                t->sampledEpoch = g_deko3dTexDropEpoch.load(std::memory_order_relaxed);
                t->usedWaitEpoch = g_deko3dWaitEpoch.load(std::memory_order_relaxed);
            }
        }
    }

    void Deko3DCommandList::markTargetsWritten() {
        if (!framebuffer)
            return;
        const uint64_t now = g_deko3dWaitEpoch.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < framebuffer->colorCount; i++) {
            if (const Deko3DTexture *t = framebuffer->colorTextures[i])
                t->writtenWaitEpoch = now;
        }
        if (framebuffer->hasDepth && framebuffer->depthTexture)
            framebuffer->depthTexture->writtenWaitEpoch = now;
    }

    void Deko3DCommandList::markBoundSampled() {
        for (int st = 0; st < 3; st++) {
            for (uint32_t k = 0; k < boundTexCount[st] && k < 32; k++)
                markSampled(uint32_t(boundTex[st][k]) & 0xFFFFFu);
        }
        for (uint32_t k = 0; k < extraFragTexCount; k++)
            markSampled(extraFragTex[k]);
    }

    void Deko3DCommandList::bindTextures() {
        const Deko3DGraphicsPipeline *p = graphicsPipeline;
        if (!(graphicsUboValid & (1u << SharedUboSlot)))
            return;
        const DkGpuAddr addr = graphicsUbo[SharedUboSlot].addr;
        // banjo-nx (2026-10-27, render thread night): the words read from the renderer's copy, nothing they come from
        // changed since the last whole run (see texPipeline): the handles and sizes are those bound and pushed then.
        if (g_texSkip && sharedWords && p == texPipeline && !(texLinesWritten & p->texLines) &&
            texEpoch == descriptorEpoch && texImageSet == boundImageSetObj && texSharedAddr == addr) {
            ++statTexSkips;
            return;
        }
        ++statTexRuns;
        // banjo-nx (2026-10-19, render-thread proposal 5): pushed shared constants: the renderer's copy.
        if (sharedWords) {
            sharedCpu = sharedWords;
            sharedBase = addr;
            sharedSize = sharedWordsSize;
        } else if (!sharedCpu || addr < sharedBase || addr >= sharedBase + sharedSize) {
            if (!device->findBuffer(addr, sharedBase, sharedSize, sharedCpu)) {
                sharedCpu = nullptr;
                static std::atomic<uint32_t> logged{0};
                if (logged.fetch_add(1) < 8)
                    logf("bound textures: no buffer holds the shared constants at 0x%" PRIx64, addr);
                return;
            }
        }
        const uint8_t *words = sharedCpu + (addr - sharedBase);
        const uint32_t limit = uint32_t(sharedBase + sharedSize - addr);
        for (int st = 0; st < 3; st++) {
            const auto &bindings = p->texBindings[st];
            if (bindings.empty())
                continue;
            DkResHandle handles[32];
            const uint32_t n = std::min<uint32_t>(uint32_t(bindings.size()), 32);
            bool same = n == boundTexCount[st];
            for (uint32_t k = 0; k < n; k++) {
                const Deko3DShaderUsage::TexBinding &b = bindings[k];
                uint32_t texture = 0, sampler = 0;
                if (b.textureWord + 4u <= limit)
                    memcpy(&texture, words + b.textureWord, 4);
                if (b.samplerWord != Deko3DShaderUsage::SamplerNone && b.samplerWord + 4u <= limit)
                    memcpy(&sampler, words + b.samplerWord, 4);
                handles[k] = (texture & 0xFFFFFu) | ((sampler & 0xFFF00000u) + b.samplerVariant);  // vivapinata-nx: the variant
                same = same && handles[k] == boundTex[st][k];
            }
            // banjo-nx (2026-10-19, GPU, txq): the sizes of the bindings the shader reads from the texture size
            // buffer (entry k for the pixel stage, 32 + k for the vertex stage), updated where they changed. Also when
            // the handles are the same: a descriptor slot may hold another texture now.
            if (p->txqMask[st]) {
                const uint32_t base = st == 2 ? 0u : 32u;
                // banjo-nx (2026-10-28, per-draw night): the changed entries gathered into runs of consecutive ones,
                // each run one push (the shadow holds what was pushed; an entry not changed ends a run).
                uint32_t run[32][4];
                uint32_t run_first = 0, run_count = 0;
                const auto flush = [&] {
                    if (run_count) {
                        dkCmdBufPushConstants(cmd, device->texSizeAddr, Deko3DDevice::TexSizeSize, run_first * 16,
                                              run_count * 16, run);
                        run_count = 0;
                    }
                };
                for (uint32_t mask = p->txqMask[st]; mask;) {
                    const uint32_t k = uint32_t(__builtin_ctz(mask));
                    mask &= mask - 1;
                    if (k >= n || k >= 32)
                        continue;
                    const uint32_t id = handles[k] & 0xFFFFFu;
                    uint32_t size[4] = {0, 0, 0, 0};
                    if (boundImageSetObj && id < boundImageSetObj->imageSizes.size()) {
                        const auto &dims = boundImageSetObj->imageSizes[id];
                        size[0] = dims[0];
                        size[1] = dims[1];
                        size[2] = dims[2];
                    }
                    const uint32_t e = base + k;
                    if (((texSizeValid >> e) & 1) && !memcmp(texSizeShadow[e], size, sizeof(size))) {
                        flush();
                        continue;
                    }
                    if (run_count && e != run_first + run_count)
                        flush();
                    if (!run_count)
                        run_first = e;
                    memcpy(run[run_count++], size, sizeof(size));
                    memcpy(texSizeShadow[e], size, sizeof(size));
                    texSizeValid |= uint64_t(1) << e;
                }
                flush();
            }
            if (same)
                continue;
            // banjo-nx (2026-10-28, per-draw night): with the stage's count as bound, only the handles from the first
            // to the last that differ (each bind is a selector, a no-op, a header and the handles into the driver
            // constant buffer; a draw changing one or two textures rewrote all of them). The buffer ends the same.
            uint32_t first = 0, end = n;
            if (n == boundTexCount[st]) {
                while (first < n && handles[first] == boundTex[st][first])
                    first++;
                while (end > first && handles[end - 1] == boundTex[st][end - 1])
                    end--;
            }
            dkCmdBufBindTextures(cmd, GraphicsStages[st], first, handles + first, end - first);
            memcpy(boundTex[st] + first, handles + first, (end - first) * sizeof(DkResHandle));
            boundTexCount[st] = n;
            // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate): the ones bound now are sampled in this epoch
            // (those bound before were marked at their bind or at the last drop).
            {  // sasrt-nx (2026-11-08): kept whatever the options (an A/B half switching one on finds the marks current)
                for (uint32_t k = first; k < end; k++)
                    markSampled(uint32_t(handles[k]) & 0xFFFFFu);
                if (st == 2)
                    extraFragTexCount = 0;  // the fragment stage bound whole: Deko3DBindFragmentTextures' ids replaced
            }
        }
        // banjo-nx (2026-10-27): what this run read (only from the renderer's copy: a ring upload's words are elsewhere).
        if (sharedWords) {
            texPipeline = p;
            texEpoch = descriptorEpoch;
            texImageSet = boundImageSetObj;
            texSharedAddr = addr;
            texLinesWritten = 0;
        } else {
            texPipeline = nullptr;
        }
    }

    void Deko3DCommandList::noteUserClip() {
        if (boundUserClip < 0)
            return;
        const int want = graphicsPipeline->noUserClip ? 0 : Deko3DGetOptions().userClipEnable;
        if (want == boundUserClip || want < 0)
            return;
        const uint32_t words[2] = {(1u << 29) | (1u << 16) | 0x544u, uint32_t(want)};
        dkCmdBufReplayCmds(cmd, words, 2);
        boundUserClip = want;
    }

    void Deko3DCommandList::drawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex, uint32_t startInstance) {
        if (!graphicsPipeline || !graphicsPipeline->valid)
            return;
        flushDescriptorInvalidate();
        bindGraphicsBuffers();
        if (graphicsPipeline->boundTextures)
            bindTextures();
        flushBarriers();
        noteDepthWrite();  // sasrt-nx (2026-11-08, preciseDepthResolve)
        noteUserClip();  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
        if (g_deko3dTrace)
            Deko3DTraceToken("D");
        const bool discard = discardNextDraw;  // sasrt-nx (2026-11-08, sasrt_gpu_export_raster_discard)
        discardNextDraw = false;
        if (discard) {
            const uint32_t off = (4u << 29) | (0u << 16) | (0u << 13) | 0x0DFu;
            dkCmdBufReplayCmds(cmd, &off, 1);
        }
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        // sasrt-nx (2026-11-08, sasrt_gpu_export_list_topology): Deko3DSetNextPrimitive's, once
        const DkPrimitive drawPrim = nextPrimitive ? DkPrimitive(nextPrimitive - 1) : graphicsPipeline->primitive;
        nextPrimitive = 0;
        dkCmdBufDraw(cmd, drawPrim, vertexCount, instanceCount, startVertex, startInstance);
        if (discard) {
            const uint32_t on = (4u << 29) | (1u << 16) | (0u << 13) | 0x0DFu;
            dkCmdBufReplayCmds(cmd, &on, 1);
        }
        ++statDraws;
    }

    void Deko3DCommandList::drawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex, int32_t baseVertex, uint32_t startInstance) {
        if (!graphicsPipeline || !graphicsPipeline->valid || !indexAddress)
            return;
        flushDescriptorInvalidate();
        bindGraphicsBuffers();
        if (graphicsPipeline->boundTextures)
            bindTextures();
        flushBarriers();
        // sasrt-nx (2026-10-30, Deko3DOptions::stripRestart): a strip cut at the all-ones index, as on Vulkan.
        // sasrt-nx (2026-11-08, sasrt_gpu_export_list_topology): Deko3DSetNextPrimitive's, once
        const DkPrimitive prim = nextPrimitive ? DkPrimitive(nextPrimitive - 1) : graphicsPipeline->primitive;
        nextPrimitive = 0;
        uint32_t restart = 0;
        if ((prim == DkPrimitive_TriangleStrip || prim == DkPrimitive_LineStrip) && Deko3DGetOptions().stripRestart) {
            restart = boundIndexFormat == DkIdxFormat_Uint32 ? 0xFFFFFFFFu : boundIndexFormat == DkIdxFormat_Uint8 ? 0xFFu : 0xFFFFu;
            ++statStripRestarts;
        }
        if (restart != boundRestart) {
            dkCmdBufSetPrimitiveRestart(cmd, restart != 0, restart);
            boundRestart = restart;
        }
        noteDepthWrite();  // sasrt-nx (2026-11-08, preciseDepthResolve)
        noteUserClip();  // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec)
        if (g_deko3dTrace)
            Deko3DTraceToken("D");
        const bool discard = discardNextDraw;  // sasrt-nx (2026-11-08, sasrt_gpu_export_raster_discard)
        discardNextDraw = false;
        if (discard) {
            const uint32_t off = (4u << 29) | (0u << 16) | (0u << 13) | 0x0DFu;
            dkCmdBufReplayCmds(cmd, &off, 1);
        }
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        dkCmdBufDrawIndexed(cmd, prim, indexCount, instanceCount, startIndex, baseVertex, startInstance);
        if (discard) {
            const uint32_t on = (4u << 29) | (1u << 16) | (0u << 13) | 0x0DFu;
            dkCmdBufReplayCmds(cmd, &on, 1);
        }
        ++statDraws;
    }

    void Deko3DCommandList::setPipeline(const RenderPipeline *pipeline) {
        const Deko3DPipeline *base = static_cast<const Deko3DPipeline *>(pipeline);
        if (base == nullptr)
            return;
        if (base->type == Deko3DPipeline::Type::Compute) {
            const Deko3DComputePipeline *p = static_cast<const Deko3DComputePipeline *>(base);
            if (p != computePipeline && p->valid) {
                dkCmdBufBindShaders(cmd, DkStageFlag_Compute, &p->shader, 1);
                computeUboDirty = computeSsboDirty = true;
            }
            computePipeline = p;
            return;
        }
        const Deko3DGraphicsPipeline *p = static_cast<const Deko3DGraphicsPipeline *>(base);
        // banjo-nx (2026-10-21, asyncVariants): the same pipeline again binds its shaders when a stage changed since
        // (Deko3DRefreshPipeline: its own variant done).
        if (p == graphicsPipeline && (!p->valid || (p->stageShaders[0] == boundShaders[0] &&
                                                    p->stageShaders[1] == boundShaders[1] &&
                                                    p->stageShaders[2] == boundShaders[2])))
            return;
        graphicsPipeline = p;
        graphicsBuffersDirty = true;  // banjo-nx (2026-10-23, batch 6): its masks
        if (!p->valid)
            return;
        // banjo-nx (2026-10-27, deko3d recording): the texture bindings' arrays (heap memory) bindTextures reads at
        // the draw, while the shaders and the state groups are bound.
        if (p->boundTextures) {
            for (int st = 0; st < 3; st++) {
                if (!p->texBindings[st].empty())
                    __builtin_prefetch(p->texBindings[st].data());
            }
        }
        // banjo-nx (2026-10-19): a load and a store (a lost count is possible between two recording threads), not
        // an exclusive read-modify-write loop: the A57 has no LSE atomics.
        auto bump = [](std::atomic<uint64_t> &c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); };
        bump(GetDeko3DStats().pipelineBinds);
        // Shaders: rebound when any stage differs (absent stages are disabled by the bind).
        // banjo-nx (2026-10-28, per-draw night): only the stages that differ (deko3d binds each stage on its own: its
        // program, its compiler constant buffer, the fragment stage's state; a stage in the mask without a shader is
        // disabled). The list's first bind binds them all with the whole graphics mask, as every bind did before.
        const DkShader *const want[3] = {p->stageShaders[0], p->stageShaders[1], p->stageShaders[2]};
        if (want[0] != boundShaders[0] || want[1] != boundShaders[1] || want[2] != boundShaders[2]) {
            if (boundShaders[0] == kDeko3DShadersUnknown) {
                dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, p->shaders, p->shaderCount);
            } else {
                static constexpr uint32_t kStageFlags[3] = {DkStageFlag_Vertex, DkStageFlag_Geometry, DkStageFlag_Fragment};
                const DkShader *list[3];
                uint32_t count = 0, mask = 0;
                for (int st = 0; st < 3; st++) {
                    if (want[st] != boundShaders[st]) {
                        mask |= kStageFlags[st];
                        if (want[st])
                            list[count++] = want[st];
                    }
                }
                dkCmdBufBindShaders(cmd, mask, list, count);
            }
            boundShaders[0] = want[0];
            boundShaders[1] = want[1];
            boundShaders[2] = want[2];
            // A new program in a stage reads its constant buffers from the stage's bindings, which stay; nothing to
            // rebind.
        }
        if (p->stateHash != boundStateHash) {
            bump(GetDeko3DStats().stateBinds);
            // banjo-nx (2026-10-28, ARM audit): the lines of the groups that differ asked for together first (the
            // binds below read them one after the other, past the pipeline's hot block: a miss each; the c4 console
            // profile's hottest setPipeline loads were the vertex input counts and the blend words).
            {
                uint32_t differ = 0;
                for (uint32_t g = 0; g < 9; g++)
                    differ |= uint32_t(boundGroups[g] != p->groupHash[g]) << g;
                const auto fetch = [](const void *from, size_t bytes) {
                    const char *c = static_cast<const char *>(from);
                    for (size_t at = 0; at < bytes; at += 64)
                        __builtin_prefetch(c + at);
                    __builtin_prefetch(c + bytes - 1);
                };
                if (differ & 0xF)
                    fetch(&p->rasterizer, sizeof(p->rasterizer) + sizeof(p->multisample) + sizeof(p->color) +
                                              sizeof(p->colorWrite));
                if ((differ & 0x10) && p->blendCount) {
                    fetch(&p->blend[0], sizeof(p->blend[0]) * p->blendCount);
                    fetch(&p->blendWords[0], sizeof(p->blendWords[0]) * p->blendCount);
                }
                if (differ & 0xE0)
                    fetch(&p->depthStencil, size_t(reinterpret_cast<const char *>(&p->attribs[0]) -
                                                   reinterpret_cast<const char *>(&p->depthStencil)));
                if (differ & 0x100) {
                    fetch(&p->attribs[0], sizeof(p->attribs) + sizeof(p->attribCount));
                    fetch(&p->vtxBuffers[0], sizeof(p->vtxBuffers) + sizeof(p->vtxBufferCount));
                }
            }
            // banjo-nx (2026-10-19, CPU proposal 13): only the groups whose state differs from what is bound (a group
            // the GPU already holds is a redundant command). The stencil values and the depth bias are left as they
            // are by a pipeline that does not set them (no stencil, a dynamic bias), as before; the dynamic setters
            // make their groups unknown, so the next pipeline that sets them does.
            auto changed = [&](uint32_t g) {
                if (boundGroups[g] == p->groupHash[g])
                    return false;
                boundGroups[g] = p->groupHash[g];
                return true;
            };
            if (changed(0))
                dkCmdBufBindRasterizerState(cmd, &p->rasterizer);
            if (changed(1))
                dkCmdBufBindMultisampleState(cmd, &p->multisample);
            if (changed(2))
                dkCmdBufBindColorState(cmd, &p->color);
            if (changed(3))
                dkCmdBufBindColorWriteState(cmd, &p->colorWrite);
            if (p->blendCount && changed(4)) {
                dkCmdBufBindBlendStates(cmd, 0, p->blend, p->blendCount);
                // deko3d 0.5.0's FuncAlphaDst (see blendWords): the six registers of each target again.
                for (uint32_t i = 0; i < p->blendCount; i++)
                    dkCmdBufReplayCmds(cmd, p->blendWords[i], 7);
            }
            if (changed(5))
                dkCmdBufBindDepthStencilState(cmd, &p->depthStencil);
            if (p->stencilEnabled && changed(6))
                dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, p->stencilWriteMask, p->stencilReference, p->stencilReadMask);
            if (!p->dynamicDepthBias && changed(7))
                dkCmdBufSetDepthBias(cmd, p->depthBias, p->depthBiasClamp, p->slopeScaledDepthBias);
            // Always, also with none: deko3d makes the attributes after the last one given constants and disables the
            // buffers after the last one, so a pipeline without vertex input leaves no earlier attribute fetching from
            // a buffer that may be gone. (banjo-nx: with the same attributes and buffers as bound, nothing to do.)
            if (!Deko3DGetOptions().vsInputMask && changed(8)) {
                dkCmdBufBindVtxAttribState(cmd, p->attribs, p->attribCount);
                dkCmdBufBindVtxBufferState(cmd, p->vtxBuffers, p->vtxBufferCount);
            }
            boundStateHash = p->stateHash;
        }
        // sasrt-nx (2026-11-08, Deko3DOptions::vsInputMask): after the state groups (whose hash leaves the vertex variant
        // out): the same pipeline can be bound with another variant of its vertex shader, which reads other attributes.
        if (Deko3DGetOptions().vsInputMask)
            bindVertexInput(p, want[0]);
    }

    void Deko3DCommandList::bindVertexInput(const Deko3DGraphicsPipeline *p, const DkShader *vs) {
        static_assert(offsetof(Deko3DShaderVariant, shader) == 0, "a pipeline's DkShader * is its variant's address");
        const uint32_t used = vs ? reinterpret_cast<const Deko3DShaderVariant *>(vs)->vsInputs : ~0u;
        const uint32_t drop = p->fetchedAttribs & ~used;
        // The group's hash with the dropped attributes in it (0 dropped: the pipeline's own, as changed(8) has it).
        const uint64_t h = drop ? p->groupHash[8] ^ ((uint64_t(drop) + 1) * 0x9E3779B97F4A7C15ull) : p->groupHash[8];
        if (boundGroups[8] == h)
            return;
        boundGroups[8] = h;
        if (!drop) {
            dkCmdBufBindVtxAttribState(cmd, p->attribs, p->attribCount);
        } else {
            DkVtxAttribState a[32];
            memcpy(a, p->attribs, sizeof(DkVtxAttribState) * p->attribCount);
            for (uint32_t m = drop; m; m &= m - 1) {
                DkVtxAttribState &st = a[__builtin_ctz(m)];
                memset(&st, 0, sizeof(st));
                st.isFixed = 1;  // a constant (zero), as an attribute no element feeds
                st.size = DkVtxAttribSize_1x32;
                st.type = DkVtxAttribType_Float;
            }
            dkCmdBufBindVtxAttribState(cmd, a, p->attribCount);
            bumpRound(32);  // sasrt::gpu_round::kVsInputMask: binds with attributes left unfetched
        }
        dkCmdBufBindVtxBufferState(cmd, p->vtxBuffers, p->vtxBufferCount);
    }

    void Deko3DCommandList::setComputePipelineLayout(const RenderPipelineLayout *pipelineLayout) {
        computeLayout = static_cast<const Deko3DPipelineLayout *>(pipelineLayout);
    }

    void Deko3DCommandList::setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) {
        if (!computeLayout || rangeIndex >= computeLayout->pushRanges.size())
            return;
        const RenderPushConstantRange &r = computeLayout->pushRanges[rangeIndex];
        if (size == 0)
            size = r.size;
        const uint32_t at = r.offset + offset;
        if (at + size > sizeof(computePush))
            return;
        memcpy(computePush + at, data, size);
        computePushSize = std::max(computePushSize, at + size);
    }

    // A descriptor set's tables and buffers into the bindings: the image and sampler tables are deko3d's descriptor
    // sets (one of each is bound at a time, for every stage), the buffers go to UBO / SSBO slots by the layout.
    static void bindSetTables(Deko3DCommandList &cl, const Deko3DDescriptorSet *set) {
        if (set->imageBlock) {
            const DkGpuAddr addr = dkMemBlockGetGpuAddr(set->imageBlock);
            if (addr != cl.boundImageSet || set->imageCount != cl.boundImageCount) {
                dkCmdBufBindImageDescriptorSet(cl.cmd, addr, set->imageCount);
                cl.boundImageSet = addr;
                cl.boundImageCount = set->imageCount;
            }
            cl.boundImageSetObj = set;  // banjo-nx (2026-10-19, txq): its sizes
        }
        if (set->samplerBlock) {
            const DkGpuAddr addr = dkMemBlockGetGpuAddr(set->samplerBlock);
            if (addr != cl.boundSamplerSet || set->samplerCount != cl.boundSamplerCount) {
                dkCmdBufBindSamplerDescriptorSet(cl.cmd, addr, set->samplerCount);
                cl.boundSamplerSet = addr;
                cl.boundSamplerCount = set->samplerCount;
            }
        }
    }

    static bool isConstantRange(RenderDescriptorRangeType t) {
        return t == RenderDescriptorRangeType::CONSTANT_BUFFER || t == RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC;
    }

    static bool isStorageRange(RenderDescriptorRangeType t) {
        switch (t) {
        case RenderDescriptorRangeType::STRUCTURED_BUFFER:
        case RenderDescriptorRangeType::READ_WRITE_STRUCTURED_BUFFER:
        case RenderDescriptorRangeType::BYTE_ADDRESS_BUFFER:
        case RenderDescriptorRangeType::READ_WRITE_BYTE_ADDRESS_BUFFER:
        case RenderDescriptorRangeType::FORMATTED_BUFFER:
        case RenderDescriptorRangeType::READ_WRITE_FORMATTED_BUFFER:
            return true;
        default:
            return false;
        }
    }

    static void bindSetBuffers(const Deko3DPipelineLayout *layout, uint32_t setIndex, const Deko3DDescriptorSet *set,
                               const uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount,
                               DkBufExtents *ubo, uint32_t &uboValid, uint32_t *uboBound,
                               DkBufExtents *ssbo, uint32_t &ssboValid, uint32_t *ssboBound) {
        if (!layout || setIndex >= layout->sets.size() || set->buffers.empty())
            return;
        uint32_t dynamicIndex = 0;
        for (const Deko3DPipelineLayout::Range &r : layout->sets[setIndex].ranges) {
            const bool constant = isConstantRange(r.type);
            const bool storage = isStorageRange(r.type);
            if (!constant && !storage)
                continue;
            for (uint32_t i = 0; i < r.count; i++) {
                const uint32_t d = r.descriptorIndex + i;
                const uint32_t slot = r.slot + i;
                if (d >= set->buffers.size() || slot >= 16)
                    break;
                DkBufExtents e = set->buffers[d];
                if (r.type == RenderDescriptorRangeType::CONSTANT_BUFFER_DYNAMIC && dynamicIndex < dynamicOffsetsCount)
                    e.addr += dynamicOffsets[dynamicIndex++];
                DkBufExtents *dst = constant ? ubo : ssbo;
                uint32_t &valid = constant ? uboValid : ssboValid;
                uint32_t *bound = constant ? uboBound : ssboBound;
                if (!(valid & (1u << slot)) || dst[slot].addr != e.addr || dst[slot].size != e.size) {
                    dst[slot] = e;
                    valid |= 1u << slot;
                    if (bound)
                        bound[slot] = 0;
                }
            }
        }
    }

    void Deko3DCommandList::setComputeDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) {
        const Deko3DDescriptorSet *set = static_cast<const Deko3DDescriptorSet *>(descriptorSet);
        if (!set)
            return;
        bindSetTables(*this, set);
        const uint32_t uboBefore = computeUboValid, ssboBefore = computeSsboValid;
        DkBufExtents uboOld[16], ssboOld[16];
        memcpy(uboOld, computeUbo, sizeof(uboOld));
        memcpy(ssboOld, computeSsbo, sizeof(ssboOld));
        bindSetBuffers(computeLayout, setIndex, set, nullptr, 0, computeUbo, computeUboValid, nullptr, computeSsbo, computeSsboValid, nullptr);
        if (computeUboValid != uboBefore || memcmp(uboOld, computeUbo, sizeof(uboOld)) != 0)
            computeUboDirty = true;
        if (computeSsboValid != ssboBefore || memcmp(ssboOld, computeSsbo, sizeof(ssboOld)) != 0)
            computeSsboDirty = true;
    }

    void Deko3DCommandList::setGraphicsPipelineLayout(const RenderPipelineLayout *pipelineLayout) {
        graphicsLayout = static_cast<const Deko3DPipelineLayout *>(pipelineLayout);
    }

    void Deko3DCommandList::setGraphicsPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) {
        if (!graphicsLayout || rangeIndex >= graphicsLayout->pushRanges.size())
            return;
        const RenderPushConstantRange &r = graphicsLayout->pushRanges[rangeIndex];
        if (size == 0)
            size = r.size;
        const uint32_t at = r.offset + offset;
        if (at + size > Deko3DDevice::PushSize)
            return;
        // Pipelined: draws before this one keep the old values (deko3d's push constants).
        dkCmdBufPushConstants(cmd, device->pushAddr, Deko3DDevice::PushSize, at, alignUp(size, 4), data);
    }

    void Deko3DCommandList::setGraphicsDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) {
        const Deko3DDescriptorSet *set = static_cast<const Deko3DDescriptorSet *>(descriptorSet);
        if (!set)
            return;
        bindSetTables(*this, set);
        bindSetBuffers(graphicsLayout, setIndex, set, nullptr, 0, graphicsUbo, graphicsUboValid, uboBoundStages, graphicsSsbo, graphicsSsboValid, ssboBoundStages);
        graphicsBuffersDirty = true;  // banjo-nx (2026-10-23, batch 6)
    }

    void Deko3DCommandList::setGraphicsDescriptorSetDynamic(RenderDescriptorSet *descriptorSet, uint32_t setIndex, const uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) {
        const Deko3DDescriptorSet *set = static_cast<const Deko3DDescriptorSet *>(descriptorSet);
        if (!set)
            return;
        bindSetTables(*this, set);
        bindSetBuffers(graphicsLayout, setIndex, set, dynamicOffsets, dynamicOffsetsCount, graphicsUbo, graphicsUboValid, uboBoundStages, graphicsSsbo, graphicsSsboValid, ssboBoundStages);
        graphicsBuffersDirty = true;  // banjo-nx (2026-10-23, batch 6)
    }

    void Deko3DCommandList::setGraphicsRootDescriptor(RenderBufferReference, uint32_t) {}
    void Deko3DCommandList::setRaytracingPipelineLayout(const RenderPipelineLayout *) {}
    void Deko3DCommandList::setRaytracingPushConstants(uint32_t, const void *, uint32_t, uint32_t) {}
    void Deko3DCommandList::setRaytracingDescriptorSet(RenderDescriptorSet *, uint32_t) {}

    void Deko3DCommandList::setIndexBuffer(const RenderIndexBufferView *view) {
        if (!view || !view->buffer.ref) {
            indexAddress = 0;
            return;
        }
        const Deko3DBuffer *b = static_cast<const Deko3DBuffer *>(view->buffer.ref);
        indexFormat = view->format == RenderFormat::R32_UINT ? DkIdxFormat_Uint32 : view->format == RenderFormat::R8_UINT ? DkIdxFormat_Uint8 : DkIdxFormat_Uint16;
        const DkGpuAddr addr = b->gpu + view->buffer.offset;
        if (addr != indexAddress || indexFormat != boundIndexFormat) {
            dkCmdBufBindIdxBuffer(cmd, indexFormat, addr);
            indexAddress = addr;
            boundIndexFormat = indexFormat;
        }
    }

    void Deko3DCommandList::setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView *views, uint32_t viewCount, const RenderInputSlot *inputSlots) {
        // The strides are the pipeline's (as the Vulkan back end, which ignores inputSlots); a missing buffer reads the
        // device's zero buffer, as Vulkan's null buffer.
        (void)inputSlots;
        DkBufExtents extents[16];
        const uint32_t count = std::min<uint32_t>(viewCount, 16 - std::min<uint32_t>(startSlot, 16));
        for (uint32_t i = 0; i < count; i++) {
            const Deko3DBuffer *b = static_cast<const Deko3DBuffer *>(views[i].buffer.ref);
            extents[i] = (b && b->block) ? DkBufExtents{b->gpu + views[i].buffer.offset, std::max<uint32_t>(views[i].size, 4)}
                                         : DkBufExtents{device->zeroAddr, Deko3DDevice::ZeroSize};
        }
        // banjo-nx (2026-10-26, Unleashed-style step 2): only the slots whose range differs from the one this list bound
        // there last (runs of consecutive ones in one call).
        if (!g_vtxDedupe) {
            if (count)
                dkCmdBufBindVtxBuffers(cmd, startSlot, extents, count);
            return;
        }
        for (uint32_t i = 0; i < count;) {
            const uint32_t slot = startSlot + i;
            if (((boundVtxValid >> slot) & 1) && boundVtx[slot].addr == extents[i].addr && boundVtx[slot].size == extents[i].size) {
                ++i;
                continue;
            }
            uint32_t n = 0;
            while (i + n < count) {
                const uint32_t s = startSlot + i + n;
                if (((boundVtxValid >> s) & 1) && boundVtx[s].addr == extents[i + n].addr && boundVtx[s].size == extents[i + n].size)
                    break;
                boundVtx[s] = extents[i + n];
                boundVtxValid |= 1u << s;
                ++n;
            }
            dkCmdBufBindVtxBuffers(cmd, slot, extents + i, n);
            i += n;
        }
    }

    // ViewportTransform registers (method 0x280 + 8 * viewport): ScaleY at +1, TranslateY at +4.
    static uint32_t floatBits(float f) {
        uint32_t u;
        memcpy(&u, &f, sizeof(u));
        return u;
    }

    void Deko3DCommandList::setViewports(const RenderViewport *vps, uint32_t count) {
        count = std::min<uint32_t>(count, 16);
        bool flipped = false;
        for (uint32_t i = 0; i < count; i++) {
            const RenderViewport &v = vps[i];
            DkViewport &d = viewports[i];
            // A negative height is a flipped viewport (D3D's and Vulkan's); deko3d derives its clip rectangle from the
            // height, so it gets the positive one and the transform's y is written after (below).
            d.x = v.x;
            d.width = v.width;
            d.y = v.height < 0.0f ? v.y + v.height : v.y;
            d.height = std::fabs(v.height);
            d.near = v.minDepth;
            d.far = v.maxDepth;
            flipped = flipped || v.height < 0.0f;
        }
        viewportCount = count;
        dkCmdBufSetViewports(cmd, 0, viewports, count);
        if (flipped) {
            for (uint32_t i = 0; i < count; i++) {
                if (vps[i].height >= 0.0f)
                    continue;
                // deko3d's transform with the upper-left origin: scaleY = -h/2, translateY = y + h/2 (y and h of the
                // positive viewport). The flipped one mirrors it: +h/2 around the same centre.
                const float half = 0.5f * viewports[i].height;
                const uint32_t words[2] = {(1u << 29) | (1u << 16) | (0x281u + 8u * i), floatBits(half)};
                dkCmdBufReplayCmds(cmd, words, 2);
            }
        }
    }

    void Deko3DCommandList::setScissors(const RenderRect *rects, uint32_t count) {
        count = std::min<uint32_t>(count, 16);
        for (uint32_t i = 0; i < count; i++) {
            const RenderRect &r = rects[i];
            DkScissor &s = scissors[i];
            s.x = uint32_t(std::max(r.left, 0));
            s.y = uint32_t(std::max(r.top, 0));
            s.width = uint32_t(std::max(r.right - std::max(r.left, 0), 0));
            s.height = uint32_t(std::max(r.bottom - std::max(r.top, 0), 0));
        }
        scissorCount = count;
        dkCmdBufSetScissors(cmd, 0, scissors, count);
    }

    void Deko3DCommandList::restoreScissors() {
        if (scissorCount)
            dkCmdBufSetScissors(cmd, 0, scissors, scissorCount);
        else {
            const DkScissor all = {0, 0, 16384, 16384};
            dkCmdBufSetScissors(cmd, 0, &all, 1);
        }
    }

    // The framebuffer sRGB register (Maxwell 0x56E) deko3d 0.5.0 never writes (SwitchWakerHD).
    static void writeFramebufferSrgb(DkCmdBuf cmd, bool srgb) {
        const uint32_t words[2] = {(1u << 29) | (1u << 16) | 0x56Eu, srgb ? 1u : 0u};
        dkCmdBufReplayCmds(cmd, words, 2);
    }

    void Deko3DCommandList::setFramebuffer(const RenderFramebuffer *fb) {
        // Bound every time: an application can free a framebuffer and make another at the same address.
        const Deko3DFramebuffer *f = static_cast<const Deko3DFramebuffer *>(fb);
        // sasrt-nx (2026-10-21, Deko3DOptions::depthResolve): a compressed depth target leaving the bind point is
        // decompressed while it is still bound (its read's barrier may already be recorded: one follows).
        if (depthDirty) {
            const DkImageView *was = framebuffer && framebuffer->hasDepth ? &framebuffer->depth : nullptr;
            const DkImageView *next = f && f->hasDepth ? &f->depth : nullptr;
            if (!was || !next || was->pImage != next->pImage || was->layerOffset != next->layerOffset ||
                was->mipLevelOffset != next->mipLevelOffset) {
                // renut-ultimate (2026-11-08, depthResolveLazy): left for the read that needs it, unless the same image
                // is bound next through another view (one pending view an image).
                if (Deko3DGetOptions().depthResolveLazy && was && framebuffer->depthTexture &&
                    !(next && was->pImage == next->pImage))
                    deferBoundDepth();
                // sasrt-nx (2026-11-08, lazyDepthResolve): its texture keeps depthUnresolved; its read decompresses it.
                else if (framebuffer && lazyResolvable(framebuffer->depthTexture) && framebuffer->depthTexture->depthUnresolved)
                    depthDirty = false;
                else
                    resolveBoundDepth(true);
            }
        }
        // renut-ultimate (2026-11-08, depthResolveLazy): a target left for later and bound again: through the view it was
        // written through, the bound target's tracking takes it over; through another, it is decompressed first.
        if (f && f->hasDepth && f->depthTexture && f->depthTexture->depthPending) {
            const DkImageView &pv = f->depthTexture->depthPendingView;
            if (pv.layerOffset != f->depth.layerOffset || pv.mipLevelOffset != f->depth.mipLevelOffset) {
                resolvePendingDepth(f->depthTexture, false);
                if (Deko3DGetOptions().mergeBarriers) {
                    pendingBarrierMode = pendingBarrier ? std::max(pendingBarrierMode, DkBarrier_Fragments) : DkBarrier_Fragments;
                    pendingBarrierInvalidate |= DkInvalidateFlags_Image;
                    pendingBarrier = true;
                }
                else
                    emitBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
            }
            else {
                f->depthTexture->depthPending = false;
                depthDirty = true;
            }
        }
        if (Deko3DGetOptions().diagBarriers) {  // sasrt-nx (2026-11-08): the census
            bumpStat(GetDeko3DStats().diagFramebufferBinds);
            const DkImage *was = framebuffer && framebuffer->hasDepth ? framebuffer->depth.pImage : nullptr;
            const DkImage *next = f && f->hasDepth ? f->depth.pImage : nullptr;
            if (next && was != next)
                bumpStat(GetDeko3DStats().diagDepthChanges);
        }
        if (g_deko3dTrace) {
            char t[32];
            snprintf(t, sizeof(t), "T%u:%u", f && f->colorCount ? traceImage(f->colors[0].pImage) : 0u,
                     f && f->hasDepth ? traceImage(f->depth.pImage) : 0u);
            Deko3DTraceToken(t);
        }
        framebuffer = f;
        markTargetsWritten();  // sasrt-nx (2026-11-08, trackedReadWait): its targets are written from now (always kept)
        if (!f)
            return;
        // sasrt-nx (2026-11-08, preciseDepthResolve): an image known decompressed since its last write is not dirty.
        if (!depthDirty && f->hasDepth && f->depthCompressed && Deko3DGetOptions().depthResolve) {
            if (!Deko3DGetOptions().preciseDepthResolve || !f->depthTexture || f->depthTexture->depthUnresolved)
                depthDirty = true;
            else
                bumpRound(1);
        }
#if defined(PLUME_DEKO3D_GUARDS)
        if (g_traceTargets.fetch_add(1) < 40)
            logf("trace targets: %u colour (%ux%u, format %d%s), depth %s", f->colorCount, f->width, f->height,
                 f->colorCount ? int(f->colors[0].format ? f->colors[0].format : f->colors[0].pImage ? 0 : -1) : -1,
                 f->colorCount && f->srgb[0] ? ", sRGB" : "", f->hasDepth ? "yes" : "no");
#endif
        // sasrt-nx (2026-11-08, Deko3DOptions::sameTargetsKept): the GPU has these targets bound already.
        if (Deko3DGetOptions().sameTargetsKept && f->serial == boundTargetsSerial) {
            bumpRound(20);
            return;
        }
        const DkImageView *colors[8];
        for (uint32_t i = 0; i < f->colorCount; i++)
            colors[i] = &f->colors[i];
        flushBarriers();
        dkCmdBufBindRenderTargets(cmd, colors, f->colorCount, f->hasDepth ? &f->depth : nullptr);
        writeFramebufferSrgb(cmd, f->colorCount && f->srgb[0]);
        boundTargetsSerial = f->serial;
    }

    void Deko3DCommandList::setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) {
        dkCmdBufSetDepthBias(cmd, depthBias, depthBiasClamp, slopeScaledDepthBias);
        boundGroups[7] = 0;  // banjo-nx (2026-10-19): unknown for the next pipeline that sets a bias
    }

    void Deko3DCommandList::setStencilReference(uint32_t reference) {
        const Deko3DGraphicsPipeline *p = graphicsPipeline;
        dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, p ? p->stencilWriteMask : 0xFF, uint8_t(reference), p ? p->stencilReadMask : 0xFF);
        boundGroups[6] = 0;  // banjo-nx (2026-10-19): unknown for the next pipeline that sets the stencil values
    }

    void Deko3DCommandList::clearColor(uint32_t attachmentIndex, RenderColor color, const RenderRect *rects, uint32_t rectsCount) {
#if defined(PLUME_DEKO3D_GUARDS)
        if (g_traceClears.fetch_add(1) < 40) {
            const Deko3DFramebuffer *f = framebuffer;
            logf("trace clear: target %u of %u (%ux%u, image format %d) to (%.3f %.3f %.3f %.3f), %u rects%s", attachmentIndex,
                 f ? f->colorCount : 0, f ? f->width : 0, f ? f->height : 0,
                 f && attachmentIndex < f->colorCount ? int(f->colors[attachmentIndex].pImage ? 0 : -1) : -2, color.r, color.g,
                 color.b, color.a, rectsCount, rectsCount ? "" : " (whole target)");
        }
#endif
        // deko3d's clears go through scissor 0 (it enables the clear's scissor flag): a rectangle is a scissor.
        if (rectsCount == 0) {
            const DkScissor all = {0, 0, 16384, 16384};
            dkCmdBufSetScissors(cmd, 0, &all, 1);
            flushBarriers();
            if (Deko3DGetOptions().diagBarriers)
                bumpStat(GetDeko3DStats().diagClears);  // sasrt-nx (2026-11-08): the census
            if (g_deko3dTrace)
                Deko3DTraceToken("Cc");
            dkCmdBufClearColorFloat(cmd, attachmentIndex, DkColorMask_RGBA, color.r, color.g, color.b, color.a);
        }
        else {
            for (uint32_t i = 0; i < rectsCount; i++) {
                const RenderRect &r = rects[i];
                const DkScissor s = {uint32_t(std::max(r.left, 0)), uint32_t(std::max(r.top, 0)),
                                     uint32_t(std::max(r.right - r.left, 0)), uint32_t(std::max(r.bottom - r.top, 0))};
                dkCmdBufSetScissors(cmd, 0, &s, 1);
                flushBarriers();
                if (Deko3DGetOptions().diagBarriers)
                    bumpStat(GetDeko3DStats().diagClears);  // sasrt-nx (2026-11-08): the census
                if (g_deko3dTrace)
                    Deko3DTraceToken("Cc");
                dkCmdBufClearColorFloat(cmd, attachmentIndex, DkColorMask_RGBA, color.r, color.g, color.b, color.a);
            }
        }
        restoreScissors();
    }

    void Deko3DCommandList::clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue, const RenderRect *rects, uint32_t rectsCount) {
        if (rectsCount == 0) {
            const DkScissor all = {0, 0, 16384, 16384};
            dkCmdBufSetScissors(cmd, 0, &all, 1);
            flushBarriers();
            if (Deko3DGetOptions().diagBarriers)
                bumpStat(GetDeko3DStats().diagClears);  // sasrt-nx (2026-11-08): the census
            if (g_deko3dTrace)
                Deko3DTraceToken("Cd");
            dkCmdBufClearDepthStencil(cmd, clearDepth, depthValue, clearStencil ? 0xFF : 0, uint8_t(stencilValue));
        }
        else {
            for (uint32_t i = 0; i < rectsCount; i++) {
                const RenderRect &r = rects[i];
                const DkScissor s = {uint32_t(std::max(r.left, 0)), uint32_t(std::max(r.top, 0)),
                                     uint32_t(std::max(r.right - r.left, 0)), uint32_t(std::max(r.bottom - r.top, 0))};
                dkCmdBufSetScissors(cmd, 0, &s, 1);
                flushBarriers();
                if (Deko3DGetOptions().diagBarriers)
                    bumpStat(GetDeko3DStats().diagClears);  // sasrt-nx (2026-11-08): the census
                if (g_deko3dTrace)
                    Deko3DTraceToken("Cd");
                dkCmdBufClearDepthStencil(cmd, clearDepth, depthValue, clearStencil ? 0xFF : 0, uint8_t(stencilValue));
            }
        }
        restoreScissors();
        // sasrt-nx (2026-10-21, Deko3DOptions::depthResolve): a clear leaves compressed tiles.
        if (framebuffer && framebuffer->hasDepth && framebuffer->depthCompressed && Deko3DGetOptions().depthResolve)
            depthDirty = true;
        if (framebuffer && framebuffer->depthTexture)
            framebuffer->depthTexture->depthUnresolved = true;  // sasrt-nx (2026-11-08, preciseDepthResolve)
    }

    // Port 161 (from fnxf_xiii): see copyRunFenced (plume_deko3d.h).
    void Deko3DCommandList::beforeBufferCopy() {
        if (pendingBarrier && copyRunFenced && Deko3DGetOptions().copyRunBarriers) {
            pendingBarrierMode = DkBarrier_Full;
            return;
        }
        flushBarriersRaw();
    }

    void Deko3DCommandList::copyBufferRegion(RenderBufferReference dst, RenderBufferReference src, uint64_t size) {
        const Deko3DBuffer *d = static_cast<const Deko3DBuffer *>(dst.ref);
        const Deko3DBuffer *s = static_cast<const Deko3DBuffer *>(src.ref);
        if (!d || !s || size == 0)
            return;
        beforeBufferCopy();  // port 161
        GetDeko3DStats().copyBuffer.store(GetDeko3DStats().copyBuffer.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
        if (g_deko3dTrace)
            Deko3DTraceToken("K");  // sasrt-nx (2026-11-08, the trace)
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        dkCmdBufCopyBuffer(cmd, s->gpu + src.offset, d->gpu + dst.offset, uint32_t(size));
    }

    static DkImageView subresourceView(const Deko3DTexture *t, uint32_t mip, uint32_t layer) {
        DkImageView v = t->defaultView();
        v.mipLevelOffset = uint8_t(mip);
        v.mipLevelCount = 1;
        if (t->type == DkImageType_2DArray || t->type == DkImageType_Cubemap || t->type == DkImageType_CubemapArray ||
            t->type == DkImageType_1DArray || t->type == DkImageType_2DMSArray) {
            v.layerOffset = uint16_t(layer);
            v.layerCount = 1;
        }
        return v;
    }

    static uint32_t mipDim(uint32_t d, uint32_t mip) { return std::max<uint32_t>(d >> mip, 1); }

    // Bytes per block of a plume format, and its block width (1 for uncompressed).
    static uint32_t blockBytes(RenderFormat f) {
        switch (f) {
        case RenderFormat::BC1_TYPELESS: case RenderFormat::BC1_UNORM: case RenderFormat::BC1_UNORM_SRGB:
        case RenderFormat::BC4_TYPELESS: case RenderFormat::BC4_UNORM: case RenderFormat::BC4_SNORM:
            return 8;
        case RenderFormat::BC2_TYPELESS: case RenderFormat::BC2_UNORM: case RenderFormat::BC2_UNORM_SRGB:
        case RenderFormat::BC3_TYPELESS: case RenderFormat::BC3_UNORM: case RenderFormat::BC3_UNORM_SRGB:
        case RenderFormat::BC5_TYPELESS: case RenderFormat::BC5_UNORM: case RenderFormat::BC5_SNORM:
        case RenderFormat::BC6H_TYPELESS: case RenderFormat::BC6H_UF16: case RenderFormat::BC6H_SF16:
        case RenderFormat::BC7_TYPELESS: case RenderFormat::BC7_UNORM: case RenderFormat::BC7_UNORM_SRGB:
            return 16;
        default:
            return RenderFormatSize(f);
        }
    }

    // sasrt-nx (2026-11-08, Deko3DOptions::trackedTargetWait, trackedImageInvalidate): a texture a copy, resolve, blit
    // or decompression uses now: the next move into a target layout waits for it (and a read move drops the cache).
    static inline void markUsed(const RenderTexture *texture) {
        if (const Deko3DTexture *t = static_cast<const Deko3DTexture *>(texture)) {
            t->usedWaitEpoch = g_deko3dWaitEpoch.load(std::memory_order_relaxed);
            t->writtenWaitEpoch = t->usedWaitEpoch;  // sasrt-nx (2026-11-08, trackedReadWait): a write, or so taken
            t->sampledEpoch = g_deko3dTexDropEpoch.load(std::memory_order_relaxed);
        }
    }

    void Deko3DCommandList::copyTextureRegion(const RenderTextureCopyLocation &dstLoc, const RenderTextureCopyLocation &srcLoc, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox) {
        markUsed(dstLoc.texture);  // sasrt-nx (2026-11-08)
        markUsed(srcLoc.texture);
#if defined(PLUME_DEKO3D_GUARDS)
        if (g_traceCopies.fetch_add(1) < 60) {
            auto describe = [](const RenderTextureCopyLocation &l, char *out, size_t n) {
                if (l.type == RenderTextureCopyType::PLACED_FOOTPRINT)
                    snprintf(out, n, "buffer (%ux%u, format %d, row %u texels)", l.placedFootprint.width, l.placedFootprint.height,
                             int(l.placedFootprint.format), l.placedFootprint.rowWidth);
                else {
                    const Deko3DTexture *t = static_cast<const Deko3DTexture *>(l.texture);
                    snprintf(out, n, "texture %p (%ux%u, format %d, mip %u layer %u)", (const void *)t, t ? t->desc.width : 0,
                             t ? t->desc.height : 0, t ? int(t->desc.format) : -1, l.subresource.mipLevel, l.subresource.arrayIndex);
                }
            };
            char a[160], b[160];
            describe(srcLoc, a, sizeof(a));
            describe(dstLoc, b, sizeof(b));
            logf("trace copy: %s -> %s at (%u, %u, %u)%s", a, b, dstX, dstY, dstZ, srcBox ? " (box)" : "");
        }
#endif
        if (dstLoc.type == RenderTextureCopyType::SUBRESOURCE && srcLoc.type == RenderTextureCopyType::PLACED_FOOTPRINT) {
            const Deko3DTexture *t = static_cast<const Deko3DTexture *>(dstLoc.texture);
            const Deko3DBuffer *b = static_cast<const Deko3DBuffer *>(srcLoc.buffer);
            if (!t || !t->valid || !b)
                return;
            const auto &fp = srcLoc.placedFootprint;
            const uint32_t bw = RenderFormatBlockWidth(t->desc.format);
            const uint32_t rowBytes = (fp.rowWidth + bw - 1) / bw * blockBytes(t->desc.format);
            const uint32_t rows = (fp.height + bw - 1) / bw;
            const DkCopyBuf src = {b->gpu + fp.offset, rowBytes, rowBytes * rows};
            DkImageView v = subresourceView(t, dstLoc.subresource.mipLevel, dstLoc.subresource.arrayIndex);
            const bool layered = t->type == DkImageType_3D;
            const DkImageRect rect = {dstX, dstY, layered ? dstZ : 0, fp.width, fp.height, std::max<uint32_t>(fp.depth, 1)};
            flushBarriers();
            GetDeko3DStats().copyUpload.store(GetDeko3DStats().copyUpload.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
            if (g_deko3dTrace)
                Deko3DTraceToken("K");  // sasrt-nx (2026-11-08, the trace)
            workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
            dkCmdBufCopyBufferToImage(cmd, &src, &v, &rect, 0);
            invalidateZcullAfterCopy(t);  // banjo-nx (2026-10-18): an upload into a depth target, the same as a copy
        }
        else if (dstLoc.type == RenderTextureCopyType::PLACED_FOOTPRINT && srcLoc.type == RenderTextureCopyType::SUBRESOURCE) {
            const Deko3DTexture *t = static_cast<const Deko3DTexture *>(srcLoc.texture);
            const Deko3DBuffer *b = static_cast<const Deko3DBuffer *>(dstLoc.buffer);
            if (!t || !t->valid || !b)
                return;
            const auto &fp = dstLoc.placedFootprint;
            const uint32_t bw = RenderFormatBlockWidth(t->desc.format);
            const uint32_t rowBytes = (fp.rowWidth + bw - 1) / bw * blockBytes(t->desc.format);
            const uint32_t rows = (fp.height + bw - 1) / bw;
            const DkCopyBuf dst = {b->gpu + fp.offset, rowBytes, rowBytes * rows};
            DkImageView v = subresourceView(t, srcLoc.subresource.mipLevel, srcLoc.subresource.arrayIndex);
            DkImageRect rect;
            if (srcBox)
                rect = {uint32_t(srcBox->left), uint32_t(srcBox->top), uint32_t(srcBox->front), uint32_t(srcBox->right - srcBox->left),
                        uint32_t(srcBox->bottom - srcBox->top), uint32_t(std::max(srcBox->back - srcBox->front, 1))};
            else
                rect = {0, 0, 0, fp.width, fp.height, std::max<uint32_t>(fp.depth, 1)};
            // The copy engine finishes, and its writes leave the GPU's L2, before the CPU reads them (after a fence).
            // sasrt-nx (2026-11-08, Deko3DOptions::readbackMerge): both full barriers as pending ones: the first with the
            // pending one before it (the texture's move into the copy layout), the second with the next one after the
            // copy (its move back), each emitted before the next work or at the list's end.
            const bool merge = Deko3DGetOptions().mergeBarriers && Deko3DGetOptions().readbackMerge;
            if (merge) {
                bumpRound(6);
                pendingBarrierMode = DkBarrier_Full;
                pendingBarrierInvalidate |= DkInvalidateFlags_Image;
                pendingBarrierWhy |= Deko3DWhyCopy | (pendingBarrier ? uint32_t(Deko3DWhyMerged) : 0u);
                pendingBarrier = true;
                flushBarriers();
            } else {
                flushBarriers();
                dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
                flushBarriers();
            }
            GetDeko3DStats().copyReadback.store(GetDeko3DStats().copyReadback.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
            if (g_deko3dTrace)
                Deko3DTraceToken("K");  // sasrt-nx (2026-11-08, the trace)
            workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
            dkCmdBufCopyImageToBuffer(cmd, &v, &rect, &dst, 0);
            if (merge) {
                flushBarriers();
                pendingBarrierMode = DkBarrier_Full;
                pendingBarrierInvalidate |= DkInvalidateFlags_L2Cache;
                pendingBarrierWhy = Deko3DWhyCopy;
                pendingBarrier = true;
            } else {
                flushBarriers();
                dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
            }
        }
        else {
            const Deko3DTexture *dt = static_cast<const Deko3DTexture *>(dstLoc.texture);
            const Deko3DTexture *st = static_cast<const Deko3DTexture *>(srcLoc.texture);
            if (!dt || !st || !dt->valid || !st->valid)
                return;
            DkImageView dv = subresourceView(dt, dstLoc.subresource.mipLevel, dstLoc.subresource.arrayIndex);
            DkImageView sv = subresourceView(st, srcLoc.subresource.mipLevel, srcLoc.subresource.arrayIndex);
            DkImageRect srect;
            if (srcBox)
                srect = {uint32_t(srcBox->left), uint32_t(srcBox->top), uint32_t(srcBox->front), uint32_t(srcBox->right - srcBox->left),
                         uint32_t(srcBox->bottom - srcBox->top), uint32_t(std::max(srcBox->back - srcBox->front, 1))};
            else
                srect = {0, 0, 0, mipDim(st->desc.width, srcLoc.subresource.mipLevel), mipDim(st->desc.height, srcLoc.subresource.mipLevel),
                         st->type == DkImageType_3D ? mipDim(st->desc.depth, srcLoc.subresource.mipLevel) : 1};
            DkImageRect drect = {dstX, dstY, dt->type == DkImageType_3D ? dstZ : 0, srect.width, srect.height, srect.depth};
            {  // renut-ultimate (port 64, 2026-11-10, sasrt-nx's GPU hang): never past either image (dkCmdBufCopyImage does not check)
                const uint32_t sw = mipDim(st->desc.width, srcLoc.subresource.mipLevel);
                const uint32_t sh = mipDim(st->desc.height, srcLoc.subresource.mipLevel);
                const uint32_t dw = mipDim(dt->desc.width, dstLoc.subresource.mipLevel);
                const uint32_t dh = mipDim(dt->desc.height, dstLoc.subresource.mipLevel);
                if (srect.x >= sw || srect.y >= sh || drect.x >= dw || drect.y >= dh) {
                    static std::atomic<uint32_t> logged{0};
                    if (logged.fetch_add(1) < 16)
                        logf("image copy refused: source %ux%u at (%u, %u), destination %ux%u at (%u, %u)", sw, sh, srect.x,
                             srect.y, dw, dh, drect.x, drect.y);
                    return;
                }
                const uint32_t w = std::min({srect.width, sw - srect.x, dw - drect.x});
                const uint32_t h = std::min({srect.height, sh - srect.y, dh - drect.y});
                if (w != srect.width || h != srect.height) {
                    static std::atomic<uint32_t> logged{0};
                    if (logged.fetch_add(1) < 16)
                        logf("image copy clamped: %ux%u to %ux%u (source %ux%u at (%u, %u), destination %ux%u at (%u, %u))",
                             srect.width, srect.height, w, h, sw, sh, srect.x, srect.y, dw, dh, drect.x, drect.y);
                    srect.width = drect.width = w;
                    srect.height = drect.height = h;
                }
            }
            flushBarriers();
            GetDeko3DStats().copyImage.store(GetDeko3DStats().copyImage.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
            if (g_deko3dTrace)
                Deko3DTraceToken("K");  // sasrt-nx (2026-11-08, the trace)
            workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
            dkCmdBufCopyImage(cmd, &sv, &srect, &dv, &drect, 0);
            invalidateZcullAfterCopy(dt);
        }
    }

    void Deko3DCommandList::invalidateZcullAfterCopy(const Deko3DTexture *dst) {
        dst->depthUnresolved = true;  // sasrt-nx (2026-11-08, preciseDepthResolve): written by a copy
        // banjo-nx (2026-10-18): deko3d drops the Zcull data only when another depth target is bound; a copy into the
        // bound one (the copy engine does not update Zcull) leaves it describing the old depth, which would cull draws
        // the new depth passes.
        // sasrt-nx (2026-10-30): the braces. Without them only flushBarriers() was conditional and the Zcull
        // invalidate was recorded after every copy and upload, into any texture, with Zcull on or off. With the queue's
        // Zcull off (banjo_deko3d_zcull = false) sasrt-nx's console stopped during the boot twice (its newlogs6), most
        // likely this: a Zcull invalidate on a queue without a Zcull context.
        if (Deko3DGetOptions().zcull && (dst->desc.flags & RenderTextureFlag::DEPTH_TARGET)) {
            flushBarriers();
            dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Zcull);
        }
    }

    // renut-ultimate (2026-11-08, Deko3DOptions::depthResolveLazy).
    void Deko3DCommandList::deferBoundDepth() {
        if (!depthDirty)
            return;
        if (!framebuffer || !framebuffer->hasDepth || !framebuffer->depthTexture) {
            resolveBoundDepth(false);
            return;
        }
        depthDirty = false;
        const Deko3DTexture *t = framebuffer->depthTexture;
        t->depthPending = true;
        t->depthPendingView = framebuffer->depth;
        Deko3DStats &st = GetDeko3DStats();
        st.depthResolvesDeferred.store(st.depthResolvesDeferred.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    void Deko3DCommandList::resolvePendingDepth(const Deko3DTexture *t, bool rebind) {
        if (!t->depthPending)
            return;
        t->depthPending = false;
        flushBarriers();
        // renut-ultimate (2026-11-13, sasrt-nx's finding merging port 45): as resolveUnboundDepth. The texture used and
        // written now (trackedTargetWait, trackedReadWait: the read's barrier recorded next waits for this decompression);
        // work since the last fragment wait (leanBarrierSkip); the bound targets' serial dropped (sameTargetsKept: with no
        // framebuffer to bind again, or rebind false, the GPU has only this view bound, and the next setFramebuffer of the
        // last bound framebuffer skipped its bind: draws with no targets); the image known decompressed
        // (preciseDepthResolve).
        markUsed(t);
        if (g_deko3dTrace)
            Deko3DTraceToken("Z");
        dkCmdBufBindRenderTargets(cmd, nullptr, 0, &t->depthPendingView);
        boundTargetsSerial = 0;
        workSinceFragmentWait = true;
        dkCmdBufResolveDepthValues(cmd);
        t->depthUnresolved = false;
        if (rebind) {
            if (framebuffer) {
                const DkImageView *colors[8];
                for (uint32_t i = 0; i < framebuffer->colorCount; i++)
                    colors[i] = &framebuffer->colors[i];
                dkCmdBufBindRenderTargets(cmd, colors, framebuffer->colorCount, framebuffer->hasDepth ? &framebuffer->depth : nullptr);
                boundTargetsSerial = framebuffer->serial;
            }
            else
                dkCmdBufBindRenderTargets(cmd, nullptr, 0, nullptr);
        }
        Deko3DStats &st = GetDeko3DStats();
        st.depthResolvesLater.store(st.depthResolvesLater.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    // sasrt-nx (2026-10-21, Deko3DOptions::depthResolve).
    void Deko3DCommandList::resolveBoundDepth(bool barrierAfter) {
        if (!depthDirty)
            return;
        depthDirty = false;
        if (framebuffer && framebuffer->hasDepth)  // sasrt-nx (2026-11-08)
            markUsed(framebuffer->depthTexture);
        if (!Deko3DGetOptions().depthResolve)
            return;
        // sasrt-nx (2026-11-08, Deko3DOptions::decompressMerge): the pending barrier after the decompression, with the
        // one that follows it (below): one barrier. The decompression is ordered after the draws before it as when
        // nothing is pending (then nothing was emitted before it either). A full barrier (the copy engine) goes first.
        const bool keepPending = barrierAfter && Deko3DGetOptions().mergeBarriers && Deko3DGetOptions().decompressMerge &&
                                 pendingBarrier && pendingBarrierMode != DkBarrier_Full;
        if (!keepPending)
            flushBarriers();
        else
            bumpRound(5);
        if (g_deko3dTrace)
            Deko3DTraceToken("Z");
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        dkCmdBufResolveDepthValues(cmd);
        {
            Deko3DStats &st = GetDeko3DStats();
            st.depthResolvesBound.store(st.depthResolvesBound.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        if (framebuffer && framebuffer->depthTexture)
            framebuffer->depthTexture->depthUnresolved = false;  // sasrt-nx (2026-11-08, preciseDepthResolve)
        if (!barrierAfter)
            return;
        if (Deko3DGetOptions().mergeBarriers) {
            pendingBarrierMode = pendingBarrier ? std::max(pendingBarrierMode, DkBarrier_Fragments) : DkBarrier_Fragments;
            pendingBarrierInvalidate |= DkInvalidateFlags_Image;
            pendingBarrier = true;
        }
        else
            emitBarrier(DkBarrier_Fragments, DkInvalidateFlags_Image);
    }

    // sasrt-nx (2026-11-08, Deko3DOptions::lazyDepthResolve): the image bound alone as the depth target, decompressed,
    // then this list's targets bound again (deko3d invalidates Zcull at each change of depth target: it is empty after,
    // never another target's).
    bool Deko3DCommandList::lazyResolvable(const Deko3DTexture *t) {
        return t && t->desc.mipLevels <= 1 && t->desc.arraySize <= 1 && t->desc.depth <= 1 &&
               Deko3DGetOptions().lazyDepthResolve && Deko3DGetOptions().preciseDepthResolve;
    }

    void Deko3DCommandList::resolveUnboundDepth(const Deko3DTexture *t) {
        flushBarriers();
        markUsed(t);  // sasrt-nx (2026-11-08)
        const DkImageView v = t->defaultView();
        dkCmdBufBindRenderTargets(cmd, nullptr, 0, &v);
        boundTargetsSerial = 0;  // sasrt-nx (2026-11-08, sameTargetsKept): set again below
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        dkCmdBufResolveDepthValues(cmd);
        t->depthUnresolved = false;
        {  // counted with renut-ultimate's later decompresses (lazyDepthResolve, sasrt-nx's item 14, off by default)
            Deko3DStats &st = GetDeko3DStats();
            st.depthResolvesLater.store(st.depthResolvesLater.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        bumpRound(14);
        if (framebuffer) {
            const DkImageView *colors[8];
            for (uint32_t i = 0; i < framebuffer->colorCount; i++)
                colors[i] = &framebuffer->colors[i];
            dkCmdBufBindRenderTargets(cmd, colors, framebuffer->colorCount, framebuffer->hasDepth ? &framebuffer->depth : nullptr);
            boundTargetsSerial = framebuffer->serial;
        } else {
            dkCmdBufBindRenderTargets(cmd, nullptr, 0, nullptr);
        }
    }

    void Deko3DCommandList::copyBuffer(const RenderBuffer *dst, const RenderBuffer *src) {
        const Deko3DBuffer *d = static_cast<const Deko3DBuffer *>(dst);
        const Deko3DBuffer *s = static_cast<const Deko3DBuffer *>(src);
        if (!d || !s)
            return;
        beforeBufferCopy();  // port 161
        GetDeko3DStats().copyBuffer.store(GetDeko3DStats().copyBuffer.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
        if (g_deko3dTrace)
            Deko3DTraceToken("K");  // sasrt-nx (2026-11-08, the trace)
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        dkCmdBufCopyBuffer(cmd, s->gpu, d->gpu, uint32_t(std::min(d->desc.size, s->desc.size)));
    }

    void Deko3DCommandList::copyTexture(const RenderTexture *dst, const RenderTexture *src) {
        const Deko3DTexture *dt = static_cast<const Deko3DTexture *>(dst);
        const Deko3DTexture *st = static_cast<const Deko3DTexture *>(src);
        markUsed(dst);  // sasrt-nx (2026-11-08)
        markUsed(src);
        if (!dt || !st || !dt->valid || !st->valid)
            return;
        const uint32_t levels = std::min(std::max<uint32_t>(dt->desc.mipLevels, 1), std::max<uint32_t>(st->desc.mipLevels, 1));
        const uint32_t layers = std::max<uint32_t>(std::min(dt->desc.arraySize, st->desc.arraySize), 1);
        for (uint32_t m = 0; m < levels; m++) {
            for (uint32_t l = 0; l < layers; l++) {
                DkImageView dv = subresourceView(dt, m, l);
                DkImageView sv = subresourceView(st, m, l);
                const DkImageRect r = {0, 0, 0, mipDim(st->desc.width, m), mipDim(st->desc.height, m),
                                       st->type == DkImageType_3D ? mipDim(st->desc.depth, m) : 1};
                flushBarriers();
                GetDeko3DStats().copyImage.store(GetDeko3DStats().copyImage.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
                if (g_deko3dTrace)
                    Deko3DTraceToken("K");  // sasrt-nx (2026-11-08, the trace)
                workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
                dkCmdBufCopyImage(cmd, &sv, &r, &dv, &r, 0);
            }
        }
        invalidateZcullAfterCopy(dt);
    }

    void Deko3DCommandList::resolveTexture(const RenderTexture *dst, const RenderTexture *src) {
        const Deko3DTexture *dt = static_cast<const Deko3DTexture *>(dst);
        const Deko3DTexture *st = static_cast<const Deko3DTexture *>(src);
        markUsed(dst);  // sasrt-nx (2026-11-08)
        markUsed(src);
        if (!dt || !st || !dt->valid || !st->valid)
            return;
        DkImageView dv = dt->defaultView();
        DkImageView sv = st->defaultView();
        flushBarriers();
        GetDeko3DStats().copyResolve.store(GetDeko3DStats().copyResolve.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        dkCmdBufResolveImage(cmd, &sv, &dv);
        invalidateZcullAfterCopy(dt);
    }

    void Deko3DCommandList::resolveTextureRegion(const RenderTexture *dst, uint32_t dstX, uint32_t dstY, const RenderTexture *src, const RenderRect *srcRect, RenderResolveMode) {
        const Deko3DTexture *dt = static_cast<const Deko3DTexture *>(dst);
        const Deko3DTexture *st = static_cast<const Deko3DTexture *>(src);
        markUsed(dst);  // sasrt-nx (2026-11-08)
        markUsed(src);
        if (!dt || !st || !dt->valid || !st->valid)
            return;
        const bool whole = dstX == 0 && dstY == 0 && dt->desc.width == st->desc.width && dt->desc.height == st->desc.height &&
                           (!srcRect || (srcRect->left == 0 && srcRect->top == 0 && uint32_t(srcRect->right) == st->desc.width &&
                                         uint32_t(srcRect->bottom) == st->desc.height));
        if (whole) {
            resolveTexture(dst, src);
            return;
        }
        // A region: the 2D engine's blit of the multisampled source (it averages the samples it covers).
        DkImageView dv = dt->defaultView();
        DkImageView sv = st->defaultView();
        const RenderRect r = srcRect ? *srcRect : RenderRect(0, 0, int32_t(st->desc.width), int32_t(st->desc.height));
        // renut-ultimate (port 64, 2026-11-10, sasrt-nx): the region never past either image (dkCmdBufBlitImage does not
        // check). Without scaling here (the same width and height both sides), so both rectangles shrink alike.
        if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top || uint32_t(r.left) >= st->desc.width ||
            uint32_t(r.top) >= st->desc.height || dstX >= dt->desc.width || dstY >= dt->desc.height) {
            static std::atomic<uint32_t> logged{0};
            if (logged.fetch_add(1) < 16)
                logf("resolve blit refused: source rect (%d, %d)-(%d, %d) of %ux%u, destination %ux%u at (%u, %u)", r.left, r.top,
                     r.right, r.bottom, st->desc.width, st->desc.height, dt->desc.width, dt->desc.height, dstX, dstY);
            return;
        }
        DkImageRect srect = {uint32_t(r.left), uint32_t(r.top), 0, uint32_t(r.right - r.left), uint32_t(r.bottom - r.top), 1};
        {
            const uint32_t w = std::min({srect.width, st->desc.width - srect.x, dt->desc.width - dstX});
            const uint32_t h = std::min({srect.height, st->desc.height - srect.y, dt->desc.height - dstY});
            if (w != srect.width || h != srect.height) {
                static std::atomic<uint32_t> logged{0};
                if (logged.fetch_add(1) < 16)
                    logf("resolve blit clamped: %ux%u to %ux%u (source %ux%u at (%u, %u), destination %ux%u at (%u, %u))", srect.width,
                         srect.height, w, h, st->desc.width, st->desc.height, srect.x, srect.y, dt->desc.width, dt->desc.height,
                         dstX, dstY);
                srect.width = w;
                srect.height = h;
            }
        }
        const DkImageRect drect = {dstX, dstY, 0, srect.width, srect.height, 1};
        flushBarriers();
        GetDeko3DStats().copyBlit.store(GetDeko3DStats().copyBlit.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
        workSinceFragmentWait = true;  // sasrt-nx (2026-11-08, leanBarrierSkip)
        dkCmdBufBlitImage(cmd, &sv, &srect, &dv, &drect, DkBlitFlag_FilterLinear, 0);
        invalidateZcullAfterCopy(dt);
    }

    void Deko3DCommandList::buildBottomLevelAS(const RenderAccelerationStructure *, RenderBufferReference, const RenderBottomLevelASBuildInfo &) {}
    void Deko3DCommandList::buildTopLevelAS(const RenderAccelerationStructure *, RenderBufferReference, RenderBufferReference, const RenderTopLevelASBuildInfo &) {}
    void Deko3DCommandList::discardTexture(const RenderTexture *) {}
    void Deko3DCommandList::resetQueryPool(const RenderQueryPool *, uint32_t, uint32_t) {}

    void Deko3DCommandList::writeTimestamp(const RenderQueryPool *queryPool, uint32_t queryIndex) {
        const Deko3DQueryPool *pool = static_cast<const Deko3DQueryPool *>(queryPool);
        if (!pool || !pool->block || queryIndex >= pool->results.size())
            return;
        // The timestamp when the work before it has finished (bottom of the pipe), as Vulkan's.
        flushBarriers();
        dkCmdBufReportCounter(cmd, DkCounter_Timestamp, dkMemBlockGetGpuAddr(pool->block) + uint64_t(queryIndex) * 16);
    }

    void Deko3DCommandList::setBlendConstant(const float rgba[4]) {
        if (memcmp(blendConstant, rgba, sizeof(blendConstant)) == 0)
            return;
        memcpy(blendConstant, rgba, sizeof(blendConstant));
        dkCmdBufSetBlendConst(cmd, rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    bool Deko3DBindMemExport(RenderCommandList *commandList, const RenderBuffer *buffer) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        const Deko3DBuffer *b = static_cast<const Deko3DBuffer *>(buffer);
        if (!cl || !b || !b->block)
            return false;
        constexpr uint32_t slot = 14;
        const DkBufExtents e = {b->gpu, uint32_t(std::min<uint64_t>(b->desc.size, 0xFFFFF000u))};
        if (!(cl->graphicsSsboValid & (1u << slot)) || cl->graphicsSsbo[slot].addr != e.addr || cl->graphicsSsbo[slot].size != e.size) {
            cl->graphicsSsbo[slot] = e;
            cl->graphicsSsboValid |= 1u << slot;
            cl->ssboBoundStages[slot] = 0;
            cl->graphicsBuffersDirty = true;
        }
        return true;
    }

    bool Deko3DBindFragmentTextures(RenderCommandList *commandList, uint32_t first, const uint32_t *slots, uint32_t count) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        if (!cl || !cl->cmd || !slots || count == 0 || count > 8 || first + count > 32)
            return false;
        DkResHandle handles[8];
        for (uint32_t k = 0; k < count; k++)
            handles[k] = dkMakeTextureHandle(slots[k] & 0xFFFFFu, 0);
        dkCmdBufBindTextures(cl->cmd, DkStage_Fragment, first, handles, count);
        cl->boundTexCount[2] = 0;  // the next bindTextures binds the whole stage again
        {  // sasrt-nx (2026-11-08): kept whatever the options
            cl->extraFragTexCount = 0;
            for (uint32_t k = 0; k < count; k++) {
                cl->markSampled(slots[k] & 0xFFFFFu);
                cl->extraFragTex[cl->extraFragTexCount++] = slots[k] & 0xFFFFFu;
            }
        }
        cl->texPipeline = nullptr;  // and does not skip its run
        return true;
    }

    bool g_deko3dTrace = false;
    namespace {
        std::string g_traceText, g_traceLast;
        uint32_t g_traceRun = 0;
        std::vector<const void *> g_traceImages;
        void traceFlushRun() {
            if (!g_traceRun)
                return;
            g_traceText += g_traceLast;
            if (g_traceRun > 1)
                g_traceText += "*" + std::to_string(g_traceRun);
            g_traceText += ' ';
            g_traceRun = 0;
        }
        uint32_t traceImage(const void *p) {
            if (!p)
                return 0;
            for (uint32_t i = 0; i < g_traceImages.size(); i++)
                if (g_traceImages[i] == p)
                    return i + 1;
            g_traceImages.push_back(p);
            return uint32_t(g_traceImages.size());
        }
    }
    void Deko3DTraceToken(const char *token) {
        if (g_traceText.size() > (1u << 20))
            return;
        if (g_traceRun && g_traceLast == token) {
            ++g_traceRun;
            return;
        }
        traceFlushRun();
        g_traceLast = token;
        g_traceRun = 1;
    }
    std::string Deko3DTraceTake() {
        traceFlushRun();
        std::string t;
        t.swap(g_traceText);
        g_traceImages.clear();
        return t;
    }

    void Deko3DBufferWait(RenderCommandList *commandList, uint32_t invalidate, bool countsAsBufferWait) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        if (!cl)
            return;
        if (countsAsBufferWait)
            ++cl->bufferWaits;
        if (Deko3DGetOptions().mergeBarriers) {
            cl->pendingBarrierMode = cl->pendingBarrier ? std::max(cl->pendingBarrierMode, DkBarrier_Primitives) : DkBarrier_Primitives;
            cl->pendingBarrierInvalidate |= invalidate;
            cl->pendingBarrierWhy = cl->pendingBarrier ? (cl->pendingBarrierWhy | Deko3DWhyBuffer | Deko3DWhyMerged) : uint32_t(Deko3DWhyBuffer);
            cl->pendingBarrier = true;
        } else {
            cl->flushBarriers();
            cl->pendingBarrierWhy = Deko3DWhyBuffer;
            cl->emitBarrier(DkBarrier_Primitives, invalidate);
        }
    }

    void Deko3DSetSampleMask(RenderCommandList *commandList, uint32_t mask) {  // port 166
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        mask &= 0xFFFFu;  // the hardware's mask has 16 bits (fnxf_xiii: D3D's 0xFFFFFFFF as it is was a method error)
        if (!cl || cl->sampleMaskSet == mask)
            return;
        cl->sampleMaskSet = mask;
        dkCmdBufSetSampleMask(cl->cmd, mask);
    }

    void Deko3DDiscardNextDraw(RenderCommandList *commandList) {
        if (Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList))
            cl->discardNextDraw = true;
    }

    void Deko3DSetNextPrimitive(RenderCommandList *commandList, uint32_t primitive) {
        if (Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList))
            cl->nextPrimitive = primitive + 1;
    }

    uint64_t Deko3DBufferWaits(const RenderCommandList *commandList) {
        const Deko3DCommandList *cl = static_cast<const Deko3DCommandList *>(commandList);
        return cl ? cl->bufferWaits : 0;
    }

    void Deko3DFlushL2(RenderCommandList *commandList) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        if (!cl)
            return;
        if (Deko3DGetOptions().mergeBarriers) {
            cl->pendingBarrierMode = cl->pendingBarrier ? std::max(cl->pendingBarrierMode, DkBarrier_Primitives) : DkBarrier_Primitives;
            cl->pendingBarrierInvalidate |= DkInvalidateFlags_L2Cache;
            cl->pendingBarrierWhy |= Deko3DWhyBuffer | Deko3DWhyCompute;
            cl->pendingBarrier = true;
        } else {
            cl->flushBarriers();
            cl->pendingBarrierWhy = Deko3DWhyBuffer | Deko3DWhyCompute;
            cl->emitBarrier(DkBarrier_Primitives, DkInvalidateFlags_L2Cache);
        }
    }

    bool Deko3DBindFetchData(RenderCommandList *commandList, uint64_t address, uint32_t size) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        if (!cl || !address)
            return false;
        const DkBufExtents e = {address, std::max<uint32_t>(alignUp(std::min(size, 0xFFFFF000u), 16), 16)};
        if (!(cl->graphicsSsboValid & (1u << AddressWindowSlot)) || cl->graphicsSsbo[AddressWindowSlot].addr != e.addr ||
            cl->graphicsSsbo[AddressWindowSlot].size != e.size) {
            cl->graphicsSsbo[AddressWindowSlot] = e;
            cl->graphicsSsboValid |= 1u << AddressWindowSlot;
            cl->ssboBoundStages[AddressWindowSlot] = 0;
            cl->graphicsBuffersDirty = true;
        }
        return true;
    }

    bool Deko3DBindStorage(RenderCommandList *commandList, uint32_t slot, uint64_t address, uint32_t size) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        if (!cl || !address || slot >= 14)
            return false;
        const DkBufExtents e = {address, std::max<uint32_t>(alignUp(size, 16), 16)};
        if (!(cl->graphicsSsboValid & (1u << slot)) || cl->graphicsSsbo[slot].addr != e.addr || cl->graphicsSsbo[slot].size != e.size) {
            cl->graphicsSsbo[slot] = e;
            cl->graphicsSsboValid |= 1u << slot;
            cl->ssboBoundStages[slot] = 0;
            cl->graphicsBuffersDirty = true;
        }
        return true;
    }

    void Deko3DSetVertexBufferDedupe(bool on) {
        g_vtxDedupe = on;
    }

    bool Deko3DBindUniform(RenderCommandList *commandList, uint32_t slot, uint64_t address, uint32_t size) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        if (!cl || !address || slot >= 16)
            return false;
        const DkBufExtents e = {address, size};
        if (!(cl->graphicsUboValid & (1u << slot)) || cl->graphicsUbo[slot].addr != e.addr || cl->graphicsUbo[slot].size != e.size) {
            cl->graphicsUbo[slot] = e;
            cl->graphicsUboValid |= 1u << slot;
            cl->uboBoundStages[slot] = 0;
            cl->graphicsBuffersDirty = true;
        }
        return true;
    }

    bool Deko3DPushUniformSlow(RenderCommandList *commandList, uint64_t address, uint32_t uboSize, uint32_t offset,
                               const void *data, uint32_t size) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        if (!cl || !address || !size || offset + size > uboSize)
            return false;
        // Pipelined: the draws recorded before it read the old values (as setGraphicsPushConstants' updates). In pieces
        // of at most 1 KB: Ryujinx stages one update packet in a 512-word buffer and throws on a longer one (emulator
        // run 60: "Destination is too short" in ConstantBufferUpdater.Update, the GPU thread gone); the console takes
        // either, and consecutive pieces cost a few command words each.
        constexpr uint32_t kPiece = 1024;
        const uint8_t *bytes = static_cast<const uint8_t *>(data);
        for (uint32_t done = 0; done < size;) {
            const uint32_t n = std::min(kPiece, size - done);
            dkCmdBufPushConstants(cl->cmd, address, uboSize, offset + done, alignUp(n, 4), bytes + done);
            done += n;
        }
        return true;
    }

    bool Deko3DSetSharedWords(RenderCommandList *commandList, const void *cpu, uint32_t size) {
        Deko3DCommandList *cl = static_cast<Deko3DCommandList *>(commandList);
        if (!cl)
            return false;
        cl->sharedWords = static_cast<const uint8_t *>(cpu);
        cl->sharedWordsSize = size;
        cl->texLinesWritten = ~0u;  // banjo-nx (2026-10-27): another copy, or none
        if (!cpu)
            cl->sharedCpu = nullptr;  // looked up again where the bound buffer is
        return true;
    }

    void Deko3DReportCounter(RenderCommandList *commandList, const RenderQueryPool *queryPool, uint32_t index,
                             uint32_t counter) {
        auto *list = static_cast<Deko3DCommandList *>(commandList);
        const auto *pool = static_cast<const Deko3DQueryPool *>(queryPool);
        if (!list || !list->cmd || !pool || !pool->block || index >= pool->results.size())
            return;
        list->flushBarriers();
        dkCmdBufReportCounter(list->cmd, DkCounter(counter), dkMemBlockGetGpuAddr(pool->block) + uint64_t(index) * 16);
    }

    const volatile uint64_t *Deko3DQueryPoolWords(const RenderQueryPool *queryPool) {
        const auto *pool = static_cast<const Deko3DQueryPool *>(queryPool);
        return pool && pool->block ? static_cast<const volatile uint64_t *>(dkMemBlockGetCpuAddr(pool->block)) : nullptr;
    }

    bool Deko3DSetBlendConstant(RenderCommandList *commandList, const float rgba[4]) {
        if (!commandList)
            return false;
        static_cast<Deko3DCommandList *>(commandList)->setBlendConstant(rgba);
        return true;
    }

    // ---- Deko3DCommandQueue

    // A GPU self-test of the frame capture's path (debug builds): a render target cleared to a known colour, copied to a
    // buffer by the copy engine, read back by the CPU; with and without hardware compression. Logs the first pixel.
    static void readbackSelfTest(Deko3DDevice *device) {
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, device->dk);
        DkCmdBuf cmd = dkCmdBufCreate(&cm);
        DkMemBlock cmdMem = device->makeBlock(64u << 10, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "self-test commands");
        DkMemBlock out = device->makeBlock(64u << 10, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "self-test readback");
        for (int compressed = 0; compressed < 2; compressed++) {
            DkImageLayoutMaker m;
            dkImageLayoutMakerDefaults(&m, device->dk);
            m.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | (compressed ? DkImageFlags_HwCompression : 0);
            m.format = DkImageFormat_RGBA8_Unorm;
            m.dimensions[0] = 64;
            m.dimensions[1] = 64;
            DkImageLayout layout;
            dkImageLayoutInitialize(&layout, &m);
            const uint32_t size = uint32_t(dkImageLayoutGetSize(&layout));
            DkMemBlock imgMem = device->makeBlock(alignUp(size, 1u << 16), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "self-test image");
            DkImage image;
            dkImageInitialize(&image, &layout, imgMem, 0);
            memset(dkMemBlockGetCpuAddr(out), 0x11, 64 * 64 * 4);
            dkCmdBufClear(cmd);
            dkCmdBufAddMemory(cmd, cmdMem, 0, 64u << 10);
            DkImageView view;
            dkImageViewDefaults(&view, &image);
            dkCmdBufBindRenderTarget(cmd, &view, nullptr);  // (the start-up probe: its own list)
            const DkScissor all = {0, 0, 64, 64};
            dkCmdBufSetScissors(cmd, 0, &all, 1);
            dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.25f, 0.5f, 0.75f, 1.0f);
            dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
            const DkImageRect rect = {0, 0, 0, 64, 64, 1};
            const DkCopyBuf dst = {dkMemBlockGetGpuAddr(out), 64 * 4, 64 * 64 * 4};
            GetDeko3DStats().copyReadback.store(GetDeko3DStats().copyReadback.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);  // banjo-nx (2026-10-19)
            dkCmdBufCopyImageToBuffer(cmd, &view, &rect, &dst, 0);
            dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
            DkFence fence{};
            dkCmdBufSignalFence(cmd, &fence, true);
            dkQueueSubmitCommands(device->queue, dkCmdBufFinishList(cmd));
            dkQueueFlush(device->queue);
            const DkResult r = dkFenceWait(&fence, 2'000'000'000ll);
            const uint8_t *px = static_cast<const uint8_t *>(dkMemBlockGetCpuAddr(out));
            logf("self-test (%s render target): clear (0.25, 0.5, 0.75, 1) read back as %u %u %u %u (expected 64 128 191 255; "
                 "fence %d)", compressed ? "compressed" : "plain", px[0], px[1], px[2], px[3], int(r));
            dkQueueWaitIdle(device->queue);
            device->destroyBlock(imgMem);
        }
        dkCmdBufDestroy(cmd);
        device->destroyBlock(cmdMem);
        device->destroyBlock(out);
    }

    Deko3DCommandQueue::Deko3DCommandQueue(Deko3DDevice *device, RenderCommandListType type) : device(device), type(type) {
#if defined(PLUME_DEKO3D_GUARDS)
        readbackSelfTest(device);
#endif
    }

    std::unique_ptr<RenderCommandList> Deko3DCommandQueue::createCommandList() {
        return std::make_unique<Deko3DCommandList>(this);
    }

    std::unique_ptr<RenderSwapChain> Deko3DCommandQueue::createSwapChain(const RenderSwapChainDesc &desc) {
        auto swapchain = std::make_unique<Deko3DSwapChain>(this, desc);
        return swapchain;
    }

    void Deko3DCommandQueue::executeCommandLists(const RenderCommandList **commandLists, uint32_t count, RenderCommandSemaphore **, uint32_t, RenderCommandSemaphore **, uint32_t, RenderCommandFence *signalFence) {
        // One queue: its order already serialises what semaphores order in Vulkan (acquires and presents are queue
        // operations in deko3d).
        checkQueue(device, "before a submission");  // port 167
        for (uint32_t i = 0; i < count; i++) {
            const Deko3DCommandList *cl = static_cast<const Deko3DCommandList *>(commandLists[i]);
            if (cl && cl->list) {
                dkQueueSubmitCommands(device->queue, cl->list);
                checkQueue(device, "after a command list was submitted");  // port 167
            }
        }
        if (signalFence) {
            Deko3DCommandFence *f = static_cast<Deko3DCommandFence *>(signalFence);
            // flush: the GPU's caches are written out before the fence signals, so the CPU reads what the GPU wrote
            // (readbacks, timestamps) once it has waited for it.
            dkQueueSignalFence(device->queue, &f->fence, true);
            f->pending = true;
        }
        device->noteSubmission();
        checkQueue(device, "before a flush");  // port 167
        dkQueueFlush(device->queue);
        checkQueue(device, "after a flush");  // port 167
        device->checkGuards();
        device->collectRetired();
    }

    void Deko3DCommandQueue::waitForCommandFence(RenderCommandFence *fence) {
        Deko3DCommandFence *f = static_cast<Deko3DCommandFence *>(fence);
        if (!f || !f->pending)
            return;
        // Until it signals, or the queue has failed: memory the submission uses (command memory with its compute
        // launch descriptors, uploads) is reused once this returns, so giving up on a late fence made the GPU run
        // garbage (Ryujinx run 16: shaders decoded from data, then a lost device). Logged at 2, 4, 8, 16 ... s.
        for (int waited = 1;; waited++) {
            const DkResult r = dkFenceWait(&f->fence, 2'000'000'000ll);
            if (r == DkResult_Success)
                break;
            const bool failed = dkQueueIsInErrorState(device->queue);
            if (failed || (waited & (waited - 1)) == 0)
                logf("a fence has not signalled after %d s (result %d)%s", waited * 2, int(r),
                     failed ? "; the queue is in an error state: not waited for" : "");
            if (failed) {
                checkQueue(device, "while waiting for a fence");  // port 167: why
                break;
            }
        }
        f->pending = false;
    }

    // ---- Deko3DSwapChain

    // banjo-nx (2026-10-21, from sa2-nx 1.0.3.1): Deko3DSetSwapChainSize's size (width << 32 | height; 0 = the window's).
    static std::atomic<uint64_t> g_wantedSwapChainSize{0};

    void Deko3DSetSwapChainSize(uint32_t width, uint32_t height) {
        g_wantedSwapChainSize.store(width && height ? (uint64_t(width) << 32 | height) : 0, std::memory_order_relaxed);
    }

    Deko3DSwapChain::Deko3DSwapChain(Deko3DCommandQueue *queue, const RenderSwapChainDesc &desc) : queue(queue), device(queue->device), desc(desc) {
        resize();
    }

    Deko3DSwapChain::~Deko3DSwapChain() { destroy(); }

    }  // namespace plume
    extern "C" void RexSwitchPerfPresentQuiesce(NWindow *nw);  // sdk/src/ui/switch_perf.cpp (port 145)
    namespace plume {

    void Deko3DSwapChain::destroy() {
        if (!swapchain)
            return;
        dkQueueWaitIdle(device->queue);
        // renut-ultimate (2026-11-16, port 145; tw2de_nx's, banjo_async_present): the buffer dequeued ahead goes back
        // first (libnx does not release the window's buffers while one is dequeued).
        RexSwitchPerfPresentQuiesce(desc.renderWindow ? desc.renderWindow : nwindowGetDefault());
        dkSwapchainDestroy(swapchain);
        swapchain = nullptr;
        textures.clear();
        for (DkMemBlock b : blocks)
            device->destroyBlock(b);
        blocks.clear();
    }

    bool Deko3DSwapChain::resize() {
        // The window's size, read before the old swapchain lets go of the window (afterwards it reads 1x1: the first
        // emulator runs). An implausible size keeps the current one (1280x720 at first); the same size keeps the
        // swapchain.
        uint32_t w = 0, h = 0;
        NWindow *window = desc.renderWindow ? desc.renderWindow : nwindowGetDefault();
        // banjo-nx (2026-10-21, from sa2-nx 1.0.3.1): a size set by Deko3DSetSwapChainSize (the console's display: 1080p
        // docked) comes first; the window takes it below, once the old swapchain has let go of its buffers.
        const uint64_t wanted = g_wantedSwapChainSize.load(std::memory_order_relaxed);
        if (wanted) {
            w = uint32_t(wanted >> 32);
            h = uint32_t(wanted);
        } else if (nwindowGetDimensions(window, &w, &h) != 0 || w < 64 || h < 64) {
            w = width ? width : 1280;
            h = height ? height : 720;
        }
        if (swapchain && w == width && h == height)
            return true;
        destroy();
        if (wanted) {
            uint32_t cw = 0, ch = 0;
            if (nwindowGetDimensions(window, &cw, &ch) != 0 || cw != w || ch != h) {
                const Result rc = nwindowSetDimensions(window, w, h);
                logf("window: %ux%u -> %ux%u (%s)", cw, ch, w, h, R_SUCCEEDED(rc) ? "set" : "failed");
            }
        }
        width = w;
        height = h;
        DkImageFormat format = toDkFormat(desc.format);
        if (format == DkImageFormat_None)
            format = DkImageFormat_RGBA8_Unorm;
        const uint32_t count = std::max<uint32_t>(desc.textureCount, 2);
        std::vector<const DkImage *> images;
        for (uint32_t i = 0; i < count; i++) {
            auto t = std::make_unique<Deko3DTexture>(device);
            t->desc = RenderTextureDesc::ColorTarget(w, h, desc.format);
            t->format = format;
            t->type = DkImageType_2D;
            t->ownsMemory = false;
            DkImageLayoutMaker m;
            dkImageLayoutMakerDefaults(&m, device->dk);
            m.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression | DkImageFlags_Usage2DEngine;
            m.format = format;
            m.dimensions[0] = w;
            m.dimensions[1] = h;
            dkImageLayoutInitialize(&t->layout, &m);
            const uint32_t size = uint32_t(dkImageLayoutGetSize(&t->layout));
            const uint32_t align = dkImageLayoutGetAlignment(&t->layout);
            DkMemBlock block = device->makeBlock(alignUp(size, std::max<uint32_t>(align, DK_MEMBLOCK_ALIGNMENT)), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "swapchain image");
            blocks.push_back(block);
            dkImageInitialize(&t->image, &t->layout, block, 0);
            t->valid = true;
            images.push_back(&t->image);
            textures.push_back(std::move(t));
        }
        DkSwapchainMaker sm;
        dkSwapchainMakerDefaults(&sm, device->dk, window, images.data(), count);
        swapchain = dkSwapchainCreate(&sm);
        if (!swapchain) {
            logf("dkSwapchainCreate failed");
            return false;
        }
        // port 165: Deko3DOptions::swapInterval refreshes an image with vsync
        const uint32_t interval = std::min<uint32_t>(std::max<uint32_t>(Deko3DGetOptions().swapInterval, 1u), 4u);
        dkSwapchainSetSwapInterval(swapchain, vsync ? interval : 0);
        logf("swapchain: %u images of %ux%u (format %d), swap interval %u", count, w, h, int(format), vsync ? interval : 0u);
        return true;
    }

    bool Deko3DSwapChain::needsResize() const {
        if (swapchain == nullptr)
            return true;
        // banjo-nx (2026-10-21, from sa2-nx 1.0.3.1): another size wanted (docking or undocking).
        const uint64_t wanted = g_wantedSwapChainSize.load(std::memory_order_relaxed);
        return wanted && (uint32_t(wanted >> 32) != width || uint32_t(wanted) != height);
    }

    void Deko3DSwapChain::setVsyncEnabled(bool vsyncEnabled) {
        vsync = vsyncEnabled;
        if (swapchain)
            dkSwapchainSetSwapInterval(swapchain, vsync ? std::min<uint32_t>(std::max<uint32_t>(Deko3DGetOptions().swapInterval, 1u), 4u) : 0);  // port 165
    }

    RenderTexture *Deko3DSwapChain::getTexture(uint32_t textureIndex) {
        return textureIndex < textures.size() ? textures[textureIndex].get() : nullptr;
    }

    bool Deko3DSwapChain::acquireTexture(RenderCommandSemaphore *, uint32_t *textureIndex) {
        if (!swapchain)
            return false;
        // A queue operation: the queue waits for the image before the commands submitted after this.
        const int slot = dkQueueAcquireImage(device->queue, swapchain);
        if (slot < 0)
            return false;
        *textureIndex = uint32_t(slot);
        return true;
    }

    bool Deko3DSwapChain::present(uint32_t textureIndex, RenderCommandSemaphore **, uint32_t) {
        if (!swapchain)
            return false;
        dkQueuePresentImage(device->queue, swapchain, int(textureIndex));
        return true;
    }

    void Deko3DSwapChain::wait() {}
};

// Port 167 (from fnxf_xiii, its note of 2026-10-27): a kickoff that failed (sdk/src/ui/switch_perf.cpp's
// __wrap_nvGpuChannelKickoff calls this), reported at once: deko3d turns its queue to "error" inside that flush and
// stops the game at its next one, so a check around deko3d's calls never runs. Port 157 sends a refused submission
// again in parts first; what fails here failed that too.
extern "C" void RexSwitchGpuKickoffFailed(NvGpuChannel *channel, uint32_t rc) {
    plume::reportChannel(channel, "a kickoff failed", rc);
}
