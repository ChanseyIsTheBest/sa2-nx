// sa2-nx (2026-10-28): hand-written sub_82147340: transposes the 3x3 part of a 3x4 matrix (r3 into r4, or r4 in place) and zeroes its
// translation. Made from its localized copy (app/generated/hot/n_82147340.h, its reference) by
// tools/hand_natives/make_card_batches.py: the stores' card marks (nine or twelve) as one mark of [r4, +48) before its return
// (the r4 = 0 return stores nothing). Guarded.
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


namespace sa2::hot::n_matrix_transpose {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_f6 = ctx.f6;
  PPCRegister l_f7 = ctx.f7;
  PPCRegister l_f8 = ctx.f8;
  PPCRegister l_f9 = ctx.f9;
  PPCRegister l_f10 = ctx.f10;
  PPCRegister l_f11 = ctx.f11;
  PPCRegister l_f12 = ctx.f12;
  PPCRegister l_f13 = ctx.f13;
  PPCRegister l_r3 = ctx.r3;
  PPCRegister l_r4 = ctx.r4;
#define SA2_SYNC_OUT() do { ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r3 = l_r3; ctx.r4 = l_r4; } while (0)
#define SA2_SYNC_IN() do { l_f6 = ctx.f6; l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r3 = ctx.r3; l_r4 = ctx.r4; } while (0)
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister f0{};
	PPCRegister temp{};
	PPCRegister fb0{};
	PPCRegister fb6{};
	PPCRegister fb7{};
	PPCRegister fb8{};
	PPCRegister fb9{};
	PPCRegister fb10{};
	PPCRegister fb11{};
	PPCRegister fb12{};
	PPCRegister fb13{};
	cr6.compare<uint32_t>(l_r3.u32, l_r4.u32, xer);
	if (!cr6.eq) goto loc_82147384;
	cr6.compare<uint32_t>(l_r4.u32, 0, xer);
	if (cr6.eq) { do {} while (0); return; }
	fb13.u32 = REX_LOAD_D_U32(l_r4.u32, 4);
	l_f13.f64 = rex::ppc::SingleToDoublePure(fb13.u32);
	fb12.u32 = REX_LOAD_D_U32(l_r4.u32, 16);
	l_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	fb11.u32 = REX_LOAD_D_U32(l_r4.u32, 8);
	l_f11.f64 = rex::ppc::SingleToDoublePure(fb11.u32);
	fb10.u32 = REX_LOAD_D_U32(l_r4.u32, 32);
	l_f10.f64 = rex::ppc::SingleToDoublePure(fb10.u32);
	fb9.u32 = REX_LOAD_D_U32(l_r4.u32, 24);
	l_f9.f64 = rex::ppc::SingleToDoublePure(fb9.u32);
	fb8.u32 = REX_LOAD_D_U32(l_r4.u32, 36);
	l_f8.f64 = rex::ppc::SingleToDoublePure(fb8.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb13.u32);
	REX_STORE_D_U32(l_r4.u32, 16, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb11.u32);
	REX_STORE_D_U32(l_r4.u32, 32, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_D_U32(l_r4.u32, 4, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb10.u32);
	REX_STORE_D_U32(l_r4.u32, 8, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb8.u32);
	REX_STORE_D_U32(l_r4.u32, 24, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb9.u32);
	REX_STORE_D_U32(l_r4.u32, 36, temp.u32);
	goto loc_821473CC;
loc_82147384:
	fb0.u32 = REX_LOAD_D_U32(l_r3.u32, 0);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r4.u32, 0, temp.u32);
	fb13.u32 = REX_LOAD_D_U32(l_r3.u32, 4);
	l_f13.f64 = rex::ppc::SingleToDoublePure(fb13.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb13.u32);
	REX_STORE_D_U32(l_r4.u32, 16, temp.u32);
	fb12.u32 = REX_LOAD_D_U32(l_r3.u32, 8);
	l_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_D_U32(l_r4.u32, 32, temp.u32);
	fb11.u32 = REX_LOAD_D_U32(l_r3.u32, 16);
	l_f11.f64 = rex::ppc::SingleToDoublePure(fb11.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb11.u32);
	REX_STORE_D_U32(l_r4.u32, 4, temp.u32);
	fb10.u32 = REX_LOAD_D_U32(l_r3.u32, 20);
	l_f10.f64 = rex::ppc::SingleToDoublePure(fb10.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb10.u32);
	REX_STORE_D_U32(l_r4.u32, 20, temp.u32);
	fb9.u32 = REX_LOAD_D_U32(l_r3.u32, 24);
	l_f9.f64 = rex::ppc::SingleToDoublePure(fb9.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb9.u32);
	REX_STORE_D_U32(l_r4.u32, 36, temp.u32);
	fb8.u32 = REX_LOAD_D_U32(l_r3.u32, 32);
	l_f8.f64 = rex::ppc::SingleToDoublePure(fb8.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb8.u32);
	REX_STORE_D_U32(l_r4.u32, 8, temp.u32);
	fb7.u32 = REX_LOAD_D_U32(l_r3.u32, 36);
	l_f7.f64 = rex::ppc::SingleToDoublePure(fb7.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb7.u32);
	REX_STORE_D_U32(l_r4.u32, 24, temp.u32);
	fb6.u32 = REX_LOAD_D_U32(l_r3.u32, 40);
	l_f6.f64 = rex::ppc::SingleToDoublePure(fb6.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb6.u32);
	REX_STORE_D_U32(l_r4.u32, 40, temp.u32);
loc_821473CC:
	r11.s64 = -2112946176;
	fb0.u32 = REX_RO_CFG_U32(0x820EAA40, 0x0u, REX_LOAD_D_U32(r11.u32, -21952));
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r4.u32, 44, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r4.u32, 28, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r4.u32, 12, temp.u32);
#if !defined(REX_STORE_CARDS_OFF)
	::sa2::hot::Card(base, l_r4.u32, 48);
#endif
	{ do { ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_matrix_transpose
