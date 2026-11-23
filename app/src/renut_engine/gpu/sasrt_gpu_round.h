#pragma once
// sasrt-nx (2026-11-08): the GPU round after the move to the ultimate base (docs/gpu-round-1108.md). Pixel-identical GPU
// optimisations, each with its own key (on by default), an in-run A/B target (banjo_renut_ab_target = 90 + its number:
// its "off" half runs without it) and a count in the 10 s "[sasrt gpu]" line.
#include <atomic>
#include <cstdint>
#include <string>

#include "renut_engine/gpu/banjo_renut_tuning.h"

namespace sasrt::gpu_round {

// X(name, key, what it does): the items in order; the A/B target of item i is 90 + i, from kRt2DropThrottle on 190 and
// up (AbTargetOf).
#define SASRT_GPU_ROUND_ITEMS(X)                                                                                         \
  X(kInertDraws, sasrt_gpu_skip_inert_draws, "draws that write nothing skipped before their targets are bound")      \
  X(kPreciseDepth, sasrt_gpu_precise_depth_resolve, "depth decompressed only when written since its last decompression") \
  X(kPlainFetches, sasrt_gpu_plain_fetches, "pixel shaders without the sign / gamma / exponent code of plain fetches") \
  X(kAlphaSpec, sasrt_gpu_alpha_spec, "the alpha test's function in the pipeline: no discard without the test")         \
  X(kGammaOnce, sasrt_gpu_gamma_upload_once, "the game's gamma ramp copied to its texture only when it changes")       \
  X(kDecompressMerge, sasrt_gpu_decompress_merge, "the barrier after a depth decompression merged with the pending one") \
  X(kReadbackMerge, sasrt_gpu_readback_barrier_merge, "a readback's full barriers merged with those next to them")    \
  X(kFetchCopyCompute, sasrt_gpu_fetch_copy_compute, "memexport seeds and fetch data copied by the 3D engine's compute") \
  X(kDataInvalidate, sasrt_gpu_data_cache_invalidate, "buffer barriers drop the shaders' data caches, not their code") \
  X(kOneX, sasrt_gpu_one_x, "a draw at 1x: the texture scale codes and the pixel position's scale folded")        \
  X(kFaceSign, sasrt_gpu_face_sign, "the face register's sign test without its reciprocal")                         \
  X(kReadbackCompute, sasrt_gpu_readback_compute, "the memexport readbacks copied by the 3D engine's compute")      \
  X(kTiledCache, sasrt_gpu_tiled_cache, "Maxwell's tiled caching (binning) on (off by default: the console's to judge)") \
  X(kTexelReadback, sasrt_gpu_texel_readback_compute, "the 1x1 luminance read back for the CPU by the 3D engine's compute") \
  X(kLazyDepth, sasrt_gpu_lazy_depth_resolve, "depth decompressed right before a read, not at each unbind (off by default)") \
  /* the second round (2026-11-08, docs/gpu-round-1108.md "Round 2"): A/B 105 and up */                                 \
  X(kSeedLead, sasrt_gpu_seed_no_lead_wait, "memexport seeds into fresh windows without a wait for the 3D engine first") \
  X(kConvertLead, sasrt_gpu_convert_lead_wait, "an exported stream conversion without its wait when no export was drawn since one") \
  X(kIdentityRamp, sasrt_gpu_identity_ramp, "the scan-out's gamma ramp not looked up when it is the identity at 8 bits") \
  X(kConvertMemoKeep, sasrt_gpu_convert_memo_keep, "exported stream conversions kept for later draws across new exports") \
  X(kDepthFloatPs, sasrt_gpu_depth_resolve_float_ps, "a depth resolve without words by a shader that computes the float alone") \
  X(kSameTargets, sasrt_gpu_same_targets_kept, "the targets bound already not bound again after a dispatch or resolve") \
  X(kReadbackSpans, sasrt_gpu_readback_spans, "the memexport readback's windows that follow one another copied by one dispatch") \
  X(kSeedWaitOnly, sasrt_gpu_seed_wait_only, "the wait after the seeds keeps the texture, data and constant caches") \
  X(kFenceFlush, sasrt_gpu_readback_fence_flush, "the compute readbacks without an L2 flush of their own (the slot's fence flushes it)") \
  X(kConvertTail, sasrt_gpu_convert_tail_vertex_only, "the wait after a stream conversion keeps the shaders' data and constant caches") \
  X(kConvertLeadData, sasrt_gpu_convert_lead_data_only, "the wait before a stream conversion drops the global-data cache alone") \
  X(kClipSpec, sasrt_gpu_clip_spec, "vertex shaders without the user clip distance where no plane is set, the clip slot off") \
  X(kLeanSkip, sasrt_gpu_lean_barrier_skip, "a move into a target layout with nothing to wait for is not a barrier") \
  X(kReadbackTrim, sasrt_gpu_readback_barriers_trim, "the memexport readback's waits: the data cache alone first, none after") \
  X(kExportDiscard, sasrt_gpu_export_raster_discard, "export-only draws with the rasterizer off, not only an empty scissor") \
  /* the third round (2026-11-08, docs/gpu-round-1108.md "Round 3"): A/B 120 and up */                                 \
  X(kSeedCpu, sasrt_gpu_seed_cpu_direct, "memexport seeds written into their windows by the CPU: no copy dispatch, no wait") \
  X(kConvertTailNoImage, sasrt_gpu_convert_tail_no_texture_drop, "the wait after a stream conversion keeps the texture cache too") \
  X(kVsInputMask, sasrt_gpu_vs_input_mask, "vertex attributes the bound vertex shader variant never reads are not fetched") \
  X(kExportNoPs, sasrt_gpu_export_no_pixel_shader, "export-only draws (rasterizer off) built without their pixel shader") \
  X(kVsScreenSpec, sasrt_gpu_vs_screen_space_spec, "the vertex shaders' screen-space branch as a constant of the draw") \
  X(kVsRemapSpec, sasrt_gpu_vs_depth_remap_spec, "the vertex shaders' depth remap branch as a constant of the draw") \
  X(kStackSpec, sasrt_gpu_stacked_3d_spec, "a 3D fetch's stacked-or-volume choice as a constant of the draw") \
  X(kStackFilterSpec, sasrt_gpu_stacked_filter_spec, "a stacked fetch's layer filter and wrap as constants of the draw") \
  X(kFetchEndianSpec, sasrt_gpu_fetch_endian_spec, "a vertex shader's memory fetches' byte order as constants of the draw") \
  X(kGprCap, sasrt_gpu_gpr_cap, "three pixel shaders just over 32 registers compiled with 32 (a register or two spilled)") \
  X(kGprCapFilter, sasrt_gpu_gpr_cap_filter, "the 60-register filter pass 9041AFF6 compiled with 32 (full occupancy, spills)") \
  X(kExportFmtSpec, sasrt_gpu_export_format_spec, "a memexport's format word as a constant of the draw: its one pack compiled") \
  X(kTexDropTracked, sasrt_gpu_texture_drop_tracked, "a texture made readable drops the texture cache only if it was sampled since the last drop") \
  X(kTargetWaitTracked, sasrt_gpu_target_wait_tracked, "a texture made a target waits only if it was sampled or copied since the last wait") \
  X(kReadWaitTracked, sasrt_gpu_read_wait_tracked, "a texture made readable waits only if it was written since the last wait (or drops the cache)") \
  /* the fourth round (2026-11-08, night: the 33 ms goal; docs/gpu-round-1108.md "Round 4"): A/B 135 and up */          \
  X(kHandoverSmaa, sasrt_gpu_handover_with_smaa, "banjo's colour hand-over kept on with SMAA (SMAA's target settled first)") \
  X(kExportWindowUbo, sasrt_gpu_export_window_ubo, "memexport windows read from the constant buffer, not a local array (constant 944)") \
  X(kExportListTopology, sasrt_gpu_export_list_topology, "export draws with the rasterizer off drawn as triangle or line lists, not points") \
  X(kExportNoPointGs, sasrt_gpu_export_no_point_gs, "export point lists with the rasterizer off drawn without the point-sprite geometry shader") \
  /* the fifth round (2026-11-09, docs/gpu-round-1108.md "Round 5"): A/B 139 and up */                                     \
  X(kRestoreDirect, sasrt_gpu_depth_restore_direct, "SASRT only: the EDRAM depth restore in one pass") \
  X(kRestoreClear, sasrt_gpu_depth_restore_whole_clear, "the depth restore clears depth and stencil together when it draws the whole target") \
  X(kFetchConvSpec, sasrt_gpu_fetch_conversion_spec, "a non-plain fetch's sign, gamma and exponent words as constants of the draw (950-957)") \
  X(kFetchExpSpec, sasrt_gpu_fetch_exp_3d_spec, "a 3D or cube fetch's exponent adjust as a constant of the draw (958-973)") \
  X(kConvertVertexPass, sasrt_gpu_convert_vertex_pass, "exported stream conversions drawn as a vertex pass (flat: off)") \
  /* sasrt-nx's render-thread round (2026-11-09; port 54): A/B 144 and up, and 200 for the group (kAbRtGroup) */          \
  X(kRtRestoreCandidates, sasrt_rt_restore_candidates, "render thread: a depth target's EDRAM-restore source found among its own few candidate colour targets, kept until a target is built or retired, not by two scans of every target a draw") \
  X(kRtCachedExportSeeds, sasrt_rt_cached_export_seeds, "render thread: the memexport windows' buffer CPU-cached, so the CPU writes the seeds at cache speed and cleans their lines (chosen at start: no in-run A/B)") \
  /* sasrt-nx's render-thread rounds 2-4 (2026-11-20, their universal items; docs/render-thread-ideas-1120.md there): A/B 190 and up (200 skipped: kAbRtGroup), 241-243 for each round */ \
  X(kRt2DropThrottle, sasrt_rt2_drop_throttle, "render thread: DropReplacedRanges' pass over the stream cache's limit every 4th frame, not every frame (memory only)") \
  X(kRt2PagesTail, sasrt_rt2_pages_tail, "render thread: PagesClean's last 1-7 pages of a range in one masked NEON block when none of their cards is written") \
  X(kRt2RingPrefetch, sasrt_rt2_ring_prefetch, "render thread: the next ring item's narrowed device delta prefetched to six lines, not four") \
  X(kRt2ReadableMemo, sasrt_rt2_readable_memo, "render thread: the vertex fills' guest readability answers kept while no page has lost read access (no heap mutex, no page walk)") \
  X(kRt2TextureMemoTwoWay, sasrt_rt2_texture_memo_two_way, "render thread: the texture slot memo two-way (2,048 sets of two), the way of an earlier frame replaced") \
  X(kRt2RepeatRing, sasrt_rt2_repeat_ring, "render thread: WholeGeometryTry's repeat answer kept for the last four meshes of a kick, not one") \
  X(kRt2HeldExportBitmap, sasrt_rt2_held_export_bitmap, "render thread: GetStreamSource looks up the kept exports only where a 64 KB granule bitmap says one may lie") \
  X(kRt2ColorAliasMemo, sasrt_rt2_color_alias_memo, "render thread: ColorAliasesLocked's answer kept per surface until a target is made or retired") \
  X(kRt3SurfaceMemo256, sasrt_rt3_surface_memo_256, "render thread: GetHostSurfaceLocked's memo 256 entries, not 64") \
  X(kRt3DepthBiasFields, sasrt_rt3_depth_bias_fields, "render thread: the fast draw's depth bias not set again when its packets are unchanged since the draw before") \
  X(kRt4PushRunsMerged, sasrt_rt4_push_runs_merged, "render thread: two runs of pushed constant groups with one or two groups between them pushed as one run (the gap's current device values too; never over a literal register)") \
  X(kRt4StateIdCrc, sasrt_rt4_state_id_crc, "render thread: RenderStateIdLocked's table index by CRC32C of the words (14 crc32cx), not 27 dependent multiplies") \
  X(kRt4ExportReadsFast, sasrt_rt4_export_reads_fast, "render thread: a draw whose only memexport reads are host-layout vertex streams found on the GPU stays on the fast path (the streams converted there as the full path does)")

enum Item : int {
#define SASRT_GPU_ROUND_ENUM(name, key, what) name,
  SASRT_GPU_ROUND_ITEMS(SASRT_GPU_ROUND_ENUM)
#undef SASRT_GPU_ROUND_ENUM
  kCount
};

constexpr int kAbBase = 90;
// sasrt-nx (2026-11-09, the render-thread round; port 54): A/B target 200 switches every render-thread item (from
// kRtRestoreCandidates on) at once: the round's total, best with banjo_renut_ab_s 30 (4 s halves cannot resolve 1-3 ms
// of render thread).
constexpr int kAbRtGroup = 200;
// sasrt-nx's render-thread rounds 2-4 (2026-11-20; the base, 2026-11-22): their items' A/B targets are 190 and up in the
// list's order, 200 skipped (kAbRtGroup above); 241, 242 and 243 switch each round's items at once (its total; with
// banjo_renut_ab_s 30: a round is 1-3 % of the render thread).
constexpr Item kRt2First = kRt2DropThrottle, kRt3First = kRt3SurfaceMemo256, kRt4First = kRt4PushRunsMerged;
constexpr int kAbRt2Base = 190;
constexpr int kAbRt2Group = 241, kAbRt3Group = 242, kAbRt4Group = 243;
constexpr int AbTargetOf(int i) {
  if (i < kRt2First)
    return kAbBase + i;
  const int n = kAbRt2Base + (i - kRt2First);
  return n >= kAbRtGroup ? n + 1 : n;
}
// plume counts its own items (Deko3DStats::sasrtRound, 64 counts; Report adds them to the items below 64); the
// render-thread rounds' items are counted here alone.
static_assert(kRt2First <= 64, "plume's Deko3DStats::sasrtRound has 64 counts");
static_assert(kAbBase + kRt2First <= kAbRt2Base, "the first items' targets end before the rounds' begin");
static_assert(AbTargetOf(kCount - 1) < 211, "the rounds' targets end before blur-nx's 211");

struct Latched {
  bool on[kCount] = {};
};
extern Latched g_latched;
extern std::atomic<bool> g_latched_ready;
void LatchNow();

// The item's key as latched at the first use (the toml is final by then), and not in its A/B "off" half.
// sasrt-nx (2026-11-12, batch C/D, item D3; port 93): the flag read relaxed, as banjo's Latch(): no load-acquire at
// each switch a draw passes (it kept the A57 from starting the draw's later loads early). The values come from the
// configuration only: on deko3d they are latched before the device exists (InstallShaderCache's ApplyPlume), elsewhere
// at the first use; a thread that finds the flag unset latches them itself.
// banjo-nx (2026-11-20, N&B's render thread against renut-nx 1.0.2 in Showdown Town): each item's answer kept in one
// byte (0 not latched yet, 1 off, 2 on: zero-initialised means unlatched), set by LatchNow and again by
// RefreshEffective at each A/B switch (AbApply).
// On() is that one load: before, every call read the latch flag and the A/B half (two atomics) and the latched key,
// FastDrawLocked asks dozens of times a draw, and those loads were 0.13 us of the 9.2 us a draw (1 ms sampling).
// banjo-nx (2026-11-21): plain bytes (relaxed atomics before: loads the compiler could not merge), written at the latch
// and at each A/B switch (the render thread), as g_ab_off_plain.
extern uint8_t g_effective[kCount];
void RefreshEffective();  // the answers again from the latched keys and banjo::renut_gpu::g_ab_off
[[gnu::always_inline]] inline bool On(Item i) {
  uint8_t v = g_effective[i];
  if (v == 0) [[unlikely]] {
    LatchNow();
    v = g_effective[i];
  }
  return v == 2;
}

// Counts for the 10 s line: a load and a store (no read-modify-write on the A57; a count lost between two threads is a
// diagnostic's error, not the renderer's).
extern std::atomic<uint64_t> g_counts[kCount][2];
[[gnu::always_inline]] inline void Count(Item i, uint64_t n = 1, int which = 0) {
  if (!banjo::renut_gpu::Latch().counts)  // banjo-nx (2026-11-20): off with the logs (banjo_renut_draw_counts)
    return;
  std::atomic<uint64_t> &c = g_counts[i][which];
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

const char *AbName(int target);  // nullptr: not one of these
// The items that live in plume's deko3d back end, set from the keys and the A/B half (at start-up and at each A/B
// switch, on the render thread).
void ApplyPlume();
std::string Report();            // the 10 s line's text (and the counts start again)

// sasrt-nx (2026-11-11, port 82; its GPU list's rows 1, 2 and 4; UNTESTED): three pixel-identical rows no game has run
// yet, outside the items above (their own keys, read at start; no A/B target, not in the "[sasrt gpu]" line). Row 2 is
// plume's Deko3DOptions::preciseStencilWrite (ApplyPlume); its count is plume's g_deko3dPreciseStencilSaved.
bool StencilNoopOn();   // sasrt_gpu_stencil_noop (row 1: render/pipeline.cpp Build)
bool SkipCullBothOn();  // sasrt_gpu_skip_cull_both and banjo_renut_cull_both (row 4: render/draw.cpp DrawStateLocked)
// The counts since the start: pipelines built with a no-op stencil off (row 1), pipelines built culling both faces (row
// 4's census) and the cull-both draws left out (row 4; the render thread's, a load and a store).
extern std::atomic<uint64_t> g_census_stencil_noop, g_census_cull_both, g_census_cull_both_skipped;
std::string UntestedReport();  // the 10 s "[renut] untested GPU rows" line's text

}  // namespace sasrt::gpu_round
