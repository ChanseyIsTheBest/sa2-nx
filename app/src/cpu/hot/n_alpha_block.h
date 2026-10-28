// sa2-nx (2026-10-28): hand-written sub_82833938, a DXT5 block to 16 RGBA float texels (2.6 % of the game thread in
// console gameplay, round-6 renut profile): it byte-swaps the block's 16-bit words to the stack, decodes the colour
// half with sub_82833568 (hot/n_dxt1_block.h), builds the eight-entry alpha palette on the stack ([r1+112..143]: the
// two endpoints, then six interpolated entries, or four and 0.0 / 1.0), and writes each texel's alpha (3-bit index)
// to [r3 + 12 + 16k]. Made from its localized copy (app/generated/hot/n_82833938.h), which stays its reference:
// re-run tools/hand_natives/make_alpha_block.py after the copy changes (it checks the four loops it replaces statement
// by statement).
//
// Optimisation: every store of the copy's loops marks a card, a byte store that may alias anything, so GCC reloads
// after each: the palette loops reload the endpoints [r1+112], [r1+116] and move the counters to double through two
// frame slots at every entry, and the texel loops reload the palette entry from the stack. The fast loops hold the
// endpoints and the palette in registers, convert directly, store the last entry's frame slots once, and mark the
// cards once.
//
// Audit: a palette loop applies when its entries [r10+4, r10+4+4n) neither wrap nor overlap the frame slots [r1+80,
// r1+96) or the endpoints [r1+112, r1+120) (r10 = r1 + 116: they never do); a texel loop when its alpha words neither
// wrap nor overlap the palette [r1+112, r1+144). Then the copy's reloads read what the fast loop holds, its frame slot
// round trips give back the values stored, and its intermediate slot stores are overwritten by the last. Every other
// statement is the copy's, in its order; the registers each loop leaves (f10-f13, r8-r10, r11, ctr; r8-r10, r11, ctr,
// f0) are the copy's last iteration's; xer is the copy's dead local (only its carry is written). REX_FLUSH_KNOWN_OFF
// runs once a loop (nothing in a loop changes the mode). The cards are the pages of every store's first byte (stores
// 4 or 16 bytes apart: every page between the first and the last has one), zeroed after the data. Otherwise the
// copy's loop runs.
//
// Unguarded like its copy (it calls sub_82833568); in verify mode (sa2_cpu_verify) each fast loop checks itself
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

REX_EXTERN(sub_82833568);

namespace sa2::hot::n_alpha_block {

inline void Native(PPCContext& ctx, uint8_t* base) {
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
#define SA2_SYNC_OUT() do { ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	REX_LOOP_LOCAL(PPCRegister, ll_f10, l_f10);
	REX_LOOP_LOCAL(PPCRegister, ll_f11, l_f11);
	REX_LOOP_LOCAL(PPCRegister, ll_f12, l_f12);
	REX_LOOP_LOCAL(PPCRegister, ll_f13, l_f13);
	REX_LOOP_LOCAL(PPCRegister, ll_r1, l_r1);
	REX_LOOP_LOCAL(PPCRegister, ll_r8, l_r8);
	REX_LOOP_LOCAL(PPCRegister, ll_r9, l_r9);
	REX_LOOP_LOCAL(PPCRegister, ll_r10, l_r10);
	PPCRegister ctr{};
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f0{};
	PPCRegister temp{};
	PPCRegister fb0{};
	PPCRegister fb13{};
	uint32_t ea{};
	::sa2::hot::fast_loop::Check* check_hi = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check_lo = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check4 = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check6 = nullptr;  // verify mode only
	r12.u64 = ctx.lr;
	ea = -176 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	r11.u64 = l_r4.u64;
	l_r10.u64 = REX_LOAD_D_U16(l_r4.u32, 0);
	l_r4.s64 = l_r1.s64 + 104;
	l_r7.u64 = ((uint32_t)(l_r10.u32 << 8) & 0xFF00u);
	l_r10.u64 = (l_r10.u32 >> 8);
	r31.u64 = l_r3.u64;
	l_r9.u64 = REX_LOAD_D_U16(r11.u32, 2);
	l_r8.u64 = REX_LOAD_D_U16(r11.u32, 4);
	l_r6.u64 = REX_LOAD_D_U16(r11.u32, 6);
	l_r5.u64 = ((uint32_t)(l_r9.u32 << 8) & 0xFF00u);
	r11.u64 = REX_LOAD_D_U64(r11.u32, 8);
	l_r9.u64 = (l_r9.u32 >> 8);
	r30.u64 = ((uint32_t)(l_r8.u32 << 8) & 0xFF00u);
	r29.u64 = ((uint32_t)(l_r6.u32 << 8) & 0xFF00u);
	l_r8.u64 = (l_r8.u32 >> 8);
	l_r6.u64 = (l_r6.u32 >> 8);
	REX_STORE_D_U64(l_r1.u32, 104, r11.u64);
	r11.u64 = l_r7.u64 | l_r10.u64;
	l_r10.u64 = l_r5.u64 | l_r9.u64;
	REX_STORE_D_U16(l_r1.u32, 96, r11.u16);
	l_r9.u64 = r30.u64 | l_r8.u64;
	r11.u64 = r29.u64 | l_r6.u64;
	REX_STORE_D_U16(l_r1.u32, 98, l_r10.u16);
	REX_STORE_D_U16(l_r1.u32, 100, l_r9.u16);
	REX_STORE_D_U16(l_r1.u32, 102, r11.u16);
	{ ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_82833568(ctx, base); l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	cr0.compare<int32_t>(l_r3.s32, 0, xer);
	if (cr0.lt) goto loc_82833B5C;
	r11.u64 = REX_LOAD_D_U8(l_r1.u32, 97);
	l_r10.s64 = -2113929216;
	l_r9.u64 = REX_LOAD_D_U8(l_r1.u32, 96);
	REX_DEAD_DEF(r8, l_r8.u64 = r11.u64);
	cr6.compare<uint32_t>(l_r9.u32, r11.u32, xer);
	REX_DEAD_DEF(r7, l_r7.u64 = l_r9.u64);
	REX_STORE_D_U64(l_r1.u32, 80, r11.u64);
	l_f13.u64 = REX_LOAD_D_U64(l_r1.u32, 80);
	REX_STORE_D_U64(l_r1.u32, 80, l_r9.u64);
	l_f12.u64 = REX_LOAD_D_U64(l_r1.u32, 80);
	l_f12.f64 = double(l_f12.s64);
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_RO_U32(0x82001624, 0x3B808081);
	f0.f64 = double(temp.f32);
	l_f13.f64 = double(l_f13.s64);
	r11.s64 = 1;
	l_r10.s64 = l_r1.s64 + 116;
	l_f12.f64 = double(float(l_f12.f64));
	l_f13.f64 = double(float(l_f13.f64));
	l_f12.f64 = REX_FMULS(l_f12.f64, f0.f64);
	temp.f32 = float(l_f12.f64);
	REX_STORE_D_U32(l_r1.u32, 112, temp.u32);
	f0.f64 = REX_FMULS(l_f13.f64, f0.f64);
	temp.f32 = float(f0.f64);
	REX_STORE_D_U32(l_r1.u32, 116, temp.u32);
	if (!cr6.gt) goto loc_82833A6C;
	l_r9.s64 = 6;
	ctr.u64 = l_r9.u64;
	l_r9.s64 = -2113536000;
	temp.u32 = REX_RO_U32(0x8205F6CC, 0x3E124925);
	f0.f64 = double(temp.f32);
	REX_LOOP_IN(ll_f10, l_f10);
	REX_LOOP_IN(ll_f11, l_f11);
	REX_LOOP_IN(ll_f12, l_f12);
	REX_LOOP_IN(ll_f13, l_f13);
	REX_LOOP_IN(ll_r1, l_r1);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
	{
		// sa2-nx (2026-10-28): sub_82833938 fast alpha palette (6 entries) where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the entries (the copy's counter)
		const uint64_t first = uint32_t(ll_r10.u32 + 4), frame = ll_r1.u32;  // the first entry's address
		if (n != 0 && ::sa2::hot::fast_loop::Flat(first, 4ull * n) && ::sa2::hot::fast_loop::Apart(first, 4ull * n, frame + 80, 16) && ::sa2::hot::fast_loop::Apart(first, 4ull * n, frame + 112, 8)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check6 = new ::sa2::hot::fast_loop::Check{};
				check6->Add(uint32_t(first), 4 * n);
				check6->Add(ll_r1.u32 + 80, 16);
				::sa2::hot::fast_loop::Begin(*check6, base);
				check6->in[0] = ll_f10.u64;
				check6->in[1] = ll_f11.u64;
				check6->in[2] = ll_f12.u64;
				check6->in[3] = ll_f13.u64;
				check6->in[4] = ll_r8.u64;
				check6->in[5] = ll_r9.u64;
				check6->in[6] = ll_r10.u64;
				check6->in[7] = r11.u64;
				check6->in[8] = ctr.u64;
				check6->in[9] = uint64_t(ea);
				check6->in[10] = temp.u64;
			}
			// The endpoints [r1+116], [r1+112], reloaded by the copy at every entry (no entry lands on them).
			const float a116 = REX_LOAD_D_F32(ll_r1.u32, 116), a112 = REX_LOAD_D_F32(ll_r1.u32, 112);
			uint32_t left = n;
			REX_FLUSH_KNOWN_OFF();
			do {
				REX_LOOP_BARRIER();
				ll_r9.u64 = static_cast<uint64_t>(7) - r11.u64;
				ll_f13.f64 = double(a116);
				ll_r8.u64 = r11.u64 & 0xFFFFFFFF;
				ll_f12.f64 = double(a112);
				ll_r9.u64 = ll_r9.u64 & 0xFFFFFFFF;
				ll_f11.u64 = ll_r8.u64;  // the copy's store and load of [r1+80]
				ll_f10.u64 = ll_r9.u64;  // and of [r1+88]
				ll_f11.f64 = double(ll_f11.s64);
				r11.s64 = r11.s64 + 1;
				ll_f11.f64 = double(float(ll_f11.f64));
				ll_f10.f64 = double(ll_f10.s64);
				ll_f13.f64 = REX_FMULS(ll_f11.f64, ll_f13.f64);
				ll_f11.f64 = double(float(ll_f10.f64));
				ll_f13.f64 = double(float(std::fma(ll_f11.f64, ll_f12.f64, ll_f13.f64)));
				ll_f13.f64 = REX_FMULS(ll_f13.f64, f0.f64);
				ea = 4 + ll_r10.u32;
				temp.f32 = float(ll_f13.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r10.u32 = ea;
			} while (--left != 0);
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
			// The last entry's frame slots, and the cards of the entries' pages (a store's first byte each).
			REX_STORE_D_U64(ll_r1.u32, 80, ll_r8.u64);
			REX_STORE_D_U64(ll_r1.u32, 88, ll_r9.u64);
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(first), 4 * (n - 1) + 1);
#endif
			if (!check6) [[likely]] goto loc_82833A1C_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check6->out[0] = ll_f10.u64;
			check6->out[1] = ll_f11.u64;
			check6->out[2] = ll_f12.u64;
			check6->out[3] = ll_f13.u64;
			check6->out[4] = ll_r8.u64;
			check6->out[5] = ll_r9.u64;
			check6->out[6] = ll_r10.u64;
			check6->out[7] = r11.u64;
			check6->out[8] = ctr.u64;
			check6->out[9] = uint64_t(ea);
			check6->out[10] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check6, base);
			ll_f10.u64 = check6->in[0];
			ll_f11.u64 = check6->in[1];
			ll_f12.u64 = check6->in[2];
			ll_f13.u64 = check6->in[3];
			ll_r8.u64 = check6->in[4];
			ll_r9.u64 = check6->in[5];
			ll_r10.u64 = check6->in[6];
			r11.u64 = check6->in[7];
			ctr.u64 = check6->in[8];
			ea = uint32_t(check6->in[9]);
			temp.u64 = check6->in[10];
		}
	}
loc_82833A1C:
	REX_LOOP_BARRIER();
	xer.ca = r11.u32 <= 7;
	ll_r9.u64 = static_cast<uint64_t>(7) - r11.u64;
	REX_FLUSH_KNOWN_OFF();
	ll_f13.f64 = double(REX_LOAD_D_F32(ll_r1.u32, 116));
	ll_r8.u64 = r11.u64 & 0xFFFFFFFF;
	ll_f12.f64 = double(REX_LOAD_D_F32(ll_r1.u32, 112));
	ll_r9.u64 = ll_r9.u64 & 0xFFFFFFFF;
	REX_STORE_D_U64(ll_r1.u32, 80, ll_r8.u64);
	ll_f11.u64 = REX_LOAD_D_U64(ll_r1.u32, 80);
	REX_STORE_D_U64(ll_r1.u32, 88, ll_r9.u64);
	ll_f10.u64 = REX_LOAD_D_U64(ll_r1.u32, 88);
	ll_f11.f64 = double(ll_f11.s64);
	r11.s64 = r11.s64 + 1;
	ll_f11.f64 = double(float(ll_f11.f64));
	ll_f10.f64 = double(ll_f10.s64);
	ll_f13.f64 = REX_FMULS(ll_f11.f64, ll_f13.f64);
	ll_f11.f64 = double(float(ll_f10.f64));
	ll_f13.f64 = double(float(std::fma(ll_f11.f64, ll_f12.f64, ll_f13.f64)));
	ll_f13.f64 = REX_FMULS(ll_f13.f64, f0.f64);
	ea = 4 + ll_r10.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r10.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82833A1C;
	if (check6) [[unlikely]] {
		uint64_t want_[9];
		want_[0] = ll_f10.u64;
		want_[1] = ll_f11.u64;
		want_[2] = ll_f12.u64;
		want_[3] = ll_f13.u64;
		want_[4] = ll_r8.u64;
		want_[5] = ll_r9.u64;
		want_[6] = ll_r10.u64;
		want_[7] = r11.u64;
		want_[8] = ctr.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check6, base, want_, 9, "sub_82833938 fast alpha palette (6 entries)", checks_);
	}
loc_82833A1C_done:
	REX_LOOP_OUT(ll_f10, l_f10);
	REX_LOOP_OUT(ll_f11, l_f11);
	REX_LOOP_OUT(ll_f12, l_f12);
	REX_LOOP_OUT(ll_f13, l_f13);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	goto loc_82833AE0;
loc_82833A6C:
	l_r9.s64 = 4;
	ctr.u64 = l_r9.u64;
	l_r9.s64 = -2113863680;
	REX_FLUSH_KNOWN_OFF();
	temp.u32 = REX_RO_U32(0x820150E8, 0x3E4CCCCD);
	f0.f64 = double(temp.f32);
	REX_LOOP_IN(ll_f10, l_f10);
	REX_LOOP_IN(ll_f11, l_f11);
	REX_LOOP_IN(ll_f12, l_f12);
	REX_LOOP_IN(ll_f13, l_f13);
	REX_LOOP_IN(ll_r1, l_r1);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
	{
		// sa2-nx (2026-10-28): sub_82833938 fast alpha palette (4 entries) where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the entries (the copy's counter)
		const uint64_t first = uint32_t(ll_r10.u32 + 4), frame = ll_r1.u32;  // the first entry's address
		if (n != 0 && ::sa2::hot::fast_loop::Flat(first, 4ull * n) && ::sa2::hot::fast_loop::Apart(first, 4ull * n, frame + 80, 16) && ::sa2::hot::fast_loop::Apart(first, 4ull * n, frame + 112, 8)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check4 = new ::sa2::hot::fast_loop::Check{};
				check4->Add(uint32_t(first), 4 * n);
				check4->Add(ll_r1.u32 + 80, 16);
				::sa2::hot::fast_loop::Begin(*check4, base);
				check4->in[0] = ll_f10.u64;
				check4->in[1] = ll_f11.u64;
				check4->in[2] = ll_f12.u64;
				check4->in[3] = ll_f13.u64;
				check4->in[4] = ll_r8.u64;
				check4->in[5] = ll_r9.u64;
				check4->in[6] = ll_r10.u64;
				check4->in[7] = r11.u64;
				check4->in[8] = ctr.u64;
				check4->in[9] = uint64_t(ea);
				check4->in[10] = temp.u64;
			}
			// The endpoints [r1+116], [r1+112], reloaded by the copy at every entry (no entry lands on them).
			const float a116 = REX_LOAD_D_F32(ll_r1.u32, 116), a112 = REX_LOAD_D_F32(ll_r1.u32, 112);
			uint32_t left = n;
			REX_FLUSH_KNOWN_OFF();
			do {
				REX_LOOP_BARRIER();
				ll_r8.u64 = r11.u64 & 0xFFFFFFFF;
				ll_f13.f64 = double(a116);
				ll_r9.u64 = static_cast<uint64_t>(5) - r11.u64;
				ll_f12.f64 = double(a112);
				r11.s64 = r11.s64 + 1;
				ll_r9.u64 = ll_r9.u64 & 0xFFFFFFFF;
				ll_f11.u64 = ll_r8.u64;  // the copy's store and load of [r1+88]
				ll_f11.f64 = double(ll_f11.s64);
				ll_f10.u64 = ll_r9.u64;  // and of [r1+80]
				ll_f10.f64 = double(ll_f10.s64);
				ll_f11.f64 = double(float(ll_f11.f64));
				ll_f10.f64 = double(float(ll_f10.f64));
				ll_f13.f64 = REX_FMULS(ll_f11.f64, ll_f13.f64);
				ll_f13.f64 = double(float(std::fma(ll_f10.f64, ll_f12.f64, ll_f13.f64)));
				ll_f13.f64 = REX_FMULS(ll_f13.f64, f0.f64);
				ea = 4 + ll_r10.u32;
				temp.f32 = float(ll_f13.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r10.u32 = ea;
			} while (--left != 0);
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
			// The last entry's frame slots, and the cards of the entries' pages (a store's first byte each).
			REX_STORE_D_U64(ll_r1.u32, 88, ll_r8.u64);
			REX_STORE_D_U64(ll_r1.u32, 80, ll_r9.u64);
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(first), 4 * (n - 1) + 1);
#endif
			if (!check4) [[likely]] goto loc_82833A7C_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check4->out[0] = ll_f10.u64;
			check4->out[1] = ll_f11.u64;
			check4->out[2] = ll_f12.u64;
			check4->out[3] = ll_f13.u64;
			check4->out[4] = ll_r8.u64;
			check4->out[5] = ll_r9.u64;
			check4->out[6] = ll_r10.u64;
			check4->out[7] = r11.u64;
			check4->out[8] = ctr.u64;
			check4->out[9] = uint64_t(ea);
			check4->out[10] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check4, base);
			ll_f10.u64 = check4->in[0];
			ll_f11.u64 = check4->in[1];
			ll_f12.u64 = check4->in[2];
			ll_f13.u64 = check4->in[3];
			ll_r8.u64 = check4->in[4];
			ll_r9.u64 = check4->in[5];
			ll_r10.u64 = check4->in[6];
			r11.u64 = check4->in[7];
			ctr.u64 = check4->in[8];
			ea = uint32_t(check4->in[9]);
			temp.u64 = check4->in[10];
		}
	}
loc_82833A7C:
	REX_LOOP_BARRIER();
	ll_r8.u64 = r11.u64 & 0xFFFFFFFF;
	REX_FLUSH_KNOWN_OFF();
	ll_f13.f64 = double(REX_LOAD_D_F32(ll_r1.u32, 116));
	xer.ca = r11.u32 <= 5;
	ll_r9.u64 = static_cast<uint64_t>(5) - r11.u64;
	ll_f12.f64 = double(REX_LOAD_D_F32(ll_r1.u32, 112));
	REX_STORE_D_U64(ll_r1.u32, 88, ll_r8.u64);
	r11.s64 = r11.s64 + 1;
	ll_r9.u64 = ll_r9.u64 & 0xFFFFFFFF;
	REX_STORE_D_U64(ll_r1.u32, 80, ll_r9.u64);
	ll_f11.u64 = REX_LOAD_D_U64(ll_r1.u32, 88);
	ll_f11.f64 = double(ll_f11.s64);
	ll_f10.u64 = REX_LOAD_D_U64(ll_r1.u32, 80);
	ll_f10.f64 = double(ll_f10.s64);
	ll_f11.f64 = double(float(ll_f11.f64));
	ll_f10.f64 = double(float(ll_f10.f64));
	ll_f13.f64 = REX_FMULS(ll_f11.f64, ll_f13.f64);
	ll_f13.f64 = double(float(std::fma(ll_f10.f64, ll_f12.f64, ll_f13.f64)));
	ll_f13.f64 = REX_FMULS(ll_f13.f64, f0.f64);
	ea = 4 + ll_r10.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r10.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82833A7C;
	if (check4) [[unlikely]] {
		uint64_t want_[9];
		want_[0] = ll_f10.u64;
		want_[1] = ll_f11.u64;
		want_[2] = ll_f12.u64;
		want_[3] = ll_f13.u64;
		want_[4] = ll_r8.u64;
		want_[5] = ll_r9.u64;
		want_[6] = ll_r10.u64;
		want_[7] = r11.u64;
		want_[8] = ctr.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check4, base, want_, 9, "sub_82833938 fast alpha palette (4 entries)", checks_);
	}
loc_82833A7C_done:
	REX_LOOP_OUT(ll_f10, l_f10);
	REX_LOOP_OUT(ll_f11, l_f11);
	REX_LOOP_OUT(ll_f12, l_f12);
	REX_LOOP_OUT(ll_f13, l_f13);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	r11.s64 = -2113929216;
	l_r10.s64 = -2113929216;
	fb0.u32 = REX_RO_CFG_U32(0x82001630, 0x0u, REX_LOAD_D_U32(r11.u32, 5680));
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	fb13.u32 = REX_RO_CFG_U32(0x820018B8, 0x3F800000u, REX_LOAD_D_U32(l_r10.u32, 6328));
	l_f13.f64 = rex::ppc::SingleToDoublePure(fb13.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32(l_r1.u32, 136, temp.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb13.u32);
	REX_STORE_D_U32(l_r1.u32, 140, temp.u32);
loc_82833AE0:
	l_r8.u64 = REX_LOAD_D_U8(l_r1.u32, 99);
	l_r10.s64 = 8;
	l_r7.u64 = REX_LOAD_D_U8(l_r1.u32, 100);
	l_r9.s64 = r31.s64 + -4;
	r11.u64 = REX_LOAD_D_U8(l_r1.u32, 98);
	l_r8.u64 = ((uint32_t)(l_r7.u32 << 8) & 0xFF00u) | (l_r8.u64 & 0xFFFFFFFFFFFF00FF);
	l_r8.u64 = l_r8.u32 & 0xFFFF;
	ctr.u64 = l_r10.u64;
	r11.u64 = ((uint32_t)(l_r8.u32 << 8)) | (r11.u64 & 0xFFFFFFFF000000FF);
	REX_LOOP_IN(ll_r1, l_r1);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
	{
		// sa2-nx (2026-10-28): sub_82833938 fast alpha texels (0-7) where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the texels (the copy's counter)
		const uint64_t first = uint32_t(ll_r9.u32 + 16), frame = ll_r1.u32;  // the first alpha word
		if (n != 0 && ::sa2::hot::fast_loop::Flat(first, 16ull * (n - 1) + 4) && ::sa2::hot::fast_loop::Apart(first, 16ull * (n - 1) + 4, frame + 112, 32)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check_lo = new ::sa2::hot::fast_loop::Check{};
				check_lo->Add(uint32_t(first), 16 * (n - 1) + 4);
				::sa2::hot::fast_loop::Begin(*check_lo, base);
				check_lo->in[0] = ll_r8.u64;
				check_lo->in[1] = ll_r9.u64;
				check_lo->in[2] = ll_r10.u64;
				check_lo->in[3] = r11.u64;
				check_lo->in[4] = ctr.u64;
				check_lo->in[5] = f0.u64;
				check_lo->in[6] = uint64_t(ea);
				check_lo->in[7] = temp.u64;
			}
			// The eight entries [r1+112..143], reloaded by the copy at every texel (no texel lands on them).
			float entry[8];
			for (uint32_t k = 0; k < 8; ++k) entry[k] = REX_LOAD_F32(uint32_t(ll_r1.u32 + 112 + 4 * k));
			uint32_t left = n;
			REX_FLUSH_KNOWN_OFF();
			do {
				REX_LOOP_BARRIER();
				ll_r10.u64 = ((uint32_t)(r11.u32 << 2) & 0x1Cu);
				ll_r8.s64 = ll_r1.s64 + 112;
				r11.u64 = (r11.u32 >> 3);
				f0.f64 = double(entry[ll_r10.u32 >> 2]);
				ea = 16 + ll_r9.u32;
				temp.f32 = float(f0.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r9.u32 = ea;
			} while (--left != 0);
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(first), 16 * (n - 1) + 1);  // the pages of the stores' first bytes
#endif
			if (!check_lo) [[likely]] goto loc_82833B04_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check_lo->out[0] = ll_r8.u64;
			check_lo->out[1] = ll_r9.u64;
			check_lo->out[2] = ll_r10.u64;
			check_lo->out[3] = r11.u64;
			check_lo->out[4] = ctr.u64;
			check_lo->out[5] = f0.u64;
			check_lo->out[6] = uint64_t(ea);
			check_lo->out[7] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check_lo, base);
			ll_r8.u64 = check_lo->in[0];
			ll_r9.u64 = check_lo->in[1];
			ll_r10.u64 = check_lo->in[2];
			r11.u64 = check_lo->in[3];
			ctr.u64 = check_lo->in[4];
			f0.u64 = check_lo->in[5];
			ea = uint32_t(check_lo->in[6]);
			temp.u64 = check_lo->in[7];
		}
	}
loc_82833B04:
	REX_LOOP_BARRIER();
	ll_r10.u64 = ((uint32_t)(r11.u32 << 2) & 0x1Cu);
	ll_r8.s64 = ll_r1.s64 + 112;
	r11.u64 = (r11.u32 >> 3);
	REX_FLUSH_KNOWN_OFF();
	f0.f64 = double(REX_LOAD_F32(ll_r10.u32 + ll_r8.u32));
	ea = 16 + ll_r9.u32;
	temp.f32 = float(f0.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r9.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82833B04;
	if (check_lo) [[unlikely]] {
		uint64_t want_[6];
		want_[0] = ll_r8.u64;
		want_[1] = ll_r9.u64;
		want_[2] = ll_r10.u64;
		want_[3] = r11.u64;
		want_[4] = ctr.u64;
		want_[5] = f0.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check_lo, base, want_, 6, "sub_82833938 fast alpha texels (0-7)", checks_);
	}
loc_82833B04_done:
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	l_r8.u64 = REX_LOAD_D_U8(l_r1.u32, 102);
	l_r10.s64 = 8;
	l_r7.u64 = REX_LOAD_D_U8(l_r1.u32, 103);
	l_r9.s64 = r31.s64 + 124;
	r11.u64 = REX_LOAD_D_U8(l_r1.u32, 101);
	l_r8.u64 = ((uint32_t)(l_r7.u32 << 8) & 0xFF00u) | (l_r8.u64 & 0xFFFFFFFFFFFF00FF);
	l_r8.u64 = l_r8.u32 & 0xFFFF;
	ctr.u64 = l_r10.u64;
	r11.u64 = ((uint32_t)(l_r8.u32 << 8)) | (r11.u64 & 0xFFFFFFFF000000FF);
	REX_LOOP_IN(ll_r1, l_r1);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
	{
		// sa2-nx (2026-10-28): sub_82833938 fast alpha texels (8-15) where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the texels (the copy's counter)
		const uint64_t first = uint32_t(ll_r9.u32 + 16), frame = ll_r1.u32;  // the first alpha word
		if (n != 0 && ::sa2::hot::fast_loop::Flat(first, 16ull * (n - 1) + 4) && ::sa2::hot::fast_loop::Apart(first, 16ull * (n - 1) + 4, frame + 112, 32)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check_hi = new ::sa2::hot::fast_loop::Check{};
				check_hi->Add(uint32_t(first), 16 * (n - 1) + 4);
				::sa2::hot::fast_loop::Begin(*check_hi, base);
				check_hi->in[0] = ll_r8.u64;
				check_hi->in[1] = ll_r9.u64;
				check_hi->in[2] = ll_r10.u64;
				check_hi->in[3] = r11.u64;
				check_hi->in[4] = ctr.u64;
				check_hi->in[5] = f0.u64;
				check_hi->in[6] = uint64_t(ea);
				check_hi->in[7] = temp.u64;
			}
			// The eight entries [r1+112..143], reloaded by the copy at every texel (no texel lands on them).
			float entry[8];
			for (uint32_t k = 0; k < 8; ++k) entry[k] = REX_LOAD_F32(uint32_t(ll_r1.u32 + 112 + 4 * k));
			uint32_t left = n;
			REX_FLUSH_KNOWN_OFF();
			do {
				REX_LOOP_BARRIER();
				ll_r10.u64 = ((uint32_t)(r11.u32 << 2) & 0x1Cu);
				ll_r8.s64 = ll_r1.s64 + 112;
				r11.u64 = (r11.u32 >> 3);
				f0.f64 = double(entry[ll_r10.u32 >> 2]);
				ea = 16 + ll_r9.u32;
				temp.f32 = float(f0.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r9.u32 = ea;
			} while (--left != 0);
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(first), 16 * (n - 1) + 1);  // the pages of the stores' first bytes
#endif
			if (!check_hi) [[likely]] goto loc_82833B40_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check_hi->out[0] = ll_r8.u64;
			check_hi->out[1] = ll_r9.u64;
			check_hi->out[2] = ll_r10.u64;
			check_hi->out[3] = r11.u64;
			check_hi->out[4] = ctr.u64;
			check_hi->out[5] = f0.u64;
			check_hi->out[6] = uint64_t(ea);
			check_hi->out[7] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check_hi, base);
			ll_r8.u64 = check_hi->in[0];
			ll_r9.u64 = check_hi->in[1];
			ll_r10.u64 = check_hi->in[2];
			r11.u64 = check_hi->in[3];
			ctr.u64 = check_hi->in[4];
			f0.u64 = check_hi->in[5];
			ea = uint32_t(check_hi->in[6]);
			temp.u64 = check_hi->in[7];
		}
	}
loc_82833B40:
	REX_LOOP_BARRIER();
	ll_r10.u64 = ((uint32_t)(r11.u32 << 2) & 0x1Cu);
	ll_r8.s64 = ll_r1.s64 + 112;
	r11.u64 = (r11.u32 >> 3);
	REX_FLUSH_KNOWN_OFF();
	f0.f64 = double(REX_LOAD_F32(ll_r10.u32 + ll_r8.u32));
	ea = 16 + ll_r9.u32;
	temp.f32 = float(f0.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r9.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82833B40;
	if (check_hi) [[unlikely]] {
		uint64_t want_[6];
		want_[0] = ll_r8.u64;
		want_[1] = ll_r9.u64;
		want_[2] = ll_r10.u64;
		want_[3] = r11.u64;
		want_[4] = ctr.u64;
		want_[5] = f0.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check_hi, base, want_, 6, "sub_82833938 fast alpha texels (8-15)", checks_);
	}
loc_82833B40_done:
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	l_r3.s64 = 0;
loc_82833B5C:
	l_r1.s64 = l_r1.s64 + 176;
	{ do { ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_alpha_block
