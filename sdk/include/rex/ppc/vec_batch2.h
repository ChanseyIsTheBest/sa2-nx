// blur-nx (2026-11-11, CPU batch 2: docs/vector-ideas-2.md VX-15 and VX-14; the user: "do all the exact ones"): two
// or four vmsum3fp128 / vmsum4fp128 merged by vmrghw / vmrglw, as one pairwise-add tree (tools/vec_dp_merge.py).
//
// A dot product (simde_mm_dp_ps with a high nibble of products and a low nibble of 0xF, rex_dp_ps_ in the pch on
// AArch64) is p = a * b (masked), then two pairwise adds: every lane holds (p0 + p1) + (p2 + p3). The XDK merges two
// such broadcasts with vmrghw / vmrglw: [d1 d2 d1 d2] (lane 0 first), and two of those again into [d1 d3 d2 d4]. Here
// the products stay where the dot products were (DpProd) and the merge does the adds:
//   DpPair(P1, P2) = faddp(t, t), t = faddp(P1, P2)              -> [d1 d2 d1 d2]
//   DpQuad(Pw, Px, Py, Pz) = faddp(faddp(Pw, Px), faddp(Py, Pz))  -> [dw dx dy dz]
// Each sum is (p0 + p1) + (p2 + p3) of the same products in the same operand order as rex_dp_ps_'s, so the bits are
// the same (NaN choice included), in the same flush mode (the pass moves no add past a mode statement). 2 adds instead of
// 4 adds and a zip for a pair, 3 instead of 8 and 3 zips for a quad.
//
// Checked against the statements it replaces by the app's forms self-check on the device, and by the guard on the PC
// (the pass rewrites only the copies, so the guard's original statements keep the dot products and zips).
#pragma once

#include <simde/x86/sse4.1.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace rex::ppc {

#if defined(__aarch64__) && !defined(REX_DP_MERGE_OFF)
#define REX_DP_MERGE_NEON 1
template <int Imm>
[[gnu::always_inline]] inline simde__m128 DpProd(simde__m128 a, simde__m128 b) {
  static_assert((Imm & 0x0F) == 0x0F, "a broadcast dot product");
  float32x4_t p = vmulq_f32(simde__m128_to_neon_f32(a), simde__m128_to_neon_f32(b));
  if constexpr ((Imm & 0xF0) != 0xF0) {
    const uint32x4_t m = {(Imm & 0x10) ? ~0u : 0u, (Imm & 0x20) ? ~0u : 0u, (Imm & 0x40) ? ~0u : 0u,
                          (Imm & 0x80) ? ~0u : 0u};
    p = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(p), m));
  }
  return simde__m128_from_neon_f32(p);
}
[[gnu::always_inline]] inline simde__m128 DpPair(simde__m128 p1, simde__m128 p2) {
  const float32x4_t t = vpaddq_f32(simde__m128_to_neon_f32(p1), simde__m128_to_neon_f32(p2));
  return simde__m128_from_neon_f32(vpaddq_f32(t, t));
}
[[gnu::always_inline]] inline simde__m128 DpQuad(simde__m128 pw, simde__m128 px, simde__m128 py, simde__m128 pz) {
  const float32x4_t u = vpaddq_f32(simde__m128_to_neon_f32(pw), simde__m128_to_neon_f32(px));
  const float32x4_t v = vpaddq_f32(simde__m128_to_neon_f32(py), simde__m128_to_neon_f32(pz));
  return simde__m128_from_neon_f32(vpaddq_f32(u, v));
}
#else
// Elsewhere (the PC): the same products and the same adds in the same operand order, with SSE (x86's DPPS also sums
// (p0 + p1) + (p2 + p3), each lane's first operand the lower one), so the guard on the PC compares this form, products
// left in their registers included, with the original statements (tools/vec_dp_merge.py rewrites only the copies).
template <int Imm>
[[gnu::always_inline]] inline simde__m128 DpProd(simde__m128 a, simde__m128 b) {
  static_assert((Imm & 0x0F) == 0x0F, "a broadcast dot product");
  const simde__m128 p = simde_mm_mul_ps(a, b);
  if constexpr ((Imm & 0xF0) != 0xF0) {
    const simde__m128i m = simde_mm_set_epi32((Imm & 0x80) ? -1 : 0, (Imm & 0x40) ? -1 : 0, (Imm & 0x20) ? -1 : 0,
                                              (Imm & 0x10) ? -1 : 0);
    return simde_mm_and_ps(p, simde_mm_castsi128_ps(m));
  }
  return p;
}
// [x0 + x1, x2 + x3, y0 + y1, y2 + y3] (NEON's faddp), the lower lane the first operand of each add.
[[gnu::always_inline]] inline simde__m128 PairwiseAdd(simde__m128 x, simde__m128 y) {
  return simde_mm_add_ps(simde_mm_shuffle_ps(x, y, SIMDE_MM_SHUFFLE(2, 0, 2, 0)),
                         simde_mm_shuffle_ps(x, y, SIMDE_MM_SHUFFLE(3, 1, 3, 1)));
}
[[gnu::always_inline]] inline simde__m128 DpPair(simde__m128 p1, simde__m128 p2) {
  const simde__m128 t = PairwiseAdd(p1, p2);
  return PairwiseAdd(t, t);
}
[[gnu::always_inline]] inline simde__m128 DpQuad(simde__m128 pw, simde__m128 px, simde__m128 py, simde__m128 pz) {
  return PairwiseAdd(PairwiseAdd(pw, px), PairwiseAdd(py, pz));
}
#endif

// The statements the pass replaces, for the self-check: two dot products and their merge; four and the tree.
template <int Imm>
inline simde__m128 DpPairRef(simde__m128 a1, simde__m128 b1, simde__m128 a2, simde__m128 b2) {
  const simde__m128 d1 = simde_mm_dp_ps(a1, b1, Imm), d2 = simde_mm_dp_ps(a2, b2, Imm);
  return simde_mm_castsi128_ps(simde_mm_unpackhi_epi32(simde_mm_castps_si128(d1), simde_mm_castps_si128(d2)));
}
template <int Imm>
inline simde__m128 DpPairNew(simde__m128 a1, simde__m128 b1, simde__m128 a2, simde__m128 b2) {
  return DpPair(DpProd<Imm>(a1, b1), DpProd<Imm>(a2, b2));
}

}  // namespace rex::ppc
