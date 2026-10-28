// sa2-nx (2026-10-28): hand-written sub_82146E48, a 3x4 matrix (12 floats at r4) written as a transposed 4x4 (at r3)
// with the last column 0, 0, 0, 1. Written against its localized copy (app/generated/hot/n_82146E48.h), which stays
// its reference and its fallback.
//
// Optimisations: the copy marks a store card at each of its 16 stores (a byte store each; GCC keeps them all, as any
// guest store may be one of the cards) and goes through the float registers; here the 12 words are read once, the 16
// written as words, and the cards of the 64 bytes marked once (sa2_hot_common.h Card: two byte stores).
//
// Audit (against n_82146E48.h):
//   r4 == 0: return, nothing written (the copy's first branch).
//   Stores: [r3 + 16*c + 4*r] = word r*4 + c of the source for r < 3, the constant 0 (ro 0x820EAA40) for r = 3 and
//   c < 3, 0x3F800000 (ro 0x820EAA4C) at [r3+60]; each word passed through rex::ppc::QuietSingleBits (the copy's lfs /
//   stfs pair), as the copy does.
//   The copy loads and stores in an interleaved order, so with overlapping ranges ([r4, r4+48) and [r3, r3+64)) its
//   result depends on that order: then the copy itself runs.
//   Registers: the copy writes back f3-f13 and r10 (r3, r4 unchanged; f0, r11 are locals): f12 = word 2, f11 = word 3,
//   f10 = word 4, f9 = word 5, f8 = word 6, f7 = word 7, f6 = word 8, f5 = word 9, f4 = word 10, f3 = word 11 (each
//   rex::ppc::SingleToDoublePure, as lfs); f13 = 1.0 (word 1 first, then the constant); r10 = 0x820EAA40 sign-extended
//   (-2112946176 + -21952). The guard compares every register (Cmp::kAll) and the 64 bytes.
#pragma once

#include "../sa2_hot_common.h"
#include "../../../generated/hot/n_82146E48.h"

namespace sa2::hot::n_mtx34_to_44 {

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t src = ctx.r4.u32, dst = ctx.r3.u32;
  if (src == 0) return;
  // Overlap (with the 32-bit wrap the copy's addresses have): the copy's order decides; run it.
  const uint64_t s = src, d = dst;
  if (d < s + 48 && s < d + 64) {
    ::sa2::hot::n_82146E48::Native(ctx, base);
    return;
  }
  uint32_t m[12];
  for (int i = 0; i < 12; ++i) m[i] = Ld32(base, src + 4 * i);
  uint32_t out[16];
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 3; ++r) out[4 * c + r] = rex::ppc::QuietSingleBits(m[4 * r + c]);
    out[4 * c + 3] = rex::ppc::QuietSingleBits(c == 3 ? 0x3F800000u : 0u);
  }
  uint8_t* p = Raw(base, dst);
  for (int i = 0; i < 16; ++i) {
    const uint32_t v = __builtin_bswap32(out[i]);
    std::memcpy(p + 4 * i, &v, 4);
  }
  Card(base, dst, 64);
  ctx.f12.f64 = rex::ppc::SingleToDoublePure(m[2]);
  ctx.f11.f64 = rex::ppc::SingleToDoublePure(m[3]);
  ctx.f10.f64 = rex::ppc::SingleToDoublePure(m[4]);
  ctx.f9.f64 = rex::ppc::SingleToDoublePure(m[5]);
  ctx.f8.f64 = rex::ppc::SingleToDoublePure(m[6]);
  ctx.f7.f64 = rex::ppc::SingleToDoublePure(m[7]);
  ctx.f6.f64 = rex::ppc::SingleToDoublePure(m[8]);
  ctx.f5.f64 = rex::ppc::SingleToDoublePure(m[9]);
  ctx.f4.f64 = rex::ppc::SingleToDoublePure(m[10]);
  ctx.f3.f64 = rex::ppc::SingleToDoublePure(m[11]);
  ctx.f13.f64 = rex::ppc::SingleToDoublePure(0x3F800000u);
  ctx.r10.s64 = int64_t(-2112946176) + -21952;
}

}  // namespace sa2::hot::n_mtx34_to_44
