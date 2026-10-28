// sa2-nx (2026-10-28): hand-written sub_8214B3A0 (render-state handler 8, the game thread's hottest function in the 2P console profile):
// splits r3 into its four fields and branches to sub_8214D6D8. Made from its localized copy
// (app/generated/hot/n_8214B3A0.h, its reference) by tools/hand_natives/make_card_batches.py: the body runs as
// n_state_block (hot/n_state_block.h) inline, not through sub_8214D6D8's hook, which only dispatched to the copy it was
// made from. Unguarded as its copy.
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

#include "n_state_block.h"


namespace sa2::hot::n_state_handler8 {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_r3 = ctx.r3;
  PPCRegister l_r4 = ctx.r4;
  PPCRegister l_r5 = ctx.r5;
  PPCRegister l_r6 = ctx.r6;
#define SA2_SYNC_OUT() do { ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; } while (0)
#define SA2_SYNC_IN() do { l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; } while (0)
	REX_FUNC_PROLOGUE();
	l_r6.u64 = ((l_r3.u32 >> 13) & 0x3u);
	l_r5.u64 = ((l_r3.u32 >> 16) & 0x3u);
	l_r4.u64 = ((l_r3.u32 >> 18) & 0x3u);
	l_r3.u64 = l_r3.u32 & 0x1FFF;
	{ do { ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; } while (0); ::sa2::hot::n_state_block::Native(ctx, base); SA2_SYNC_IN(); }
	{ do {} while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_state_handler8
