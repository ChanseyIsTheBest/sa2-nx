// sa2-nx (2026-10-28): hand-written sub_8214F028: a model's triangle strips into the vertex buffer of the descriptor
// at 0x8389F0F8 (r31), positions only, 12 bytes a vertex, from an index stream of stride r28 (the strided sibling of
// sub_8214E2B0, hot/n_position_strips.h). Made from its localized copy (app/generated/hot/n_8214F028.h), which stays
// its reference: re-run tools/hand_natives/make_strided_strips.py after the copy changes (it checks the loop it
// replaces).
//
// Optimisation, audit: as hot/n_position_strips.h (the buffer base and the cursor held; the cursor stored once; the
// vertices' cards marked once). It applies when the count n != 0, the cursor r11 ([r31+20], loaded or just stored) is
// below 4 GB and does not wrap, and the vertices [dst, dst + 12n) neither wrap nor cross the hosts' physical-range
// offset nor overlap the descriptor [r31, r31+24). The registers the loop leaves (r7-r10, f12, f13, r30, r11, f0) are
// the copy's last vertex's; cr0 / xer are its dead locals. Otherwise the copy's loop runs.
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

REX_EXTERN(sub_82139C90);

namespace sa2::hot::n_strided_strips {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_f12 = ctx.f12;
  PPCRegister l_f13 = ctx.f13;
  PPCRegister l_r1 = ctx.r1;
  PPCRegister l_r3 = ctx.r3;
  PPCRegister l_r4 = ctx.r4;
  PPCRegister l_r5 = ctx.r5;
  PPCRegister l_r7 = ctx.r7;
  PPCRegister l_r8 = ctx.r8;
  PPCRegister l_r9 = ctx.r9;
  PPCRegister l_r10 = ctx.r10;
#define SA2_SYNC_OUT() do { ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	REX_LOOP_LOCAL(PPCRegister, ll_f12, l_f12);
	REX_LOOP_LOCAL(PPCRegister, ll_f13, l_f13);
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
	r11.u64 = ((l_r5.u32 >> 14) & 0x3u);
	l_r10.s64 = -2088108032;
	r11.s64 = r11.s64 + 1;
	r30.u64 = l_r3.u64;
	r28.u64 = ((uint32_t)(r11.u32 << 1));
	r11.s64 = -2088435712;
	r29.u64 = l_r4.u64;
	r26.u64 = l_r5.u32 & 0x3FFF;
	r27.s64 = r11.s64 + 13312;
	r31.s64 = l_r10.s64 + -3848;
loc_8214F05C:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
	r30.s64 = r30.s64 + 2;
	l_r7.s64 = r11.s16;
	cr6.compare<int32_t>(l_r7.s32, 0, xer);
	if (!cr6.lt) goto loc_8214F074;
	l_r7.s64 = static_cast<int64_t>(-l_r7.u64);
loc_8214F074:
	l_r8.u64 = REX_LOAD_D_U32(r31.u32, 12);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	cr6.compare<uint32_t>(l_r8.u32, 0, xer);
	if (cr6.eq) goto loc_8214F09C;
	l_r10.u64 = REX_LOAD_D_U32(r27.u32, 628);
	REX_STORE_D_U32_C(r31.u32, 24, r11.u32);
	l_r10.u64 = REX_LOAD_D_U32(l_r10.u32, 28);
	r11.u64 = l_r10.u64 + r11.u64;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	goto loc_8214F0B8;
loc_8214F09C:
	l_r10.s64 = 152;
	REX_STORE_D_U32_C(r31.u32, 16, r11.u32);
	l_r9.s64 = 0;
	REX_STORE_D_U32_C(r31.u32, 4, l_r10.u32);
	l_r10.s64 = 0;
	REX_STORE_D_U32_C(r31.u32, 8, l_r9.u32);
	REX_STORE_D_U32_C(r31.u32, 24, l_r10.u32);
loc_8214F0B8:
	l_r10.u64 = l_r7.u32 & 0xFFFF;
	l_r10.u64 = l_r10.u64 + l_r8.u64;
	REX_STORE_D_U32_C(r31.u32, 12, l_r10.u32);
	REX_LOOP_IN(ll_f12, l_f12);
	REX_LOOP_IN(ll_f13, l_f13);
	REX_LOOP_IN(ll_r7, l_r7);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
	{
		// sa2-nx (2026-10-28): sub_8214F028 fast vertex loop where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ll_r7.u32;                        // the strip's vertices (the loop's counter)
		const uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);     // the vertex buffer
		const uint64_t cur0 = r11.u64;                       // its cursor, [r31+20] when below 4 GB
		const uint64_t dst = uint64_t(buf) + cur0, bytes = 12ull * n;
		if (n != 0 && cur0 + bytes < 0x100000000ull && ::sa2::hot::fast_loop::Flat(dst, bytes) && ::sa2::hot::fast_loop::Apart(dst, bytes, r31.u32, 24)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check = new ::sa2::hot::fast_loop::Check{};
				check->Add(uint32_t(dst), uint32_t(bytes));
				check->Add(r31.u32 + 20, 4);
				::sa2::hot::fast_loop::Begin(*check, base);
				check->in[0] = ll_r7.u64;
				check->in[1] = ll_r8.u64;
				check->in[2] = ll_r9.u64;
				check->in[3] = ll_r10.u64;
				check->in[4] = ll_f12.u64;
				check->in[5] = ll_f13.u64;
				check->in[6] = r30.u64;
				check->in[7] = r11.u64;
				check->in[8] = f0.u64;
				check->in[9] = fb0.u64;
				check->in[10] = fb12.u64;
				check->in[11] = uint64_t(ea);
				check->in[12] = temp.u64;
			}
			uint64_t cur = cur0;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				ll_r10.u64 = REX_LOAD_D_U16(r30.u32, 0);
				ll_r7.s64 = ll_r7.s64 + -1;
				ll_r8.u64 = buf;
				r30.u64 = r28.u64 + r30.u64;
				ll_r9.s64 = ll_r10.s16;
				ll_r10.u64 = ll_r8.u64 + r11.u64;
				ll_r9.u64 = ((uint32_t)(ll_r9.u32 << 5));
				ll_r9.u64 = ll_r9.u64 + r29.u64;
				fb0.u32 = REX_LOAD_D_U32(ll_r9.u32, 8);
				ll_f13.f64 = double(REX_LOAD_D_F32(ll_r9.u32, 4));
				fb12.u32 = REX_LOAD_D_U32(ll_r9.u32, 0);
				ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
				temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
				REX_STORE_U32(ll_r8.u32 + r11.u32, temp.u32);
				ea = 4 + ll_r10.u32;
				temp.f32 = float(ll_f13.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r10.u32 = ea;
				temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
				REX_STORE_D_U32(ll_r10.u32, 4, temp.u32);
				cur += 12;
				r11.u64 = cur;  // the copy's [r31+20] + 12, stored back
			} while (ll_r7.s32 != 0);
			f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);  // the copy's f0 of the last vertex
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check) [[likely]] goto loc_8214F0C4_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check->out[0] = ll_r7.u64;
			check->out[1] = ll_r8.u64;
			check->out[2] = ll_r9.u64;
			check->out[3] = ll_r10.u64;
			check->out[4] = ll_f12.u64;
			check->out[5] = ll_f13.u64;
			check->out[6] = r30.u64;
			check->out[7] = r11.u64;
			check->out[8] = f0.u64;
			check->out[9] = fb0.u64;
			check->out[10] = fb12.u64;
			check->out[11] = uint64_t(ea);
			check->out[12] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check, base);
			ll_r7.u64 = check->in[0];
			ll_r8.u64 = check->in[1];
			ll_r9.u64 = check->in[2];
			ll_r10.u64 = check->in[3];
			ll_f12.u64 = check->in[4];
			ll_f13.u64 = check->in[5];
			r30.u64 = check->in[6];
			r11.u64 = check->in[7];
			f0.u64 = check->in[8];
			fb0.u64 = check->in[9];
			fb12.u64 = check->in[10];
			ea = uint32_t(check->in[11]);
			temp.u64 = check->in[12];
		}
	}
loc_8214F0C4:
	REX_LOOP_BARRIER();
	ll_r10.u64 = REX_LOAD_D_U16(r30.u32, 0);
	xer.ca = ll_r7.u32 > 0;
	ll_r7.s64 = ll_r7.s64 + -1;
	cr0.compare<int32_t>(ll_r7.s32, 0, xer);
	ll_r8.u64 = REX_LOAD_D_U32(r31.u32, 0);
	r30.u64 = r28.u64 + r30.u64;
	ll_r9.s64 = ll_r10.s16;
	ll_r10.u64 = ll_r8.u64 + r11.u64;
	ll_r9.u64 = ((uint32_t)(ll_r9.u32 << 5));
	ll_r9.u64 = ll_r9.u64 + r29.u64;
	fb0.u32 = REX_LOAD_D_U32(ll_r9.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	ctx.fpscr.disableFlushMode();
	ll_f13.f64 = double(REX_LOAD_D_F32(ll_r9.u32, 4));
	fb12.u32 = REX_LOAD_D_U32(ll_r9.u32, 0);
	ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_U32_C(ll_r8.u32 + r11.u32, temp.u32);
	ea = 4 + ll_r10.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r10.u32 = ea;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r10.u32, 4, temp.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	r11.s64 = r11.s64 + 12;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	if (!cr0.eq) goto loc_8214F0C4;
	if (check) [[unlikely]] {
		uint64_t want_[9];
		want_[0] = ll_r7.u64;
		want_[1] = ll_r8.u64;
		want_[2] = ll_r9.u64;
		want_[3] = ll_r10.u64;
		want_[4] = ll_f12.u64;
		want_[5] = ll_f13.u64;
		want_[6] = r30.u64;
		want_[7] = r11.u64;
		want_[8] = f0.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check, base, want_, 9, "sub_8214F028 fast vertex loop", checks_);
	}
loc_8214F0C4_done:
	REX_LOOP_OUT(ll_f12, l_f12);
	REX_LOOP_OUT(ll_f13, l_f13);
	REX_LOOP_OUT(ll_r7, l_r7);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	{ ctx.r1 = l_r1; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82139C90(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	xer.ca = r26.u32 > 0;
	r26.s64 = r26.s64 + -1;
	cr0.compare<int32_t>(r26.s32, 0, xer);
	if (!cr0.eq) goto loc_8214F05C;
	l_r1.s64 = l_r1.s64 + 144;
	{ do { ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_strided_strips
