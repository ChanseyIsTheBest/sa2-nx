// blur-nx (2026-11-12, CPU batch 3): the guest vector's byte order for lane-wise kernels.
//
// The generated code's vector registers hold a guest quadword with all 16 bytes reversed (REX_BSWAP128: lane 0 is the
// guest's word 3). GCC writes that reversal as one `tbl` with a table register. The console's blur_microbench (2026-11-12,
// the A57 at 1020 MHz): the mixer kernel (two loads, three swaps, one fmla a vector) took 714 ns a KB with the reversal
// and 470 ns with `rev32` (each word's bytes swapped, the lanes kept in the guest's order).
//
// The reversal is Rev32 then LaneRev (each word's bytes, then the order of the four words), so a kernel whose arithmetic
// is lane by lane (the same operation on every lane, nothing across lanes) gives the same bytes in memory with Rev32 on
// its loads and stores, and needs LaneRev only for a value it leaves in a register in the generated code's layout.
// Off the AArch64 both are SSE shuffles (the PC's guard checks the kernels that use them against their originals).
#pragma once

#include <simde/x86/sse2.h>
#include <simde/x86/ssse3.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace blur_swap {

// Each 32-bit word's four bytes reversed, the words where they are.
[[gnu::always_inline]] inline simde__m128i Rev32(simde__m128i v) {
#if defined(__aarch64__) && !defined(BLUR_SWAP_PORTABLE)
  return simde__m128i_from_neon_u8(vrev32q_u8(simde__m128i_to_neon_u8(v)));
#else
  return simde_mm_shuffle_epi8(v, simde_mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3));
#endif
}

// The four 32-bit lanes in reverse order.
[[gnu::always_inline]] inline simde__m128 LaneRev(simde__m128 v) {
#if defined(__aarch64__) && !defined(BLUR_SWAP_PORTABLE)
  const float32x4_t r = vrev64q_f32(simde__m128_to_neon_f32(v));
  return simde__m128_from_neon_f32(vextq_f32(r, r, 2));
#else
  return simde_mm_shuffle_ps(v, v, SIMDE_MM_SHUFFLE(0, 1, 2, 3));
#endif
}

// A guest quadword at host address p as a word-swapped vector in the guest's lane order, and back.
[[gnu::always_inline]] inline simde__m128 LoadWords(const uint8_t* p) {
  return simde_mm_castsi128_ps(Rev32(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(p))));
}
[[gnu::always_inline]] inline void StoreWords(uint8_t* p, simde__m128 v) {
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(p), Rev32(simde_mm_castps_si128(v)));
}

}  // namespace blur_swap
