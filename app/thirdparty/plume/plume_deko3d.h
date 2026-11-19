//
// plume deko3d back end (banjo-nx, 2026-10-17): plume's render interface on deko3d, the Switch's own GPU API, for
// Renut's renderer (docs/deko3d-renderer.md). MIT, as plume. deko3d is zlib (fincs, devkitPro); the shaders are compiled
// by uam (thirdparty/uam: zlib, Mesa's files MIT, SwitchWakerHD's patches MPL-2.0).
//
// Shaders come as RenderShaderFormat::DEKO3D: a Deko3DShaderBlob header, then GLSL that uam takes (tools/deko3d_glsl.py
// wrote it, with its "// banjo-dk" line of bindings), then optionally the DKSH of the default variant. A pipeline's
// specialization constants select the variant: the same GLSL with the constants defined first, compiled once by uam
// (the device's compile thread) unless a DKSH cache has it.
//
#pragma once

#include "plume_render_interface.h"

#include <deko3d.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>  // banjo-nx (2026-10-21): Deko3DDevice::asyncQueued
#include <vector>

namespace plume {
    // RenderShaderFormat::DEKO3D's container.
    struct Deko3DShaderBlob {
        static constexpr uint32_t Magic = 0x334B4442; // "BDK3"
        uint32_t magic = Magic;
        uint32_t stage = 0;     // DkStage
        uint32_t glslSize = 0;  // bytes of GLSL after this header
        uint32_t dkshSize = 0;  // bytes of DKSH after the GLSL (the default variant), or 0
    };

    // What a DKSH cache gives the device: the DKSH for a variant's full GLSL text (its hash), or null.
    using Deko3DShaderCacheLookup = std::function<bool(uint64_t sourceHash, std::vector<uint8_t> &dksh)>;
    // Called with every DKSH the device compiled (to keep in a cache).
    using Deko3DShaderCacheStore = std::function<void(uint64_t sourceHash, const std::vector<uint8_t> &dksh)>;
    void SetDeko3DShaderCache(Deko3DShaderCacheLookup lookup, Deko3DShaderCacheStore store);
    // banjo-nx (2026-10-23): whether the cache holds a variant, without reading it (its index only): such a variant is
    // read in the background (Deko3DOptions::asyncCacheReads) instead of on the drawing thread.
    using Deko3DShaderCacheContains = std::function<bool(uint64_t sourceHash)>;
    void SetDeko3DShaderCacheContains(Deko3DShaderCacheContains contains);

    // Counters for the renderer's reports.
    struct Deko3DStats {
        std::atomic<uint64_t> shaderCompiles{0}, shaderCompileNs{0}, shaderCacheHits{0}, shaderCompileFailures{0};
        // banjo-nx (2026-10-21, Deko3DOptions::asyncVariants): stages given their generic variant while their own was
        // compiled in the background, background compiles done, stages that took their own variant afterwards.
        std::atomic<uint64_t> variantFallbacks{0}, variantBackground{0}, variantUpgrades{0};
        std::atomic<uint64_t> variantCacheReadsBackground{0};  // banjo-nx (2026-10-23): asyncCacheReads
        // banjo-nx (2026-10-23): blocking compiles: those a drawing thread waited for (the game stopped) and the time
        // waited, those a background thread (prebuild) waited for; boost mode during them (compileWaitBoost): times
        // and time; the uam thread's rests between background compiles (backgroundCompileShare).
        std::atomic<uint64_t> compileWaits{0}, compileWaitNs{0}, compileBackgroundWaits{0}, backgroundRestNs{0};
        std::atomic<uint64_t> fastCompiles{0};  // port 56: the drawWaits compiles made with fastFirstCompile
        std::atomic<uint64_t> compileBoosts{0}, compileBoostNs{0};
        std::atomic<uint64_t> imageBytes{0}, bufferBytes{0}, codeBytes{0}, commandBytes{0};
        std::atomic<uint64_t> descriptorWrites{0}, pipelineBinds{0}, stateBinds{0}, draws{0};
        // renut-ultimate (2026-11-10, Deko3DOptions::descriptorCache): image descriptor writes served from a view's or
        // texture's cached descriptor.
        std::atomic<uint64_t> descriptorCacheHits{0};
        std::atomic<uint64_t> windowDraws{0}; // draws whose shaders read fetch data from memory (storage binding 15)
        // banjo-nx (2026-10-27): bindTextures' whole runs and the draws it skipped (Deko3DSetTextureSkip).
        std::atomic<uint64_t> textureRuns{0}, textureSkips{0};
        // banjo-nx (2026-10-19): the barriers recorded, by what they wait for (DkBarrier_Primitives, _Fragments, _Full:
        // the 3D pipe drained, the copy engine as well), and those that invalidate the texture cache.
        std::atomic<uint64_t> barriersPrimitives{0}, barriersFragments{0}, barriersFull{0}, barriersImageInvalidate{0};
        // banjo-nx (2026-10-19): copy-engine and 2D-engine work, by kind (each fenced by Full barriers in Renut):
        // buffer copies, buffer-to-image (texture uploads), image-to-buffer (readbacks), image copies, resolves, blits.
        std::atomic<uint64_t> copyBuffer{0}, copyUpload{0}, copyReadback{0}, copyImage{0}, copyResolve{0}, copyBlit{0};
        // sasrt-nx (2026-10-30): pipelines made for a multisampled target, those of them with alpha to coverage, and
        // the multisampled ones given the standard sample positions (Deko3DOptions::msaaPositions).
        std::atomic<uint64_t> msaaPipelines{0}, msaaAlphaToCoverage{0}, msaaPositioned{0};
        // sasrt-nx (2026-10-30, Deko3DOptions::stripRestart): indexed strip draws drawn with primitive restart.
        std::atomic<uint64_t> stripRestartDraws{0};
        // renut-ultimate (2026-11-08): depth decompresses done where the target was bound, left for later
        // (depthResolveLazy), and done later for a read.
        std::atomic<uint64_t> depthResolvesBound{0}, depthResolvesDeferred{0}, depthResolvesLater{0};
        // sasrt-nx (2026-11-08, Deko3DOptions::diagBarriers): the barriers emitted by what they wait for and why
        // (Deko3DBarrierWhy bits of every call merged into them): [mode 0 primitives, 1 fragments, 2 full][why].
        std::atomic<uint64_t> barrierWhy[3][256] = {};
        // sasrt-nx (2026-11-08, the GPU round): what plume's items did, by the round's item number (sasrt_gpu_round.h;
        // load and store counts, as the others): 1 binds of a compressed depth image known decompressed (not marked
        // dirty), 5 decompressions whose barrier merged with a pending one, 6 readback barriers merged, 8 buffer
        // barriers with the data-only invalidate.
        std::atomic<uint64_t> sasrtRound[64] = {};  // sasrt-nx (2026-11-08): 32 with the second round, 64 with the third
        // sasrt-nx (2026-11-08, sasrt_deko3d_diag_barriers, the second GPU round's census): descriptor and code cache
        // invalidations recorded (flushDescriptorInvalidate), framebuffer binds and the depth target changes among them,
        // command lists ended, compute dispatches, colour and depth clears.
        std::atomic<uint64_t> diagDescInvalidates{0}, diagCodeInvalidates{0}, diagFramebufferBinds{0},
            diagDepthChanges{0}, diagListEnds{0}, diagDispatches{0}, diagClears{0};
    };
    // banjo-nx (2026-10-19, render-thread proposal 13): read inline (no call per draw).
    extern Deko3DStats g_deko3dStats;
    inline Deko3DStats &GetDeko3DStats() { return g_deko3dStats; }

    // Where the back end's messages go (stderr until set: the application's log, so they reach it on the console and in
    // an emulator alike).
    void SetDeko3DLogCallback(void (*callback)(const char *message));

    struct Deko3DDevice;
    struct Deko3DCommandQueue;

    struct Deko3DBuffer : RenderBuffer {
        Deko3DDevice *device = nullptr;
        RenderBufferDesc desc;
        DkMemBlock block = nullptr;
        DkGpuAddr gpu = 0;
        uint8_t *cpu = nullptr;
        bool cpuCached = false;

        Deko3DBuffer(Deko3DDevice *device, const RenderBufferDesc &desc);
        ~Deko3DBuffer() override;
        void *map(uint32_t subresource, const RenderRange *readRange) override;
        void unmap(uint32_t subresource, const RenderRange *writtenRange) override;
        std::unique_ptr<RenderBufferFormattedView> createBufferFormattedView(RenderFormat format) override;
        void setName(const std::string &name) override;
        uint64_t getDeviceAddress() const override;
    };

    struct Deko3DBufferFormattedView : RenderBufferFormattedView {
        RenderFormat format = RenderFormat::UNKNOWN;
        Deko3DBuffer *buffer = nullptr;
    };

    // A piece of the device's image heap.
    struct Deko3DImageAlloc {
        DkMemBlock block = nullptr;
        uint32_t offset = 0;
        uint32_t size = 0;
        int32_t chunk = -1;     // -1: its own block (swapchain images)
    };

    struct Deko3DTexture : RenderTexture {
        Deko3DDevice *device = nullptr;
        RenderTextureDesc desc;
        DkImageLayout layout{};
        DkImage image{};
        DkImageFormat format = DkImageFormat_None;
        DkImageType type = DkImageType_2D;
        Deko3DImageAlloc alloc;
        bool ownsMemory = true;
        bool valid = false;
        bool compressed = false;  // sasrt-nx (2026-10-21): made with DkImageFlags_HwCompression
        // renut-ultimate (2026-11-08, Deko3DOptions::depthResolveLazy): a compressed depth target written through
        // depthPendingView and not decompressed since it left the bind point. Decompressed when it next moves to a read
        // layout (a barrier), or before it is bound through another view. The command lists that touch it are recorded
        // one after another, in the order the GPU runs them.
        mutable bool depthPending = false;
        mutable DkImageView depthPendingView{};
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate): the texture-cache drop epoch
        // (g_deko3dTexDropEpoch) in which a draw last had it bound for sampling; 0: never.
        mutable uint64_t sampledEpoch = 0;
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedTargetWait): the fragment-wait epoch (g_deko3dWaitEpoch) of its last
        // sampling bind, copy, resolve or decompression; 0: never.
        mutable uint64_t usedWaitEpoch = 0;
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedReadWait): the fragment-wait epoch of its last write (bound as a target,
        // cleared, copied or resolved into, decompressed); 0: never.
        mutable uint64_t writtenWaitEpoch = 0;
        // sasrt-nx (2026-11-08, Deko3DOptions::preciseDepthResolve): a compressed depth image may hold compressed tiles
        // written since its last decompression (true until one: nothing known). Set where the GPU writes it (a draw that
        // writes depth or stencil, a clear, a copy, upload, blit or resolve into it), cleared by dkCmdBufResolveDepthValues
        // on it; kept in the order the lists are recorded (the order the GPU runs them).
        mutable bool depthUnresolved = true;

        Deko3DTexture(Deko3DDevice *device, const RenderTextureDesc &desc);
        Deko3DTexture(Deko3DDevice *device) : device(device) {} // swapchain images
        ~Deko3DTexture() override;
        std::unique_ptr<RenderTextureView> createTextureView(const RenderTextureViewDesc &desc) const override;
        void setName(const std::string &name) override;
        // The view of the whole texture as plume's default (no view given).
        DkImageView defaultView() const;
        // renut-ultimate (2026-11-10, from LostOdysseyRecomp ff013164; Deko3DOptions::descriptorCache): the sampled image
        // descriptor of defaultView(), made at its first write (the image never changes once made).
        mutable DkImageDescriptor sampledDescriptor{};
        mutable std::atomic<bool> sampledDescriptorReady{false};
    };

    struct Deko3DTextureView : RenderTextureView {
        const Deko3DTexture *texture = nullptr;
        DkImageView view{};
        RenderTextureViewDesc desc;
        // renut-ultimate (2026-11-10, Deko3DOptions::descriptorCache): the view's sampled image descriptor, made at its
        // first write. Renut gives every texture slot a view, so this is the cache that serves it.
        mutable DkImageDescriptor sampledDescriptor{};
        mutable std::atomic<bool> sampledDescriptorReady{false};
    };

    struct Deko3DSampler : RenderSampler {
        DkSamplerDescriptor descriptor{};
        RenderSamplerDesc desc;
    };

    // The bindings a shader uses (its banjo-dk line).
    struct Deko3DShaderUsage {
        // banjo-nx (2026-10-27): a vertex shader whose GLSL names gl_VertexID (true for any other stage and when unknown).
        bool readsVertexId = true;
        uint32_t uboMask = 0;
        uint32_t ssboMask = 0;
        bool textures = false;
        // Bound textures (the game shaders' texb): binding k's handle is made from two words of the shared constants
        // (UBO binding 2): the texture word's low 20 bits and the sampler word's high 12 (SamplerNone: sampler 0).
        // vivapinata-nx (2026-11-03): samplerVariant, added to the sampler's part of the handle: a fetch that names its own
        // filters takes the slot that many after its word's (render/textures.cpp SamplerSlotUncached).
        struct TexBinding {
            uint16_t textureWord = 0, samplerWord = 0;
            uint32_t samplerVariant = 0;
        };
        static constexpr uint16_t SamplerNone = 0xFFFF;
        std::vector<TexBinding> texBindings;
        // Vertex shaders: the original location of each attribute the shader reads, in its attribute order.
        std::vector<uint32_t> vertexInputs;
        // banjo-nx (2026-10-18): the user varying locations (below 32) the shader declares: a pixel shader's inputs, a
        // vertex shader's outputs (specialization constant 905, createGraphicsPipeline).
        uint32_t inputMask = 0;
        uint32_t outputMask = 0;
        // A vertex shader's outputs it writes as zero (its "// banjo-vszero=0x..." line): constant 906 of the pixel shader.
        uint32_t zeroOutputMask = 0;
        // banjo-nx (2026-10-19, GPU): the texture bindings whose level-0 size the shader reads from the texture size
        // buffer (UBO binding 3, entry k of the pixel stage, 32 + k of the vertex stage) instead of a texture query
        // (the banjo-dk line's txq=; tools/deko3d_glsl_opt.py, pass txq).
        uint32_t txqMask = 0;
        // vivapinata-nx (2026-11-03): a vertex shader that has the kill's constant 914 (tools/deko3d_glsl_opt.py vskill).
        bool writesVertexKill = false;
        // blur-nx (2026-11-12): a vertex or geometry shader whose text names gl_ClipDistance (true for the other stages).
        // One that does not writes no clip distance in any variant, and its pipelines draw with the clip slot off
        // (Deko3DGraphicsPipeline::noUserClip).
        bool writesClipDistance = true;
    };

    struct Deko3DShader : RenderShader {
        Deko3DDevice *device = nullptr;
        DkStage stage = DkStage_Vertex;
        // banjo-nx (2026-10-21): shared, so a background compile (and a pipeline waiting for one) holds the text without
        // a copy, also after the shader object is gone (Renut's prebuild workers destroy theirs).
        std::shared_ptr<const std::string> glsl;
        std::vector<uint8_t> defaultDksh;
        Deko3DShaderUsage usage;
        // Specialization constant ids the GLSL defines (SPIRV_CROSS_CONSTANT_ID_n), so a variant key holds only those.
        std::vector<uint32_t> specIds;
        std::string name;

        void setName(const std::string &name) override { this->name = name; }
    };

    // A loaded variant: DKSH in the device's code memory.
    struct Deko3DShaderVariant {
        DkShader shader{};  // first: a pipeline's const DkShader * is its variant's address (vsInputs below)
        bool valid = false;
        // sasrt-nx (2026-11-08, Deko3DOptions::vsInputMask): a vertex variant's generic attributes its program reads
        // (bit a: location a), from its shader program header's input map; all ones when not known.
        uint32_t vsInputs = ~0u;
    };

    struct Deko3DPipelineLayout : RenderPipelineLayout {
        struct Range {
            RenderDescriptorRangeType type = RenderDescriptorRangeType::UNKNOWN;
            uint32_t binding = 0;
            uint32_t count = 0;
            uint32_t descriptorIndex = 0; // first descriptor of the range in the set's contiguous numbering
            uint32_t slot = 0;           // deko3d UBO / SSBO binding of the first descriptor
        };
        struct Set {
            std::vector<Range> ranges;
        };
        std::vector<Set> sets;
        std::vector<RenderPushConstantRange> pushRanges;
        uint32_t pushSize = 0;
    };

    struct Deko3DDescriptorSet : RenderDescriptorSet {
        struct Range {
            RenderDescriptorRangeType type = RenderDescriptorRangeType::UNKNOWN;
            uint32_t binding = 0;
            uint32_t first = 0;  // first descriptor index
            uint32_t count = 0;
        };
        Deko3DDevice *device = nullptr;
        std::vector<Range> ranges;
        // Image and sampler descriptors: a block each (32 bytes a descriptor), the deko3d descriptor sets that bind
        // when this set is bound. Index i of the image table is image id i (and sampler id i).
        DkMemBlock imageBlock = nullptr, samplerBlock = nullptr;
        uint32_t imageCount = 0, samplerCount = 0;
        uint32_t imageFirst = 0, samplerFirst = 0; // descriptor index of the first image / sampler descriptor
        // Buffers (constant and storage): address and size per descriptor index.
        std::vector<DkBufExtents> buffers;
        // banjo-nx (2026-10-19, GPU): per image id, the level-0 size of the view written there as a texture query
        // reads it (width, height, depth or layers), for the texture size buffer (txq).
        std::vector<std::array<uint32_t, 3>> imageSizes;
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate): per image id, the texture written there (null: none).
        std::vector<const struct Deko3DTexture *> imageTextures;

        Deko3DDescriptorSet(Deko3DDevice *device, const RenderDescriptorSetDesc &desc);
        ~Deko3DDescriptorSet() override;
        void setBuffer(uint32_t descriptorIndex, const RenderBuffer *buffer, uint64_t bufferSize, const RenderBufferStructuredView *bufferStructuredView, const RenderBufferFormattedView *bufferFormattedView) override;
        void setTexture(uint32_t descriptorIndex, const RenderTexture *texture, RenderTextureLayout textureLayout, const RenderTextureView *textureView) override;
        void setSampler(uint32_t descriptorIndex, const RenderSampler *sampler) override;
        void setAccelerationStructure(uint32_t descriptorIndex, const RenderAccelerationStructure *accelerationStructure) override;
        const Range *rangeOf(uint32_t descriptorIndex) const;
    };

    struct Deko3DPipeline : RenderPipeline {
        enum class Type { Graphics, Compute };
        Type type = Type::Graphics;
        Deko3DDevice *device = nullptr;
        const Deko3DPipelineLayout *layout = nullptr;
        std::string name;
        void setName(const std::string &name) override { this->name = name; }
        RenderPipelineProgram getProgram(const std::string &) const override { return {}; }
    };

    // banjo-nx (2026-10-21, asyncVariants): a variant to compile in the background: its key (0: none), the shader's
    // shared text, the defines plume inserts after its first line, the stage and the shader's name (for the log).
    struct Deko3DVariantRequest {
        uint64_t key = 0;
        std::shared_ptr<const std::string> glsl;
        std::string defines;
        DkStage stage = DkStage_Vertex;
        std::string name;
    };

    struct Deko3DGraphicsPipeline : Deko3DPipeline {
        // banjo-nx (2026-10-27, deko3d recording): what a draw's bind and draw read, first and together (three lines
        // after the base), the fixed-function state's bulk after it. Before, these were spread before and after the
        // blend, attribute and vertex buffer states (over a kilobyte): a new pipeline (60 % of the city's draws) cost
        // four to five misses in setPipeline, bindGraphicsBuffers, bindTextures and Deko3DRefreshPipeline (the b35
        // console profile's hottest plume lines). Callers prefetch these lines once they know the pipeline.
        bool valid = false;
        bool boundTextures = false;
        bool textures = false;
        bool usesBlendConstant = false;
        bool stencilEnabled = false;
        // sasrt-nx (2026-11-08, Deko3DOptions::preciseDepthResolve): a draw with it may write the depth target (depth
        // writes, or a stencil test with an operation other than keep).
        bool writesDepthStencil = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec): its vertex shader has constant 929 = 0 (no user clip distance): the
        // draws set the clip slot off (SET_USER_CLIP_ENABLE 0) while it is bound.
        bool noUserClip = false;
        bool dynamicStencilReference = false;
        bool dynamicDepthBias = false;
        std::atomic<bool> provisional{false};  // (see pending, below; counted in g_deko3dProvisionalPipelines)
        ~Deko3DGraphicsPipeline();  // banjo-nx (2026-10-28): a provisional pipeline that goes leaves the count
        DkPrimitive primitive = DkPrimitive_Triangles;
        uint32_t stageMask = 0;
        uint32_t shaderCount = 0;
        uint32_t blendCount = 0;
        const DkShader *shaders[3] = {};  // vertex, geometry, fragment (null when absent)
        // banjo-nx (2026-10-19, CPU proposal 13): the shader of each stage (vertex, geometry, fragment; null when
        // absent), so a bind does not ask deko3d, and a hash of each group of the fixed-function state, so a bind
        // after a pipeline of other state sends only the groups that differ (kStateGroups, Deko3DCommandList).
        const DkShader *stageShaders[3] = {};
        uint64_t stateHash = 0; // equal hashes: the same fixed-function state (binds skipped)
        // UBO / SSBO slots each stage reads (vertex, geometry, fragment).
        uint32_t uboMask[3] = {}, ssboMask[3] = {};
        uint32_t txqMask[3] = {};  // banjo-nx (2026-10-19): Deko3DShaderUsage::txqMask of each stage
        // banjo-nx (2026-10-27, render thread night): the 64-byte lines of the shared constants that hold its
        // bindings' texture and sampler words (bindTextures reads only those).
        uint32_t texLines = 0;
        std::vector<Deko3DShaderUsage::TexBinding> texBindings[3];
        uint64_t groupHash[9] = {};
        // The bulk: read when the state groups differ from what is bound.
        DkRasterizerState rasterizer{};
        DkMultisampleState multisample{};
        DkColorState color{};
        DkColorWriteState colorWrite{};
        DkBlendState blend[8]{};
        uint32_t blendWords[8][7]{}; // the IndependentBlend registers written again (deko3d 0.5.0's FuncAlphaDst)
        DkDepthStencilState depthStencil{};
        uint8_t stencilReadMask = 0xFF, stencilWriteMask = 0xFF, stencilReference = 0;
        float depthBias = 0.0f, depthBiasClamp = 0.0f, slopeScaledDepthBias = 0.0f;
        DkVtxAttribState attribs[32]{};
        uint32_t attribCount = 0;
        uint32_t fetchedAttribs = 0;  // sasrt-nx (2026-11-08, Deko3DOptions::vsInputMask): the attribs that read a buffer
        DkVtxBufferState vtxBuffers[16]{};
        uint32_t vtxBufferCount = 0;
        // banjo-nx (2026-10-21, Deko3DOptions::asyncVariants): the stages drawn with their shader's generic variant
        // while their own is compiled in the background: what to compile (Deko3DVariantRequest: the variant's key, the
        // shared text and its defines; not the shader object, which Renut's prebuild workers destroy when a batch is
        // done); 'provisional' while any stage waits, and the device's async epoch it was last refreshed at
        // (Deko3DRefreshPipeline, which also queues a request the queue had no room for).
        Deko3DVariantRequest pending[3];
        uint64_t pendingEpoch = 0;
    };

    struct Deko3DComputePipeline : Deko3DPipeline {
        const DkShader *shader = nullptr;
        bool valid = false;
        uint32_t uboMask = 0, ssboMask = 0;
    };

    struct Deko3DCommandFence : RenderCommandFence {
        DkFence fence{};
        bool pending = false;
    };

    struct Deko3DCommandSemaphore : RenderCommandSemaphore {
    };

    struct Deko3DFramebuffer : RenderFramebuffer {
        DkImageView colors[8]{};
        uint32_t colorCount = 0;
        DkImageView depth{};
        bool hasDepth = false;
        bool depthCompressed = false;  // sasrt-nx (2026-10-21): the depth target has hardware compression
        const Deko3DTexture *depthTexture = nullptr;  // renut-ultimate (2026-11-08): its texture (depthResolveLazy)
        const Deko3DTexture *colorTextures[8] = {};  // sasrt-nx (2026-11-08, Deko3DOptions::trackedReadWait)
        uint64_t serial = 0;  // sasrt-nx (2026-11-08, Deko3DOptions::sameTargetsKept): unique to this object
        uint32_t width = 0, height = 0;
        bool srgb[8] = {};
        uint32_t getWidth() const override { return width; }
        uint32_t getHeight() const override { return height; }
    };

    struct Deko3DQueryPool : RenderQueryPool {
        Deko3DDevice *device = nullptr;
        DkMemBlock block = nullptr;
        std::vector<uint64_t> results;
        Deko3DQueryPool(Deko3DDevice *device, uint32_t count);
        ~Deko3DQueryPool() override;
        void queryResults() override;
        bool queryResults(uint32_t count) override;
        bool partialResults() const override { return true; }
        const uint64_t *getResults() const override { return results.data(); }
        uint32_t getCount() const override { return uint32_t(results.size()); }
    };

    struct Deko3DSwapChain;

    // banjo-nx (2026-10-28, per-draw night): what a command list holds as its bound shaders before its first bind.
    inline const DkShader *const kDeko3DShadersUnknown = reinterpret_cast<const DkShader *>(uintptr_t(1));

    struct Deko3DCommandList : RenderCommandList {
        Deko3DCommandQueue *queue = nullptr;
        Deko3DDevice *device = nullptr;
        DkCmdBuf cmd = nullptr;
        DkCmdList list = 0;
        bool open = false;
        // Command memory: blocks reused from the start at each begin() (the application waits for the list's last
        // submission before it begins it again, as Vulkan requires).
        std::vector<std::pair<DkMemBlock, uint32_t>> memory;
        size_t memoryUsed = 0;
        // Transient data (compute push constants): a linear area per list, reset at begin().
        std::vector<std::pair<DkMemBlock, uint32_t>> transient;
        size_t transientBlock = 0;
        uint32_t transientUsed = 0;

        // What is bound (to skip binds that change nothing).
        const Deko3DGraphicsPipeline *graphicsPipeline = nullptr;
        const Deko3DComputePipeline *computePipeline = nullptr;
        uint64_t boundStateHash = 0;
        // banjo-nx (2026-10-28, per-draw night): kDeko3DShadersUnknown until the list's first bind (which binds every
        // stage); then the shaders bound per stage, so that a bind rebinds only the stages that changed.
        const DkShader *boundShaders[3] = {kDeko3DShadersUnknown, kDeko3DShadersUnknown, kDeko3DShadersUnknown};
        // banjo-nx (2026-10-19): the groups of fixed-function state as last bound (Deko3DGraphicsPipeline::groupHash;
        // 0: unknown). The dynamic depth bias and stencil reference make theirs unknown.
        uint64_t boundGroups[9] = {};
        const Deko3DPipelineLayout *graphicsLayout = nullptr, *computeLayout = nullptr;
        DkGpuAddr boundImageSet = 0, boundSamplerSet = 0;
        uint32_t boundImageCount = 0, boundSamplerCount = 0;
        // banjo-nx (2026-10-19, GPU): the descriptor set whose image table is bound (its sizes, Deko3DDescriptorSet::
        // imageSizes), and what the texture size buffer holds after this list's updates so far (valid bit per entry).
        const struct Deko3DDescriptorSet *boundImageSetObj = nullptr;
        uint32_t texSizeShadow[64][4] = {};
        uint64_t texSizeValid = 0;
        DkBufExtents graphicsUbo[16]{}, graphicsSsbo[16]{};
        uint32_t graphicsUboValid = 0, graphicsSsboValid = 0;
        uint32_t uboBoundStages[16] = {}, ssboBoundStages[16] = {}; // DkStage bits a slot was bound for
        // banjo-nx (2026-10-26, Unleashed-style step 2): the vertex buffers bound in this list (a slot's address and
        // size), so a slot bound again with the same range is not sent again (the renderer binds every stream and the
        // zero stream at every draw).
        DkBufExtents boundVtx[16]{};
        uint32_t boundVtxValid = 0;
        // banjo-nx (2026-10-23, batch 6): a slot or the graphics pipeline changed since bindGraphicsBuffers last ran
        // (with neither, every slot its pipeline reads is bound for its stages already: the walk binds nothing).
        bool graphicsBuffersDirty = true;
        DkBufExtents computeUbo[16]{}, computeSsbo[16]{};
        uint32_t computeUboValid = 0, computeSsboValid = 0;
        bool computeUboDirty = true, computeSsboDirty = true;
        uint8_t computePush[256]{};
        uint32_t computePushSize = 0;
        DkIdxFormat indexFormat = DkIdxFormat_Uint16, boundIndexFormat = DkIdxFormat_Uint16;
        uint32_t indexSize = 2;
        DkGpuAddr indexAddress = 0;
        bool descriptorsInvalidated = false;
        uint64_t descriptorEpoch = 0;
        // banjo-nx (2026-10-28, per-draw night): the device's two numbers as of the last drop (flushDescriptorInvalidate
        // drops only the caches whose number moved).
        uint64_t seenDescriptors = 0, seenCode = 0;
        const Deko3DFramebuffer *framebuffer = nullptr;
        // sasrt-nx (2026-10-21, Deko3DOptions::depthResolve): the bound depth target is compressed and may hold tiles
        // the texture unit cannot read (resolveBoundDepth).
        bool depthDirty = false;
        // Bound textures: the handles last bound per graphics stage, and the buffer the shared constants were last
        // found in (GPU range and CPU address).
        DkResHandle boundTex[3][32]{};
        uint32_t boundTexCount[3] = {};
        // banjo-nx (2026-10-27, render thread night): bindTextures' last whole run (its pipeline, descriptor epoch, image
        // set and shared uniform buffer) and the shared constant lines the renderer wrote since (Deko3DSharedLinesWritten);
        // with the same of all four and none of the pipeline's lines written, its run would bind and push nothing.
        const Deko3DGraphicsPipeline *texPipeline = nullptr;
        uint64_t texEpoch = 0;
        const void *texImageSet = nullptr;
        DkGpuAddr texSharedAddr = 0;
        uint32_t texLinesWritten = ~0u;
        DkGpuAddr sharedBase = 0;
        uint32_t sharedSize = 0;
        const uint8_t *sharedCpu = nullptr;
        // banjo-nx (2026-10-19, render-thread proposal 5, Deko3DSetSharedWords): the CPU's copy of the shared constants
        // the next draws read, when the renderer pushes them (its uniform buffer holds them only on the GPU's side).
        const uint8_t *sharedWords = nullptr;
        uint32_t sharedWordsSize = 0;
        DkViewport viewports[16]{};
        uint32_t viewportCount = 0;
        DkScissor scissors[16]{};
        uint32_t scissorCount = 0;
        float blendConstant[4] = {};

        Deko3DCommandList(Deko3DCommandQueue *queue);
        ~Deko3DCommandList() override;
        void begin() override;
        void end() override;
        void barriers(RenderBarrierStages stages, const RenderBufferBarrier *bufferBarriers, uint32_t bufferBarriersCount, const RenderTextureBarrier *textureBarriers, uint32_t textureBarriersCount) override;
        void dispatch(uint32_t threadGroupCountX, uint32_t threadGroupCountY, uint32_t threadGroupCountZ) override;
        void traceRays(uint32_t width, uint32_t height, uint32_t depth, RenderBufferReference shaderBindingTable, const RenderShaderBindingGroupsInfo &shaderBindingGroupsInfo) override;
        void drawInstanced(uint32_t vertexCountPerInstance, uint32_t instanceCount, uint32_t startVertexLocation, uint32_t startInstanceLocation) override;
        void drawIndexedInstanced(uint32_t indexCountPerInstance, uint32_t instanceCount, uint32_t startIndexLocation, int32_t baseVertexLocation, uint32_t startInstanceLocation) override;
        void setPipeline(const RenderPipeline *pipeline) override;
        void setComputePipelineLayout(const RenderPipelineLayout *pipelineLayout) override;
        void setComputePushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override;
        void setComputeDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override;
        void setGraphicsPipelineLayout(const RenderPipelineLayout *pipelineLayout) override;
        void setGraphicsPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override;
        void setGraphicsDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override;
        void setGraphicsDescriptorSetDynamic(RenderDescriptorSet *descriptorSet, uint32_t setIndex, const uint32_t *dynamicOffsets, uint32_t dynamicOffsetsCount) override;
        void setGraphicsRootDescriptor(RenderBufferReference bufferReference, uint32_t rootDescriptorIndex) override;
        void setRaytracingPipelineLayout(const RenderPipelineLayout *pipelineLayout) override;
        void setRaytracingPushConstants(uint32_t rangeIndex, const void *data, uint32_t offset, uint32_t size) override;
        void setRaytracingDescriptorSet(RenderDescriptorSet *descriptorSet, uint32_t setIndex) override;
        void setIndexBuffer(const RenderIndexBufferView *view) override;
        void setVertexBuffers(uint32_t startSlot, const RenderVertexBufferView *views, uint32_t viewCount, const RenderInputSlot *inputSlots) override;
        void setViewports(const RenderViewport *viewports, uint32_t count) override;
        void setScissors(const RenderRect *scissorRects, uint32_t count) override;
        void setFramebuffer(const RenderFramebuffer *framebuffer) override;
        void setDepthBias(float depthBias, float depthBiasClamp, float slopeScaledDepthBias) override;
        void setStencilReference(uint32_t reference) override;
        void clearColor(uint32_t attachmentIndex, RenderColor colorValue, const RenderRect *clearRects, uint32_t clearRectsCount) override;
        void clearDepthStencil(bool clearDepth, bool clearStencil, float depthValue, uint32_t stencilValue, const RenderRect *clearRects, uint32_t clearRectsCount) override;
        void copyBufferRegion(RenderBufferReference dstBuffer, RenderBufferReference srcBuffer, uint64_t size) override;
        void copyTextureRegion(const RenderTextureCopyLocation &dstLocation, const RenderTextureCopyLocation &srcLocation, uint32_t dstX, uint32_t dstY, uint32_t dstZ, const RenderBox *srcBox) override;
        void copyBuffer(const RenderBuffer *dstBuffer, const RenderBuffer *srcBuffer) override;
        void copyTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override;
        void resolveTexture(const RenderTexture *dstTexture, const RenderTexture *srcTexture) override;
        void resolveTextureRegion(const RenderTexture *dstTexture, uint32_t dstX, uint32_t dstY, const RenderTexture *srcTexture, const RenderRect *srcRect, RenderResolveMode resolveMode) override;
        void buildBottomLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, const RenderBottomLevelASBuildInfo &buildInfo) override;
        void buildTopLevelAS(const RenderAccelerationStructure *dstAccelerationStructure, RenderBufferReference scratchBuffer, RenderBufferReference instancesBuffer, const RenderTopLevelASBuildInfo &buildInfo) override;
        void discardTexture(const RenderTexture *texture) override;
        void resetQueryPool(const RenderQueryPool *queryPool, uint32_t queryFirstIndex, uint32_t queryCount) override;
        void writeTimestamp(const RenderQueryPool *queryPool, uint32_t queryIndex) override;

        // banjo-nx: the blend constant (BLEND_FACTOR), which plume has no call for.
        void setBlendConstant(const float rgba[4]);

        // internal
        void addMemory(size_t minimum);
        bool transientAlloc(uint32_t size, uint32_t alignment, void **cpu, DkGpuAddr *gpu);
        void flushDescriptorInvalidate();
        // banjo-nx (2026-10-19, Deko3DOptions::mergeBarriers): the barrier recorded but not yet emitted.
        DkBarrier pendingBarrierMode = DkBarrier_None;
        uint32_t pendingBarrierInvalidate = 0;
        bool pendingBarrier = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_convert_lead_wait): buffer barriers requested (each a wait for idle with the
        // data caches dropped, before the next work), counted for Deko3DBufferWaits.
        uint64_t bufferWaits = 0;
        // sasrt-nx (2026-11-08, Deko3DOptions::sameTargetsKept): the serial of the framebuffer whose targets the GPU has
        // bound in this list (0: unknown).
        uint64_t boundTargetsSerial = 0;
        // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec): SET_USER_CLIP_ENABLE as this list last wrote it (-1: plume does not
        // write it, Deko3DOptions::userClipEnable -1).
        int boundUserClip = -1;
        // sasrt-nx (2026-11-08, Deko3DOptions::leanBarrierSkip): work recorded since the last barrier that waited for the
        // fragments (true at a list's start: the list before may still run).
        bool workSinceFragmentWait = true;
        // sasrt-nx (2026-11-08, leanBarrierSkip): a dispatch since the last wait for idle (a fragment barrier does not wait
        // for compute jobs: it then leaves workSinceFragmentWait set).
        bool dispatchSinceIdle = true;
        // sasrt-nx (2026-11-08, sasrt_gpu_export_raster_discard): the next draw with the rasterizer off (Deko3DDiscardNextDraw).
        bool discardNextDraw = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_export_list_topology): the next draw's primitive instead of the pipeline's
        // (DkPrimitive + 1; 0 none), taken once.
        uint32_t nextPrimitive = 0;
        uint32_t pendingBarrierWhy = 0;  // sasrt-nx (2026-11-08, Deko3DOptions::diagBarriers): Deko3DBarrierWhy bits
        // banjo-nx (2026-10-19, render-thread proposal 13): per-draw counters of this list, added to the totals at end()
        // (no atomic read-modify-write per draw: the A57 has no LSE).
        uint64_t statDraws = 0, statWindowDraws = 0;
        uint64_t statStripRestarts = 0;  // sasrt-nx (2026-10-30)
        // sasrt-nx (2026-10-30, Deko3DOptions::stripRestart): the restart index set on the queue (0: off), 1 when not
        // known (a list's start: the queue keeps whatever the list before it set).
        uint32_t boundRestart = 1;
        uint64_t statTexRuns = 0, statTexSkips = 0;  // banjo-nx (2026-10-27): into Deko3DStats at end()
        void emitBarrier(DkBarrier mode, uint32_t invalidate);
        void flushBarriersRaw() {
            if (pendingBarrier)
                emitBarrier(pendingBarrierMode, pendingBarrierInvalidate);
        }
        // Port 161 (from fnxf_xiii, its note of 2026-10-30; Deko3DOptions::copyRunBarriers): a run of buffer copies
        // needs no barrier between its copies. deko3d's buffer copy is the copy engine's LaunchDma with
        // TransferType::NonPipelined (deko3d source/maxwell/gpu_transfer.cpp), so one copy of a channel finishes before
        // the next starts, whatever they read and write; and a Full barrier is two gpfifo entries, which is what the
        // channel runs out of (fnxf_xiii: 2020 entries at a kickoff that timed out, 675 copy-kernel draws each fenced
        // before and after; port 157 handles such a refusal after the fact). So once a Full barrier has run
        // (copyRunFenced: nothing but buffer copies since it), a barrier recorded before another buffer copy stays
        // pending, as a Full, and is emitted before the next command the copy engine does not run: every other
        // command calls flushBarriers. Without a Full first nothing is held back.
        bool copyRunFenced = false;
        uint32_t sampleMaskSet = 0xFFFFFFFFu;  // port 166: the mask last recorded (none yet in this list)
        void flushBarriers() {
            flushBarriersRaw();
            copyRunFenced = false;
        }
        void beforeBufferCopy();  // plume_deko3d_commands.cpp
        void invalidateZcullAfterCopy(const Deko3DTexture *dst);
        // sasrt-nx (2026-11-08, Deko3DOptions::preciseDepthResolve): the draw about to be recorded may write the bound
        // compressed depth target: it is dirty (decompressed before it is read or leaves the bind point).
        // sasrt-nx (2026-11-08): Deko3DStats::sasrtRound[i] + 1 (a load and a store).
        // sasrt-nx (2026-11-08, sasrt_gpu_clip_spec): the clip slot off for a pipeline whose vertex shader writes no clip
        // distance, back to Deko3DOptions::userClipEnable for one that does (written only when it changes).
        void noteUserClip();
        // sasrt-nx (2026-11-08, Deko3DOptions::vsInputMask): the pipeline's vertex attributes and buffers, the ones its
        // bound vertex variant does not read as constants (bound only when that differs from what is bound).
        void bindVertexInput(const Deko3DGraphicsPipeline *p, const DkShader *vs);
        // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate): image id 'id' of the bound image set sampled in
        // the current epoch; every bound one again (after a drop: later draws sample them still).
        void markSampled(uint32_t id);
        void markBoundSampled();
        void markTargetsWritten();  // sasrt-nx (2026-11-08, trackedReadWait): the bound framebuffer's textures
        uint32_t extraFragTex[8] = {};  // Deko3DBindFragmentTextures' ids (not in boundTex)
        uint32_t extraFragTexCount = 0;
        static void bumpStat(std::atomic<uint64_t> &c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
        static void bumpRound(int i) {
            std::atomic<uint64_t> &c = GetDeko3DStats().sasrtRound[i];
            c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        // sasrt-nx (2026-11-08, Deko3DOptions::lazyDepthResolve): whether the image may wait for its read.
        static bool lazyResolvable(const Deko3DTexture *t);
        void resolveUnboundDepth(const Deko3DTexture *t);
        void noteDepthWrite() {
            if (framebuffer && framebuffer->depthCompressed && graphicsPipeline->writesDepthStencil) {
                depthDirty = true;
                if (framebuffer->depthTexture)
                    framebuffer->depthTexture->depthUnresolved = true;
            }
        }
        // sasrt-nx (2026-10-21): dkCmdBufResolveDepthValues on the bound depth target when depthDirty (barrierAfter:
        // a texture-cache barrier follows it, for reads whose own barrier was recorded before it).
        void resolveBoundDepth(bool barrierAfter);
        uint64_t currentEpoch() const;
        void bindGraphicsBuffers();
        void bindTextures();
        void restoreScissors();
        // renut-ultimate (2026-11-08, Deko3DOptions::depthResolveLazy): the bound dirty depth target left for later (its
        // texture's depthPending), and a pending one decompressed through its own view (rebind: the bound targets
        // bound again after).
        void deferBoundDepth();
        void resolvePendingDepth(const Deko3DTexture *t, bool rebind);
    };

    struct Deko3DCommandQueue : RenderCommandQueue {
        Deko3DDevice *device = nullptr;
        RenderCommandListType type = RenderCommandListType::UNKNOWN;

        Deko3DCommandQueue(Deko3DDevice *device, RenderCommandListType type);
        std::unique_ptr<RenderCommandList> createCommandList() override;
        std::unique_ptr<RenderSwapChain> createSwapChain(const RenderSwapChainDesc &desc) override;
        void executeCommandLists(const RenderCommandList **commandLists, uint32_t commandListCount, RenderCommandSemaphore **waitSemaphores, uint32_t waitSemaphoreCount, RenderCommandSemaphore **signalSemaphores, uint32_t signalSemaphoreCount, RenderCommandFence *signalFence) override;
        void waitForCommandFence(RenderCommandFence *fence) override;
    };

    struct Deko3DSwapChain : RenderSwapChain {
        Deko3DCommandQueue *queue = nullptr;
        Deko3DDevice *device = nullptr;
        RenderSwapChainDesc desc;
        DkSwapchain swapchain = nullptr;
        std::vector<std::unique_ptr<Deko3DTexture>> textures;
        std::vector<DkMemBlock> blocks;
        uint32_t width = 0, height = 0;
        bool vsync = true;

        Deko3DSwapChain(Deko3DCommandQueue *queue, const RenderSwapChainDesc &desc);
        ~Deko3DSwapChain() override;
        bool present(uint32_t textureIndex, RenderCommandSemaphore **waitSemaphores, uint32_t waitSemaphoreCount) override;
        void wait() override;
        bool resize() override;
        bool needsResize() const override;
        void setVsyncEnabled(bool vsyncEnabled) override;
        bool isVsyncEnabled() const override { return vsync; }
        uint32_t getWidth() const override { return width; }
        uint32_t getHeight() const override { return height; }
        RenderTexture *getTexture(uint32_t textureIndex) override;
        uint32_t getTextureCount() const override { return uint32_t(textures.size()); }
        bool acquireTexture(RenderCommandSemaphore *signalSemaphore, uint32_t *textureIndex) override;
        RenderWindow getWindow() const override { return desc.renderWindow; }
        bool isEmpty() const override { return swapchain == nullptr; }
        uint32_t getRefreshRate() const override { return 60; }
        void destroy();
    };

    struct Deko3DDevice : RenderDevice {
        DkDevice dk = nullptr;
        DkQueue queue = nullptr;
        RenderDeviceCapabilities capabilities;
        RenderDeviceDescription description;

        // Image heap: chunks with first-fit free lists; frees wait for the GPU (see retire()).
        struct Chunk {
            DkMemBlock block = nullptr;
            uint32_t size = 0;
            std::vector<std::pair<uint32_t, uint32_t>> freeList; // offset, size, sorted by offset
            // sasrt-nx (2026-11-09, Deko3DSetDedicatedTargets; port 58): one render or depth target's own, given back to
            // the GPU region as soon as that target is freed; never shared.
            bool dedicated = false;
        };
        std::mutex heapMutex;
        std::vector<Chunk> chunks;
        // sasrt-nx (2026-11-11, Deko3DSetTargetBlockCacheMb): emptied dedicated targets' blocks kept for the next
        // targets (heapMutex), oldest first.
        struct CachedBlock {
            DkMemBlock block = nullptr;
            uint32_t size = 0;
        };
        std::vector<CachedBlock> targetCache;
        uint64_t targetCacheBytes = 0;
        // blur-nx (2026-11-18, for 1 % lows, every port; Deko3DSetImageSpareChunk): a thread that makes the image heap's
        // next shared chunk ahead, so a texture that finds no room takes a ready chunk instead of making a 32 MB block
        // where it is (20-38 ms on Blur's console, 4-5 times a race, on the thread creating the texture with heapMutex
        // held). wantSpareLocked (heapMutex held) asks for one when no shared chunk is empty; heapMutex, then spareMutex.
        std::mutex spareMutex;
        std::condition_variable spareWake, spareDone;
        bool spareWanted = false, spareMaking = false, spareStop = false, spareStarted = false, spareRunning = false;
        void wantSpareLocked();
        void spareWorker();
        void cacheTargetBlockLocked(DkMemBlock block, uint32_t size);
        DkMemBlock takeCachedTargetBlockLocked(uint32_t size, uint32_t &blockSize);
        uint64_t releaseTargetCacheLocked();

        // Shader code: blocks, bump allocated.
        std::mutex codeMutex;
        std::vector<std::pair<DkMemBlock, uint32_t>> codeBlocks; // block, used
        std::atomic<uint64_t> codeEpoch{1}; // advances with each code load (command lists invalidate the shader cache)
        std::unordered_map<uint64_t, std::unique_ptr<Deko3DShaderVariant>> variants; // by variant source hash
        std::mutex variantMutex;

        // The uam worker: one thread (uam is not reentrant), 8 MB of stack.
        std::thread compileThread;
        std::mutex compileMutex;
        std::condition_variable compileWake, compileDone;
        struct CompileJob {
            DkStage stage;
            const std::string *source;
            std::vector<uint8_t> *dksh;
            std::string *log;
            bool done = false;
            bool ok = false;
            // banjo-nx (2026-10-21, asyncVariants): nobody waits; the uam thread finishes it (finishAsync) and deletes
            // it. The text and the results are the job's own.
            bool async = false;
            bool cached = false;  // banjo-nx (2026-10-23): read from the cache file, not compiled (asyncCacheReads)
            bool drawWaits = false;  // banjo-nx (2026-10-23): blocking, and the waiting thread draws (the game stops)
            bool background = false;  // asked by a thread that does not draw (Deko3DSetBackgroundThread): queued last
            Deko3DVariantRequest request;  // the text is made from it on the uam thread, one at a time
            std::string ownSource;
            std::vector<uint8_t> ownDksh;
            std::string ownLog;
        };
        std::vector<CompileJob *> compileQueue;
        // banjo-nx (2026-10-18): the uam thread's handle, the priorities of the threads waiting for a compile, and the
        // priority last set (applyCompilePriority).
        uint32_t compileThreadHandle = 0;
        std::vector<int32_t> compileWaiters;
        int32_t compileAppliedPriority = -1;
        int32_t compileBasePriority = 0;  // the uam thread's own, as the kernel took it (0: not started)
        bool compileStop = false;
        bool compilerReady = false;
        // banjo-nx (2026-10-21, asyncVariants): the variant keys queued or compiling in the background (compileMutex),
        // the epoch each finished one advances, and one refresh of a provisional pipeline at a time.
        std::unordered_set<uint64_t> asyncQueued;
        uint32_t asyncBackgroundQueued = 0;  // of them, asked by background threads (capped: kAsyncBackgroundCap)
        std::atomic<uint64_t> asyncEpoch{0};
        std::mutex refreshMutex;

        // Retired memory: freed once the queue has passed the submission that was current when it was retired.
        std::mutex retireMutex;
        struct Retired {
            uint64_t serial;
            Deko3DImageAlloc image;
            DkMemBlock block;
        };
        std::vector<Retired> retired;
        std::atomic<uint64_t> submitSerial{1};
        std::atomic<uint64_t> completedSerial{0};
        static constexpr uint32_t SerialFences = 16;
        DkFence serialFences[SerialFences]{};
        uint64_t serialFenceValue[SerialFences] = {};

        // Descriptors written by the CPU since the GPU's descriptor cache was last invalidated (by a command list).
        std::atomic<uint64_t> descriptorEpoch{1};

        // The push constant UBO (graphics stages; dkCmdBufPushConstants updates it in command order).
        DkMemBlock pushBlock = nullptr;
        DkGpuAddr pushAddr = 0;
        static constexpr uint32_t PushSize = 256;
        // banjo-nx (2026-10-19, GPU): the texture size buffer (UBO binding 3: 64 uvec4, pixel bindings then vertex),
        // updated in command order like the push constants.
        DkMemBlock texSizeBlock = nullptr;
        DkGpuAddr texSizeAddr = 0;
        static constexpr uint32_t TexSizeSize = 64 * 16;
        // Zeros for missing vertex buffers (Vulkan's null buffer).
        DkMemBlock zeroBlock = nullptr;
        DkGpuAddr zeroAddr = 0;
        static constexpr uint32_t ZeroSize = 4096;
        // Scratch (local) memory per warp the queue was made with; a shader that needs more is refused (logged).
        uint32_t perWarpScratch = 0;

        // Every buffer by its GPU address (bound textures read the shared constants' words through it).
        std::mutex bufferMutex;
        std::map<DkGpuAddr, std::pair<uint32_t, const uint8_t *>> bufferRanges; // GPU address -> size, CPU address
        bool findBuffer(DkGpuAddr address, DkGpuAddr &base, uint32_t &size, const uint8_t *&cpu);

        Deko3DDevice();
        ~Deko3DDevice() override;
        bool initialize();

        std::unique_ptr<RenderDescriptorSet> createDescriptorSet(const RenderDescriptorSetDesc &desc) override;
        std::unique_ptr<RenderShader> createShader(const void *data, uint64_t size, const char *entryPointName, RenderShaderFormat format) override;
        std::unique_ptr<RenderSampler> createSampler(const RenderSamplerDesc &desc) override;
        std::unique_ptr<RenderPipeline> createComputePipeline(const RenderComputePipelineDesc &desc) override;
        std::unique_ptr<RenderPipeline> createGraphicsPipeline(const RenderGraphicsPipelineDesc &desc) override;
        std::unique_ptr<RenderPipeline> createRaytracingPipeline(const RenderRaytracingPipelineDesc &desc, const RenderPipeline *previousPipeline) override;
        std::unique_ptr<RenderCommandQueue> createCommandQueue(RenderCommandListType type) override;
        std::unique_ptr<RenderBuffer> createBuffer(const RenderBufferDesc &desc) override;
        std::unique_ptr<RenderTexture> createTexture(const RenderTextureDesc &desc) override;
        std::unique_ptr<RenderAccelerationStructure> createAccelerationStructure(const RenderAccelerationStructureDesc &desc) override;
        std::unique_ptr<RenderPool> createPool(const RenderPoolDesc &desc) override;
        std::unique_ptr<RenderPipelineLayout> createPipelineLayout(const RenderPipelineLayoutDesc &desc) override;
        std::unique_ptr<RenderCommandFence> createCommandFence() override;
        std::unique_ptr<RenderCommandSemaphore> createCommandSemaphore() override;
        std::unique_ptr<RenderFramebuffer> createFramebuffer(const RenderFramebufferDesc &desc) override;
        std::unique_ptr<RenderQueryPool> createQueryPool(uint32_t queryCount) override;
        void setBottomLevelASBuildInfo(RenderBottomLevelASBuildInfo &buildInfo, const RenderBottomLevelASMesh *meshes, uint32_t meshCount, bool preferFastBuild, bool preferFastTrace) override;
        void setTopLevelASBuildInfo(RenderTopLevelASBuildInfo &buildInfo, const RenderTopLevelASInstance *instances, uint32_t instanceCount, bool preferFastBuild, bool preferFastTrace) override;
        void setShaderBindingTableInfo(RenderShaderBindingTableInfo &tableInfo, const RenderShaderBindingGroups &groups, const RenderPipeline *pipeline, RenderDescriptorSet **descriptorSets, uint32_t descriptorSetCount) override;
        const RenderDeviceCapabilities &getCapabilities() const override { return capabilities; }
        const RenderDeviceDescription &getDescription() const override { return description; }
        RenderSampleCounts getSampleCountsSupported(RenderFormat format) const override;
        bool beginCapture() override { return false; }
        bool endCapture() override { return false; }

        // internal
        DkMemBlock makeBlock(uint32_t size, uint32_t flags, const char *what);
        DkMemBlock makeBlockUntimed(uint32_t size, uint32_t flags, const char *what);  // blur-nx (2026-11-18): makeBlock's body
        void destroyBlock(DkMemBlock block);
        // PLUME_DEKO3D_GUARDS (debug builds): every block's storage has guard bytes after it, checked here.
        void checkGuards();
        struct GuardedBlock {
            DkMemBlock block;
            uint8_t *storage;
            uint32_t size;
            const char *what;
        };
        std::mutex guardMutex;
        std::vector<GuardedBlock> guarded;
        // banjo-nx (2026-10-28, the console's OOM).
        // Frees the image heap's empty chunks (heapMutex held); the bytes given back.
        uint64_t releaseEmptyChunksLocked();
        bool imageAlloc(uint32_t size, uint32_t alignment, Deko3DImageAlloc &out, bool dedicated = false);
        void retire(const Deko3DImageAlloc &image, DkMemBlock block);
        void collectRetired();
        void noteSubmission(); // after a submission: a fence for its serial
        // banjo-nx (2026-10-21): 'pendingKey' set (asyncVariants): a variant neither loaded nor in the cache is queued
        // for the uam thread and the shader's generic one is returned, its key in *pendingKey (0 otherwise).
        // banjo-nx (2026-10-23): 'noCompile': nullptr instead of compiling a variant neither loaded nor in the cache.
        const DkShader *loadVariant(const Deko3DShader *shader, const RenderSpecConstant *specConstants, uint32_t specConstantsCount,
                                    Deko3DVariantRequest *pending = nullptr, bool noCompile = false);
        // Queues the request once (false: a background thread's, and their cap reached). compileMutex not held.
        bool queueAsync(const Deko3DVariantRequest &request, bool background, bool cached = false);
        bool compile(DkStage stage, const std::string &source, std::vector<uint8_t> &dksh, std::string &log);
        void finishAsync(CompileJob *job);
        // banjo-nx (2026-10-21): Deko3DRefreshPipeline's work; true while a stage still waits.
        bool refreshPipeline(Deko3DGraphicsPipeline *p, bool &changed);
        bool loadCode(const std::vector<uint8_t> &dksh, DkShader &shader, const char *what, uint32_t *vsInputs = nullptr);
        void compileWorker();
        void applyCompilePriority();
    };

    struct Deko3DInterface : RenderInterface {
        RenderInterfaceCapabilities capabilities;
        std::vector<std::string> deviceNames;

        Deko3DInterface();
        std::unique_ptr<RenderDevice> createDevice(const std::string &preferredDeviceName) override;
        const std::vector<std::string> &getDeviceNames() const override { return deviceNames; }
        const RenderInterfaceCapabilities &getCapabilities() const override { return capabilities; }
    };

    std::unique_ptr<RenderInterface> CreateDeko3DInterface();

    // vivapinata-nx (2026-11-12, diagnostic: renut_gpu_profile_counters): a hardware counter (a DkCounter) reported into
    // a query pool's entry 'index' (16 bytes an entry; the report of a counter is its 64-bit value, then the time), and
    // the pool's words as the GPU wrote them.
    void Deko3DReportCounter(RenderCommandList *commandList, const RenderQueryPool *pool, uint32_t index, uint32_t counter);
    const volatile uint64_t *Deko3DQueryPoolWords(const RenderQueryPool *pool);
    // banjo-nx: the blend constant on a deko3d command list (false when the list is not one).
    bool Deko3DSetBlendConstant(RenderCommandList *commandList, const float rgba[4]);
    // banjo-nx: the memexport buffer of the next draws (storage binding 14 of the graphics stages).
    bool Deko3DBindMemExport(RenderCommandList *commandList, const RenderBuffer *buffer);
    // banjo-nx: the vertex fetch data of the next draws (storage binding 15 of the graphics stages): size bytes from
    // address, which the shaders index from 0 (shader_common.h, g_DkAddressWindow).
    bool Deko3DBindFetchData(RenderCommandList *commandList, uint64_t address, uint32_t size);
    // sasrt-nx (2026-11-08, sasrt_gpu_readback_compute): a wait for the 3D engine and its compute, then the L2's dirty
    // lines to memory (DkInvalidateFlags_L2Cache: L2FlushDirty + L2SysmemInvalidate), as a pending barrier: what a compute
    // shader wrote for the CPU is in memory when the list's fence is reached.
    void Deko3DFlushL2(RenderCommandList *commandList);
    // sasrt-nx (2026-11-08, sasrt_gpu_export_raster_discard): the next draw recorded with the rasterizer off (3D method
    // 0x0DF RasterizerEnable 0, then 1 again: plume's pipelines all rasterize): its vertex shader runs and exports, no
    // primitive reaches clipping, setup or the scissor.
    void Deko3DDiscardNextDraw(RenderCommandList *commandList);
    // Port 166 (from fnxf_xiii): the samples the next draws write (dkCmdBufSetSampleMask, 16 bits), recorded only when it
    // changes (Deko3DCommandList::sampleMaskSet; 0xFFFF: every sample).
    void Deko3DSetSampleMask(RenderCommandList *commandList, uint32_t mask);
    // sasrt-nx (2026-11-08, sasrt_gpu_export_list_topology; the fourth GPU round): the next draw recorded with this
    // DkPrimitive instead of its pipeline's (with the rasterizer off: only which vertices the vertex shader runs for).
    void Deko3DSetNextPrimitive(RenderCommandList *commandList, uint32_t primitive);
    // sasrt-nx (2026-11-08, sasrt_gpu_convert_lead_wait): the list's buffer barriers so far (Deko3DCommandList::bufferWaits).
    uint64_t Deko3DBufferWaits(const RenderCommandList *commandList);
    // sasrt-nx (2026-11-08, Deko3DOptions::trackedImageInvalidate): the texture-cache drop epoch (plume_deko3d_commands.cpp).
    extern std::atomic<uint64_t> g_deko3dTexDropEpoch;
    extern std::atomic<uint64_t> g_deko3dWaitEpoch;  // sasrt-nx (2026-11-08, Deko3DOptions::trackedTargetWait)
    // sasrt-nx (2026-11-08, the second GPU round): a wait for the 3D engine (merged with the pending barrier like any
    // other) that drops only the caches its reader reads through: DkInvalidateFlags_Image (the texture cache, which the
    // vertex fetch is given), Deko3DGlobalDataInvalidate (storage-buffer loads). countsAsBufferWait: it drops the global-
    // data cache, so it counts in Deko3DBufferWaits as a buffer barrier does.
    void Deko3DBufferWait(RenderCommandList *commandList, uint32_t invalidate, bool countsAsBufferWait);
    // sasrt-nx (2026-11-08, sasrt_deko3d_diag_trace; the second GPU round): while g_deko3dTrace, the command lists'
    // work as tokens in recording order (runs of one token as "token*n"), for one frame at a time: barriers ("B" +
    // P primitives / F fragments / X full + the causes + "i" if it drops the texture cache), draws "D", dispatches "X",
    // target binds "T<colour>:<depth>" (images numbered in order of appearance), clears "Cc" / "Cd", depth
    // decompressions "Z", copy-engine copies "K", descriptor invalidations "I", list ends "E".
    extern bool g_deko3dTrace;
    void Deko3DTraceToken(const char *token);
    std::string Deko3DTraceTake();
    // banjo-nx (2026-10-20, banjo_deko3d_skin_once): a storage buffer of the next draws at 'slot' (0-13) of the
    // graphics stages that read it (the fur's skin cache: slot 13).
    bool Deko3DBindStorage(RenderCommandList *commandList, uint32_t slot, uint64_t address, uint32_t size);
    // banjo-nx (2026-10-19, render-thread proposals 2 and 5): a uniform buffer the renderer keeps up to date with
    // pipelined updates (Deko3DPushUniform: dkCmdBufPushConstants; the draws before an update keep the old values), bound
    // at 'slot' for the graphics stages that read it; the update of 'size' bytes at 'offset' of the 'uboSize'-byte
    // buffer at 'address'; and the CPU's copy of the shared constants bound textures are read from (bindTextures), for
    // a shared uniform buffer the renderer pushes (nullptr: read where the bound buffer's memory is, as before).
    bool Deko3DBindUniform(RenderCommandList *commandList, uint32_t slot, uint64_t address, uint32_t size);
    // banjo-nx (2026-10-26): whether setVertexBuffers sends only the slots whose range changed (default true).
    void Deko3DSetVertexBufferDedupe(bool on);
    // banjo-nx (2026-10-27, render thread night): whether bindTextures skips a draw whose pipeline, descriptors and
    // texture words are those of its last run (default true); and the 64-byte lines of the CPU copy of the shared
    // constants (Deko3DSetSharedWords) the renderer changed (bit i = line i).
    void Deko3DSetTextureSkip(bool on);
    void Deko3DSharedLinesWritten(RenderCommandList *commandList, uint32_t lines);
    // katamari-nx (2026-10-30): fragment-stage textures bound by descriptor slot (sampler 0), bindings first.., for a
    // host pipeline whose shader declares them bound (tools/deko3d_glsl.py: host shaders' multisampled textures,
    // dk_ms_b<k>); the stage's tracked handles are forgotten, so the next pipeline with bound textures binds its own.
    bool Deko3DBindFragmentTextures(RenderCommandList *commandList, uint32_t first, const uint32_t *slots, uint32_t count);
    bool Deko3DPushUniformSlow(RenderCommandList *commandList, uint64_t address, uint32_t uboSize, uint32_t offset,
                               const void *data, uint32_t size);
    // banjo-nx (2026-10-27, deko3d recording): one piece (every push the renderer makes: 64-byte constant groups and
    // runs, the shared constants' lines) inline, straight to deko3d; longer ones in pieces (Deko3DPushUniformSlow).
    // (The b35 profile: the call's entry 0.4 % of the render thread, at several calls a draw.)
    inline bool Deko3DPushUniform(RenderCommandList *commandList, uint64_t address, uint32_t uboSize, uint32_t offset,
                                  const void *data, uint32_t size) {
        if (size - 1u < 1024u && address && commandList && offset + size <= uboSize) {
            dkCmdBufPushConstants(static_cast<Deko3DCommandList *>(commandList)->cmd, address, uboSize, offset,
                                  (size + 3u) & ~3u, data);
            return true;
        }
        return Deko3DPushUniformSlow(commandList, address, uboSize, offset, data, size);
    }
    bool Deko3DSetSharedWords(RenderCommandList *commandList, const void *cpu, uint32_t size);
    // banjo-nx: the queue lost (dkQueueIsInErrorState).
    bool Deko3DDeviceLost(RenderDevice *device);
    // banjo-nx (2026-10-21, Deko3DOptions::asyncVariants): a pipeline made while some of its variants were compiled in
    // the background draws with the generic ones; this gives it the stages whose own variants are done since (one atomic
    // load when there is nothing to do). True when a stage changed now: bind the pipeline again to use it.
    bool Deko3DRefreshPipelineSlow(RenderPipeline *pipeline);
    // banjo-nx (2026-10-27): whether a shader may read gl_VertexID (Deko3DShaderUsage::readsVertexId); true for null.
    bool Deko3DShaderReadsVertexId(const RenderShader *shader);
    // vivapinata-nx (2026-11-03): whether a vertex shader has the kill's constant 914 (Deko3DShaderUsage::writesVertexKill).
    bool Deko3DShaderWritesVertexKill(const RenderShader *shader);
    // banjo-nx (2026-10-21, asyncVariants): this thread does not draw (Renut's pipeline prebuild workers): the variants it
    // asks for are compiled after the draws' ones, and only so many wait at a time (the rest when a draw needs them).
    void Deko3DSetBackgroundThread(bool background);
    // banjo-nx (2026-11-01): the pipeline this thread builds draws much slower with its generic variants: compile them now, here.
    void Deko3DSetUrgentVariants(bool urgent);
    // banjo-nx (2026-10-28, the console's OOM): called when a memory block's storage cannot be had from the heap, before
    // asking again (any thread; it must not take the application's renderer lock): the application's spares go back.
    void Deko3DSetMemoryPressureCallback(void (*callback)());

    // banjo-nx (2026-10-28, h10): GPU memory blocks from a region of their own at the heap's top (plume_deko3d.cpp,
    // "The GPU region"); on by default, set before the device is made. Its figures for the memory report.
    void Deko3DSetGpuRegion(bool on);
    struct Deko3DGpuRegionInfo {
        bool on = false;
        uint64_t size = 0, used = 0, peak = 0, freeBytes = 0, largestFree = 0, givenBack = 0, refused = 0;
        uint32_t freeRanges = 0, blocks = 0, growths = 0, givebacks = 0;
    };
    void Deko3DGpuRegionFigures(Deko3DGpuRegionInfo &out);
    // sasrt-nx (2026-11-09; port 58, for a title's memory governor): the heap's free bytes as the memory-block log lines
    // count them (the free chunks inside what malloc has taken and what sbrk can still give; mallinfo walks every free
    // piece under malloc's lock: 7-21 ms in SASRT's docked race, a cold-path figure).
    uint64_t Deko3DHeapFreeBytes();
    // sasrt-nx (2026-11-09 night; port 58): what sbrk can still give (newlib's room below the GPU region), O(1): the
    // figure for a look every few frames.
    uint64_t Deko3DHeapRoomBytes();
    // sasrt-nx (2026-11-09 night; port 58): a texture or a memory block plume could not make sets this; the renderer
    // takes it (true once) at its next frame and frees what it can then, without waiting for its next look.
    bool Deko3DTakeMemoryEmergency();
    // sasrt-nx (2026-11-09 night; port 58): the GPU region places a block in the smallest free range it fits (its bottom
    // range last), not the highest: SASRT's region held 200-300 MB free in 40-54 ranges, the largest 28-55 MB, and a
    // 27 MB target found no room. On by default.
    void Deko3DSetRegionBestFit(bool on);
    // sasrt-nx (2026-11-09 night; port 58): an image goes to the smallest free range it fits among the image heap's
    // shared chunks (the first that fit before, chunk by chunk): fewer holes, and chunks that empty go back to the region.
    void Deko3DSetImageBestFit(bool on);
    // blur-nx (2026-11-18, for 1 % lows, every port): the image heap keeps one empty shared chunk ready, made on a thread
    // of its own (the uam thread's core and priority) when an image takes the last empty one; the texture that would
    // have made a chunk (a 32 MB block, 20-38 ms on the console, the heap's lock held) finds room. Not while the heap's
    // room is short; the governor's Deko3DReleaseEmptyImageChunks gives the spare back too. On by default.
    void Deko3DSetImageSpareChunk(bool on);
    // sasrt-nx (2026-11-09; port 58): render and depth targets of 1 MB and more in a chunk of their own (on by default),
    // so a target the renderer retires (a resolution change's set, an idle scale's) gives its memory back to the GPU
    // region at once, for buffers too, instead of a hole in a shared image chunk that only images can use; and the
    // image heap's empty chunks given back on request (a governor).
    void Deko3DSetDedicatedTargets(bool on);
    uint64_t Deko3DReleaseEmptyImageChunks();
    // sasrt-nx (2026-11-11, the stutter hunt: a dynamic-resolution step made ~33 dedicated blocks in one frame, ~2.5 ms
    // each, and destroyed the old ones two frames later): an emptied dedicated target's block stays, up to this many
    // MB in all, for the next target of its size or up to an eighth smaller; given back with the empty image chunks
    // (Deko3DReleaseEmptyImageChunks: the governor's low mark, a block that could not be made). 0: off (destroyed at
    // once, as before).
    void Deko3DSetTargetBlockCacheMb(uint32_t mb);
    struct Deko3DTargetCacheInfo {
        uint64_t bytes = 0, hits = 0, misses = 0, kept = 0, evicted = 0, released = 0;
        uint32_t blocks = 0;
    };
    void Deko3DTargetCacheFigures(Deko3DTargetCacheInfo &out);
    // sasrt-nx (2026-11-09; port 58, SASRT's docked crash: the command buffers' arena found no room in the heap while the
    // GPU region held 500-700 MB free): the region's free bottom back to newlib now (it goes back by itself only once
    // kRegionGiveBackAt there is free); the bytes given. The region is carved from the heap's top and blocks pack at its
    // top, so its bottom is where freed GPU memory can become the CPU side's again.
    uint64_t Deko3DRegionGiveBack();
    // banjo-nx (2026-10-28, render-thread round): how many graphics pipelines are provisional now (made at creation,
    // dropped when refreshPipeline finds every variant or the pipeline goes), and whether a draw may take a zero count
    // as its answer (Deko3DSetProvisionalCount; Banjo's A/B 68). Every draw asked its pipeline (a line of the pipeline
    // object: night_gpu5's profile had the check at 0.9 % of the render thread) although nearly every frame has none.
    extern std::atomic<int> g_deko3dProvisionalPipelines;
    extern bool g_deko3dProvisionalCount;
    void Deko3DSetProvisionalCount(bool on);
    inline bool Deko3DRefreshPipeline(RenderPipeline *pipeline) {
        if (g_deko3dProvisionalCount && g_deko3dProvisionalPipelines.load(std::memory_order_relaxed) == 0)
            return false;  // none is provisional: the answer the pipeline's flag would give
        auto *p = static_cast<Deko3DPipeline *>(pipeline);
        if (p == nullptr || p->type != Deko3DPipeline::Type::Graphics ||
            !static_cast<Deko3DGraphicsPipeline *>(p)->provisional.load(std::memory_order_relaxed))
            return false;
        return Deko3DRefreshPipelineSlow(pipeline);
    }
    // banjo-nx (2026-10-27, deko3d recording): the lines a graphics pipeline's bind and draw read (the base, the
    // draw-time block, the texture bindings' first entries), fetched once the caller knows it will bind it.
    inline void Deko3DPrefetchPipeline(const RenderPipeline *pipeline) {
        const char *p = reinterpret_cast<const char *>(pipeline);
        if (!p)
            return;
        for (uint32_t line = 0; line < 4; line++)
            __builtin_prefetch(p + line * 64);
    }
    // banjo-nx (2026-10-27, deko3d recording): its stages' shader objects (128 bytes each, in their variants' heap
    // objects), which dkCmdBufBindShaders reads (the b35 profile: its hottest line, 0.7 % of the render thread). Call
    // once the pipeline's own lines are in (some work after Deko3DPrefetchPipeline).
    inline void Deko3DPrefetchPipelineShaders(const RenderPipeline *pipeline) {
        const auto *p = static_cast<const Deko3DPipeline *>(pipeline);
        if (!p || p->type != Deko3DPipeline::Type::Graphics)
            return;
        for (const DkShader *s : static_cast<const Deko3DGraphicsPipeline *>(p)->stageShaders) {
            if (s) {
                __builtin_prefetch(s);
                __builtin_prefetch(reinterpret_cast<const char *>(s) + 64);
            }
        }
    }
    // banjo-nx (2026-10-21, from sa2-nx 1.0.3.1): the size the swapchain should have, the console display's (1920x1080
    // docked, 1280x720 handheld); 0 x 0 = the window's, as before. A swapchain of another size asks for a resize
    // (needsResize), which gives the window that size while no buffer is registered (nwindowSetDimensions) and makes
    // the images again. Any thread.
    void Deko3DSetSwapChainSize(uint32_t width, uint32_t height);
    // banjo-nx (2026-10-19, latency batch 3): a copy of what a command fence's last submit signals, taken on the thread
    // that submits (the fence itself is re-armed by the next submit of its slot), and whether the GPU has signalled it
    // (dkFenceWait with no wait): any thread may test its own copy.
    struct Deko3DFenceCopy {
        DkFence fence{};
        bool valid = false;  // a submit's signal to test
        bool done = false;   // already waited for (nothing pending)
    };
    Deko3DFenceCopy Deko3DCopyFence(const RenderCommandFence *fence);
    bool Deko3DFenceCopyDone(Deko3DFenceCopy &copy);
    // banjo-nx (2026-10-20, input latency): waits until the copy's signal arrives (or the queue fails); true when it did.
    bool Deko3DFenceCopyWait(Deko3DFenceCopy &copy, RenderDevice *device);

    // banjo-nx (2026-10-18, GPU bandwidth): options read when the device and its textures are made; set them before
    // CreateDeko3DInterface. depthCompression: depth/stencil targets get hardware compression (as colour targets do).
    // zcull: the queue keeps its Zcull context (hierarchical depth culling; SwitchWakerHD saw depth-biased decals dropped in
    // 4x8 tiles with it).
    struct Deko3DOptions {
        bool depthCompression = false;
        bool zcull = false;
        // sasrt-nx (2026-11-11, port 82; its GPU list's row 2; UNTESTED), sasrt_gpu_precise_stencil_write: a pipeline's
        // "may write the depth target" (writesDepthStencil, the mark a read decompresses after) leaves out the stencil
        // outcomes that cannot happen (fail when it compares always, depth-fail when the depth test cannot fail) and a
        // stencil write mask of 0. Read as each pipeline is built.
        bool preciseStencilWrite = false;
        // A pipeline's vertex shader gets specialization constant 905 (when it declares it), the locations its pixel
        // shader reads of the ones it writes (none without a pixel shader; not with a geometry shader): banjo-nx's
        // tools/deko3d_glsl_opt.py, pass vsout, drops the other outputs and what computes them.
        bool vsOutputs = false;
        // uam's banjo-nx codegen passes (uam_api.h setBanjoFlags): 1 PRMT byte moves, 2 storage buffer offsets in the
        // load, 4 shift distribution, 8 + 16 the input interpolation cache where it pays.
        unsigned uamFlags = 447;
        // The uam thread's core (-1: any) and priority (Horizon: a higher number is a lower priority; 0: unchanged).
        // A thread that waits for a compile lends its priority to the uam thread while it waits.
        int compileCore = -1;
        int compilePriority = 0;
        // banjo-nx (2026-10-23): the share of its core (percent) background compiles may use: after each one the uam
        // thread rests in proportion (a compile a thread waits for ends the rest). An application's lowest priority
        // is the game threads' own (0x3B), so a lower one for the uam thread is refused and it shared the core half
        // and half with them. 100: no rests.
        int backgroundCompileShare = 100;
        // banjo-nx (2026-10-23, the user's request): boost mode (CPU 1785 MHz, GPU 76.8 MHz) while a drawing thread
        // waits for a compile, normal again as soon as it is done. Background compiles never boost.
        bool compileWaitBoost = false;
        // mk9-nx (2026-10-24; port 56): a compile a drawing thread waits for (a generic variant missing from the DKSH
        // cache) is a single back-end pass (uam::compileFast: no CompileBest alternatives, about a quarter of a pixel
        // shader's full compile), stored in the cache as it is. Background compiles stay full. mk9-nx: the fight intros'
        // compile waits ~2.5x shorter, GPU 10.00 against 10.05 ms a frame (no difference to see).
        bool fastFirstCompile = false;
        // banjo-nx (2026-10-19, GPU): barriers recorded one after another with no work between them are one barrier
        // (the strongest wait and every invalidation of them), emitted right before the next command that does GPU
        // work: the same ordering, one drain instead of several.
        bool mergeBarriers = false;
        // banjo-nx (2026-10-19, GPU): a transition into a layout that is only written next (colour, depth, present)
        // waits for the work before it but keeps the texture cache: only a read after a write needs it invalidated,
        // and that read's own transition does it.
        bool leanBarriers = false;
        // Port 161 (from fnxf_xiii): no barrier between back-to-back buffer copies after a Full one (beforeBufferCopy).
        bool copyRunBarriers = false;
        // Port 165 (from fnxf_xiii, its notes of 2026-10-25 and 10-29): the display refreshes each swapchain image is
        // held for with vsync on (1-4; 2 paces a 30 fps game evenly from the display itself).
        uint32_t swapInterval = 1;
        // sasrt-nx (2026-10-30): a multisampled pipeline gets the standard sample positions for its sample count
        // (dkMultisampleStateSetLocations with none given, as nouveau's gm200_validate_sample_locations). deko3d's
        // dkMultisampleStateDefaults puts every sample at the pixel's centre and dkCmdBufBindMultisampleState always
        // writes them (method 0x11E0, libdeko3d disassembled): without this, 2x/4x targets had no edge antialiasing at
        // all on the console. Ryujinx keeps the standard positions either way.
        bool msaaPositions = true;
        // sasrt-nx (2026-10-30): the hardware's alpha-to-coverage dither, which dkMultisampleStateDefaults turns on
        // and nothing turned off: a fixed screen-space grid where a texture's alpha gives the coverage (seen in
        // Final Fantasy XIII's ice on deko3d). The other back ends (Vulkan, D3D12) give no dither control and Xenos
        // dithers with its own offsets (RB_COLORCONTROL ALPHA_TO_MASK_OFFSET), not this pattern.
        bool alphaToCoverageDither = false;
        // sasrt-nx (2026-10-30): an indexed strip (triangle or line) restarts at the all-ones index of its index size,
        // as the Vulkan back end (primitiveRestartEnable for strips) and D3D's strip cut. deko3d never turns
        // primitive restart on (only dkCmdBufSetPrimitiveRestart writes method 0x591, libdeko3d disassembled) and
        // plume did not either, so a cut drew triangles to and from vertex 65535 on deko3d alone.
        bool stripRestart = true;
        // banjo-nx (2026-10-21, after a console freeze in a run-time compile that never ended): a variant missing from
        // the cache is compiled on the uam thread in the background; until it is done the pipeline draws with the
        // shader's generic variant (without the specializations that only fold what the shader reads at run time:
        // the bools, blend skip, output trim, alpha function, tessellation kind, vertex outputs, zero inputs, early
        // depth), and takes its own at a later bind (Deko3DRefreshPipeline). Off: the draw waits for the compile.
        bool asyncVariants = false;
        // banjo-nx (2026-10-23, the console's stutters on entering an area: hundreds of new pipelines, each variant read
        // from the cache file on the card by the drawing thread): with asyncVariants, a variant the cache file holds is
        // read on the uam thread (ahead of the compiles, with no rest after it) while the draw uses the generic
        // variant, as for one that compiles. Same pixels.
        bool asyncCacheReads = false;
        // rr6-nx (2026-11-05, ported into renut-ultimate): Maxwell's SET_USER_CLIP_ENABLE (method 0x544, register 0x1510),
        // written at the start of every command list. deko3d never writes it (libdeko3d disassembled; no API), so on the
        // console every clip slot acts as enabled and a vertex shader that writes gl_ClipDistance[0] alone has slots 1-7
        // clipping on whatever its unwritten outputs hold. Bit i: slot i. -1 leaves the register alone.
        int userClipEnable = 1;
        // sasrt-nx (2026-10-21, ported into renut-ultimate): with depthCompression, a depth target is decompressed
        // (dkCmdBufResolveDepthValues, Maxwell's DECOMPRESS_ZETA_SURFACE, as NVN's nvnCommandBufferResolveDepthBuffer)
        // before it is read: when it moves to a read layout while bound, when another depth target (or none) is bound,
        // and at the end of a list. The texture unit cannot read the GPU's compressed depth tiles. Ryujinx emulates no
        // compression, so only the console shows it.
        bool depthResolve = false;
        // renut-ultimate (2026-11-08): with depthResolve, a target leaving the bind point (or bound at the end of a list)
        // is decompressed only when something reads it: when it next moves to a read layout, or before it is bound
        // through another view. Ridge Racer 6, which reads no depth, paid 0.46 ms a frame on the console for the eager
        // form. Every read of a depth target goes through a barrier (Renut's other back ends need the layout).
        bool depthResolveLazy = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_precise_depth_resolve): with depthResolve, a depth target is decompressed when
        // it leaves the bind point or is read only if its image was written since its last decompression
        // (Deko3DTexture::depthUnresolved), not every time it was bound. Draws that only test depth (the lighting
        // passes, which take the scene depth on and off dozens of times a frame) leave its tiles as they were: a
        // decompression of tiles already plain changes nothing. Read at each use (the in-run A/B may change it between
        // lists); the flag is kept either way.
        bool preciseDepthResolve = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_decompress_merge): resolveBoundDepth(true) with a barrier pending (not a full
        // one, mergeBarriers) records the decompression first and merges its following barrier into the pending one.
        bool decompressMerge = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_readback_barrier_merge): copyTextureRegion's image-to-buffer readback records
        // its full barriers before and after the copy as pending ones (mergeBarriers): one with the pending barrier
        // before it, one with the next barrier after it (emitted before the next work or at the list's end).
        bool readbackMerge = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_data_cache_invalidate): a buffer barrier invalidates the shaders' global-data
        // and constant caches only (Maxwell 3D method 0x369 InvalidateShaderCaches, bits 4 and 12), not their
        // instruction cache (DkInvalidateFlags_Shader sets bit 0 as well): Deko3DDataInvalidate in the pending flags.
        bool dataOnlyInvalidate = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_tiled_cache; off by default, for the console to judge): Maxwell's tiled caching
        // (binning, the hardware's tile-based rendering, which NVN uses and deko3d leaves off: its queue sets 128x128 tiles
        // up and never enables them) on from the start of every command list. deko3d's barriers order tiles and
        // fragments for it (DkBarrier_Tiles / _Fragments), its fences flush it; a full barrier (the copy engine next)
        // gets a tiled cache flush ahead of it here. Off: a list starts with it off when it was ever on (the in-run A/B).
        bool tiledCache = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_lazy_depth_resolve; off by default, for the console to judge): with depthResolve
        // and preciseDepthResolve, a written compressed depth target (one layer, one level) is not decompressed when it
        // leaves the bind point or at a list's end, but right before something reads it (a texture barrier into a read
        // or copy layout): bound alone for the decompression, then the targets bound again. A target written, unbound and
        // bound to be written again before anything reads it is decompressed once instead of at every unbind.
        bool lazyDepthResolve = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_same_targets_kept; the second GPU round): setFramebuffer with the framebuffer
        // object whose targets this list bound last (a framebuffer's views never change, and one in use is alive) does
        // not bind them again: the renderer asks for its targets again after every compute dispatch and resolve.
        bool sameTargetsKept = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_seed_no_lead_wait / sasrt_gpu_convert_lead_wait): the constant cache dropped at
        // the start of every command list. A compute dispatch's push constants are fresh transient memory (an address
        // no earlier read of this recording used), but the list's last recording read the same addresses; deko3d drops
        // no constant cache for them, and the buffer barrier that used to come right before every seed and conversion
        // (which dropped it) is gone with those items. One inline method a list.
        bool freshComputeConstants = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_lean_barrier_skip; the second GPU round): a barrier that only moves textures into
        // a write layout (colour or depth target: deko3d's fragment barrier, no cache dropped: leanBarriers) is not
        // recorded when no draw, dispatch, copy or depth decompression came since the last barrier that waited for the
        // fragments (or more): the reads it would order before the writes have finished already. Clears read no
        // texture and write in order with the draws.
        bool leanBarrierSkip = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_vs_input_mask; the third GPU round): a pipeline's attributes that the bound vertex
        // variant does not read (Deko3DShaderVariant::vsInputs) bound as constants: the vertex attribute fetch reads no
        // memory for them.
        bool vsInputMask = false;
        // sasrt-nx (2026-11-09, sasrt_gpu_exp_sph_no_global_store): an EXPERIMENT: vertex programs loaded with their shader
        // program header's DoesGlobalStore bit (word 0 bit 16) cleared; bit 0 of the value: that bit, bit 1: DoesLoadOrStore
        // (bit 26) too.
        uint32_t expSphStoreBits = 0;
        // sasrt-nx (2026-11-08, sasrt_gpu_texture_drop_tracked; the third GPU round): a texture moved into a read layout
        // drops the texture cache only when it was bound for sampling since the last drop that waited for the GPU
        // (Deko3DTexture::sampledEpoch): the cache holds no line of an image no draw sampled since.
        bool trackedImageInvalidate = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_target_wait_tracked): a barrier that only moves textures into target layouts is
        // not recorded when none of them was used (sampled, copied, resolved, decompressed) since the last fragment wait
        // (Deko3DTexture::usedWaitEpoch).
        bool trackedTargetWait = false;
        // sasrt-nx (2026-11-08, sasrt_gpu_read_wait_tracked): a move into a read layout waits only for a texture written
        // since the last fragment wait (Deko3DTexture::writtenWaitEpoch) or one whose cache drop is due.
        bool trackedReadWait = false;
        // sasrt-nx (2026-11-08, sasrt_deko3d_diag_barriers): count the barriers emitted by their causes
        // (Deko3DStats::barrierWhy, Deko3DBarrierWhy). A diagnostic: one array add per barrier.
        bool diagBarriers = false;
        // renut-ultimate (2026-11-10, port 59, from LostOdysseyRecomp ff013164; lo_deko3d_descriptor_cache): a sampled image
        // descriptor is made once per view (or per texture for its default view) and copied at later writes, instead of
        // dkImageDescriptorInitialize at every write.
        bool descriptorCache = true;
        // renut-ultimate (2026-11-10, lo_deko3d_relaxed_descriptor_counters): the descriptor write count and the device's
        // descriptor epoch counted with relaxed increments. The sequentially consistent and release increments made each
        // write wait for its store into the uncached descriptor block to drain (most of setTexture's cost in
        // LostOdysseyRecomp's profile). Nothing on the CPU reads a descriptor after the epoch: the epoch only decides an
        // invalidate in the list being recorded, and the GPU sees the stores after the submission's system call.
        bool relaxedDescriptorCounters = true;
    };
    // sasrt-nx (2026-11-08, Deko3DOptions::dataOnlyInvalidate): plume's own invalidate bit, above deko3d's flags.
    constexpr uint32_t Deko3DDataInvalidate = 1u << 30;
    // sasrt-nx (2026-11-08, the second GPU round): the shaders' global-data cache alone (InvalidateShaderCaches bit 4),
    // for a wait whose reader loads storage buffers (Deko3DBufferWait).
    constexpr uint32_t Deko3DGlobalDataInvalidate = 1u << 29;
    // sasrt-nx (2026-11-08): why a barrier was recorded (Deko3DStats::barrierWhy's bits).
    enum Deko3DBarrierWhy : uint32_t {
        Deko3DWhyRead = 1,        // a texture into SHADER_READ / DEPTH_READ / GENERAL
        Deko3DWhyCopy = 2,        // a texture into a copy or resolve layout, or the COPY stage
        Deko3DWhyColor = 4,       // a texture into COLOR_WRITE
        Deko3DWhyDepth = 8,       // a texture into DEPTH_WRITE
        Deko3DWhyOther = 16,      // PRESENT or another layout
        Deko3DWhyBuffer = 32,     // a buffer barrier
        Deko3DWhyCompute = 64,    // the COMPUTE stage
        Deko3DWhyMerged = 128,    // more than one barriers() call merged into it
    };
    void Deko3DSetOptions(const Deko3DOptions &options);
    // banjo-nx (2026-10-19, render-thread proposal 13): read inline (barriers() reads it per call).
    extern Deko3DOptions g_deko3dOptions;
    // sasrt-nx (2026-11-11, port 82; its GPU list's row 2; UNTESTED): the pipelines whose depth mark
    // preciseStencilWrite dropped (the old rule marked them), since the start.
    extern std::atomic<uint64_t> g_deko3dPreciseStencilSaved;
    inline const Deko3DOptions &Deko3DGetOptions() { return g_deko3dOptions; }
};
