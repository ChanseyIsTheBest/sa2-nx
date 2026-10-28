// sa2-nx (2026-10-28): hand-written sub_8214E130 (1.3 % of the game thread in console gameplay; 2.4 % on the PC
// profile): a model's triangle strips into the vertex buffer of the descriptor at 0x8389F0F8 (r31), 32 bytes a
// vertex: the 24-byte vertex its index names (copied as memcpy: inline, or by the call where the copy calls), then its
// two texture coordinates (s16, >> 4 when r6 != 0, times 1/255). The 24-byte-vertex sibling of sub_8214E3F0
// (hot/n_strip_builder.h). Made from its localized copy (app/generated/hot/n_8214E130.h), which stays its reference:
// re-run tools/hand_natives/make_vertex24_strips.py after the copy changes (it checks the loop it replaces).
//
// Optimisation: the copy's vertex loop reloads the buffer base [r31+0] twice and the cursor [r31+20] twice a vertex,
// stores the cursor back twice, marks a card at its three stores, and moves the coordinates to double through two
// frame slots. The fast loop holds the base and the cursor, stores the cursor once, converts directly, writes the last
// vertex's frame slots once and marks the coordinates' cards once.
//
// Audit: it applies when the count n != 0, the vertices [dst, dst + 32n) (dst = base + cursor) neither wrap nor cross
// the hosts' physical-range offset, nor overlap the descriptor [r31, r31+24) or the frame slots [r1+80, r1+112), and
// the cursor does not wrap: then the copy's reloads read what the fast loop holds (nothing it stores, the 24-byte copy
// included, lands on them; the copy's memcpy writes [dst..] and, inline, the dead slot [r1-8], as here), and its
// intermediate cursor and slot stores are overwritten by the last. Every other statement is the copy's, in its order:
// the vertex copy (inline or the call, with its conditions), the strip stream and vertex loads and the coordinate
// stores (so a buffer overlapping them reads what the copy reads). The registers the loop leaves are the copy's last
// vertex's; cr0 / cr6 / xer are its locals, written before any read after the loop. disableFlushMode runs once (the
// loop changes no mode: a call, when taken, is memcpy). The cards: the coordinates' stores (two a vertex) mark the pages
// of their first bytes, [dst + 24, dst + 32n - 4] (no gap of 4096), zeroed after the data; the cursor's as the copy's.
// Otherwise the copy's loop runs.
//
// Unguarded like its copy; in verify mode (sa2_cpu_verify) the fast loop checks itself against the copy's
// (hot/fast_loop_check.h).
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

namespace sa2::hot::n_vertex24_strips {

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
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister r23{};
	PPCRegister r24{};
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
	uint32_t ea{};
	::sa2::hot::fast_loop::Check* check = nullptr;  // verify mode only
	r12.u64 = ctx.lr;
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -88, f31.u64));
	ea = -208 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	r11.s64 = -2088435712;
	r30.u64 = l_r3.u64;
	r26.u64 = l_r4.u64;
	r27.u64 = l_r6.u64;
	r23.u64 = l_r3.u64;
	r24.u64 = l_r5.u32 & 0x3FFF;
	cr6.compare<int32_t>(l_r6.s32, 0, xer);
	l_r10.s64 = r11.s64 + 13312;
	if (cr6.eq) goto loc_8214E16C;
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 664);
	goto loc_8214E170;
loc_8214E16C:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 668);
loc_8214E170:
	REX_STORE_D_U32_C(l_r10.u32, 628, r11.u32);
	l_r3.u64 = r30.u64;
	{ do { ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r10 = l_r10; } while (0); sub_8213A428(ctx, base); SA2_SYNC_IN(); }
	cr6.compare<int32_t>(l_r3.s32, 0, xer);
	if (!cr6.eq) goto loc_8214E2A0;
	l_r10.s64 = -2112946176;
	r11.s64 = -2088108032;
	l_r9.s64 = l_r10.s64 + -21952;
	r31.s64 = r11.s64 + -3848;
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_RO_U32(0x820EABD0, 0x3B808081);
	f31.f64 = double(temp.f32);
loc_8214E198:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
	r30.s64 = r30.s64 + 2;
	r25.s64 = r11.s16;
	cr6.compare<int32_t>(r25.s32, 0, xer);
	if (!cr6.lt) goto loc_8214E1B0;
	r25.s64 = static_cast<int64_t>(-r25.u64);
loc_8214E1B0:
	l_r3.u64 = r25.u32 & 0xFFFF;
	{ ctx.r3 = l_r3; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82139B48(ctx, base); l_r3 = ctx.r3; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	{
		// sa2-nx (2026-10-28): sub_8214E130 fast vertex loop where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = r25.u32;                         // the strip's vertices (the loop's counter)
		const uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);    // the vertex buffer
		const uint64_t cur0 = r11.u64;                      // its cursor [r31+20], loaded just above
		const uint64_t dst = uint64_t(buf) + cur0, bytes = 32ull * n, frame = l_r1.u32;
		if (n != 0 && cur0 + bytes < 0x100000000ull && ::sa2::hot::fast_loop::Flat(dst, bytes) && ::sa2::hot::fast_loop::Apart(dst, bytes, r31.u32, 24) && ::sa2::hot::fast_loop::Apart(dst, bytes, frame + 80, 32)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check = new ::sa2::hot::fast_loop::Check{};
				check->Add(uint32_t(dst), uint32_t(bytes));
				check->Add(r31.u32 + 20, 4);
				check->Add(uint32_t(frame + 80), 32);
				check->Add(uint32_t(frame - 8), 8);
				::sa2::hot::fast_loop::Begin(*check, base);
				check->in[0] = l_r3.u64;
				check->in[1] = l_r4.u64;
				check->in[2] = l_r5.u64;
				check->in[3] = l_r6.u64;
				check->in[4] = l_r7.u64;
				check->in[5] = l_r8.u64;
				check->in[6] = l_r9.u64;
				check->in[7] = l_r10.u64;
				check->in[8] = l_f7.u64;
				check->in[9] = l_f8.u64;
				check->in[10] = l_f9.u64;
				check->in[11] = l_f10.u64;
				check->in[12] = l_f11.u64;
				check->in[13] = l_f12.u64;
				check->in[14] = l_f13.u64;
				check->in[15] = r11.u64;
				check->in[16] = r25.u64;
				check->in[17] = r28.u64;
				check->in[18] = r29.u64;
				check->in[19] = r30.u64;
				check->in[20] = l_r1.u64;
				check->in[21] = f0.u64;
				check->in[22] = uint64_t(ea);
				check->in[23] = temp.u64;
			}
			const bool shift = r27.s32 != 0;  // the copy's per-vertex test of r27 (cr6.eq), the same at every vertex
			uint64_t cur = cur0;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				l_r9.u64 = REX_LOAD_D_U16(r30.u32, 0);
				l_r5.s64 = 24;
				ea = 2 + r30.u32;
				r29.u64 = REX_LOAD_U16(ea);
				r30.u32 = ea;
				l_r10.u64 = buf;
				l_r8.s64 = l_r9.s16;
				l_r3.u64 = r11.u64 + l_r10.u64;
				r11.u64 = ((uint32_t)(l_r8.u32 << 5));
				ea = 2 + r30.u32;
				r28.u64 = REX_LOAD_U16(ea);
				r30.u32 = ea;
				l_r4.u64 = r11.u64 + r26.u64;
				r30.s64 = r30.s64 + 2;
				// The vertex's 24 bytes, as the copy copies them (inline, or by the call where it does not inline).
				{ ctx.r3 = l_r3; ctx.r4 = l_r4; if (!SA2_MEMCPY_INLINE(24)) { ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_827F05C0(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } }
				r11.u64 = cur + 24;  // the copy's [r31+20] + 24, stored back
				l_r10.u64 = buf;
				if (shift) {
					l_r9.s64 = r29.s16;
					l_r8.s64 = r28.s16;
					l_r7.s64 = l_r9.s32 >> 4;
					l_r6.s64 = l_r8.s32 >> 4;
					l_r5.s64 = l_r7.s16;
					l_r4.s64 = l_r6.s16;
					l_r3.s64 = l_r5.s32;
					l_r9.s64 = l_r4.s32;
					f0.u64 = l_r3.u64;     // the copy's store and load of [r1+80]
					l_f13.u64 = l_r9.u64;  // and of [r1+88]
					l_f12.f64 = double(f0.s64);
					l_f11.f64 = double(l_f13.s64);
				} else {
					l_r6.s64 = r29.s16;
					l_r7.s64 = r28.s16;
					l_f13.u64 = l_r6.u64;  // the copy's store and load of [r1+104]
					f0.u64 = l_r7.u64;     // and of [r1+96]
					l_f11.f64 = double(f0.s64);
					l_f12.f64 = double(l_f13.s64);
				}
				l_f10.f64 = double(float(l_f12.f64));
				l_r9.u64 = r11.u64 + l_r10.u64;
				l_f9.f64 = double(float(l_f11.f64));
				r25.s64 = r25.s64 + -1;
				l_f8.f64 = double(float(l_f10.f64 * f31.f64));
				temp.f32 = float(l_f8.f64);
				REX_STORE_U32(r11.u32 + l_r10.u32, temp.u32);
				l_f7.f64 = double(float(l_f9.f64 * f31.f64));
				temp.f32 = float(l_f7.f64);
				REX_STORE_D_U32(l_r9.u32, 4, temp.u32);
				cur += 32;
				r11.u64 = cur;  // the copy's [r31+20] + 8, stored back
			} while (r25.s32 != 0);
			// The last vertex's frame slots, the cursor, and the cards of the texture coordinates' stores (the copy's
			// marked stores: two a vertex, 24 bytes apart at most, so every page from the first to the last has one).
			if (shift) {
				REX_STORE_D_U64(l_r1.u32, 80, l_r3.u64);
				REX_STORE_D_U64(l_r1.u32, 88, l_r9.u64);
			} else {
				REX_STORE_D_U64(l_r1.u32, 104, l_r6.u64);
				REX_STORE_D_U64(l_r1.u32, 96, l_r7.u64);
			}
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst + 24), uint32_t(bytes - 27));
#endif
			if (!check) [[likely]] goto loc_8214E1BC_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check->out[0] = l_r3.u64;
			check->out[1] = l_r4.u64;
			check->out[2] = l_r5.u64;
			check->out[3] = l_r6.u64;
			check->out[4] = l_r7.u64;
			check->out[5] = l_r8.u64;
			check->out[6] = l_r9.u64;
			check->out[7] = l_r10.u64;
			check->out[8] = l_f7.u64;
			check->out[9] = l_f8.u64;
			check->out[10] = l_f9.u64;
			check->out[11] = l_f10.u64;
			check->out[12] = l_f11.u64;
			check->out[13] = l_f12.u64;
			check->out[14] = l_f13.u64;
			check->out[15] = r11.u64;
			check->out[16] = r25.u64;
			check->out[17] = r28.u64;
			check->out[18] = r29.u64;
			check->out[19] = r30.u64;
			check->out[20] = l_r1.u64;
			check->out[21] = f0.u64;
			check->out[22] = uint64_t(ea);
			check->out[23] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check, base);
			l_r3.u64 = check->in[0];
			l_r4.u64 = check->in[1];
			l_r5.u64 = check->in[2];
			l_r6.u64 = check->in[3];
			l_r7.u64 = check->in[4];
			l_r8.u64 = check->in[5];
			l_r9.u64 = check->in[6];
			l_r10.u64 = check->in[7];
			l_f7.u64 = check->in[8];
			l_f8.u64 = check->in[9];
			l_f9.u64 = check->in[10];
			l_f10.u64 = check->in[11];
			l_f11.u64 = check->in[12];
			l_f12.u64 = check->in[13];
			l_f13.u64 = check->in[14];
			r11.u64 = check->in[15];
			r25.u64 = check->in[16];
			r28.u64 = check->in[17];
			r29.u64 = check->in[18];
			r30.u64 = check->in[19];
			l_r1.u64 = check->in[20];
			f0.u64 = check->in[21];
			ea = uint32_t(check->in[22]);
			temp.u64 = check->in[23];
		}
	}
loc_8214E1BC:
	REX_LOOP_BARRIER();
	l_r9.u64 = REX_LOAD_D_U16(r30.u32, 0);
	l_r5.s64 = 24;
	ea = 2 + r30.u32;
	r29.u64 = REX_LOAD_U16(ea);
	r30.u32 = ea;
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 0);
	l_r8.s64 = l_r9.s16;
	l_r3.u64 = r11.u64 + l_r10.u64;
	r11.u64 = ((uint32_t)(l_r8.u32 << 5));
	ea = 2 + r30.u32;
	r28.u64 = REX_LOAD_U16(ea);
	r30.u32 = ea;
	l_r4.u64 = r11.u64 + r26.u64;
	r30.s64 = r30.s64 + 2;
	{ ctx.r3 = l_r3; ctx.r4 = l_r4; if (!SA2_MEMCPY_INLINE(24)) { ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_827F05C0(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } }
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 0);
	cr6.compare<int32_t>(r27.s32, 0, xer);
	r11.s64 = r11.s64 + 24;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	if (cr6.eq) goto loc_8214E23C;
	l_r9.s64 = r29.s16;
	l_r8.s64 = r28.s16;
	xer.ca = (l_r9.s32 < 0) & ((l_r9.u32 & 0xF) != 0);
	l_r7.s64 = l_r9.s32 >> 4;
	xer.ca = (l_r8.s32 < 0) & ((l_r8.u32 & 0xF) != 0);
	l_r6.s64 = l_r8.s32 >> 4;
	l_r5.s64 = l_r7.s16;
	l_r4.s64 = l_r6.s16;
	l_r3.s64 = l_r5.s32;
	l_r9.s64 = l_r4.s32;
	REX_STORE_D_U64(l_r1.u32, 80, l_r3.u64);
	f0.u64 = REX_LOAD_D_U64(l_r1.u32, 80);
	REX_STORE_D_U64(l_r1.u32, 88, l_r9.u64);
	l_f13.u64 = REX_LOAD_D_U64(l_r1.u32, 88);
	l_f12.f64 = double(f0.s64);
	l_f11.f64 = double(l_f13.s64);
	goto loc_8214E25C;
loc_8214E23C:
	l_r6.s64 = r29.s16;
	l_r7.s64 = r28.s16;
	REX_STORE_D_U64(l_r1.u32, 104, l_r6.u64);
	l_f13.u64 = REX_LOAD_D_U64(l_r1.u32, 104);
	REX_STORE_D_U64(l_r1.u32, 96, l_r7.u64);
	f0.u64 = REX_LOAD_D_U64(l_r1.u32, 96);
	l_f11.f64 = double(f0.s64);
	l_f12.f64 = double(l_f13.s64);
loc_8214E25C:
	ctx.fpscr.disableFlushMode();
	l_f10.f64 = double(float(l_f12.f64));
	l_r9.u64 = r11.u64 + l_r10.u64;
	l_f9.f64 = double(float(l_f11.f64));
	xer.ca = r25.u32 > 0;
	r25.s64 = r25.s64 + -1;
	cr0.compare<int32_t>(r25.s32, 0, xer);
	l_f8.f64 = double(float(l_f10.f64 * f31.f64));
	temp.f32 = float(l_f8.f64);
	REX_STORE_U32_C(r11.u32 + l_r10.u32, temp.u32);
	l_f7.f64 = double(float(l_f9.f64 * f31.f64));
	temp.f32 = float(l_f7.f64);
	REX_STORE_D_U32_C(l_r9.u32, 4, temp.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	r11.s64 = r11.s64 + 8;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	if (!cr0.eq) goto loc_8214E1BC;
	if (check) [[unlikely]] {
		uint64_t want_[21];
		want_[0] = l_r3.u64;
		want_[1] = l_r4.u64;
		want_[2] = l_r5.u64;
		want_[3] = l_r6.u64;
		want_[4] = l_r7.u64;
		want_[5] = l_r8.u64;
		want_[6] = l_r9.u64;
		want_[7] = l_r10.u64;
		want_[8] = l_f7.u64;
		want_[9] = l_f8.u64;
		want_[10] = l_f9.u64;
		want_[11] = l_f10.u64;
		want_[12] = l_f11.u64;
		want_[13] = l_f12.u64;
		want_[14] = l_f13.u64;
		want_[15] = r11.u64;
		want_[16] = r25.u64;
		want_[17] = r28.u64;
		want_[18] = r29.u64;
		want_[19] = r30.u64;
		want_[20] = l_r1.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check, base, want_, 21, "sub_8214E130 fast vertex loop", checks_);
	}
loc_8214E1BC_done:
	{ ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82139C90(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	xer.ca = r24.u32 > 0;
	r24.s64 = r24.s64 + -1;
	cr0.compare<int32_t>(r24.s32, 0, xer);
	if (!cr0.eq) goto loc_8214E198;
	l_r3.u64 = r23.u64;
	{ do { ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r3 = l_r3; } while (0); sub_8213A318(ctx, base); SA2_SYNC_IN(); }
loc_8214E2A0:
	l_r1.s64 = l_r1.s64 + 208;
	f31.u64 = REX_LOAD_D_U64(l_r1.u32, -88);
	{ do { ctx.r1 = l_r1; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_vertex24_strips
