// sa2-nx (2026-10-28): hand-written sub_8214D6D8 (render-state handler 8's body): fills the seven words of a block at 0x8389F8A8 and calls
// sub_8213D1F8. Made from its localized copy (app/generated/hot/n_8214D6D8.h, its reference) by
// tools/hand_natives/make_card_batches.py: the block's stores' card marks (four or five on each path, then three) as
// one mark of [r3, +28) before the call; the store at +2080 of another block keeps its own. Unguarded as its copy.
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

REX_EXTERN(sub_8213D1F8);

namespace sa2::hot::n_state_block {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_r1 = ctx.r1;
  PPCRegister l_r3 = ctx.r3;
  PPCRegister l_r4 = ctx.r4;
  PPCRegister l_r5 = ctx.r5;
  PPCRegister l_r6 = ctx.r6;
  PPCRegister l_r7 = ctx.r7;
  PPCRegister l_r8 = ctx.r8;
  PPCRegister l_r9 = ctx.r9;
  PPCRegister l_r10 = ctx.r10;
#define SA2_SYNC_OUT() do { ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister r31{};
	PPCRegister temp{};
	uint32_t ea{};
	REX_LR_READ(r12);
	REX_DEAD_LR_SAVE(REX_STORE_D_U32(l_r1.u32, -8, r12.u32));
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -16, r31.u64));
	ea = -96 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	l_r10.s64 = -2075721728;
	r11.u64 = ((uint32_t)(l_r3.u32 << 1));
	l_r8.s64 = -2086928384;
	l_r7.u64 = l_r3.u64 + r11.u64;
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 15172);
	l_r10.u64 = ((uint32_t)(l_r7.u32 << 2));
	l_r3.u64 = REX_LOAD_D_U32(r11.u32, 32);
	r11.u64 = REX_LOAD_D_U32(l_r3.u32, 0);
	r11.u64 = r11.u64 + l_r10.u64;
	l_r9.u64 = REX_LOAD_D_U32(r11.u32, 8);
	r11.u64 = REX_LOAD_D_U32(l_r9.u32, 12);
	l_r10.s64 = r11.s64 + 4;
	REX_STORE_D_U32_C(l_r8.u32, 2080, l_r10.u32);
	l_r8.u64 = REX_LOAD_D_U32(r11.u32, 28);
	l_r7.u64 = (l_r8.u32 & 0x8000u);
	l_r10.u64 = REX_LOAD_D_U32(l_r10.u32, 28);
	r11.u64 = (l_r8.u32 >> 31);
	cr6.compare<uint32_t>(l_r7.u32, 0, xer);
	if (cr6.eq) goto loc_8214D764;
	l_r8.s64 = -2088108032;
	l_r9.u64 = REX_LOAD_D_U32(l_r9.u32, 8);
	xer.ca = r11.u32 > 0;
	r31.s64 = r11.s64 + -1;
	l_r3.s64 = l_r8.s64 + -1880;
	l_r8.s64 = 1;
	l_r7.s64 = 1;
	temp.u8 = (~r31.u32 + r11.u32 < ~r31.u32) | (~r31.u32 + r11.u32 + xer.ca < xer.ca);
	r11.u64 = ~r31.u64 + r11.u64 + xer.ca;
	xer.ca = temp.u8;
	REX_STORE_D_U32(l_r3.u32, 0, l_r10.u32);
	REX_STORE_D_U32(l_r3.u32, 12, l_r8.u32);
	REX_STORE_D_U32(l_r3.u32, 16, l_r7.u32);
	REX_STORE_D_U32(l_r3.u32, 24, l_r9.u32);
	goto loc_8214D790;
loc_8214D764:
	l_r9.s64 = -2088108032;
	xer.ca = r11.u32 > 0;
	l_r7.s64 = r11.s64 + -1;
	l_r3.s64 = l_r9.s64 + -1880;
	l_r9.s64 = 1;
	l_r8.s64 = 1;
	temp.u8 = (~l_r7.u32 + r11.u32 < ~l_r7.u32) | (~l_r7.u32 + r11.u32 + xer.ca < xer.ca);
	r11.u64 = ~l_r7.u64 + r11.u64 + xer.ca;
	xer.ca = temp.u8;
	REX_STORE_D_U32(l_r3.u32, 0, l_r10.u32);
	l_r10.s64 = -1;
	REX_STORE_D_U32(l_r3.u32, 12, l_r9.u32);
	REX_STORE_D_U32(l_r3.u32, 16, l_r8.u32);
	REX_STORE_D_U32(l_r3.u32, 24, l_r10.u32);
loc_8214D790:
	REX_STORE_D_U32(l_r3.u32, 20, r11.u32);
	r11.s64 = -2088435712;
	REX_STORE_D_U32(l_r3.u32, 4, l_r4.u32);
	l_r10.u64 = ((uint32_t)(l_r6.u32 << 2));
	REX_STORE_D_U32(l_r3.u32, 8, l_r5.u32);
	l_r9.s64 = r11.s64 + 1808;
	l_r4.u64 = REX_LOAD_U32(l_r10.u32 + l_r9.u32);
#if !defined(REX_STORE_CARDS_OFF)
	::sa2::hot::Card(base, l_r3.u32, 28);
#endif
	{ do { ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); sub_8213D1F8(ctx, base); SA2_SYNC_IN(); }
	l_r1.s64 = l_r1.s64 + 96;
	r12.u64 = REX_LOAD_D_U32(l_r1.u32, -8);
	REX_LR_WRITE(r12);
	r31.u64 = REX_LOAD_D_U64(l_r1.u32, -16);
	{ do { ctx.r1 = l_r1; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_state_block
