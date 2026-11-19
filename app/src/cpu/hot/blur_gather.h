// blur-nx (2026-11-11, CPU batch 2; the user: "optimise the hell out of CPU ... in large batches"): the gathers of
// the two resampler natives (sub_822D9040, sub_822D79A8 in blur_hand_natives_batch3b.h). Each position reads two
// consecutive table entries, lo at ea and hi at ea + 4, as lane 0 of two lvlx128. The console race of the LTO+PGO
// build (2026-11-11) had 30 % of sub_8230EDE0's samples (1.2 % of all CPU) on those reads, done as four byte loads per
// word: the natives' LvlxW, whose q[o + 1u] might wrap in 32 bits for all GCC knows, so it cannot merge them.
//
// When ea is a multiple of 4 its offset in the 16-byte line is 0, 4, 8 or 12, so lane 0 of lvlx128 is the whole
// big-endian word, and so it is at ea + 4. The two words are then the 8 host bytes at the host address of ea (the
// same bytes the two lvlx128 read: the first line's and, at offset 12, the next one's), unless they wrap 4 GB or
// cross into the physical range at 0xE0000000, which the Windows host places 4 KB further on (PairOk). Four pairs
// become two vectors in the generated lane order (set_epi32(x0, x1, x2, x3): x0 in the top lane): lo = [x3 .. x0],
// hi = [the hi words likewise], with two 16-byte loads' worth of 8-byte loads, two rev32 and an unzip.
//
// BLUR_GATHER_SCALAR (compile time): the portable form (four big-endian word loads per vector) on the Switch too.
// Checked against the portable form by the app's forms self-check on the device (banjo_forms_selfcheck.cpp).
#pragma once

#include <cstdint>
#include <cstring>

#include <simde/x86/sse2.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace blur_gather {

// Both words of the pair at ea are whole lane-0 words of lvlx128, and their 8 bytes are contiguous on the host (no
// 4 GB wrap, not across 0xE0000000).
[[gnu::always_inline]] inline bool PairOk(uint32_t ea) {
  return (ea & 3u) == 0 && ea <= 0xFFFFFFF8u && ea != 0xDFFFFFFCu;
}

// The big-endian word at host address p.
[[gnu::always_inline]] inline uint32_t Be32(const uint8_t* p) {
  uint32_t w;
  std::memcpy(&w, p, 4);
  return __builtin_bswap32(w);
}
[[gnu::always_inline]] inline uint32_t Be32(const uint8_t* q, uint32_t o) { return Be32(q + o); }

// The portable form, from the host addresses p0..p3 of the four pairs: lo = set_epi32(w(p0), w(p1), w(p2), w(p3)),
// hi = the same 4 bytes on.
[[gnu::always_inline]] inline void Pairs4Ref(const uint8_t* p0, const uint8_t* p1, const uint8_t* p2,
                                             const uint8_t* p3, simde__m128& lo, simde__m128& hi) {
  lo = simde_mm_castsi128_ps(simde_mm_set_epi32(int(Be32(p0)), int(Be32(p1)), int(Be32(p2)), int(Be32(p3))));
  hi = simde_mm_castsi128_ps(
      simde_mm_set_epi32(int(Be32(p0 + 4)), int(Be32(p1 + 4)), int(Be32(p2 + 4)), int(Be32(p3 + 4))));
}

// Four pairs (each PairOk; p0..p3 their host addresses) as lo = [lo(p3) lo(p2) lo(p1) lo(p0)], hi = [hi(p3) .. hi(p0)]
// (lane 0 first).
[[gnu::always_inline]] inline void Pairs4(const uint8_t* p0, const uint8_t* p1, const uint8_t* p2, const uint8_t* p3,
                                          simde__m128& lo, simde__m128& hi) {
#if defined(__aarch64__) && !defined(BLUR_GATHER_SCALAR)
  // bytes: [lo(p3) hi(p3) lo(p2) hi(p2)] and [lo(p1) hi(p1) lo(p0) hi(p0)], each word big-endian
  const uint8x16_t x = vcombine_u8(vld1_u8(p3), vld1_u8(p2));
  const uint8x16_t y = vcombine_u8(vld1_u8(p1), vld1_u8(p0));
  const uint32x4_t xs = vreinterpretq_u32_u8(vrev32q_u8(x)), ys = vreinterpretq_u32_u8(vrev32q_u8(y));
  lo = simde__m128_from_neon_f32(vreinterpretq_f32_u32(vuzp1q_u32(xs, ys)));
  hi = simde__m128_from_neon_f32(vreinterpretq_f32_u32(vuzp2q_u32(xs, ys)));
#else
  Pairs4Ref(p0, p1, p2, p3, lo, hi);
#endif
}

}  // namespace blur_gather
