// sa2-nx (2026-10-28): hand-written sub_82832298 (2.0 % of the game thread in console gameplay, round-6 renut
// profile): 16 records of four floats at r3 (a decoded block's texels, RGBA), each with its fourth (alpha) under 1.0
// rescaled: alpha 0.0 sets the first three to 0.0; alpha in (0, 1) scales each of the first three by 1/alpha when it
// is under alpha, or sets it to 1.0. Written against its localized copy (app/generated/hot/n_82832298.h), which stays
// its reference.
//
// Optimisations: the copy branches on each component's compare (taken or not by the data: a mispredict on the A57)
// and marks a card at each of a record's three stores. Here a component is a select between its product (computed
// either way: the recompiled code keeps no floating-point status flags, mffs reads the rounding mode only, so a
// multiply has no effect besides its result) and 1.0, and a stored record marks the pages of its three stores once.
//
// Audit (against n_82832298.h): the same constants (0.0 and 1.0 through REX_RO_U32), the same loads in the same
// order, the same FDIVS / FMULS and conversions; f13 = the record's alpha at every record, f11 / f12 as the last record
// that took the rescale path left them (the copy's f12 is its second component's result when it stores the third
// from f0); r11 / ctr / f0 / cr6 are its locals; r3 = 0, r9 = r10 = 0x82000000 (sign-extended) as the copy writes them
// back. A record's stores ([r-12], [r-8], [r-4]) mark the pages of [r-12, r-4]: the pages of their first bytes. The
// guard compares every register (kAllNaN) and the 256 bytes.
#pragma once

#include "../sa2_hot_common.h"

namespace sa2::hot::n_alpha_rescale {

inline void Native(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.disableFlushMode();
  PPCRegister temp{};
  temp.u32 = REX_RO_U32(0x82001630, 0x00000000);
  const double zero = double(temp.f32);  // the copy's f9
  temp.u32 = REX_RO_U32(0x820018B8, 0x3F800000);
  const double one = double(temp.f32);  // the copy's f10
  double f11 = ctx.f11.f64, f12 = ctx.f12.f64, f13 = ctx.f13.f64;
  uint32_t rec = ctx.r3.u32 + 12;  // the copy's r11: the record's alpha
  for (int k = 0; k < 16; ++k, rec += 16) {
    f13 = double(REX_LOAD_D_F32(rec, 0));
    if (f13 == zero) {  // cr6.eq of fcmpu
      temp.f32 = float(zero);
      REX_STORE_D_U32(rec, -12, temp.u32);
      REX_STORE_D_U32(rec, -8, temp.u32);
      REX_STORE_D_U32(rec, -4, temp.u32);
    } else if (f13 < one) {  // cr6.lt
      const double c0 = double(REX_LOAD_D_F32(rec, -12));
      f11 = REX_FDIVS(one, f13);
      const double p0 = REX_FMULS(c0, f11);
      f12 = c0 < f13 ? p0 : one;
      const double c1 = double(REX_LOAD_D_F32(rec, -8));
      temp.f32 = float(f12);
      REX_STORE_D_U32(rec, -12, temp.u32);
      const double p1 = REX_FMULS(c1, f11);
      f12 = c1 < f13 ? p1 : one;
      const double c2 = double(REX_LOAD_D_F32(rec, -4));
      temp.f32 = float(f12);
      REX_STORE_D_U32(rec, -8, temp.u32);
      const double p2 = REX_FMULS(c2, f11);
      temp.f32 = float(c2 < f13 ? p2 : one);
      REX_STORE_D_U32(rec, -4, temp.u32);
    } else {
      continue;
    }
#if !defined(REX_STORE_CARDS_OFF)
    Card(base, rec - 12, 9);
#endif
  }
  ctx.f9.f64 = zero;
  ctx.f10.f64 = one;
  ctx.f11.f64 = f11;
  ctx.f12.f64 = f12;
  ctx.f13.f64 = f13;
  ctx.r3.s64 = 0;
  ctx.r9.s64 = -2113929216;
  ctx.r10.s64 = -2113929216;
}

}  // namespace sa2::hot::n_alpha_rescale
