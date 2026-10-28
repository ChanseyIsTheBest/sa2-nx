// sa2-nx (2026-10-28): hand-written sub_8214F120: a model's triangle strips into the vertex buffer of the descriptor
// at 0x8389F0F8 (r31), 16 bytes a vertex (x, y, z and the colour of the 32-byte vertex its index names), from an
// index stream of stride r28: the position-and-colour sibling of sub_8214E3F0 and sub_8214F028 (hot/n_strip_builder.h,
// hot/n_strided_strips.h). Made from its localized copy (app/generated/hot/n_8214F120.h), which stays its reference:
// re-run tools/hand_natives/make_colour_strips.py after the copy changes (it checks the loop it replaces).
//
// Optimisation: the copy reloads the buffer base [r31+0] twice and the cursor [r31+20] twice a vertex, stores the
// cursor back twice and marks a card at its six stores; the fast loop holds the base and the cursor, stores the cursor
// once and marks the vertices' cards once.
//
// Audit: it applies when the count n != 0, the cursor (loaded above) does not wrap, and the vertices [dst, dst + 16n)
// neither wrap nor cross the hosts' physical-range offset nor overlap the descriptor [r31, r31+24): then the copy's
// reloads read what the fast loop holds and its intermediate cursor stores are overwritten by the last. Every other
// statement is the copy's, in its order (vertex and stream loads, the four stores). The registers it leaves (r5-r10,
// f12, f13, r11, r30, f0) are the copy's last vertex's; cr0 / xer are its dead locals; the mode is set once (nothing
// in the loop changes it). The cards are the pages of the stores' first bytes (four bytes apart over the range).
// Otherwise the copy's loop runs.
//
// Unguarded like its copy (it calls sub_82139B48 / sub_82139C90); in verify mode (sa2_cpu_verify) the fast loop
// checks itself against the copy's (hot/fast_loop_check.h).
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

namespace sa2::hot::n_colour_strips {

inline void Native(PPCContext& ctx, uint8_t* base) {
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
#define SA2_SYNC_OUT() do { ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	REX_LOOP_LOCAL(PPCRegister, ll_f12, l_f12);
	REX_LOOP_LOCAL(PPCRegister, ll_f13, l_f13);
	REX_LOOP_LOCAL(PPCRegister, ll_r5, l_r5);
	REX_LOOP_LOCAL(PPCRegister, ll_r6, l_r6);
	REX_LOOP_LOCAL(PPCRegister, ll_r7, l_r7);
	REX_LOOP_LOCAL(PPCRegister, ll_r8, l_r8);
	REX_LOOP_LOCAL(PPCRegister, ll_r9, l_r9);
	REX_LOOP_LOCAL(PPCRegister, ll_r10, l_r10);
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f0{};
	PPCRegister temp{};
	PPCRegister fb0{};
	PPCRegister fb12{};
	uint32_t ea{};
	::sa2::hot::fast_loop::Check* check = nullptr;  // verify mode only
	r12.u64 = ctx.lr;
	ea = -144 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	r11.s64 = -2088435712;
	r30.u64 = l_r3.u64;
	l_r10.s64 = r11.s64 + 13312;
	r29.u64 = l_r4.u64;
	r26.u64 = l_r3.u64;
	r31.u64 = ((l_r5.u32 >> 14) & 0x3u);
	r27.u64 = l_r5.u32 & 0x3FFF;
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 640);
	REX_STORE_D_U32_C(l_r10.u32, 628, r11.u32);
	{ do { ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); sub_8213A428(ctx, base); SA2_SYNC_IN(); }
	cr6.compare<int32_t>(l_r3.s32, 0, xer);
	if (!cr6.eq) goto loc_8214F204;
	r11.s64 = r31.s64 + 1;
	r28.u64 = ((uint32_t)(r11.u32 << 1));
	r11.s64 = -2088108032;
	r31.s64 = r11.s64 + -3848;
loc_8214F16C:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
	r30.s64 = r30.s64 + 2;
	l_r5.s64 = r11.s16;
	cr6.compare<int32_t>(l_r5.s32, 0, xer);
	if (!cr6.lt) goto loc_8214F184;
	l_r5.s64 = static_cast<int64_t>(-l_r5.u64);
loc_8214F184:
	l_r3.u64 = l_r5.u32 & 0xFFFF;
	{ ctx.r3 = l_r3; sub_82139B48(ctx, base); l_r3 = ctx.r3; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 20);
	REX_LOOP_IN(ll_f12, l_f12);
	REX_LOOP_IN(ll_f13, l_f13);
	REX_LOOP_IN(ll_r5, l_r5);
	REX_LOOP_IN(ll_r6, l_r6);
	REX_LOOP_IN(ll_r7, l_r7);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
	{
		// sa2-nx (2026-10-28): sub_8214F120 fast vertex loop where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ll_r5.u32;                        // the strip's vertices (the loop's counter)
		const uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);     // the vertex buffer
		const uint64_t cur0 = ll_r10.u64;                    // its cursor [r31+20], loaded above
		const uint64_t dst = uint64_t(buf) + cur0, bytes = 16ull * n;
		if (n != 0 && cur0 + bytes < 0x100000000ull && ::sa2::hot::fast_loop::Flat(dst, bytes) && ::sa2::hot::fast_loop::Apart(dst, bytes, r31.u32, 24)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check = new ::sa2::hot::fast_loop::Check{};
				check->Add(uint32_t(dst), uint32_t(bytes));
				check->Add(r31.u32 + 20, 4);
				::sa2::hot::fast_loop::Begin(*check, base);
				check->in[0] = ll_r5.u64;
				check->in[1] = ll_r6.u64;
				check->in[2] = ll_r7.u64;
				check->in[3] = ll_r8.u64;
				check->in[4] = ll_r9.u64;
				check->in[5] = ll_r10.u64;
				check->in[6] = ll_f12.u64;
				check->in[7] = ll_f13.u64;
				check->in[8] = r11.u64;
				check->in[9] = r30.u64;
				check->in[10] = f0.u64;
				check->in[11] = fb0.u64;
				check->in[12] = fb12.u64;
				check->in[13] = uint64_t(ea);
				check->in[14] = temp.u64;
			}
			uint64_t cur = cur0;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
				ll_r5.s64 = ll_r5.s64 + -1;
				ll_r8.u64 = buf;
				r30.u64 = r28.u64 + r30.u64;
				ll_r7.s64 = r11.s16;
				ll_r9.u64 = ll_r10.u64 + ll_r8.u64;
				r11.u64 = ((uint32_t)(ll_r7.u32 << 5));
				r11.u64 = r11.u64 + r29.u64;
				fb0.u32 = REX_LOAD_D_U32(r11.u32, 8);
				ll_f13.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
				fb12.u32 = REX_LOAD_D_U32(r11.u32, 0);
				ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
				temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
				REX_STORE_U32(ll_r10.u32 + ll_r8.u32, temp.u32);
				ea = 4 + ll_r9.u32;
				temp.f32 = float(ll_f13.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r9.u32 = ea;
				temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
				REX_STORE_D_U32(ll_r9.u32, 4, temp.u32);
				ll_r10.u64 = cur + 12;  // the copy's [r31+20] + 12, stored back
				ll_r6.u64 = REX_LOAD_D_U32(r11.u32, 24);
				ll_r9.u64 = buf;
				REX_STORE_U32(ll_r10.u32 + ll_r9.u32, ll_r6.u32);
				r11.u64 = cur + 12;  // the copy's reload of [r31+20]
				cur += 16;
				ll_r10.u64 = cur;  // r11 + 4, stored back
			} while (ll_r5.s32 != 0);
			f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);  // the copy's f0 of the last vertex
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check) [[likely]] goto loc_8214F190_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check->out[0] = ll_r5.u64;
			check->out[1] = ll_r6.u64;
			check->out[2] = ll_r7.u64;
			check->out[3] = ll_r8.u64;
			check->out[4] = ll_r9.u64;
			check->out[5] = ll_r10.u64;
			check->out[6] = ll_f12.u64;
			check->out[7] = ll_f13.u64;
			check->out[8] = r11.u64;
			check->out[9] = r30.u64;
			check->out[10] = f0.u64;
			check->out[11] = fb0.u64;
			check->out[12] = fb12.u64;
			check->out[13] = uint64_t(ea);
			check->out[14] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check, base);
			ll_r5.u64 = check->in[0];
			ll_r6.u64 = check->in[1];
			ll_r7.u64 = check->in[2];
			ll_r8.u64 = check->in[3];
			ll_r9.u64 = check->in[4];
			ll_r10.u64 = check->in[5];
			ll_f12.u64 = check->in[6];
			ll_f13.u64 = check->in[7];
			r11.u64 = check->in[8];
			r30.u64 = check->in[9];
			f0.u64 = check->in[10];
			fb0.u64 = check->in[11];
			fb12.u64 = check->in[12];
			ea = uint32_t(check->in[13]);
			temp.u64 = check->in[14];
		}
	}
loc_8214F190:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
	xer.ca = ll_r5.u32 > 0;
	ll_r5.s64 = ll_r5.s64 + -1;
	cr0.compare<int32_t>(ll_r5.s32, 0, xer);
	ll_r8.u64 = REX_LOAD_D_U32(r31.u32, 0);
	r30.u64 = r28.u64 + r30.u64;
	ll_r7.s64 = r11.s16;
	ll_r9.u64 = ll_r10.u64 + ll_r8.u64;
	r11.u64 = ((uint32_t)(ll_r7.u32 << 5));
	r11.u64 = r11.u64 + r29.u64;
	fb0.u32 = REX_LOAD_D_U32(r11.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	ctx.fpscr.disableFlushMode();
	ll_f13.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
	fb12.u32 = REX_LOAD_D_U32(r11.u32, 0);
	ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_U32_C(ll_r10.u32 + ll_r8.u32, temp.u32);
	ea = 4 + ll_r9.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r9.u32 = ea;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r9.u32, 4, temp.u32);
	ll_r10.u64 = REX_LOAD_D_U32(r31.u32, 20);
	ll_r10.s64 = ll_r10.s64 + 12;
	REX_STORE_D_U32_C(r31.u32, 20, ll_r10.u32);
	ll_r6.u64 = REX_LOAD_D_U32(r11.u32, 24);
	ll_r9.u64 = REX_LOAD_D_U32(r31.u32, 0);
	REX_STORE_U32_C(ll_r10.u32 + ll_r9.u32, ll_r6.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	ll_r10.s64 = r11.s64 + 4;
	REX_STORE_D_U32_C(r31.u32, 20, ll_r10.u32);
	if (!cr0.eq) goto loc_8214F190;
	if (check) [[unlikely]] {
		uint64_t want_[11];
		want_[0] = ll_r5.u64;
		want_[1] = ll_r6.u64;
		want_[2] = ll_r7.u64;
		want_[3] = ll_r8.u64;
		want_[4] = ll_r9.u64;
		want_[5] = ll_r10.u64;
		want_[6] = ll_f12.u64;
		want_[7] = ll_f13.u64;
		want_[8] = r11.u64;
		want_[9] = r30.u64;
		want_[10] = f0.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check, base, want_, 11, "sub_8214F120 fast vertex loop", checks_);
	}
loc_8214F190_done:
	REX_LOOP_OUT(ll_f12, l_f12);
	REX_LOOP_OUT(ll_f13, l_f13);
	REX_LOOP_OUT(ll_r5, l_r5);
	REX_LOOP_OUT(ll_r6, l_r6);
	REX_LOOP_OUT(ll_r7, l_r7);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	{ ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82139C90(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	xer.ca = r27.u32 > 0;
	r27.s64 = r27.s64 + -1;
	cr0.compare<int32_t>(r27.s32, 0, xer);
	if (!cr0.eq) goto loc_8214F16C;
	l_r3.u64 = r26.u64;
	{ do { ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r3 = l_r3; } while (0); sub_8213A318(ctx, base); SA2_SYNC_IN(); }
loc_8214F204:
	l_r1.s64 = l_r1.s64 + 144;
	{ do { ctx.r1 = l_r1; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_colour_strips
