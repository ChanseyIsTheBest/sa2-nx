#pragma once

// banjo-nx (from futari-nx): count of every self-check that found a fast path and its reference path disagreeing (each
// DIFFERENCE / MISMATCH log line of the renderer, and the native game-function replacements' verify modes). With
// banjo_cpu_verify the self-checks compare on every use and the "[verify]" report line prints this count: 0 means every
// fast path gave the same result as the original path for the whole run.
#include <atomic>
#include <cstdint>

namespace me::native {

inline std::atomic<uint64_t> g_verify_differences{0};
// Comparisons made by checks outside the ring's own counters (native game functions in verify mode).
inline std::atomic<uint64_t> g_verify_native_checks{0};

inline void VerifyDifference() { g_verify_differences.fetch_add(1, std::memory_order_relaxed); }

// banjo_direct_state_ab_s / banjo_ab_target: the enhancement the ring has switched off for the current "off" half of an
// in-run A/B measurement (0 = none; 1 = banjo_direct_state, 2 = banjo_ring_edram_memo, 3 = banjo_ring_constants_dedupe).
// Set by the ring at a Swap.
inline std::atomic<uint32_t> g_ab_off_target{0};

// The EDRAM items of 2026-10-13 the same switch can turn off, one at a time or together (target kAbEdramAll), so a
// recording of one run holds both states of the same scene: 4 = banjo_gpu_proof_tess_leftover, 5 =
// banjo_gpu_edram4_color_consumer, 6 = banjo_gpu_edram4_alias_by_shape, 7 = banjo_gpu_proof_triangle, 8 =
// banjo_gpu_edram4_copy_batch, 9 = banjo_gpu_tiles_once. Not part of "all": 11 = banjo_ring_vertex_persistent, 12 =
// banjo_ring_shader_load_memo, 13 = banjo_ring_vertex_epoch, 14 = banjo_gpu_dead_raster, 15 = banjo_gpu_rect_split,
// 16 = masseffect_native_deferred_recording, 17 = banjo_gpu_proof_depth_only, 18 = banjo_gpu_edram4_split_tiles,
// 19 = the native Direct3D draw path (banjo_d3d_native_draw_indexed and _draw_vertices, me/banjo_d3d_native.h), 20 =
// the ring's use of the Clear markers (banjo_d3d_api_clear, me/banjo_d3d_clear.h).
inline constexpr uint32_t kAbEdramAll = 10;
// banjo-nx (2026-10-16): 40 = the shader round of the twenty-first package together (29 banjo_gpu_vfetch_block with
// the 16-byte bone rows, 37 / 38 banjo_gpu_pred_select(_ps), 39 banjo_gpu_tess_grid): one PC recording checks them all.
inline constexpr uint32_t kAbShaders21 = 40;
// banjo-nx (package 25): 55 = that package's shader round together (49-54: clamp bounds, sign mask, unread outputs,
// bone padding, robustness, the EDRAM import checks).
inline constexpr uint32_t kAbShaders25 = 55;
// banjo-nx (package 26): 70 = that package's round together (56-69).
inline constexpr uint32_t kAbShaders26 = 70;
inline bool AbOn(uint32_t target) {
  const uint32_t off = g_ab_off_target.load(std::memory_order_relaxed);
  return off != target && !(off == kAbEdramAll && target >= 4 && target <= 9) &&
         !(off == kAbShaders21 && (target == 29 || (target >= 37 && target <= 39))) &&
         !(off == kAbShaders25 && target >= 49 && target <= 54) &&
         !(off == kAbShaders26 && target >= 56 && target <= 69);
}

}  // namespace me::native
