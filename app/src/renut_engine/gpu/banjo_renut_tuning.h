// banjo-nx (2026-10-17): banjo-nx's tuning of Renut's renderer for the Switch's A57 and NVK, called from a few places
// in Renut's files (tools/renut_patches/banjo.patch). Each part has its own key in the enhancement table
// (src/cpu/banjo_enhancements.cpp); docs/renut-renderer-port.md describes them.
//
//   - Thread placement: Renut's render thread and its pipeline prebuild workers are plain std::threads, which libnx
//     starts on the process's default core. The render thread goes to banjo_ring_core (the core banjo-nx's own ring
//     thread, the same job, is pinned to), the workers to banjo_background_compile_core at a lower priority, and
//     there are banjo_prewarm_threads of them. No effect on the PC.
//   - banjo_renut_pipeline_cache: the driver's compiled pipelines persist in cache/renut_pipelines.bin (plume's
//     persistent VkPipelineCache, from UnleashedRecomp-NX), so a start builds the listed pipelines from the file
//     instead of compiling them (banjo_mesa_disk_cache_off leaves Mesa's own disk cache off).
//   - banjo_renut_precise_barriers: plume's barrier access masks derived from the layouts and declared accesses
//     instead of MEMORY_READ | MEMORY_WRITE on both sides, which NVK turns into cache flushes and invalidations
//     (UnleashedRecomp-NX's SwitchPreciseBarriers).
//   - banjo_renut_pmull_hash (ARM only): the content hashes Renut takes of guest data every frame (vertex streams,
//     index ranges, texture samples and the CPU-written textures' contents) use banjo-nx's PMULL-folded 64-bit CRC
//     (me/masseffect/banjo_pmull_fingerprint.h, banjo_ring_pmull_fingerprint) instead of XXH3: half the instructions
//     per byte on the A57. The shader containers keep XXH3 (the shader cache is keyed by it).
#pragma once

#include <atomic>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <xxhash.h>

namespace plume {
struct RenderDevice;
struct RenderPipeline;
}

// banjo-nx (2026-10-19): whether the generated code marks the store cards (app/src/cpu/banjo_hot_guest.cpp).
extern "C" const bool g_banjo_store_cards;

namespace banjo::renut_gpu {

// banjo-nx (2026-10-19, CPU proposal 5): the switches the render thread reads per draw, read inline (they were
// out-of-line functions with a static guard each).
struct Latched {
  // banjo-nx (2026-11-20, render-thread batch #3): set last by LatchTuning; Latch() tests it here, on the line it reads
  // anyway, not the separate g_latched_ready (a load the compiler could not merge at each of the switches a draw reads).
  bool ready = false;
  // banjo-nx (2026-11-20, render-thread batch #4): the 10 s report's per-draw counts (logs on): off with log_level
  // "off", the release's default, where nothing reads them.
  bool counts = true;
  bool bool_spec = false;
  bool tess_grid = false;
  bool depth_only = false;
  bool output_trim = false;
  bool depth_words_view = false;
  bool tess_spec = false;
  bool fetch_spec = false;
  bool alpha_spec = false;
  bool draw_resolves = false;
  bool resolve_skip = false;
  bool depth_words_lazy = false;
  bool depth_handover = false;
  bool depth24 = false;
  bool push_constants = false;
  bool kick_submit = false;
  bool late_input = false;
  bool ring_cleaner = false;
  bool push_constants_verify = false;
  bool diag_gpu_shaders = false;
  bool diag_trim_poison = false;
  bool half_pixel_offset = false;
  bool point_minmax_radius = false;
  bool depth_words_stencil = false;
  bool alpha_to_mask_test = false;
  bool blend_skip = false;
  bool exact_streams = false;
  bool keep_gpu_written = false;
  bool poison_slots = false;
  bool shared_dedupe = false;
  bool queue_notify_idle = false;
  bool neon_swap = false;
  bool capture_pool = false;
  bool push_point_only = false;
  bool pipeline_memo = false;
  bool cpu_memos = false;
  bool store_cards = false;
  bool store_cards_verify = false;
  // bangaio-nx (2026-10-29, the v4 console profile: a load-acquire of a function static's guard for each switch
  // CaptureDelta read, every draw on the game thread): the delta chain's switches (backend/queued.cpp).
  bool deltas_narrow = false;
  bool deltas_verify = false;
  bool neon_compare = false;
  bool capture_batches = false;
  bool small_captures = false;
  bool delta_masks = false;
  bool delta_prefetch = false;  // bangaio-nx (2026-10-29, v6)
};
extern Latched g_latched;
extern std::atomic<bool> g_latched_ready;
extern std::atomic<int> g_ab_off;
// banjo-nx (2026-11-20, render-thread batch #3): g_ab_off as a plain int, written with it at each A/B switch (AbApply, on
// the render thread): the draw path's checks read this one, which the compiler can keep in a register between them.
extern int g_ab_off_plain;
[[gnu::cold, gnu::noinline]] void LatchTuning();
// banjo-nx (2026-10-19, render-thread proposal 11): always inline (it was emitted out of line and called 7 times a draw).
// banjo-nx (2026-10-27, render thread night): a plain load of the flag, not a load-acquire: an LDAR at each of the
// switches a draw reads kept the A57 from starting the draw's later loads early (0.3-0.4 % of the render thread at
// each site in the setters2 console profile). The values come from the configuration only, the render thread latches
// them as it starts (OnRenderThreadStart), before any draw; a thread that finds the flag unset latches them itself.
[[gnu::always_inline]] inline const Latched &Latch() {
  if (!g_latched.ready) [[unlikely]]
    LatchTuning();
  return g_latched;
}

// A relative path from a key (renut_pipeline_list) next to the executable rather than in the working directory, which
// the Switch's loader does not promise to be the NRO's folder (banjo-nx's own caches live there too).
std::filesystem::path NextToExecutable(const std::string &path);

// Starts 'fn' on a thread that is never joined: a std::thread elsewhere, a pthread with 'stack_bytes' of stack on the
// Switch (std::thread::detach throws there, and std::thread cannot choose its stack).
void StartLongLivedThread(void (*fn)(), size_t stack_bytes);

// On the render thread, as it starts (backend/render_thread.cpp).
void OnRenderThreadStart();
// On an untile helper of render/textures.cpp, as it starts (port 171: core 0 or 2, the render thread's priority).
void OnCopyHelperStart(uint32_t index);
// On a pipeline prebuild worker, as it starts (render/pipeline.cpp).
void OnPrebuildWorkerStart();
// On any other background worker of the renderer, as it starts (the content verifier, render/draw.cpp): the
// background core and priority, as the prebuild workers.
void OnBackgroundThreadStart(const char *what);
// blur-nx (2026-11-18, the Renut host batch's #11): OnBackgroundThreadStart's priority.
int BackgroundThreadPriority();
// How many prebuild workers to start; Renut's own count off the Switch.
uint32_t PrebuildWorkers(uint32_t renut_count);
// banjo-nx (2026-10-28, the heap): banjo_renut_prebuild_release_s (0: prebuilt pipelines stay).
uint32_t PrebuildReleaseSeconds();
// banjo_renut_upload_cached (the Switch only): the upload ring in CPU-cached memory (backend/video.cpp).
bool UploadCached();
// banjo_renut_push_point_only: the draw's push constant only for point sprites (render/draw.cpp).
[[gnu::always_inline]] inline bool PushPointOnly() { return Latch().push_point_only && g_ab_off_plain != 4; }
// banjo_renut_pipeline_memo (2026-10-19, CPU proposal 6): the GetPipelineLocked memo.
[[gnu::always_inline]] inline bool PipelineMemoOn() { return Latch().pipeline_memo; }
// banjo_renut_cpu_memos (2026-10-19, CPU proposals 2, 4, 8, 11): the render thread's per-draw lookups memoised.
[[gnu::always_inline]] inline bool CpuMemosOn() { return Latch().cpu_memos; }
// banjo_renut_store_cards (2026-10-19, CPU proposal 1): cached vertex and index copies proven by the store cards
// (tools/store_cards.py) instead of a hash at every kick; on only when the generated code marks them.
[[gnu::always_inline]] inline bool StoreCardsOn() { return Latch().store_cards; }
// banjo_renut_store_cards_verify (PC test): every copy the cards prove is hashed as well; a difference names a writer of
// guest memory that marks no card.
[[gnu::always_inline]] inline bool StoreCardsVerifyOn() { return Latch().store_cards_verify; }
void CountCards(uint64_t bytes, bool clean);
void CountCardsMissed(uint64_t bytes);
// banjo-nx (2026-10-23, batch 6, banjo_renut_kick_reuse): an earlier kick's fetch copy (kind 0) or converted stream (1)
// of this frame drawn again instead of a new one; kind 2: verify mode found its bytes stale (not used).
void CountKickReuse(int kind, uint64_t bytes);
// banjo-nx (2026-10-19, render-thread proposal 1): texture checks answered by the cards (bytes not hashed), hashed,
// dynamic re-uploads not made, and (verify mode) textures changed under clean cards.
void CountTextureCards(uint64_t bytes, int what);
// banjo_renut_capture_pool: draw captures from a pool (backend/queued.cpp).
[[gnu::always_inline]] inline bool CapturePoolOn() { return Latch().capture_pool && g_ab_off_plain != 5; }
// banjo_renut_neon_swap (ARM): the vertex conversion's byte swaps 16 bytes at a time (render/vertex_format.cpp).
[[gnu::always_inline]] inline bool NeonSwapOn() { return Latch().neon_swap; }
// banjo_renut_queue_notify_idle: a queued item wakes the render thread only while it waits (backend/render_thread.cpp).
[[gnu::always_inline]] inline bool QueueNotifyIdleOn() { return Latch().queue_notify_idle; }
// banjo_renut_shared_dedupe: a draw whose SharedConstants equal the last upload's reuses it (render/draw.cpp), counted
// for the [renut] report.
[[gnu::always_inline]] inline bool SharedDedupeOn() { return Latch().shared_dedupe && g_ab_off_plain != 3; }
// sasrt-nx (2026-11-09, the render-thread round, RT4; port 54): inline (a load and a store, as Bump in
// banjo_renut_tuning.cpp), not a call into another file at every draw (~0.7 % of Sonic Transformed's render thread without
// LTO). The same counts.
extern std::atomic<uint64_t> g_shared_reused, g_shared_uploaded;
// sasrt-nx (2026-11-09; port 54): the draws so far (CountSharedConstants: every draw, the render thread only), for the
// A/B halves' draws a frame and render-thread microseconds a draw.
extern uint64_t g_ab_draws;
[[gnu::always_inline]] inline void CountSharedConstants(bool reused) {
  // The A/B halves' draw count (a plain add) is kept with the counts off too (the base, port 244): a measured run's
  // in-run A/B keeps its draws a frame and microseconds a draw.
  ++g_ab_draws;
  if (!Latch().counts)  // banjo-nx (2026-11-20, batch #4)
    return;
  std::atomic<uint64_t> &c = reused ? g_shared_reused : g_shared_uploaded;
  c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// banjo_renut_exact_streams (package 27): Renut keeps converted copies of vertex streams and index ranges whose guest
// bytes held for a few flushes, and checks them on a background thread once every 4 frames, so a range the game
// rewrites can draw from its old copy for about 5 frames. With the key a copy is hashed whole on its first use after
// each kick (the point where the game may learn the GPU is done and rewrite the buffer; render/draw.cpp), as banjo-nx's
// renderer proves its persistent vertex blocks: a changed range is converted again before the draw. Not an
// enhancement but the exact rule: no master switch. Counted for the [renut] report: checks, bytes hashed, stale copies
// caught (and, with the key off, the ones Renut's background check found after the fact).
[[gnu::always_inline]] inline bool ExactStreamsOn() { return Latch().exact_streams && g_ab_off_plain != 6; }
void CountExactCheck(uint64_t bytes, bool stale, bool repeat_in_frame);
void CountKick();        // a KickOff reached the render thread (FlushVertexFillsLocked)
void CountDrawKind(bool depth_only, bool depth_only_plain, bool writes_nothing, bool partial_mask);  // package 28
// blur-nx (2026-11-13, #47 of Blur's docs/cpu-near-exact-50-1113.md): a draw's shaders that index their float
// constants (their masks hold every register: all of them uploaded), and the registers the others upload.
void CountConstantUse(bool vs_indexed, bool ps_indexed, uint32_t registers);
void CountExactMemoHit(uint64_t bytes);  // a check answered by the interval memo (no bytes read)
void CountFenceQuery();  // the game asked whether a fence is done (BlockOnFence / IsFencePending)
void CountLateStale(uint64_t copies);
// banjo_renut_stream_cache_pressure (2026-10-23): copies refused at the stream cache's cap, and the bytes the pressure
// pass dropped (render/draw.cpp, every 60 frames).
bool StreamCachePressureOn();
void CountStreamCachePressure(uint64_t refused, uint64_t dropped_bytes);

// banjo_renut_memory_policy (package 27; on by default on the Switch): Renut's memory limits are sized for a PC's
// gigabytes; NVK on the console gives about 1,280 MB of GPU memory budget. Not an enhancement (no master switch): the
// same picture, made to fit. Its parameters, each with Renut's own number when the policy is off:
//   - cold host textures (not bound for 120 frames) are released, and made again from guest memory if bound later,
//     when less than banjo_renut_textures_reserve_mb of the budget is left or the bindless table is three quarters
//     full (render/textures.cpp); 0 when off. Never a texture bound since (the lists in flight are 2 frames), never
//     one the GPU wrote (banjo_renut_keep_gpu_written); sa2-nx's rule, after its console glitches came from too little
//     eviction (a full descriptor table) and stale resolve memory, not from evicting;
//   - the converted-stream cache stops taking copies at banjo_renut_stream_cache_mb (render/draw.cpp), and a copy no
//     draw read for banjo_renut_stream_cache_idle_s goes;
//   - upload ring chunks a heavy frame added (a loading frame's texture uploads) are released after
//     banjo_renut_upload_trim_frames frames without such a frame (render/upload_ring.cpp).
uint64_t TextureReserveBytes();
uint64_t StreamCacheMaxBytes(uint64_t renut_max);
// sasrt-nx (2026-11-09, sasrt_memory_governor; port 58): the heap's free MB (surfaces.cpp's governor, every 10 frames on
// the Switch; UINT32_MAX before the first look and on other platforms), and whether it is under sasrt_memory_low_mb /
// sasrt_memory_critical_mb.
extern std::atomic<uint32_t> g_heap_free_mb;
bool MemoryLow();
bool MemoryCritical();
// sasrt-nx (2026-11-09 night, sasrt_memory_emergency; port 58): the governor's looks left in an emergency (a texture or
// a block plume could not make); MemoryLow and MemoryCritical answer true meanwhile. And the low mark in MB.
extern std::atomic<uint32_t> g_memory_emergency_looks;
uint32_t MemoryLowMarkMb();
uint64_t StreamCacheIdleFrames(uint64_t renut_frames);
uint32_t UploadTrimResets(uint32_t renut_resets, uint32_t frames_in_flight);
void CountReserveEviction(uint64_t textures);

// banjo_renut_keep_gpu_written (package 27): a host texture the GPU wrote (a resolve destination) is never released by
// an eviction rule. Renut's resolves stay on the GPU (nothing is written back to guest memory), so such a texture made
// again from guest memory would hold whatever that memory holds: Renut's own backstop released them after 3,600 idle
// frames. The game freeing or reusing their memory still replaces them. Not an enhancement: no master switch.
[[gnu::always_inline]] inline bool KeepGpuWritten() { return Latch().keep_gpu_written; }
// banjo_gpu_nvk_draw_contract under Renut's renderer (package 27; the Switch): NVK's per-draw paths (draw emission in
// one go, only the dirty cbuf slots and dynamic state groups, pipeline prefetch) and its dynamic descriptors by
// differences (masseffect_native_set4_differences: sets 5-7 are re-bound with new offsets every draw) are process-wide
// requests in NVK (nvk_switch_dibujo / nvk_switch_set4 in the linked Mesa, each exact and checked by its own guard).
// banjo-nx's renderer makes them; with Renut's renderer nobody did. Called once a frame from the render thread
// (requests, guards), every 10 s from the report (NVK's own figures), and before each pipeline bind (the prefetch hint).
void ControlNvkLocked();

// banjo_gpu_bool_spec under Renut's renderer (package 27; Vulkan): the bools a draw's shaders test as specialization
// constants with the draw's values (banjo_bool_spec.h in the translator, the key's bool words in render/pipeline.h). A
// combination not built yet draws with the generic pipeline while a prebuild worker builds it. In-run A/B target 7.
[[gnu::always_inline]] inline bool BoolSpecOn() { return Latch().bool_spec && g_ab_off_plain != 7; }
// banjo_gpu_blend_skip under Renut's renderer (package 27; Vulkan): pixels the blend would not change are discarded
// (BlendSkipMode in render/pipeline.cpp, banjoBlendSkip in shader_common.h). In-run A/B target 8.
[[gnu::always_inline]] inline bool BlendSkipOn() { return Latch().blend_skip && g_ab_off_plain != 8; }
// banjo_renut_alpha_to_mask_test (2026-10-18): alpha to coverage on a single-sample target drawn as an alpha test at 0.5.
[[gnu::always_inline]] inline bool AlphaToMaskTestOn() { return Latch().alpha_to_mask_test; }
// banjo_renut_depth_words_stencil (2026-10-18): the depth resolve's words carry the stencil.
[[gnu::always_inline]] inline bool DepthWordsStencilOn() { return Latch().depth_words_stencil; }
// banjo_renut_point_minmax_radius (2026-10-18): PA_SU_POINT_MINMAX read as radii (value / 8 is the diameter).
[[gnu::always_inline]] inline bool PointMinMaxRadiusOn() { return Latch().point_minmax_radius; }
// banjo_renut_half_pixel_offset (2026-10-18): the viewport moved half a pixel for D3D9 pixel centres.
[[gnu::always_inline]] inline bool HalfPixelOffsetOn() { return Latch().half_pixel_offset; }
// banjo_gpu_depth_only under Renut's renderer (package 28): depth-only draws without their pixel shader
// (DrawsWithoutPixelShader in render/draw.cpp). In-run A/B target 11. Counted for the [renut] report.
[[gnu::always_inline]] inline bool DepthOnlyOn() { return Latch().depth_only && g_ab_off_plain != 11; }
void CountDepthOnly();
// banjo_gpu_output_trim under Renut's renderer (package 28; Vulkan): the colour channels a pipeline never uses are 0
// from the pixel shader's exits on (OutputTrimMask in render/pipeline.cpp, banjoTrim in shader_common.h). A/B 12.
[[gnu::always_inline]] inline bool OutputTrimOn() { return Latch().output_trim && g_ab_off_plain != 12; }
[[gnu::always_inline]] inline bool DiagTrimPoison() { return Latch().diag_trim_poison; }  // banjo_renut_diag_trim_poison: its test
[[gnu::always_inline]] inline bool DiagGpuShadersOn() { return Latch().diag_gpu_shaders; }  // banjo_renut_diag_gpu_shaders (package 28b): the GPU profile split by pixel shader
// banjo_gpu_depth_words_view under Renut's renderer (package 28): textures over a depth resolve's memory sample its
// words texture through a view instead of a copy (WordsViewSlot in render/textures.cpp). In-run A/B target 13. Counted:
// binds of such a texture through the view or with the copy path.
[[gnu::always_inline]] inline bool DepthWordsViewOn() { return Latch().depth_words_view && g_ab_off_plain != 13; }
// banjo_renut_record_worker's half of the in-run A/B (target 14), read when a command list is opened.
bool RecordWorkerAbOn();
// banjo-nx (2026-11-20, render-thread batch #31): inline (a call into this file at ~1 draw in 1 in Showdown Town, from
// every texture path's memo answers) and skipped with log_level "off" (the 10 s line is all that reads it).
extern std::atomic<uint64_t> g_words_viewed, g_words_copied;
[[gnu::always_inline]] inline void CountDepthWords(bool viewed) {
  if (!Latch().counts)
    return;
  std::atomic<uint64_t> &c = viewed ? g_words_viewed : g_words_copied;
  c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
// vivapinata-nx (2026-11-12, vivapinata_depth_resolve_skip_unread, A/B 156; port 115): left out (0), drawn late from an
// unchanged source (1), drawn late from newer content or not at all (2).
bool DepthResolveSkipUnreadOn();
void CountDepthResolveSkip(int what);
// banjo_deko3d_draw_resolves (2026-10-18): colour resolves drawn instead of copied on deko3d.
[[gnu::always_inline]] inline bool DrawResolvesOn() { return Latch().draw_resolves; }
// 2026-10-18, a census: binds of a depth resolve's float (R32F) target, not its words. banjo-nx (2026-11-20, batch #31):
// inline and skipped with log_level "off", as CountDepthWords (1.0 million calls a 10 s in the city: a draw's).
extern std::atomic<uint64_t> g_depth_float_sampled;
[[gnu::always_inline]] inline void CountDepthFloatSampled() {
  if (!Latch().counts)
    return;
  g_depth_float_sampled.store(g_depth_float_sampled.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
// banjo_deko3d_depth_compression / banjo_deko3d_zcull (2026-10-18): read before the deko3d device is made.
bool Deko3DDepthCompressionOn();
bool Deko3DZcullOn();
// 2026-10-30, from sasrt-nx: banjo_deko3d_msaa_positions, _a2c_dither, _strip_restart (plume Deko3DOptions).
bool Deko3DMsaaPositionsOn();
bool Deko3DA2cDitherOn();
bool Deko3DStripRestartOn();
bool Deko3DGpuRegionOn();  // banjo-nx (2026-10-28, h10): banjo_deko3d_gpu_region
// banjo_deko3d_barrier_merge / _lean (2026-10-19, GPU): fewer pipeline drains and texture cache invalidations.
bool Deko3DBarrierMergeOn();
bool Deko3DBarrierLeanOn();
bool Deko3DVtxDedupeOn();  // banjo-nx (2026-10-26)
bool Deko3DTextureSkipOn();  // banjo-nx (2026-10-27)
bool Deko3DProvisionalCountOn();  // banjo-nx (2026-10-28): banjo_deko3d_provisional_count
bool Deko3DAsyncVariantsOn();  // banjo_deko3d_async_variants (2026-10-21)
// banjo_deko3d_alpha_spec (2026-10-18): the alpha test's function in the pipeline key (constant 903).
[[gnu::always_inline]] inline bool AlphaSpecOn() { return Latch().alpha_spec; }
// banjo_deko3d_tess_spec / banjo_deko3d_vs_outputs (2026-10-18): constants 904 and 905 of the deko3d shaders.
[[gnu::always_inline]] inline bool TessSpecOn() { return Latch().tess_spec; }
// banjo_gpu_fetch_spec: a tessellated draw's constants 904 and 920-923 (render/draw.cpp FetchSpecWords).
[[gnu::always_inline]] inline bool FetchSpecOn() { return Latch().fetch_spec; }
bool Deko3DVsOutputsOn();
// banjo_deko3d_uam_prmt (2026-10-18): uam's PRMT pass, and the DKSH cache file that goes with it.
bool Deko3DUamPrmtOn();
// banjo_deko3d_early_hoist: constant 907 of the deko3d GLSL (tools/deko3d_glsl_opt.py earlyhoist) left at 1.
bool EarlyHoistOn();
// banjo_deko3d_txq_constants / banjo_deko3d_txq_verify (2026-10-19, GPU): constant 908 of the deko3d GLSL
// (tools/deko3d_glsl_opt.py txq): 1 the texture size buffer (default), 0 the texture query, 2 both and magenta where
// they differ.
uint32_t TxqMode();
// banjo_renut_stall_present (2026-10-19): the overlay pump re-presents the last front buffer while the guest stalls.
bool StallPresentOn();
// banjo_deko3d_early_z (2026-10-19, GPU): constant 909 for draws that write neither depth nor stencil.
bool EarlyZOn();
// banjo_deko3d_gpr32 (2026-10-19, GPU A/B): constant 910 (the fur family compiled with 32 GPRs at most).
bool Gpr32On();
// banjo_deko3d_light_branch (2026-10-20, GPU A/B): constant 911 (the listed lit shaders' lights in branches on their
// attenuation test, tools/deko3d_glsl_opt.py lightbranch).
bool LightBranchOn();
// banjo_deko3d_skin_once (2026-10-20, GPU A/B): constant 913 (the fur's vertex shaders skin once a frame,
// tools/deko3d_glsl_opt.py skinonce; render/draw.cpp FurSkinLocked). CountSkinOnce: 0 wrote, 1 read, 2 plain, 3 no room.
bool SkinOnceOn();
// banjo_deko3d_eager_kicks (2026-10-20, input latency A/B): kick lists of 100 draws, up to 6 a frame, each submitted at
// its own kick (backend/video.cpp KickLocked); false in the off half of the in-run A/B (target 18).
bool EagerKicksOn();
bool ProgressiveSubmitOn();  // port 164 (banjo_deko3d_progressive_submit)
// banjo_renut_smaa (2026-10-21): SMAA on the final composite (render/smaa.cpp); false in the off half of the in-run A/B
// (target 19). Its preset (1 medium, 2 high), luma threshold and the composite's pixel shaders (hex, comma separated).
bool SmaaOn();
int SmaaQuality();
double SmaaThreshold();
std::string SmaaAfterPsList();
int32_t SmaaCompareSeconds();  // banjo_renut_diag_smaa_compare_s
void CountSkinOnce(uint32_t kind);
// 2026-10-20: a texture bind whose fetch constant has an exponent adjust (not applied by Renut).
void CountExpAdjust(int adjust);
// A write that missed an entry of the same shader and size: bit 0 vertex data, 1 bone data, 2 indices, 3 first
// vertex, 4 shared constants differed.
void CountSkinMiss(uint32_t fields);
// banjo_renut_diag_null_ps (2026-10-20, diagnostic): constant 912 (the fur family discards at once).
bool NullPsOn();
// banjo_renut_device_swizzle (2026-10-20, the colour flash): textures sampled through the device fetch constant's
// swizzle, not their header's.
bool DeviceSwizzleOn();
// banjo_deko3d_latency_trim (2026-10-20, input latency): hold the render thread after a present until the GPU has
// finished all but the frame's last part (backend/video.cpp).
bool LatencyTrimOn();
// Counts a bind through the device's swizzle in frame 'frame' (the periodic report's "[flash] device swizzle" line).
void CountDeviceSwizzle(uint32_t frame);
// Counts a bind whose texture header describes the same data as the device fetch constant but differs in the
// masked dwords set in 'dwords' (bit i = dword i): the "[flash] header vs device" line.
void CountHeaderDescDiffer(uint32_t dwords);
// banjo_renut_fur_shells (2026-10-20, NOT exact): 1 every fur shell drawn; N every Nth of a mesh's run of shells.
uint32_t FurShellEvery();
// banjo_renut_portrait_fur_shells: the same for targets of 128 x 128 or less (the dialogue portrait).
uint32_t PortraitFurShellEvery();
// banjo_renut_fur_shells_percent (2026-10-21, NOT exact): the share of each fur part's shells drawn (100 = all).
uint32_t FurShellPercent();
// banjo_renut_portrait_fur_shells_percent / banjo_renut_portrait_msaa_samples (2026-10-21, NOT exact): the dialogue
// portrait (targets of 128 x 128 or less): its fur shells' share (100 = as the rest) and its MSAA samples (0 = the
// game's, 1 = none, 2 = 2x).
uint32_t PortraitFurShellPercent();
uint32_t PortraitMsaaSamples();
// msaa_level (2026-11-02, katamari-nx, the player's setting): the most samples any target gets: 4 (the game's), 2 or 1.
uint32_t MsaaLevel();
void CountFurShell(bool drawn);
// banjo-nx (2026-10-23): banjo_renut_chain_reuse's answer for one draw (what: 0 targets, 1 textures, 2 viewport,
// 3 pipeline key, 4 pipeline), for the "[renut] chain reuse" line.
void CountChainReuse(int what, bool reused);
// banjo-nx (2026-10-26): many answers at once ([what][0] draws, [what][1] reused), from the render thread's own counts.
void CountChainReuseN(const uint64_t counts[5][2]);
// uam's banjo flags (banjo_deko3d_uam_prmt, banjo_deko3d_uam_gvn, banjo_deko3d_uam_uniform_branches,
// banjo_deko3d_uam_wide_rows); 1471 by default.
unsigned Deko3DUamFlags();
// banjo_background_compile_core / banjo_deko3d_compile_priority (2026-10-18): the uam thread's core and priority.
int Deko3DCompileCore();
// sasrt-nx (2026-11-22, sasrt_compile_threads_core -1): what places the uam thread (every core of the process), or null
// (then it is pinned to Deko3DCompileCore(), -1 none).
void (*Deko3DCompileThreadPlacer())();
int Deko3DCompilePriority();
int Deko3DBackgroundCompileShare();  // banjo_deko3d_background_compile_share (2026-10-23)
bool Deko3DCompileWaitBoostOn();  // banjo_deko3d_compile_wait_boost (2026-10-23)
bool Deko3DFastFirstCompileOn();  // banjo_deko3d_fast_first_compile (port 56, from mk9-nx)
bool Deko3DAsyncCacheReadsOn();  // banjo_deko3d_async_cache_reads (2026-10-23)
void CountDrawnResolve();
void CountSmaa();  // banjo_renut_smaa: a frame done
// banjo-nx (2026-10-21, internal resolution, NOT exact): the 3D image's heights by mode, the display's when docked, the
// upscale's filter (0 bilinear, 1 bicubic), and a composite scaled up for the HUD (or drawn at the display's size).
int32_t RenderHeightHandheld();
int32_t RenderHeightDocked();
int32_t DisplayHeightDocked();
int UpscaleFilter();
bool HostMipsOn();  // banjo_renut_host_mips (read at start)
void CountUiUpscale(bool scaled);
// A UI depth target filled (true), or a draw that kept a depth target smaller than its colour target (false).
void CountUiDepth(bool filled);
// 2026-10-20 (the Renut CPU port from sa2-nx): where the render thread's draws come from: 0 immediate (a whole device
// copy queued by the game thread), 1 replayed from a recorded command buffer (device deltas).
void CountDrawSource(int source, uint64_t draws = 1);
// banjo_renut_resolve_skip (2026-10-19): a resolve whose source and destination are as at the same resolve before is
// not done again (render/draw.cpp ResolveStateLocked); counted per kind.
[[gnu::always_inline]] inline bool ResolveSkipOn() { return Latch().resolve_skip; }
// banjo_renut_depth_words_lazy (2026-10-19): depth resolve words only where read; counted 0 a resolve without them,
// 1 written late, 2 read stale (the source had changed).
[[gnu::always_inline]] inline bool DepthWordsLazyOn() { return Latch().depth_words_lazy; }
void CountLazyWords(int what);
// 2026-10-20: a draw's fetch data checked for the 16-byte alignment the aw4 shader pass relies on.
void CountFetchAlignment(bool aligned);
// banjo_deko3d_depth_handover (2026-10-19, GPU): depth resolves left in their target's image (render/draw.cpp, "Depth
// hand-over"). Counted: 0 resolves handed over, 1 binds of a target's image, 2 images taken at a whole clear,
// 3 resolves done after all (the target bound or gone, or the destination needed in its texture), 4 of them from a
// taken image, 5 contents lost (none expected).
[[gnu::always_inline]] inline bool DepthHandOverOn() { return Latch().depth_handover; }
// banjo-nx (2026-10-28, GPU block): banjo_deko3d_depth_handover_readonly (A/B 64).
bool DepthHandOverReadOnlyOn();
// kameo-nx (2026-11-03, the GPU round from the third console run; docs/kameo-notes.md). A/B targets 73-79.
bool ColorHandOverOn();          // banjo_deko3d_color_handover (73)
// blur-nx (2026-11-10/11, port 76): its renderer fixes, A/B targets 211-221 (210 and 214 not ported).
bool ExpandAliasOn();            // blur_renut_expand_alias (211, blur-nx)
bool ResolveKeepDestOn();        // blur_renut_resolve_keep_dest (212, blur-nx)
bool OcclusionQueriesOn();       // blur_renut_occlusion_queries (213, blur-nx)
bool StackSurfaceHeadersOn();    // blur_renut_stack_surface_headers (215, blur-nx)
bool SampleGridResolveOn();      // blur_renut_sample_grid_resolve (216, blur-nx)
bool SurfaceSizeGuardOn();       // blur_renut_surface_size_guard (217, blur-nx)
bool WipeOverwrittenOn();        // blur_renut_wipe_overwritten (218, blur-nx)
bool ResolveExpBiasOn();         // blur_renut_resolve_exp_bias (219, blur-nx)
bool EdramInheritOn();           // blur_renut_edram_inherit (220, blur-nx)
bool MsSliceResolveOn();         // blur_renut_ms_slice_resolve (221, blur-nx)
bool ScanOutHandOverOn();        // banjo_deko3d_scanout_handover (74)
bool DepthHandOverSampledOn();   // banjo_deko3d_depth_handover_sampled (75)
bool DepthReadLayoutOn();        // banjo_deko3d_depth_read_layout (76)
bool ResolveKeepsTargetOn();     // banjo_deko3d_resolve_keeps_target (77)
bool PresentNoPowOn();           // banjo_renut_present_no_pow (78)
bool PresentTexelLoadOn();       // banjo_renut_present_texel_load (79)
bool GammaUploadOnceOn();        // banjo_renut_gamma_upload_once
// The colour hand-over's counts, as CountHandOver's: 0 not drawn, 1 binds of an image, 2 taken at a whole clear, 3 drawn after
// all, 4 of them from a taken image, 5 lost, 7 drawn for a draw writing the target, 8 for a draw sampling it, 9 other.
void CountColorHandOver(int what);
// Why a colour resolve was drawn: 0 smaller than the target, into a texture of another size; 1 part of a target into a
// texture of its size; 2 whole, but the texture is another size or scale; 3 the texture has levels; 4 format or kind.
void CountColorNotLeft(int why);
// banjo_deko3d_depth24 (2026-10-19, GPU): D24S8 depth targets as 24-bit unorm (render/format.cpp).
[[gnu::always_inline]] inline bool Depth24On() { return Latch().depth24; }
// banjo_deko3d_push_constants (2026-10-19, render-thread proposals 2 and 5): the float constants and SharedConstants
// pushed into fixed uniform buffers by what changed (render/draw.cpp, "Pushed constants"); _verify checks the model.
[[gnu::always_inline]] inline bool PushConstantsOn() { return Latch().push_constants; }
// banjo_deko3d_kick_submit (2026-10-19, input latency): a frame's command lists are submitted at the game's kicks, not
// only at the present; banjo_deko3d_ring_cleaner (render-thread proposal 23): the upload ring cleaned by another core.
[[gnu::always_inline]] inline bool KickSubmitOn() { return Latch().kick_submit; }
// banjo_late_input (2026-10-19, input latency): backend/render_thread.cpp.
[[gnu::always_inline]] inline bool LateInputOn() { return Latch().late_input; }
[[gnu::always_inline]] inline bool RingCleanerOn() { return Latch().ring_cleaner; }
void CountKickSubmit(int what);
[[gnu::always_inline]] inline bool PushConstantsVerifyOn() { return Latch().push_constants_verify; }
// sasrt-nx (2026-11-09, RT4; port 54): inline, as CountSharedConstants.
extern std::atomic<uint64_t> g_pushed[4];
[[gnu::always_inline]] inline void CountPushedConstants(uint64_t bytes, int what) {
  if (!Latch().counts)  // banjo-nx (2026-11-20, batch #4)
    return;
  g_pushed[what].store(g_pushed[what].load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
}
// banjo-nx (2026-10-19, render-thread proposal 23): upload ring bytes cleaned at the submits.
void CountRingPublished(uint64_t bytes);
void CountHandOver(int what);
void CountSkippedResolve(bool depth);
// banjo_renut_cache_save_quiet (package 28): a pipeline was built (render/pipeline.cpp Build, any thread).
void NotePipelineBuilt();
// banjo_gpu_tess_grid under Renut's renderer (package 27): tessellated draws indexed by grid vertex, so the vertex
// shader runs once per grid vertex instead of once per triangle corner (TessellationGridFor in render/draw.cpp). In-run
// A/B target 9. Counted: corners drawn and grid vertices, for the [renut] report.
[[gnu::always_inline]] inline bool TessGridOn() { return Latch().tess_grid && g_ab_off_plain != 9; }
void CountTessGrid(uint64_t corners, uint64_t vertices);
enum class BoolSpecEvent { kDraw, kGeneric, kBuilt };
void CountBoolSpec(BoolSpecEvent event);
void ReportNvk();
void BeforePipelineBind(plume::RenderPipeline *pipeline);
// banjo_renut_diag_poison_slots (a test): a descriptor slot given back points at a magenta texture instead of the null
// one, so a draw that still samples a released texture shows magenta in the captures (backend/video.cpp).
[[gnu::always_inline]] inline bool PoisonSlotsOn() { return Latch().poison_slots; }
int DiagBuildDelayMs();
void ResolveTraceFrameStart();  // banjo_renut_diag_resolve_trace: the render thread's frame boundary
// banjo-nx (2026-10-19, render-thread proposal 22): inline (read per resolve, per bind and per target change).
extern std::atomic<bool> g_trace_frame;
inline bool ResolveTraceOn() { return g_trace_frame.load(std::memory_order_relaxed); }  // this frame is traced  // banjo_renut_diag_build_delay_ms (a test): a wait before every pipeline build

// Around the plume device (backend/video.cpp): before it is created, after, and at shutdown once the prebuild has
// stopped and the queue is idle.
void BeforeDeviceCreated();
void OnDeviceCreated(plume::RenderDevice *device);
void BeforeDeviceDestroyed();

// The report and the in-run A/B (banjo_renut_ab_target, banjo_renut_ab_s), fed by renut_engine/perf/frame_stats.cpp:
// every 10 s one "[renut]" line (game frames, the render thread's busy time and the GPU's time per frame); with a
// target, its part switched off and on every banjo_renut_ab_s seconds and the two halves' figures in a
// "[renut A/B]" line. Targets: 1 precise barriers, 2 the render thread's pin to banjo_ring_core (the Switch),
// 3 SharedConstants reuse, 4 the push constant skip, 5 the capture pool, 6 banjo_renut_exact_streams (its cost).
void StatsGameFrame();                     // a D3DDevice_Swap (game thread)
void StatsRenderThreadFrame(double busy_ms);  // the render thread's frame (render thread)
void StatsGpuFrame(double ms);             // a frame's GPU time

// banjo_renut_pmull_hash: chosen once, before the renderer's first draw (BeforeDeviceCreated), after a self-test.
extern bool g_pmull_hash;
uint64_t PmullHash(const void *data, size_t size, uint64_t seed);

// The content hash of guest data: only ever compared with hashes this function made in the same session.
inline uint64_t ContentHash64(const void *data, size_t size, uint64_t seed = 0) {
  if (g_pmull_hash)
    return PmullHash(data, size, seed);
  return XXH3_64bits_withSeed(data, size, seed);
}

}  // namespace banjo::renut_gpu
