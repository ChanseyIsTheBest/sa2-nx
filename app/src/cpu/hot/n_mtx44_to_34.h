// sa2-nx (2026-10-28): hand-written sub_82146DD8, a 4x4 matrix (at r3) written transposed as a 3x4 (12 floats at r4),
// the inverse of sub_82146E48 (n_mtx34_to_44.h). Written against its localized copy (app/generated/hot/n_82146DD8.h),
// which stays its reference and its fallback.
//
// Optimisations: as n_mtx34_to_44.h: the 12 words read once and written as words, the cards of the 48 bytes marked
// once instead of a card store at each of the 12 stores.
//
// Audit (against n_82146DD8.h):
//   r4 == 0: return, nothing written.
//   Stores: [r4 + 4*k] = the source word at r3 + 16*(k % 4) + 4*(k / 4), k = 0..11, each through
//   rex::ppc::QuietSingleBits (the copy's lfs / stfs pair). Overlapping ranges ([r3, r3+60) read, [r4, r4+48) written):
//   the copy's interleaved order decides, so the copy runs.
//   Registers: the copy writes back f3-f13 (r3, r4 unchanged; f0 is a local): f13 = word at +16, f12 = +32, f11 = +48,
//   f10 = +4, f9 = +20, f8 = +36, f7 = +52, f6 = +8, f5 = +24, f4 = +40, f3 = +56 (rex::ppc::SingleToDoublePure, as
//   lfs). The guard compares every register (Cmp::kAll) and the 48 bytes.
#pragma once

#include "../sa2_hot_common.h"
#include "../../../generated/hot/n_82146DD8.h"

namespace sa2::hot::n_mtx44_to_34 {

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t src = ctx.r3.u32, dst = ctx.r4.u32;
  if (dst == 0) return;
  const uint64_t s = src, d = dst;
  if (d < s + 60 && s < d + 48) {
    ::sa2::hot::n_82146DD8::Native(ctx, base);
    return;
  }
  uint32_t w[12];
  for (int k = 0; k < 12; ++k) w[k] = Ld32(base, src + 16 * (k % 4) + 4 * (k / 4));
  uint8_t* p = Raw(base, dst);
  for (int k = 0; k < 12; ++k) {
    const uint32_t v = __builtin_bswap32(rex::ppc::QuietSingleBits(w[k]));
    std::memcpy(p + 4 * k, &v, 4);
  }
  Card(base, dst, 48);
  // w[k] holds the word at src + 16*(k % 4) + 4*(k / 4): +16 is k=1, +32 k=2, +48 k=3, +4 k=4, +20 k=5, +36 k=6,
  // +52 k=7, +8 k=8, +24 k=9, +40 k=10, +56 k=11.
  ctx.f13.f64 = rex::ppc::SingleToDoublePure(w[1]);
  ctx.f12.f64 = rex::ppc::SingleToDoublePure(w[2]);
  ctx.f11.f64 = rex::ppc::SingleToDoublePure(w[3]);
  ctx.f10.f64 = rex::ppc::SingleToDoublePure(w[4]);
  ctx.f9.f64 = rex::ppc::SingleToDoublePure(w[5]);
  ctx.f8.f64 = rex::ppc::SingleToDoublePure(w[6]);
  ctx.f7.f64 = rex::ppc::SingleToDoublePure(w[7]);
  ctx.f6.f64 = rex::ppc::SingleToDoublePure(w[8]);
  ctx.f5.f64 = rex::ppc::SingleToDoublePure(w[9]);
  ctx.f4.f64 = rex::ppc::SingleToDoublePure(w[10]);
  ctx.f3.f64 = rex::ppc::SingleToDoublePure(w[11]);
}

}  // namespace sa2::hot::n_mtx44_to_34
