// sa2-nx (2026-10-28): hand-written sub_8214B320 (render-state handler 0): fills one 20-byte record of a table at 0x83853480 from three
// lookups indexed by r3. Made from its localized copy (app/generated/hot/n_8214B320.h, its reference) by
// tools/hand_natives/make_card_batches.py: the five stores' card marks as one mark of the record (r5 = 0x83853480,
// the record at r10 + r5, 17 bytes from its first store to its byte). Guarded as its copy was.
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


namespace sa2::hot::n_record_store {

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
	PPCRegister r11{};
	PPCRegister r30{};
	PPCRegister r31{};
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -16, r30.u64));
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -8, r31.u64));
	r11.s64 = -2088435712;
	l_r8.u64 = ((l_r3.u32 >> 6) & 0x3Cu);
	l_r10.s64 = r11.s64 + 2060;
	l_r9.u64 = ((l_r3.u32 >> 14) & 0x3FCu);
	l_r7.s64 = l_r10.s64 + -164;
	l_r6.s64 = l_r10.s64 + -36;
	l_r4.u64 = ((l_r3.u32 >> 10) & 0x3Cu);
	r31.u64 = REX_LOAD_U32(l_r8.u32 + l_r10.u32);
	l_r5.s64 = -2088435712;
	l_r8.u64 = REX_LOAD_U32(l_r9.u32 + l_r7.u32);
	r11.s64 = l_r5.s64 + 13312;
	l_r7.s64 = 0;
	l_r9.s64 = l_r8.s64 + -9;
	l_r4.u64 = REX_LOAD_U32(l_r4.u32 + l_r6.u32);
	l_r5.s64 = r11.s64 + 128;
	l_r10.u64 = ((uint32_t)(l_r9.u32 << 2));
	l_r6.s64 = r11.s64 + 132;
	l_r10.u64 = l_r9.u64 + l_r10.u64;
	r30.s64 = r11.s64 + 136;
	l_r9.s64 = r11.s64 + 140;
	l_r10.u64 = ((uint32_t)(l_r10.u32 << 2));
	r11.s64 = r11.s64 + 144;
	REX_STORE_U32(l_r10.u32 + l_r6.u32, l_r8.u32);
	REX_STORE_U32(l_r10.u32 + l_r5.u32, l_r7.u32);
	REX_STORE_U32(l_r10.u32 + r30.u32, l_r4.u32);
	REX_STORE_U32(l_r10.u32 + l_r9.u32, r31.u32);
	REX_STORE_U8(l_r10.u32 + r11.u32, l_r3.u8);
#if !defined(REX_STORE_CARDS_OFF)
	::sa2::hot::Card(base, l_r10.u32 + l_r5.u32, 17);
#endif
	r30.u64 = REX_LOAD_D_U64(l_r1.u32, -16);
	r31.u64 = REX_LOAD_D_U64(l_r1.u32, -8);
	{ do { ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_record_store
