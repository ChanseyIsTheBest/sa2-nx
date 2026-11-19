/**
 * @file        rex/ppc/vec_lane.h
 * @brief       A vector register's lane by a run-time index, without a trip through memory (blur-nx, 2026-11-11)
 *
 * The generator writes stvewx / stvewx128 (and stvehx) as a store of one lane picked by the effective address:
 *   REX_STORE_U32(ea, V.u32[3 - ((ea & 0xF) >> 2)])        REX_STORE_U16(ea, V.u16[7 - ((ea & 0xF) >> 1)])
 * With a run-time index the compiler cannot read the lane from the register: it stores the whole 128-bit register to
 * the stack and loads 32 bits back at the index's offset. On the A57 that load cannot be forwarded from the wider
 * store just made, and it waits for the store to reach the cache (blur-nx's console race of 2026-11-11: about a
 * quarter of the samples of sub_8251E6F0, and in four more hot functions). tools/vec_lane_index.py writes the lane as
 * REX_LANE_U32 / REX_LANE_U16, which take the two 64-bit halves from the register (two umov) and pick by the index's
 * bits (two csel and a shift): the same value, no memory.
 *
 * Exact: lane i of the host's little-endian layout is bits [32 i, 32 i + 32) of the 128-bit value, which is half i >> 1
 * of it, shifted right by 32 (i & 1). The index is the generator's (always 0-3, or 0-7 for halfwords: it comes from the
 * low bits of an address); masked here all the same. Checked at start-up by the app's forms self-check
 * (app/src/cpu/banjo_forms_selfcheck.cpp: every index of a known vector), on the device itself, because the guard
 * compares two forms that both use these helpers.
 */
#pragma once

#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace rex::ppc {

/// u32[i & 3] of a vector register's host lanes, read from the register as a whole.
[[gnu::always_inline]] inline uint32_t VecLaneU32(const uint32_t* u32, uint32_t i) noexcept {
#if defined(__aarch64__)
  const uint64x2_t q = vreinterpretq_u64_u32(vld1q_u32(u32));
  const uint64_t lo = vgetq_lane_u64(q, 0), hi = vgetq_lane_u64(q, 1);
#else
  uint64_t lo, hi;
  std::memcpy(&lo, u32, 8);
  std::memcpy(&hi, u32 + 2, 8);
#endif
  const uint64_t d = (i & 2u) ? hi : lo;
  return uint32_t(d >> ((i & 1u) * 32u));
}

/// u16[i & 7] of a vector register's host lanes, read from the register as a whole.
[[gnu::always_inline]] inline uint16_t VecLaneU16(const uint16_t* u16, uint32_t i) noexcept {
#if defined(__aarch64__)
  const uint64x2_t q = vreinterpretq_u64_u16(vld1q_u16(u16));
  const uint64_t lo = vgetq_lane_u64(q, 0), hi = vgetq_lane_u64(q, 1);
#else
  uint64_t lo, hi;
  std::memcpy(&lo, u16, 8);
  std::memcpy(&hi, u16 + 4, 8);
#endif
  const uint64_t d = (i & 4u) ? hi : lo;
  return uint16_t(d >> ((i & 3u) * 16u));
}

}  // namespace rex::ppc
