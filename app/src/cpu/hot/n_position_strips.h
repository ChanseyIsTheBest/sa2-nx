// sa2-nx (2026-10-28): hand-written sub_8214E2B0 (1.3 % of the console session's guest instructions): a model's
// triangle strips into the vertex buffer of the descriptor at 0x8389F0F8 (r31), positions only, 12 bytes a vertex (x,
// y, z of the 32-byte vertex its index names); it opens each strip itself (sub_82139B48's code, inline) and closes it
// with sub_82139C90. The position-only sibling of sub_8214E3F0 (hot/n_strip_builder.h). Made from its localized copy
// (app/generated/hot/n_8214E2B0.h), which stays its reference: re-run tools/hand_natives/make_position_strips.py after
// the copy changes (it checks the loop it replaces statement by statement).
//
// Optimisation: the copy's inner loop reloads the buffer base [r31+0] and the cursor [r31+20] and stores the cursor
// back at every vertex, and marks a card at each of its four stores: the guest memory writes keep GCC from holding
// anything in a register across them. The fast loop holds the base and the cursor in registers, stores the cursor once
// at the end, and marks the vertices' cards once.
//
// Audit: it applies when the count n != 0, the cursor r11 (as the copy holds it at the loop: [r31+20], loaded or just
// stored) is below 4 GB and does not wrap in the loop, and the vertices [dst, dst + 12n) (dst = base + cursor) neither
// wrap at 4 GB nor overlap the descriptor [r31, r31+24): then the copy's reloads read what the fast loop holds and its
// intermediate cursor stores are overwritten by the last. Every other statement is the copy's, in its order, with its
// loads and stores of the vertex data and the strip stream in the same order (so a buffer overlapping them reads what
// the copy reads). The registers the loop leaves (r7-r10, f12, f13, r30, r11) are the copy's: the same statements of
// the last vertex; its cr0 / xer / f0 / ea are locals written before any read after the loop. The copy's
// disableFlushMode at every vertex runs once (nothing in the loop changes the mode). The cards (one per store: the page
// of its first byte) are the same pages, zeroed after the data, as the copy zeroes each after its store. Otherwise
// (n == 0 or a range above) the copy's loop runs.
//
// The function calls sub_82139C90, so the hook's guard cannot run both versions; in verify mode (sa2_cpu_verify) the
// fast loop checks itself instead: every strip it takes is run by both loops on the same inputs (the written bytes and
// registers saved and put back), the copy's result kept, and any difference counted in the [verify] line and logged
// as "[hot] DIFFERENCE".
#pragma once

#include <algorithm>
#include <atomic>
#include <vector>

#include <rex/logging.h>

#include "../sa2_enhancements.h"
#include "../sa2_hot_common.h"

REX_EXTERN(sub_82139C90);

namespace sa2::hot::n_position_strips {

// Verify mode: the guest bytes the fast loop writes (the vertices, the cursor word) and the registers either loop
// writes, saved to run the copy's loop on the same inputs and compare.
struct StripCheck {
  uint32_t at[2], len[2];
  std::vector<uint8_t> before[2], fast[2];
  uint64_t in[10], out[10];
};
inline constexpr int kStripRegs = 8;  // compared: the first 8 (f0 and ea are the copy's dead locals after it)
// (Named one by one: an array of their addresses would make them address-taken, kept in memory in the fast loop too.)
#define SA2_STRIP_REGS(a, op)                                                                                  \
  do {                                                                                                         \
    (a)[0] op ll_r7.u64; (a)[1] op ll_r8.u64; (a)[2] op ll_r9.u64; (a)[3] op ll_r10.u64; (a)[4] op ll_f12.u64; \
    (a)[5] op ll_f13.u64; (a)[6] op r30.u64; (a)[7] op r11.u64; (a)[8] op f0.u64; (a)[9] op uint64_t(ea);      \
  } while (0)
#define SA2_STRIP_REGS_BACK(a)                                                                                 \
  do {                                                                                                         \
    ll_r7.u64 = (a)[0]; ll_r8.u64 = (a)[1]; ll_r9.u64 = (a)[2]; ll_r10.u64 = (a)[3]; ll_f12.u64 = (a)[4];      \
    ll_f13.u64 = (a)[5]; r30.u64 = (a)[6]; r11.u64 = (a)[7]; f0.u64 = (a)[8]; ea = uint32_t((a)[9]);           \
  } while (0)
inline void Save(const uint8_t* base, uint32_t a, uint32_t len, std::vector<uint8_t>& out) {
  out.resize(len);
  for (uint32_t k = 0; k < len; ++k) out[k] = *Raw(base, a + k);
}
inline void Restore(uint8_t* base, uint32_t a, const std::vector<uint8_t>& in) {
  for (uint32_t k = 0; k < in.size(); ++k) *Raw(base, a + k) = in[k];
}


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
	StripCheck* strip_check = nullptr;  // verify mode only
	r12.u64 = ctx.lr;
	ea = -128 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	r11.s64 = -2088435712;
	l_r10.s64 = -2088108032;
	r29.u64 = l_r4.u64;
	r27.u64 = l_r5.u32 & 0x3FFF;
	r30.s64 = l_r3.s64 + -6;
	r28.s64 = r11.s64 + 13312;
	r31.s64 = l_r10.s64 + -3848;
loc_8214E2D8:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r30.u32, 6);
	r30.s64 = r30.s64 + 2;
	l_r7.s64 = r11.s16;
	cr6.compare<int32_t>(l_r7.s32, 0, xer);
	if (!cr6.lt) goto loc_8214E2F0;
	l_r7.s64 = static_cast<int64_t>(-l_r7.u64);
loc_8214E2F0:
	l_r8.u64 = REX_LOAD_D_U32(r31.u32, 12);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	cr6.compare<uint32_t>(l_r8.u32, 0, xer);
	if (cr6.eq) goto loc_8214E318;
	l_r10.u64 = REX_LOAD_D_U32(r28.u32, 628);
	REX_STORE_D_U32_C(r31.u32, 24, r11.u32);
	l_r10.u64 = REX_LOAD_D_U32(l_r10.u32, 28);
	r11.u64 = l_r10.u64 + r11.u64;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	goto loc_8214E334;
loc_8214E318:
	l_r10.s64 = 152;
	REX_STORE_D_U32_C(r31.u32, 16, r11.u32);
	l_r9.s64 = 0;
	REX_STORE_D_U32_C(r31.u32, 4, l_r10.u32);
	l_r10.s64 = 0;
	REX_STORE_D_U32_C(r31.u32, 8, l_r9.u32);
	REX_STORE_D_U32_C(r31.u32, 24, l_r10.u32);
loc_8214E334:
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
		// sa2-nx (2026-10-28): the strip's vertices by the fast loop where it applies (the header's conditions), else
		// by the copy's loop below.
		const uint32_t n = ll_r7.u32;                          // the vertex count (the loop's counter)
		const uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);       // the vertex buffer
		const uint64_t cur0 = r11.u64;                         // its cursor, [r31+20] when below 4 GB
		const uint64_t dst = uint64_t(buf) + cur0, bytes = 12ull * n;
		const uint64_t desc = r31.u32;
		if (n != 0 && cur0 + bytes < 0x100000000ull && dst + bytes <= 0x100000000ull &&
		    (dst + bytes <= desc || desc + 24 <= dst)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				strip_check = new StripCheck{{uint32_t(dst), r31.u32 + 20}, {uint32_t(bytes), 4}};
				for (int k = 0; k < 2; ++k) Save(base, strip_check->at[k], strip_check->len[k], strip_check->before[k]);
				SA2_STRIP_REGS(strip_check->in, =);
			}
			uint64_t cur = cur0;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				ea = 6 + r30.u32;
				ll_r10.u64 = REX_LOAD_U16(ea);
				r30.u32 = ea;
				ll_r7.s64 = ll_r7.s64 + -1;
				ll_r8.u64 = buf;
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
			// The cursor and the cards: the pages of every store's first byte (the stores tile [dst, dst + bytes) four
			// bytes apart), as the copy's stores mark.
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst), uint32_t(bytes - 3));
#endif
			if (!strip_check) [[likely]] goto loc_8214E340_done;
			// Verify mode: keep the fast loop's result, put the inputs back, and run the copy's loop to compare.
			SA2_STRIP_REGS(strip_check->out, =);
			for (int k = 0; k < 2; ++k) {
				Save(base, strip_check->at[k], strip_check->len[k], strip_check->fast[k]);
				Restore(base, strip_check->at[k], strip_check->before[k]);
			}
			SA2_STRIP_REGS_BACK(strip_check->in);
		}
	}
loc_8214E340:
	REX_LOOP_BARRIER();
	ea = 6 + r30.u32;
	ll_r10.u64 = REX_LOAD_U16(ea);
	r30.u32 = ea;
	xer.ca = ll_r7.u32 > 0;
	ll_r7.s64 = ll_r7.s64 + -1;
	cr0.compare<int32_t>(ll_r7.s32, 0, xer);
	ll_r8.u64 = REX_LOAD_D_U32(r31.u32, 0);
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
	if (!cr0.eq) goto loc_8214E340;
	if (strip_check) [[unlikely]] {
		uint64_t want[kStripRegs];
		SA2_STRIP_REGS(want, =);
		bool same = std::equal(want, want + kStripRegs, strip_check->out);
		std::vector<uint8_t> now;
		for (int k = 0; k < 2; ++k) {
			Save(base, strip_check->at[k], strip_check->len[k], now);
			same = same && now == strip_check->fast[k];
		}
		::sa2::enh::VerifyCheck();
		static std::atomic<uint32_t> checks_{0};
		const uint32_t count_ = checks_.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count_ == 1 || count_ == 4096 || count_ == 262144)
			REXLOG_INFO("[hot] sub_8214E2B0 fast vertex loop: {} checks against the copy's loop", count_);
		if (!same) {
			::sa2::enh::VerifyDifference();
			REXLOG_ERROR("[hot] DIFFERENCE: sub_8214E2B0's fast vertex loop ({} vertices to {:08X}) differs from the copy's "
			             "loop; the copy's result stands", strip_check->len[0] / 12, strip_check->at[0]);
		}
		delete strip_check;
		strip_check = nullptr;
	}
loc_8214E340_done:
	REX_LOOP_OUT(ll_f12, l_f12);
	REX_LOOP_OUT(ll_f13, l_f13);
	REX_LOOP_OUT(ll_r7, l_r7);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	{ ctx.r1 = l_r1; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82139C90(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	xer.ca = r27.u32 > 0;
	r27.s64 = r27.s64 + -1;
	cr0.compare<int32_t>(r27.s32, 0, xer);
	if (!cr0.eq) goto loc_8214E2D8;
	l_r1.s64 = l_r1.s64 + 128;
	{ do { ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

#undef SA2_STRIP_REGS
#undef SA2_STRIP_REGS_BACK

}  // namespace sa2::hot::n_position_strips
