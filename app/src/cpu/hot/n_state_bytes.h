// sa2-nx (2026-10-28): hand-written sub_8214B168 (render-state handler 5): stores r3's four bytes into the render-state block and sets
// the colour from them. Made from its localized copy (app/generated/hot/n_8214B168.h, its reference) by
// tools/hand_natives/make_card_batches.py: the four byte stores' card marks as one mark of their word (r11 + 96, before
// anything that follows reads or calls). Unguarded as its copy (it calls).
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

REX_EXTERN(sub_8213CCF0);
REX_EXTERN(sub_8214D9F8);

namespace sa2::hot::n_state_bytes {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_f1 = ctx.f1;
  PPCRegister l_f2 = ctx.f2;
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
  PPCRegister l_r5 = ctx.r5;
  PPCRegister l_r6 = ctx.r6;
  PPCRegister l_r7 = ctx.r7;
  PPCRegister l_r8 = ctx.r8;
  PPCRegister l_r9 = ctx.r9;
  PPCRegister l_r10 = ctx.r10;
#define SA2_SYNC_OUT() do { ctx.f1 = l_f1; ctx.f2 = l_f2; ctx.f3 = l_f3; ctx.f4 = l_f4; ctx.f5 = l_f5; ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f1 = ctx.f1; l_f2 = ctx.f2; l_f3 = ctx.f3; l_f4 = ctx.f4; l_f5 = ctx.f5; l_f6 = ctx.f6; l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister f0{};
	PPCRegister temp{};
	uint32_t ea{};
	REX_LR_READ(r12);
	REX_DEAD_LR_SAVE(REX_STORE_D_U32(l_r1.u32, -8, r12.u32));
	ea = -128 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	l_r8.s64 = -2076246016;
	r11.s64 = -2088108032;
	l_r10.u64 = (l_r3.u32 >> 24);
	r11.s64 = r11.s64 + -2272;
	l_r9.u64 = ((l_r3.u32 >> 16) & 0xFFu);
	l_r6.u64 = REX_LOAD_D_U32(l_r8.u32, 14552);
	l_r8.u64 = ((l_r3.u32 >> 8) & 0xFFu);
	l_r7.u64 = l_r3.u32 & 0xFF;
	l_r5.u64 = (l_r6.u32 & 0x10u);
	REX_STORE_D_U8(r11.u32, 96, l_r10.u8);
	REX_STORE_D_U8(r11.u32, 97, l_r9.u8);
	cr6.compare<uint32_t>(l_r5.u32, 0, xer);
	REX_STORE_D_U8(r11.u32, 98, l_r8.u8);
	REX_STORE_D_U8(r11.u32, 99, l_r7.u8);
#if !defined(REX_STORE_CARDS_OFF)
	::sa2::hot::Card(base, r11.u32 + 96, 4);
#endif
	if (cr6.eq) goto loc_8214B1C8;
	r11.s64 = -2076246016;
	l_r4.s64 = l_r1.s64 + 80;
	l_r3.s64 = r11.s64 + 13776;
	{ ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_8214D9F8(ctx, base); l_f3 = ctx.f3; l_f4 = ctx.f4; l_f5 = ctx.f5; l_f6 = ctx.f6; l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	l_r4.u64 = REX_LOAD_D_U32(l_r1.u32, 80);
	goto loc_8214B294;
loc_8214B1C8:
	l_r6.u64 = (l_r6.u32 & 0x20u);
	cr6.compare<uint32_t>(l_r6.u32, 0, xer);
	if (cr6.eq) goto loc_8214B290;
	r11.u64 = l_r8.u32 & 0xFF;
	l_r6.u64 = l_r7.u32 & 0xFF;
	REX_STORE_D_U64(l_r1.u32, 80, r11.u64);
	l_r7.u64 = r11.u64;
	l_r5.u64 = l_r10.u32 & 0xFF;
	REX_STORE_D_U64(l_r1.u32, 96, l_r6.u64);
	r11.u64 = l_r9.u32 & 0xFF;
	l_f12.u64 = REX_LOAD_D_U64(l_r1.u32, 96);
	l_f13.u64 = REX_LOAD_D_U64(l_r1.u32, 80);
	REX_STORE_D_U64(l_r1.u32, 80, l_r5.u64);
	REX_STORE_D_U64(l_r1.u32, 96, r11.u64);
	l_f10.u64 = REX_LOAD_D_U64(l_r1.u32, 96);
	l_f11.u64 = REX_LOAD_D_U64(l_r1.u32, 80);
	l_f7.f64 = double(l_f12.s64);
	l_f6.f64 = double(l_f13.s64);
	l_r3.s64 = -2112946176;
	l_f9.f64 = double(l_f10.s64);
	l_r9.s64 = -2076246016;
	l_f8.f64 = double(l_f11.s64);
	l_r10.s64 = l_r3.s64 + -21952;
	ctx.fpscr.disableFlushMode();
	l_f3.f64 = double(float(l_f7.f64));
	l_r8.s64 = l_r9.s64 + 13776;
	l_r4.s64 = l_r1.s64 + 80;
	l_r3.s64 = l_r1.s64 + 96;
	l_f13.f64 = double(REX_LOAD_D_F32(l_r9.u32, 13776));
	temp.u32 = REX_RO_U32(0x820EABD0, 0x3B808081);
	f0.f64 = double(temp.f32);
	l_f12.f64 = double(REX_LOAD_D_F32(l_r8.u32, 4));
	l_f11.f64 = double(REX_LOAD_D_F32(l_r8.u32, 8));
	l_f2.f64 = double(float(l_f6.f64));
	l_f10.f64 = double(REX_LOAD_D_F32(l_r8.u32, 12));
	l_f5.f64 = double(float(l_f9.f64));
	l_f4.f64 = double(float(l_f8.f64));
	l_f8.f64 = REX_FMULS(l_f3.f64, f0.f64);
	l_f7.f64 = REX_FMULS(l_f2.f64, f0.f64);
	l_f1.f64 = REX_FMULS(l_f5.f64, f0.f64);
	l_f9.f64 = REX_FMULS(l_f4.f64, f0.f64);
	l_f4.f64 = REX_FADDS(l_f13.f64, l_f8.f64);
	temp.f32 = float(l_f4.f64);
	REX_STORE_D_U32(l_r1.u32, 96, temp.u32);
	l_f3.f64 = REX_FADDS(l_f10.f64, l_f7.f64);
	temp.f32 = float(l_f3.f64);
	REX_STORE_D_U32(l_r1.u32, 108, temp.u32);
	l_f6.f64 = REX_FADDS(l_f11.f64, l_f1.f64);
	temp.f32 = float(l_f6.f64);
	REX_STORE_D_U32(l_r1.u32, 104, temp.u32);
	l_f5.f64 = REX_FADDS(l_f12.f64, l_f9.f64);
	temp.f32 = float(l_f5.f64);
	REX_STORE_D_U32(l_r1.u32, 100, temp.u32);
	{ ctx.f3 = l_f3; ctx.f4 = l_f4; ctx.f5 = l_f5; ctx.f6 = l_f6; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_8214D9F8(ctx, base); l_f3 = ctx.f3; l_f4 = ctx.f4; l_f5 = ctx.f5; l_f6 = ctx.f6; l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	l_r4.u64 = REX_LOAD_D_U32(l_r1.u32, 80);
	goto loc_8214B294;
loc_8214B290:
	l_r4.u64 = REX_LOAD_D_U32(r11.u32, 96);
loc_8214B294:
	l_r3.s64 = 4;
	{ do { ctx.f1 = l_f1; ctx.f2 = l_f2; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); sub_8213CCF0(ctx, base); SA2_SYNC_IN(); }
	l_r1.s64 = l_r1.s64 + 128;
	r12.u64 = REX_LOAD_D_U32(l_r1.u32, -8);
	REX_LR_WRITE(r12);
	{ do { ctx.r1 = l_r1; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_state_bytes
