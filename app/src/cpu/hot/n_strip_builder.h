// sa2-nx (2026-10-28): hand-written sub_8214E3F0, the hottest game-thread function of the console session (6.1 %
// of its guest instructions): a model's triangle strips into the vertex buffer of the descriptor at 0x8389F0F8 (r31,
// the descriptor sub_82139B48 opens a strip in), 24 bytes a vertex: the position (x, y, z) of the 32-byte vertex its
// index names, its colour, and its two texture coordinates (s16, >> 4 when r6 != 0, times 1/255). Made from its
// localized copy (app/generated/hot/n_8214E3F0.h, with its limited write-backs), which stays its reference: re-run 
// tools/hand_natives/make_strip_builder.py after the copy changes (it checks the statements the fast loop replaces).
//
// Optimisation: the copy's inner loop reloads the buffer base [r31+0] three times and the cursor [r31+20] three times a
// vertex, stores the cursor back three times, marks a card at each of its 9 stores, and moves the two texture
// coordinates to double through two frame slots (a store and a load each): the guest memory writes keep GCC from
// holding anything in a register across them. The fast loop holds the base and the cursor in registers, stores the
// cursor once at the end, converts the coordinates directly, writes the frame slots of the last vertex once, and
// marks the vertices' cards once.
//
// Audit: it applies when the count n != 0, the vertices [dst, dst + 24n) (dst = base + cursor) neither wrap at 4 GB
// nor overlap the descriptor [r31, r31+24) or the frame slots [r1+80, r1+112), and the cursor does not wrap: then the
// copy's reloads read what the fast loop holds (nothing it stores lands on them), its cursor arithmetic in 64 bits
// equals the 32-bit one, and its intermediate stores of the cursor and the slots are overwritten by the last. Every
// other statement is the copy's, in its order, with its loads and stores of the vertex data and the strip stream in
// the same order (so a buffer overlapping them reads what the copy reads). The registers the loop leaves (r3-r10,
// f7-f13, r30) are the copy's: the same statements of the last vertex; its cr0 / cr6 / xer / f0 / r11 / ea are locals
// written before any read after the loop. The fast loop runs the copy's REX_FLUSH_KNOWN_OFF once (nothing in the loop
// changes the mode). The cards (one per store: the page of its first byte) are the same pages, zeroed after the data,
// as the copy zeroes each after its store. Otherwise (n == 0 or a range above) the copy's loop runs.
//
// The function calls others (sub_8213A428, sub_82139B48, sub_82139C90, sub_8213A318), so the hook's guard cannot run
// both versions; in verify mode (sa2_cpu_verify) the fast loop checks itself instead: every strip it takes is run by
// both loops on the same inputs (the written bytes and registers saved and put back), the copy's result kept, and any
// difference counted in the [verify] line and logged as "[hot] DIFFERENCE".
#pragma once

#include <algorithm>
#include <atomic>
#include <vector>

#include <rex/logging.h>

#include "../sa2_enhancements.h"
#include "../sa2_hot_common.h"

REX_EXTERN(sub_82139B48);
REX_EXTERN(sub_82139C90);
REX_EXTERN(sub_8213A318);
REX_EXTERN(sub_8213A428);

namespace sa2::hot::n_strip_builder {

// Verify mode: the guest bytes the fast loop writes (the vertices, the cursor word, the frame's two slots) and the
// registers either loop writes, saved to run the copy's loop on the same inputs and compare.
struct StripCheck {
  uint32_t at[3], len[3];
  std::vector<uint8_t> before[3], fast[3];
  uint64_t in[19], out[19];
};
inline constexpr int kStripRegs = 16;  // compared: the first 16 (r11, f0 and ea are the copy's dead locals after it)
// (Named one by one: an array of their addresses would make them address-taken, kept in memory in the fast loop too.)
#define SA2_STRIP_REGS(a, op)                                                                                  \
  do {                                                                                                         \
    (a)[0] op ll_r3.u64; (a)[1] op ll_r4.u64; (a)[2] op ll_r5.u64; (a)[3] op ll_r6.u64; (a)[4] op ll_r7.u64;   \
    (a)[5] op ll_r8.u64; (a)[6] op ll_r9.u64; (a)[7] op ll_r10.u64; (a)[8] op ll_f7.u64; (a)[9] op ll_f8.u64;  \
    (a)[10] op ll_f9.u64; (a)[11] op ll_f10.u64; (a)[12] op ll_f11.u64; (a)[13] op ll_f12.u64;                 \
    (a)[14] op ll_f13.u64; (a)[15] op r30.u64; (a)[16] op r11.u64; (a)[17] op f0.u64; (a)[18] op uint64_t(ea); \
  } while (0)
#define SA2_STRIP_REGS_BACK(a)                                                                                 \
  do {                                                                                                         \
    ll_r3.u64 = (a)[0]; ll_r4.u64 = (a)[1]; ll_r5.u64 = (a)[2]; ll_r6.u64 = (a)[3]; ll_r7.u64 = (a)[4];        \
    ll_r8.u64 = (a)[5]; ll_r9.u64 = (a)[6]; ll_r10.u64 = (a)[7]; ll_f7.u64 = (a)[8]; ll_f8.u64 = (a)[9];       \
    ll_f9.u64 = (a)[10]; ll_f10.u64 = (a)[11]; ll_f11.u64 = (a)[12]; ll_f12.u64 = (a)[13];                     \
    ll_f13.u64 = (a)[14]; r30.u64 = (a)[15]; r11.u64 = (a)[16]; f0.u64 = (a)[17]; ea = uint32_t((a)[18]);     \
  } while (0)
inline void Save(const uint8_t* base, uint32_t a, uint32_t len, std::vector<uint8_t>& out) {
  out.resize(len);
  for (uint32_t k = 0; k < len; ++k) out[k] = *Raw(base, a + k);
}
inline void Restore(uint8_t* base, uint32_t a, const std::vector<uint8_t>& in) {
  for (uint32_t k = 0; k < in.size(); ++k) *Raw(base, a + k) = in[k];
}


inline void Native(PPCContext& ctx, uint8_t* base) {
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
#define SA2_SYNC_OUT() do { ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	REX_LOOP_LOCAL(PPCRegister, ll_f7, l_f7);
	REX_LOOP_LOCAL(PPCRegister, ll_f8, l_f8);
	REX_LOOP_LOCAL(PPCRegister, ll_f9, l_f9);
	REX_LOOP_LOCAL(PPCRegister, ll_f10, l_f10);
	REX_LOOP_LOCAL(PPCRegister, ll_f11, l_f11);
	REX_LOOP_LOCAL(PPCRegister, ll_f12, l_f12);
	REX_LOOP_LOCAL(PPCRegister, ll_f13, l_f13);
	REX_LOOP_LOCAL(PPCRegister, ll_r1, l_r1);
	REX_LOOP_LOCAL(PPCRegister, ll_r3, l_r3);
	REX_LOOP_LOCAL(PPCRegister, ll_r4, l_r4);
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
	PPCRegister r25{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f0{};
	PPCRegister f31{};
	PPCRegister temp{};
	PPCRegister fb0{};
	PPCRegister fb12{};
	uint32_t ea{};
	StripCheck* strip_check = nullptr;  // verify mode only
	r12.u64 = ctx.lr;
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -72, f31.u64));
	ea = -192 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	r11.s64 = -2088435712;
	r30.u64 = l_r3.u64;
	r28.u64 = l_r4.u64;
	r29.u64 = l_r6.u64;
	r31.u64 = ((l_r5.u32 >> 14) & 0x3u);
	r25.u64 = l_r3.u64;
	r26.u64 = l_r5.u32 & 0x3FFF;
	cr6.compare<int32_t>(l_r6.s32, 0, xer);
	l_r10.s64 = r11.s64 + 13312;
	if (cr6.eq) goto loc_8214E430;
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 652);
	goto loc_8214E434;
loc_8214E430:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 656);
loc_8214E434:
	REX_STORE_D_U32_C(l_r10.u32, 628, r11.u32);
	l_r3.u64 = r30.u64;
	{ do { ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); sub_8213A428(ctx, base); SA2_SYNC_IN(); }
	cr6.compare<int32_t>(l_r3.s32, 0, xer);
	if (!cr6.eq) goto loc_8214E594;
	l_r10.s64 = -2112946176;
	r11.s64 = -2088108032;
	l_r9.s64 = l_r10.s64 + -21952;
	r27.u64 = ((uint32_t)(r31.u32 << 1));
	r31.s64 = r11.s64 + -3848;
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_RO_U32(0x820EABD0, 0x3B808081);
	f31.f64 = double(temp.f32);
loc_8214E460:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
	r30.s64 = r30.s64 + 2;
	l_r4.s64 = r11.s16;
	cr6.compare<int32_t>(l_r4.s32, 0, xer);
	if (!cr6.lt) goto loc_8214E478;
	l_r4.s64 = static_cast<int64_t>(-l_r4.u64);
loc_8214E478:
	l_r3.u64 = l_r4.u32 & 0xFFFF;
	{ ctx.r3 = l_r3; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82139B48(ctx, base); l_r3 = ctx.r3; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 20);
	REX_LOOP_IN(ll_f7, l_f7);
	REX_LOOP_IN(ll_f8, l_f8);
	REX_LOOP_IN(ll_f9, l_f9);
	REX_LOOP_IN(ll_f10, l_f10);
	REX_LOOP_IN(ll_f11, l_f11);
	REX_LOOP_IN(ll_f12, l_f12);
	REX_LOOP_IN(ll_f13, l_f13);
	REX_LOOP_IN(ll_r1, l_r1);
	REX_LOOP_IN(ll_r3, l_r3);
	REX_LOOP_IN(ll_r4, l_r4);
	REX_LOOP_IN(ll_r5, l_r5);
	REX_LOOP_IN(ll_r6, l_r6);
	REX_LOOP_IN(ll_r7, l_r7);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
	{
		// sa2-nx (2026-10-28): the strip's vertices by the fast loop where it applies (the header's conditions), else
		// by the copy's loop below.
		const uint32_t n = ll_r4.u32;                          // the vertex count (the loop's counter)
		const uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);       // the vertex buffer
		const uint64_t cur0 = ll_r9.u64;                       // its cursor: [r31+20], loaded just above
		const uint64_t dst = uint64_t(buf) + cur0, bytes = 24ull * n;
		const uint64_t desc = r31.u32, slots = uint32_t(ll_r1.u32 + 80);
		if (n != 0 && dst + bytes <= 0x100000000ull && cur0 + bytes < 0x100000000ull &&
		    (dst + bytes <= desc || desc + 24 <= dst) && (dst + bytes <= slots || slots + 32 <= dst)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				strip_check = new StripCheck{{uint32_t(dst), r31.u32 + 20, uint32_t(slots)}, {uint32_t(bytes), 4, 32}};
				for (int k = 0; k < 3; ++k) Save(base, strip_check->at[k], strip_check->len[k], strip_check->before[k]);
				SA2_STRIP_REGS(strip_check->in, =);
			}
			const bool shift = r29.s32 != 0;  // the copy's per-vertex test of r29 (cr6.eq), the same at every vertex
			uint64_t cur = cur0;
			REX_FLUSH_KNOWN_OFF();
			do {
				REX_LOOP_BARRIER();
				r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
				ll_r8.u64 = buf;
				ll_r6.s64 = r11.s16;
				ea = 2 + r30.u32;
				ll_r7.u64 = REX_LOAD_U16(ea);
				r30.u32 = ea;
				ll_r10.u64 = ll_r9.u64 + ll_r8.u64;
				r11.u64 = ((uint32_t)(ll_r6.u32 << 5));
				r11.u64 = r11.u64 + r28.u64;
				ea = 2 + r30.u32;
				ll_r6.u64 = REX_LOAD_U16(ea);
				r30.u32 = ea;
				ll_r5.s64 = r30.s64 + 2;
				fb0.u32 = REX_LOAD_D_U32(r11.u32, 8);
				ll_f13.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
				fb12.u32 = REX_LOAD_D_U32(r11.u32, 0);
				temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
				REX_STORE_U32(ll_r9.u32 + ll_r8.u32, temp.u32);
				ea = 4 + ll_r10.u32;
				temp.f32 = float(ll_f13.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r10.u32 = ea;
				temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
				REX_STORE_D_U32(ll_r10.u32, 4, temp.u32);
				ll_r10.u64 = cur + 12;  // the copy's [r31+20] + 12, stored back
				ll_r3.u64 = REX_LOAD_D_U32(r11.u32, 24);
				ll_r9.u64 = buf;
				REX_STORE_U32(ll_r10.u32 + ll_r9.u32, ll_r3.u32);
				r11.u64 = cur + 16;  // the copy's [r31+20] + 4, stored back
				ll_r10.u64 = buf;
				if (shift) {
					ll_r9.s64 = ll_r7.s16;
					ll_r8.s64 = ll_r6.s16;
					ll_r7.s64 = ll_r9.s32 >> 4;
					ll_r6.s64 = ll_r8.s32 >> 4;
					ll_r3.s64 = ll_r7.s16;
					ll_r7.s64 = ll_r6.s16;
					ll_r8.s64 = ll_r3.s32;
					ll_f13.u64 = ll_r7.u64;  // the copy's store and load of [r1+88]
					f0.u64 = ll_r8.u64;      // and of [r1+80]
					ll_f11.f64 = double(ll_f13.s64);
					ll_f12.f64 = double(f0.s64);
				} else {
					ll_r8.u64 = ll_r7.u64;
					ll_r7.s64 = ll_r6.s16;
					ll_r6.s64 = ll_r8.s16;
					f0.u64 = ll_r7.u64;      // the copy's store and load of [r1+96]
					ll_f13.u64 = ll_r6.u64;  // and of [r1+104]
					ll_f11.f64 = double(f0.s64);
					ll_f12.f64 = double(ll_f13.s64);
				}
				ll_f10.f64 = double(float(ll_f12.f64));
				ll_r9.u64 = r11.u64 + ll_r10.u64;
				ll_f9.f64 = double(float(ll_f11.f64));
				ll_r4.s64 = ll_r4.s64 + -1;
				r30.u64 = r27.u64 + ll_r5.u64;
				ll_f8.f64 = double(float(ll_f10.f64 * f31.f64));
				temp.f32 = float(ll_f8.f64);
				REX_STORE_U32(r11.u32 + ll_r10.u32, temp.u32);
				ll_f7.f64 = double(float(ll_f9.f64 * f31.f64));
				temp.f32 = float(ll_f7.f64);
				REX_STORE_D_U32(ll_r9.u32, 4, temp.u32);
				cur += 24;
				ll_r9.u64 = cur;  // the copy's [r31+20] + 8, stored back
			} while (ll_r4.s32 != 0);
			// The last vertex's frame slots (every vertex stores the same two), the cursor, and the cards: the pages of
			// every store's first byte (the stores tile [dst, dst + bytes) four bytes apart), as the copy's stores mark.
			if (shift) {
				REX_STORE_D_U64(ll_r1.u32, 88, ll_r7.u64);
				REX_STORE_D_U64(ll_r1.u32, 80, ll_r8.u64);
			} else {
				REX_STORE_D_U64(ll_r1.u32, 96, ll_r7.u64);
				REX_STORE_D_U64(ll_r1.u32, 104, ll_r6.u64);
			}
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst), uint32_t(bytes - 3));
#endif
			if (!strip_check) [[likely]] goto loc_8214E484_done;
			// Verify mode: keep the fast loop's result, put the inputs back, and run the copy's loop to compare.
			SA2_STRIP_REGS(strip_check->out, =);
			for (int k = 0; k < 3; ++k) {
				Save(base, strip_check->at[k], strip_check->len[k], strip_check->fast[k]);
				Restore(base, strip_check->at[k], strip_check->before[k]);
			}
			SA2_STRIP_REGS_BACK(strip_check->in);
		}
	}
loc_8214E484:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
	cr6.compare<int32_t>(r29.s32, 0, xer);
	ll_r8.u64 = REX_LOAD_D_U32(r31.u32, 0);
	ll_r6.s64 = r11.s16;
	ea = 2 + r30.u32;
	ll_r7.u64 = REX_LOAD_U16(ea);
	r30.u32 = ea;
	ll_r10.u64 = ll_r9.u64 + ll_r8.u64;
	r11.u64 = ((uint32_t)(ll_r6.u32 << 5));
	r11.u64 = r11.u64 + r28.u64;
	ea = 2 + r30.u32;
	ll_r6.u64 = REX_LOAD_U16(ea);
	r30.u32 = ea;
	ll_r5.s64 = r30.s64 + 2;
	fb0.u32 = REX_LOAD_D_U32(r11.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	ctx.fpscr.disableFlushMode();
	ll_f13.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
	fb12.u32 = REX_LOAD_D_U32(r11.u32, 0);
	ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_U32_C(ll_r9.u32 + ll_r8.u32, temp.u32);
	ea = 4 + ll_r10.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r10.u32 = ea;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r10.u32, 4, temp.u32);
	ll_r10.u64 = REX_LOAD_D_U32(r31.u32, 20);
	ll_r10.s64 = ll_r10.s64 + 12;
	REX_STORE_D_U32_C(r31.u32, 20, ll_r10.u32);
	ll_r3.u64 = REX_LOAD_D_U32(r11.u32, 24);
	ll_r9.u64 = REX_LOAD_D_U32(r31.u32, 0);
	REX_STORE_U32_C(ll_r10.u32 + ll_r9.u32, ll_r3.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	r11.s64 = r11.s64 + 4;
	ll_r10.u64 = REX_LOAD_D_U32(r31.u32, 0);
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	if (cr6.eq) goto loc_8214E528;
	ll_r9.s64 = ll_r7.s16;
	ll_r8.s64 = ll_r6.s16;
	xer.ca = (ll_r9.s32 < 0) & ((ll_r9.u32 & 0xF) != 0);
	ll_r7.s64 = ll_r9.s32 >> 4;
	xer.ca = (ll_r8.s32 < 0) & ((ll_r8.u32 & 0xF) != 0);
	ll_r6.s64 = ll_r8.s32 >> 4;
	ll_r3.s64 = ll_r7.s16;
	ll_r7.s64 = ll_r6.s16;
	ll_r8.s64 = ll_r3.s32;
	REX_STORE_D_U64(ll_r1.u32, 88, ll_r7.u64);
	ll_f13.u64 = REX_LOAD_D_U64(ll_r1.u32, 88);
	REX_STORE_D_U64(ll_r1.u32, 80, ll_r8.u64);
	f0.u64 = REX_LOAD_D_U64(ll_r1.u32, 80);
	ll_f11.f64 = double(ll_f13.s64);
	ll_f12.f64 = double(f0.s64);
	goto loc_8214E54C;
loc_8214E528:
	ll_r8.u64 = ll_r7.u64;
	ll_r7.s64 = ll_r6.s16;
	ll_r6.s64 = ll_r8.s16;
	REX_STORE_D_U64(ll_r1.u32, 96, ll_r7.u64);
	f0.u64 = REX_LOAD_D_U64(ll_r1.u32, 96);
	REX_STORE_D_U64(ll_r1.u32, 104, ll_r6.u64);
	ll_f13.u64 = REX_LOAD_D_U64(ll_r1.u32, 104);
	ll_f11.f64 = double(f0.s64);
	ll_f12.f64 = double(ll_f13.s64);
loc_8214E54C:
	REX_FLUSH_KNOWN_OFF();
	ll_f10.f64 = double(float(ll_f12.f64));
	ll_r9.u64 = r11.u64 + ll_r10.u64;
	ll_f9.f64 = double(float(ll_f11.f64));
	xer.ca = ll_r4.u32 > 0;
	ll_r4.s64 = ll_r4.s64 + -1;
	cr0.compare<int32_t>(ll_r4.s32, 0, xer);
	r30.u64 = r27.u64 + ll_r5.u64;
	ll_f8.f64 = double(float(ll_f10.f64 * f31.f64));
	temp.f32 = float(ll_f8.f64);
	REX_STORE_U32_C(r11.u32 + ll_r10.u32, temp.u32);
	ll_f7.f64 = double(float(ll_f9.f64 * f31.f64));
	temp.f32 = float(ll_f7.f64);
	REX_STORE_D_U32_C(ll_r9.u32, 4, temp.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	ll_r9.s64 = r11.s64 + 8;
	REX_STORE_D_U32_C(r31.u32, 20, ll_r9.u32);
	if (!cr0.eq) goto loc_8214E484;
	if (strip_check) [[unlikely]] {
		uint64_t want[kStripRegs];
		SA2_STRIP_REGS(want, =);
		bool same = std::equal(want, want + kStripRegs, strip_check->out);
		std::vector<uint8_t> now;
		for (int k = 0; k < 3; ++k) {
			Save(base, strip_check->at[k], strip_check->len[k], now);
			same = same && now == strip_check->fast[k];
		}
		::sa2::enh::VerifyCheck();
		static std::atomic<uint32_t> checks_{0};
		const uint32_t count_ = checks_.fetch_add(1, std::memory_order_relaxed) + 1;
		if (count_ == 1 || count_ == 4096 || count_ == 262144)
			REXLOG_INFO("[hot] sub_8214E3F0 fast vertex loop: {} checks against the copy's loop", count_);
		if (!same) {
			::sa2::enh::VerifyDifference();
			REXLOG_ERROR("[hot] DIFFERENCE: sub_8214E3F0's fast vertex loop ({} vertices to {:08X}) differs from the copy's "
			             "loop; the copy's result stands", strip_check->len[0] / 24, strip_check->at[0]);
		}
		delete strip_check;
		strip_check = nullptr;
	}
loc_8214E484_done:
	REX_LOOP_OUT(ll_f7, l_f7);
	REX_LOOP_OUT(ll_f8, l_f8);
	REX_LOOP_OUT(ll_f9, l_f9);
	REX_LOOP_OUT(ll_f10, l_f10);
	REX_LOOP_OUT(ll_f11, l_f11);
	REX_LOOP_OUT(ll_f12, l_f12);
	REX_LOOP_OUT(ll_f13, l_f13);
	REX_LOOP_OUT(ll_r3, l_r3);
	REX_LOOP_OUT(ll_r4, l_r4);
	REX_LOOP_OUT(ll_r5, l_r5);
	REX_LOOP_OUT(ll_r6, l_r6);
	REX_LOOP_OUT(ll_r7, l_r7);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	{ ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82139C90(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	xer.ca = r26.u32 > 0;
	r26.s64 = r26.s64 + -1;
	cr0.compare<int32_t>(r26.s32, 0, xer);
	if (!cr0.eq) goto loc_8214E460;
	l_r3.u64 = r25.u64;
	{ do { ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r3 = l_r3; } while (0); sub_8213A318(ctx, base); SA2_SYNC_IN(); }
loc_8214E594:
	l_r1.s64 = l_r1.s64 + 192;
	f31.u64 = REX_LOAD_D_U64(l_r1.u32, -72);
	{ do { ctx.r1 = l_r1; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

#undef SA2_STRIP_REGS
#undef SA2_STRIP_REGS_BACK

}  // namespace sa2::hot::n_strip_builder
