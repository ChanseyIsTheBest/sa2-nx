// sa2-nx (2026-10-28): hand-written sub_8214EF60: a model's triangle strips into the vertex buffer of the descriptor
// at 0x8389F0F8 (r29), each vertex the 24 bytes of the 32-byte vertex its index names (copied as memcpy: inline, or by
// the call where the copy calls), from an index stream of stride r27. Made from its localized copy
// (app/generated/hot/n_8214EF60.h), which stays its reference: re-run tools/hand_natives/make_copied_strips.py after the
// copy changes (it checks the loop it replaces).
//
// Optimisation: the copy reloads the buffer base [r29+0] and the cursor [r29+20] at every vertex and stores the cursor
// back with its card; the fast loop holds them and stores the cursor once.
//
// Audit: it applies when the count n != 0, the cursor (loaded just above) does not wrap, and the vertices [dst, dst +
// 24n) neither wrap nor cross the hosts' physical-range offset nor overlap the descriptor [r29, r29+24): then the
// copy's reloads read what the fast loop holds (its 24-byte copies write [dst..] and, inline, the dead slot [r1-8],
// as here), and its intermediate cursor stores are overwritten by the last. Every other statement is the copy's, in
// its order (the vertex copy with its conditions and its call). The registers it leaves are the copy's last vertex's;
// cr0 / xer are its dead locals. The cursor's card is marked as the copy's last. Otherwise the copy's loop runs.
//
// Unguarded like its copy (it calls sub_82139C90); in verify mode (sa2_cpu_verify) the fast loop checks itself
// against the copy's (hot/fast_loop_check.h).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include <rex/logging.h>

#include "../sa2_enhancements.h"
#include "../sa2_hot_common.h"
#include "fast_loop_check.h"

REX_EXTERN(sub_82139B48);
REX_EXTERN(sub_82139C90);
REX_EXTERN(sub_8213A318);
REX_EXTERN(sub_8213A428);
REX_EXTERN(sub_827F05C0);

namespace sa2::hot::n_copied_strips {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_r1 = ctx.r1;
  PPCRegister l_r3 = ctx.r3;
  PPCRegister l_r4 = ctx.r4;
  PPCRegister l_r5 = ctx.r5;
  PPCRegister l_r8 = ctx.r8;
  PPCRegister l_r9 = ctx.r9;
  PPCRegister l_r10 = ctx.r10;
#define SA2_SYNC_OUT() do { ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister r25{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	uint32_t ea{};
	::sa2::hot::fast_loop::Check* check = nullptr;  // verify mode only
	r12.u64 = ctx.lr;
	ea = -144 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	r11.s64 = -2088435712;
	r31.u64 = l_r3.u64;
	l_r10.s64 = r11.s64 + 13312;
	r28.u64 = l_r4.u64;
	r25.u64 = l_r3.u64;
	r30.u64 = ((l_r5.u32 >> 14) & 0x3u);
	r26.u64 = l_r5.u32 & 0x3FFF;
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 636);
	REX_STORE_D_U32_C(l_r10.u32, 628, r11.u32);
	{ do { ctx.r1 = l_r1; ctx.r10 = l_r10; } while (0); sub_8213A428(ctx, base); SA2_SYNC_IN(); }
	cr6.compare<int32_t>(l_r3.s32, 0, xer);
	if (!cr6.eq) goto loc_8214F01C;
	r11.s64 = r30.s64 + 1;
	r27.u64 = ((uint32_t)(r11.u32 << 1));
	r11.s64 = -2088108032;
	r29.s64 = r11.s64 + -3848;
loc_8214EFAC:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r31.u32, 0);
	r31.s64 = r31.s64 + 2;
	r30.s64 = r11.s16;
	cr6.compare<int32_t>(r30.s32, 0, xer);
	if (!cr6.lt) goto loc_8214EFC4;
	r30.s64 = static_cast<int64_t>(-r30.u64);
loc_8214EFC4:
	l_r3.u64 = r30.u32 & 0xFFFF;
	{ ctx.r3 = l_r3; sub_82139B48(ctx, base); l_r3 = ctx.r3; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	r11.u64 = REX_LOAD_D_U32(r29.u32, 20);
	{
		// sa2-nx (2026-10-28): sub_8214EF60 fast vertex loop where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = r30.u32;                          // the strip's vertices (the loop's counter)
		const uint32_t buf = REX_LOAD_D_U32(r29.u32, 0);     // the vertex buffer
		const uint64_t cur0 = r11.u64;                       // its cursor [r29+20], loaded just above
		const uint64_t dst = uint64_t(buf) + cur0, bytes = 24ull * n;
		if (n != 0 && cur0 + bytes < 0x100000000ull && ::sa2::hot::fast_loop::Flat(dst, bytes) && ::sa2::hot::fast_loop::Apart(dst, bytes, r29.u32, 24)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check = new ::sa2::hot::fast_loop::Check{};
				check->Add(uint32_t(dst), uint32_t(bytes));
				check->Add(r29.u32 + 20, 4);
				check->Add(uint32_t(l_r1.u32 - 8), 8);
				::sa2::hot::fast_loop::Begin(*check, base);
				check->in[0] = l_r1.u64;
				check->in[1] = l_r3.u64;
				check->in[2] = l_r4.u64;
				check->in[3] = l_r5.u64;
				check->in[4] = l_r8.u64;
				check->in[5] = l_r9.u64;
				check->in[6] = l_r10.u64;
				check->in[7] = r11.u64;
				check->in[8] = r30.u64;
				check->in[9] = r31.u64;
			}
			uint64_t cur = cur0;
			do {
				REX_LOOP_BARRIER();
				l_r9.u64 = REX_LOAD_D_U16(r31.u32, 0);
				l_r5.s64 = 24;
				l_r10.u64 = buf;
				l_r8.s64 = l_r9.s16;
				l_r3.u64 = r11.u64 + l_r10.u64;
				r11.u64 = ((uint32_t)(l_r8.u32 << 5));
				l_r4.u64 = r11.u64 + r28.u64;
				// The vertex's 24 bytes, as the copy copies them (inline, or by the call where it does not inline).
				{ ctx.r3 = l_r3; ctx.r4 = l_r4; if (!SA2_MEMCPY_INLINE(24)) { ctx.r5 = l_r5; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_827F05C0(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } }
				r30.s64 = r30.s64 + -1;
				cur += 24;
				r11.u64 = cur;  // the copy's [r29+20] + 24, stored back
				r31.u64 = r27.u64 + r31.u64;
			} while (r30.s32 != 0);
			REX_STORE_D_U32_C(r29.u32, 20, uint32_t(cur));  // the cursor, and its card, once
			if (!check) [[likely]] goto loc_8214EFD0_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check->out[0] = l_r1.u64;
			check->out[1] = l_r3.u64;
			check->out[2] = l_r4.u64;
			check->out[3] = l_r5.u64;
			check->out[4] = l_r8.u64;
			check->out[5] = l_r9.u64;
			check->out[6] = l_r10.u64;
			check->out[7] = r11.u64;
			check->out[8] = r30.u64;
			check->out[9] = r31.u64;
			::sa2::hot::fast_loop::Swap(*check, base);
			l_r1.u64 = check->in[0];
			l_r3.u64 = check->in[1];
			l_r4.u64 = check->in[2];
			l_r5.u64 = check->in[3];
			l_r8.u64 = check->in[4];
			l_r9.u64 = check->in[5];
			l_r10.u64 = check->in[6];
			r11.u64 = check->in[7];
			r30.u64 = check->in[8];
			r31.u64 = check->in[9];
		}
	}
loc_8214EFD0:
	REX_LOOP_BARRIER();
	l_r9.u64 = REX_LOAD_D_U16(r31.u32, 0);
	l_r5.s64 = 24;
	l_r10.u64 = REX_LOAD_D_U32(r29.u32, 0);
	l_r8.s64 = l_r9.s16;
	l_r3.u64 = r11.u64 + l_r10.u64;
	r11.u64 = ((uint32_t)(l_r8.u32 << 5));
	l_r4.u64 = r11.u64 + r28.u64;
	{ ctx.r3 = l_r3; ctx.r4 = l_r4; if (!SA2_MEMCPY_INLINE(24)) { ctx.r5 = l_r5; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_827F05C0(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } }
	r11.u64 = REX_LOAD_D_U32(r29.u32, 20);
	xer.ca = r30.u32 > 0;
	r30.s64 = r30.s64 + -1;
	cr0.compare<int32_t>(r30.s32, 0, xer);
	r11.s64 = r11.s64 + 24;
	r31.u64 = r27.u64 + r31.u64;
	REX_STORE_D_U32_C(r29.u32, 20, r11.u32);
	if (!cr0.eq) goto loc_8214EFD0;
	if (check) [[unlikely]] {
		uint64_t want_[10];
		want_[0] = l_r1.u64;
		want_[1] = l_r3.u64;
		want_[2] = l_r4.u64;
		want_[3] = l_r5.u64;
		want_[4] = l_r8.u64;
		want_[5] = l_r9.u64;
		want_[6] = l_r10.u64;
		want_[7] = r11.u64;
		want_[8] = r30.u64;
		want_[9] = r31.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check, base, want_, 10, "sub_8214EF60 fast vertex loop", checks_);
	}
loc_8214EFD0_done:
	{ ctx.r5 = l_r5; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82139C90(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	xer.ca = r26.u32 > 0;
	r26.s64 = r26.s64 + -1;
	cr0.compare<int32_t>(r26.s32, 0, xer);
	if (!cr0.eq) goto loc_8214EFAC;
	l_r3.u64 = r25.u64;
	{ do { ctx.r3 = l_r3; } while (0); sub_8213A318(ctx, base); SA2_SYNC_IN(); }
loc_8214F01C:
	l_r1.s64 = l_r1.s64 + 144;
	{ do { ctx.r1 = l_r1; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_copied_strips
