// sa2-nx (2026-10-28): hand-written sub_8214D9F8: clamps the four floats at r3 to [0, 1] (stored back) and packs them as bytes at r4.
// Made from its localized copy (app/generated/hot/n_8214D9F8.h, its reference) by
// tools/hand_natives/make_card_batches.py: the four float stores' marks as one of [r3, +16) and the four byte stores'
// as one of [r4, +4), before its return. Guarded.
//
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include <rex/logging.h>

#include "../sa2_enhancements.h"
#include "../sa2_hot_common.h"
#include "fast_loop_check.h"


namespace sa2::hot::n_colour_pack {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_f3 = ctx.f3;
  PPCRegister l_f4 = ctx.f4;
  PPCRegister l_f5 = ctx.f5;
  PPCRegister l_f6 = ctx.f6;
  PPCRegister l_f7 = ctx.f7;
  PPCRegister l_f8 = ctx.f8;
  PPCRegister l_f9 = ctx.f9;
  PPCRegister l_f10 = ctx.f10;
  PPCRegister l_f11 = ctx.f11;
  PPCRegister l_f12 = ctx.f12;
  PPCRegister l_f13 = ctx.f13;
  PPCRegister l_r1 = ctx.r1;
  PPCRegister l_r3 = ctx.r3;
  PPCRegister l_r4 = ctx.r4;
  PPCRegister l_r8 = ctx.r8;
  PPCRegister l_r9 = ctx.r9;
  PPCRegister l_r10 = ctx.r10;
#define SA2_SYNC_OUT() do { ctx.f3 = l_f3; ctx.f4 = l_f4; ctx.f5 = l_f5; ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f3 = ctx.f3; l_f4 = ctx.f4; l_f5 = ctx.f5; l_f6 = ctx.f6; l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister f0{};
	PPCRegister temp{};
	r11.s64 = -2112946176;
	ctx.fpscr.disableFlushMode();
	f0.f64 = double(REX_LOAD_D_F32(l_r3.u32, 0));
	r11.s64 = r11.s64 + -21952;
	temp.u32 = REX_RO_U32(0x820EAA4C, 0x3F800000);
	l_f13.f64 = double(temp.f32);
	temp.u32 = REX_RO_U32(0x820EAA40, 0x00000000);
	l_f10.f64 = double(temp.f32);
	cr6.compare(f0.f64, l_f13.f64);
	if (!cr6.gt) goto loc_8214DA1C;
	l_f9.f64 = l_f13.f64;
	goto loc_8214DA20;
loc_8214DA1C:
	l_f9.f64 = rex::ppc::GeZeroAnyMode(f0.f64) ? f0.f64 : l_f10.f64;
loc_8214DA20:
	REX_FLUSH_KNOWN_OFF();
	f0.f64 = double(REX_LOAD_D_F32(l_r3.u32, 4));
	temp.f32 = float(l_f9.f64);
	REX_STORE_D_U32(l_r3.u32, 0, temp.u32);
	cr6.compare(f0.f64, l_f13.f64);
	if (!cr6.gt) goto loc_8214DA38;
	l_f11.f64 = l_f13.f64;
	goto loc_8214DA3C;
loc_8214DA38:
	l_f11.f64 = rex::ppc::GeZeroAnyMode(f0.f64) ? f0.f64 : l_f10.f64;
loc_8214DA3C:
	REX_FLUSH_KNOWN_OFF();
	f0.f64 = double(REX_LOAD_D_F32(l_r3.u32, 8));
	temp.f32 = float(l_f11.f64);
	REX_STORE_D_U32(l_r3.u32, 4, temp.u32);
	cr6.compare(f0.f64, l_f13.f64);
	if (!cr6.gt) goto loc_8214DA54;
	l_f12.f64 = l_f13.f64;
	goto loc_8214DA58;
loc_8214DA54:
	l_f12.f64 = rex::ppc::GeZeroAnyMode(f0.f64) ? f0.f64 : l_f10.f64;
loc_8214DA58:
	REX_FLUSH_KNOWN_OFF();
	f0.f64 = double(REX_LOAD_D_F32(l_r3.u32, 12));
	temp.f32 = float(l_f12.f64);
	REX_STORE_D_U32(l_r3.u32, 8, temp.u32);
	cr6.compare(f0.f64, l_f13.f64);
	if (cr6.gt) goto loc_8214DA6C;
	l_f13.f64 = rex::ppc::GeZeroAnyMode(f0.f64) ? f0.f64 : l_f10.f64;
loc_8214DA6C:
	REX_FLUSH_KNOWN_OFF();
	f0.f64 = double(REX_RO_CFG_F32(0x820EAC14, 0x437F0000u, REX_LOAD_D_F32(r11.u32, 468)));
	l_f9.f64 = REX_FMULS(l_f9.f64, f0.f64);
	temp.f32 = float(l_f13.f64);
	REX_STORE_D_U32(l_r3.u32, 12, temp.u32);
	l_f10.f64 = REX_FMULS(l_f13.f64, f0.f64);
	l_f8.f64 = REX_FMULS(l_f11.f64, f0.f64);
	l_f7.f64 = REX_FMULS(l_f12.f64, f0.f64);
	l_f5.s64 = rex::ppc::fctidz(l_f9.f64);
	REX_STORE_D_U64(l_r1.u32, -24, l_f5.u64);
	l_r10.u64 = REX_LOAD_D_U8(l_r1.u32, -17);
	l_f6.s64 = rex::ppc::fctidz(l_f10.f64);
	l_f4.s64 = rex::ppc::fctidz(l_f8.f64);
	REX_STORE_D_U64(l_r1.u32, -32, l_f6.u64);
	l_f3.s64 = rex::ppc::fctidz(l_f7.f64);
	REX_STORE_D_U64(l_r1.u32, -16, l_f4.u64);
	REX_STORE_D_U64(l_r1.u32, -24, l_f3.u64);
	r11.u64 = REX_LOAD_D_U8(l_r1.u32, -25);
	l_r9.u64 = REX_LOAD_D_U8(l_r1.u32, -9);
	l_r8.u64 = REX_LOAD_D_U8(l_r1.u32, -17);
	REX_STORE_D_U8(l_r4.u32, 3, l_r10.u8);
	REX_STORE_D_U8(l_r4.u32, 2, r11.u8);
	REX_STORE_D_U8(l_r4.u32, 0, l_r9.u8);
	REX_STORE_D_U8(l_r4.u32, 1, l_r8.u8);
#if !defined(REX_STORE_CARDS_OFF)
	::sa2::hot::Card(base, l_r3.u32, 16);
#endif
#if !defined(REX_STORE_CARDS_OFF)
	::sa2::hot::Card(base, l_r4.u32, 4);
#endif
	{ do { ctx.f3 = l_f3; ctx.f4 = l_f4; ctx.f5 = l_f5; ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_colour_pack
