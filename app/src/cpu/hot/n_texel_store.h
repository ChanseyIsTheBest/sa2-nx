// sa2-nx (2026-10-28): hand-written sub_82823E08 (0.66 % of the console session; 0.7 % of the game thread in
// gameplay): a span of RGBA float texels into a compressed texture's block cache (the writing sibling of sub_828243A0,
// hot/n_texel_span.h): it loads the span's row of blocks into the cache when needed, scatters the texels into their
// 4x4 blocks' 16-byte slots, and recompresses a block once all four of its rows are written. Made from its localized
// copy (app/generated/hot/n_82823E08.h), which stays its reference: re-run tools/hand_natives/make_texel_scatter.py
// after the copy changes (it checks the loop it replaces).
//
// Optimisation: the copy's scatter loop reloads the cache pointer [r26+4] at every texel, marks a card at each of a
// texel's four stores and runs disableFlushMode at every texel; the fast loop holds the pointer, marks a slot's pages
// once (one or two card stores) and sets the mode once.
//
// Audit: it applies when the slots the span can reach (r11 in [a, a+n), the slot index ((r11 & ~3) | r7) << 2 |
// (r11 & 3), r7 < 4, below 2^28 so its 16x does not wrap: [cache + 64(a & ~3), cache + 64((a+n-1) & ~3) + 256))
// neither wrap at 4 GB nor overlap [r26+4]: then the copy's reloads read what the fast loop holds. Every other
// statement is the copy's, in its order (a texel's four loads and stores interleaved as the copy's), its dead f0
// conversions left out (the last one, the fourth word's, stays). The registers it leaves are the copy's last texel's.
// A slot's four stores' first bytes are [slot, slot + 12]: the same pages, marked after the slot's data. The mode:
// nothing in the loop changes it. Otherwise the copy's loop runs.
//
// Unguarded like its bulk copy; in verify mode (sa2_cpu_verify) the fast loop checks itself against the copy's
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

REX_EXTERN(__imp__sub_82807380);
REX_EXTERN(__imp__sub_828074B8);
REX_EXTERN(__imp__sub_8281A348);
REX_EXTERN(__imp__sub_82862DF0);
REX_EXTERN(sub_827EFEA0);
REX_EXTERN(sub_82833B68);
REX_EXTERN(sub_828345B0);

namespace sa2::hot::n_texel_store {

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
	REX_LOOP_LOCAL(PPCRegister, ll_r1, l_r1);
	REX_LOOP_LOCAL(PPCRegister, ll_r3, l_r3);
	REX_LOOP_LOCAL(PPCRegister, ll_r4, l_r4);
	REX_LOOP_LOCAL(PPCRegister, ll_r5, l_r5);
	REX_LOOP_LOCAL(PPCRegister, ll_r6, l_r6);
	REX_LOOP_LOCAL(PPCRegister, ll_r7, l_r7);
	REX_LOOP_LOCAL(PPCRegister, ll_r8, l_r8);
	REX_LOOP_LOCAL(PPCRegister, ll_r9, l_r9);
	REX_LOOP_LOCAL(PPCRegister, ll_r10, l_r10);
	PPCRegister ctr{};
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister r21{};
	PPCRegister r22{};
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
	PPCRegister temp{};
	PPCRegister fb0{};
	uint32_t ea{};
	::sa2::hot::fast_loop::Check* check = nullptr;  // verify mode only
	r12.u64 = ctx.lr;
	ea = -192 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	r11.u64 = REX_LOAD_D_U32(l_r3.u32, 76);
	r31.u64 = l_r3.u64;
	l_r10.u64 = REX_LOAD_D_U32(l_r3.u32, 248);
	r23.u64 = l_r6.u64;
	r25.u64 = r11.u64 + l_r5.u64;
	r11.u64 = REX_LOAD_D_U32(l_r3.u32, 64);
	cr6.compare<uint32_t>(l_r10.u32, 0, xer);
	r27.u64 = l_r4.u64 + r11.u64;
	if (!cr6.eq) goto loc_82823E74;
	r11.u64 = REX_LOAD_D_U32(l_r3.u32, 228);
	l_r4.s64 = 612433920;
	l_r10.u64 = REX_LOAD_D_U32(l_r3.u32, 224);
	r11.s64 = int64_t(l_r10.s32) * int64_t(r11.s32);
	l_r3.u64 = ((uint32_t)(r11.u32 << 3));
	{ SA2_SYNC_OUT(); __imp__sub_82807380(ctx, base); SA2_SYNC_IN(); }
	REX_STORE_D_U32_C(r31.u32, 248, l_r3.u32);
	cr0.compare<uint32_t>(l_r3.u32, 0, xer);
	if (cr0.eq) goto loc_82824398;
	r11.u64 = REX_LOAD_D_U32(r31.u32, 224);
	l_r4.s64 = 0;
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 228);
	r11.s64 = int64_t(r11.s32) * int64_t(l_r10.s32);
	l_r5.u64 = ((uint32_t)(r11.u32 << 3));
	{ ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; sub_827EFEA0(ctx, base); l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; }
loc_82823E74:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 212);
	r22.s64 = 0;
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 200);
	l_r8.u64 = REX_LOAD_D_U32(r31.u32, 224);
	r11.u64 = r25.u64 - r11.u64;
	l_r7.u64 = r27.u64 - l_r10.u64;
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 248);
	l_r10.s64 = int64_t(r11.s32) * int64_t(l_r8.s32);
	r11.u64 = (l_r7.u32 >> 2);
	r11.u64 = l_r10.u64 + r11.u64;
	r11.u64 = ((uint32_t)(r11.u32 << 3));
	r26.u64 = r11.u64 + l_r9.u64;
	r11.u64 = REX_LOAD_D_U32(r26.u32, 4);
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	if (!cr6.eq) goto loc_82823EDC;
	r11.u64 = REX_LOAD_D_U32(r31.u32, 220);
	l_r4.s64 = 612433920;
	l_r3.u64 = ((uint32_t)(r11.u32 << 8));
	{ SA2_SYNC_OUT(); __imp__sub_82807380(ctx, base); SA2_SYNC_IN(); }
	REX_STORE_D_U32_C(r26.u32, 4, l_r3.u32);
	cr0.compare<uint32_t>(l_r3.u32, 0, xer);
	if (cr0.eq) goto loc_82824398;
	REX_STORE_D_U32_C(r26.u32, 0, r22.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 244);
	r11.s64 = r11.s64 + 1;
	REX_STORE_D_U32_C(r31.u32, 244, r11.u32);
loc_82823EDC:
	r11.u64 = REX_LOAD_D_U32(r26.u32, 0);
	r21.s64 = 1;
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	if (!cr6.eq) goto loc_8282409C;
	r11.u64 = REX_LOAD_D_U32(r31.u32, 64);
	r24.u64 = (r27.u32 & 0xFFFFFFFCu);
	l_r10.s64 = r24.s64 + 4;
	cr6.compare<uint32_t>(r24.u32, r11.u32, xer);
	if (!cr6.lt) goto loc_82823F0C;
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 152);
	cr6.compare<uint32_t>(r11.u32, l_r9.u32, xer);
	if (cr6.gt) goto loc_82823F24;
loc_82823F0C:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 72);
	cr6.compare<uint32_t>(l_r10.u32, r11.u32, xer);
	if (!cr6.gt) goto loc_82823FA0;
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 160);
	cr6.compare<uint32_t>(r11.u32, l_r10.u32, xer);
	if (!cr6.lt) goto loc_82823FA0;
loc_82823F24:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 196);
	l_r10.u64 = (r27.u32 >> 2);
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 96);
	l_r7.u64 = REX_LOAD_D_U32(r31.u32, 136);
	l_r6.u64 = (r11.u32 >> 2);
	l_r8.s64 = int64_t(l_r10.s32) * int64_t(l_r9.s32);
	l_r5.u64 = REX_LOAD_D_U32(r31.u32, 100);
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 32);
	l_r4.u64 = REX_LOAD_D_U32(r31.u32, 204);
	r29.u64 = REX_LOAD_D_U32(r26.u32, 4);
	l_r10.s64 = int64_t(l_r6.s32) * int64_t(l_r7.s32);
	l_r10.u64 = l_r8.u64 + l_r10.u64;
	l_r8.s64 = int64_t(l_r5.s32) * int64_t(r25.s32);
	l_r10.u64 = l_r10.u64 + l_r8.u64;
	r30.u64 = r11.u64;
	r28.u64 = l_r10.u64 + l_r9.u64;
	cr6.compare<uint32_t>(r11.u32, l_r4.u32, xer);
	if (!cr6.lt) goto loc_8282405C;
loc_82823F6C:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 140);
	l_r4.u64 = r28.u64;
	l_r3.u64 = r29.u64;
	ctr.u64 = r11.u64;
	{ SA2_SYNC_OUT(); REX_CALL_INDIRECT_FUNC_AT(0x82823E08, 0, ctr.u32); SA2_SYNC_IN(); }
	r11.u64 = REX_LOAD_D_U32(r31.u32, 136);
	r30.s64 = r30.s64 + 4;
	r29.s64 = r29.s64 + 256;
	r28.u64 = r11.u64 + r28.u64;
	r11.u64 = REX_LOAD_D_U32(r31.u32, 204);
	cr6.compare<uint32_t>(r30.u32, r11.u32, xer);
	if (cr6.lt) goto loc_82823F6C;
	goto loc_8282405C;
loc_82823FA0:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 196);
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 60);
	cr6.compare<uint32_t>(r11.u32, l_r10.u32, xer);
	if (!cr6.lt) goto loc_82823FFC;
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 148);
	cr6.compare<uint32_t>(l_r10.u32, l_r9.u32, xer);
	if (!cr6.gt) goto loc_82823FFC;
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 136);
	r11.u64 = (r11.u32 >> 2);
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 96);
	l_r8.u64 = (r27.u32 >> 2);
	r11.s64 = int64_t(r11.s32) * int64_t(l_r10.s32);
	l_r7.u64 = REX_LOAD_D_U32(r31.u32, 100);
	l_r6.u64 = REX_LOAD_D_U32(r31.u32, 140);
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 32);
	l_r3.u64 = REX_LOAD_D_U32(r26.u32, 4);
	ctr.u64 = l_r6.u64;
	l_r9.s64 = int64_t(l_r8.s32) * int64_t(l_r9.s32);
	r11.u64 = r11.u64 + l_r9.u64;
	l_r9.s64 = int64_t(l_r7.s32) * int64_t(r25.s32);
	r11.u64 = r11.u64 + l_r9.u64;
	l_r4.u64 = r11.u64 + l_r10.u64;
	{ SA2_SYNC_OUT(); REX_CALL_INDIRECT_FUNC_AT(0x82823E08, 1, ctr.u32); SA2_SYNC_IN(); }
loc_82823FFC:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 204);
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 68);
	cr6.compare<uint32_t>(r11.u32, l_r10.u32, xer);
	if (!cr6.gt) goto loc_8282405C;
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 156);
	cr6.compare<uint32_t>(l_r10.u32, l_r9.u32, xer);
	if (!cr6.lt) goto loc_8282405C;
	r11.s64 = r11.s64 + -4;
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 96);
	l_r8.u64 = REX_LOAD_D_U32(r31.u32, 136);
	l_r9.u64 = (r27.u32 >> 2);
	r11.u64 = (r11.u32 >> 2);
	l_r7.u64 = REX_LOAD_D_U32(r31.u32, 100);
	l_r9.s64 = int64_t(l_r9.s32) * int64_t(l_r10.s32);
	l_r6.u64 = REX_LOAD_D_U32(r31.u32, 140);
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 32);
	l_r3.u64 = REX_LOAD_D_U32(r26.u32, 4);
	ctr.u64 = l_r6.u64;
	r11.s64 = int64_t(r11.s32) * int64_t(l_r8.s32);
	r11.u64 = r11.u64 + l_r9.u64;
	l_r9.s64 = int64_t(l_r7.s32) * int64_t(r25.s32);
	r11.u64 = r11.u64 + l_r9.u64;
	l_r4.u64 = r11.u64 + l_r10.u64;
	{ SA2_SYNC_OUT(); REX_CALL_INDIRECT_FUNC_AT(0x82823E08, 2, ctr.u32); SA2_SYNC_IN(); }
loc_8282405C:
	l_r10.s64 = 4;
	r11.u64 = r22.u64;
	ctr.u64 = l_r10.u64;
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
loc_82824068:
	REX_LOOP_BARRIER();
	ll_r9.u64 = REX_LOAD_D_U32(r31.u32, 64);
	ll_r10.u64 = r11.u64 + r24.u64;
	cr6.compare<uint32_t>(ll_r10.u32, ll_r9.u32, xer);
	if (cr6.lt) goto loc_82824084;
	ll_r9.u64 = REX_LOAD_D_U32(r31.u32, 72);
	cr6.compare<uint32_t>(ll_r10.u32, ll_r9.u32, xer);
	if (cr6.lt) goto loc_82824094;
loc_82824084:
	ll_r10.u64 = REX_LOAD_D_U32(r26.u32, 0);
	ll_r9.u64 = r11.u8 & 0x20 ? 0 : (r21.u32 << (r11.u8 & 0x3F));
	ll_r10.u64 = ll_r9.u64 | ll_r10.u64;
	REX_STORE_D_U32_C(r26.u32, 0, ll_r10.u32);
loc_82824094:
	r11.s64 = r11.s64 + 1;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82824068;
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
loc_8282409C:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 84);
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	if (cr6.eq) goto loc_828240B8;
	l_r4.u64 = r23.u64;
	l_r3.u64 = r31.u64;
	{ ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; __imp__sub_82862DF0(ctx, base); l_r3 = ctx.r3; l_r4 = ctx.r4; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	r23.u64 = l_r3.u64;
loc_828240B8:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 16);
	cr6.compare<int32_t>(r11.s32, 0, xer);
	if (cr6.eq) goto loc_828240D4;
	l_r4.u64 = r23.u64;
	l_r3.u64 = r31.u64;
	{ ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; __imp__sub_8281A348(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; }
	r23.u64 = l_r3.u64;
loc_828240D4:
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 196);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 60);
	l_r8.u64 = REX_LOAD_D_U32(r31.u32, 200);
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 104);
	r11.u64 = r11.u64 - l_r9.u64;
	l_r9.u64 = r27.u64 - l_r8.u64;
	l_r10.u64 = l_r10.u64 + r11.u64;
	l_r7.u64 = l_r9.u32 & 0x3;
	cr6.compare<uint32_t>(r11.u32, l_r10.u32, xer);
	if (!cr6.lt) goto loc_8282414C;
	l_r9.u64 = l_r10.u64 - r11.u64;
	l_r10.s64 = r23.s64 + -4;
	ctr.u64 = l_r9.u64;
	REX_LOOP_IN(ll_r6, l_r6);
	REX_LOOP_IN(ll_r7, l_r7);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
	{
		// sa2-nx (2026-10-28): sub_82823E08 fast texel scatter where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;                            // the texels (the copy's counter)
		const uint32_t cache = REX_LOAD_D_U32(r26.u32, 4);     // the block cache
		// The texels' slots: 16 bytes at cache + 16 * (((r11 & ~3) | r7) << 2 | (r11 & 3)) for r11 in [a, a + n):
		// within [cache + 64 * (a & ~3), cache + 64 * ((a + n - 1) & ~3) + 256) while the index stays below 2^28.
		const uint64_t a = r11.u32, last = a + n - 1;
		const uint64_t lo = uint64_t(cache) + 64 * (a & ~3ull), hi = uint64_t(cache) + 64 * (last & ~3ull) + 256;
		if (n != 0 && last < 0xFFFFFFFFull && 4 * (last & ~3ull) + 15 < (1ull << 28) && (r11.u64 >> 32) == 0 && ll_r7.u64 < 4 && hi <= 0x100000000ull && ::sa2::hot::fast_loop::Apart(lo, hi - lo, r26.u32 + 4ull, 4)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check = new ::sa2::hot::fast_loop::Check{};
				check->Add(uint32_t(lo), uint32_t(hi - lo));
				::sa2::hot::fast_loop::Begin(*check, base);
				check->in[0] = ll_r6.u64;
				check->in[1] = ll_r8.u64;
				check->in[2] = ll_r9.u64;
				check->in[3] = ll_r10.u64;
				check->in[4] = r11.u64;
				check->in[5] = ctr.u64;
				check->in[6] = f0.u64;
				check->in[7] = fb0.u64;
				check->in[8] = uint64_t(ea);
				check->in[9] = temp.u64;
			}
			uint32_t left = n;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				ll_r9.u64 = (r11.u32 & 0xFFFFFFFCu);
				ll_r8.u64 = cache;
				ll_r6.u64 = r11.u64;
				fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 4);
				ll_r9.u64 = ll_r9.u64 | ll_r7.u64;
				r11.s64 = r11.s64 + 1;
				ll_r6.u64 = ((uint32_t)(ll_r9.u32 << 2)) | (ll_r6.u64 & 0xFFFFFFFF00000003);
				ll_r9.u64 = ((uint32_t)(ll_r6.u32 << 4));
				ll_r9.u64 = ll_r9.u64 + ll_r8.u64;
				// The copy's four loads and stores in its order (a slot over the span reads what it reads).
				REX_STORE_D_U32(ll_r9.u32, 0, rex::ppc::QuietSingleBits(fb0.u32));
				REX_STORE_D_U32(ll_r9.u32, 4, rex::ppc::QuietSingleBits(REX_LOAD_D_U32(ll_r10.u32, 8)));
				REX_STORE_D_U32(ll_r9.u32, 8, rex::ppc::QuietSingleBits(REX_LOAD_D_U32(ll_r10.u32, 12)));
				ea = 16 + ll_r10.u32;
				f0.f64 = double(REX_LOAD_F32(ea));
				ll_r10.u32 = ea;
				temp.f32 = float(f0.f64);
				REX_STORE_D_U32(ll_r9.u32, 12, temp.u32);
#if !defined(REX_STORE_CARDS_OFF)
				Card(base, ll_r9.u32, 13);  // the slot's four stores: the pages of [slot, slot + 12]
#endif
			} while (--left != 0);
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
			if (!check) [[likely]] goto loc_82824108_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check->out[0] = ll_r6.u64;
			check->out[1] = ll_r8.u64;
			check->out[2] = ll_r9.u64;
			check->out[3] = ll_r10.u64;
			check->out[4] = r11.u64;
			check->out[5] = ctr.u64;
			check->out[6] = f0.u64;
			check->out[7] = fb0.u64;
			check->out[8] = uint64_t(ea);
			check->out[9] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check, base);
			ll_r6.u64 = check->in[0];
			ll_r8.u64 = check->in[1];
			ll_r9.u64 = check->in[2];
			ll_r10.u64 = check->in[3];
			r11.u64 = check->in[4];
			ctr.u64 = check->in[5];
			f0.u64 = check->in[6];
			fb0.u64 = check->in[7];
			ea = uint32_t(check->in[8]);
			temp.u64 = check->in[9];
		}
	}
loc_82824108:
	REX_LOOP_BARRIER();
	ll_r9.u64 = (r11.u32 & 0xFFFFFFFCu);
	ll_r8.u64 = REX_LOAD_D_U32(r26.u32, 4);
	ll_r6.u64 = r11.u64;
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 4);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	ll_r9.u64 = ll_r9.u64 | ll_r7.u64;
	r11.s64 = r11.s64 + 1;
	ll_r6.u64 = ((uint32_t)(ll_r9.u32 << 2)) | (ll_r6.u64 & 0xFFFFFFFF00000003);
	ll_r9.u64 = ((uint32_t)(ll_r6.u32 << 4));
	ll_r9.u64 = ll_r9.u64 + ll_r8.u64;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r9.u32, 0, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r9.u32, 4, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 12);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r9.u32, 8, temp.u32);
	ctx.fpscr.disableFlushMode();
	ea = 16 + ll_r10.u32;
	f0.f64 = double(REX_LOAD_F32(ea));
	ll_r10.u32 = ea;
	temp.f32 = float(f0.f64);
	REX_STORE_D_U32_C(ll_r9.u32, 12, temp.u32);
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82824108;
	if (check) [[unlikely]] {
		uint64_t want_[7];
		want_[0] = ll_r6.u64;
		want_[1] = ll_r8.u64;
		want_[2] = ll_r9.u64;
		want_[3] = ll_r10.u64;
		want_[4] = r11.u64;
		want_[5] = ctr.u64;
		want_[6] = f0.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check, base, want_, 7, "sub_82823E08 fast texel scatter", checks_);
	}
loc_82824108_done:
	REX_LOOP_OUT(ll_r6, l_r6);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
loc_8282414C:
	r11.u64 = r27.u32 & 0x3;
	l_r10.u64 = REX_LOAD_D_U32(r26.u32, 0);
	r11.u64 = r11.u8 & 0x20 ? 0 : (r21.u32 << (r11.u8 & 0x3F));
	r11.u64 = r11.u64 | l_r10.u64;
	REX_STORE_D_U32_C(r26.u32, 0, r11.u32);
	cr6.compare<uint32_t>(r11.u32, 15, xer);
	if (!cr6.eq) goto loc_82824398;
	r11.u64 = REX_LOAD_D_U32(r31.u32, 196);
	l_r10.u64 = (r27.u32 >> 2);
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 96);
	l_r8.u64 = REX_LOAD_D_U32(r31.u32, 136);
	l_r7.u64 = (r11.u32 >> 2);
	l_r10.s64 = int64_t(l_r10.s32) * int64_t(l_r9.s32);
	l_r6.u64 = REX_LOAD_D_U32(r31.u32, 100);
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 32);
	l_r5.u64 = REX_LOAD_D_U32(r31.u32, 204);
	r30.u64 = REX_LOAD_D_U32(r26.u32, 4);
	l_r8.s64 = int64_t(l_r7.s32) * int64_t(l_r8.s32);
	l_r10.u64 = l_r10.u64 + l_r8.u64;
	l_r8.s64 = int64_t(l_r6.s32) * int64_t(r25.s32);
	l_r10.u64 = l_r10.u64 + l_r8.u64;
	r29.u64 = r11.u64;
	r28.u64 = l_r10.u64 + l_r9.u64;
	cr6.compare<uint32_t>(r11.u32, l_r5.u32, xer);
	if (!cr6.lt) goto loc_82824330;
	REX_STORE_D_U32(l_r1.u32, 80, r22.u32);
	r27.u64 = (r27.u32 & 0xFFFFFFFCu);
	REX_STORE_D_U32(l_r1.u32, 84, r22.u32);
	REX_STORE_D_U32(l_r1.u32, 88, r22.u32);
	REX_STORE_D_U32(l_r1.u32, 92, r21.u32);
loc_828241C4:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U32(r31.u32, 156);
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 160);
	l_r6.u64 = r11.u64 - r29.u64;
	l_r4.u64 = l_r10.u64 - r27.u64;
	cr6.compare<uint32_t>(l_r6.u32, 4, xer);
	if (!cr6.lt) goto loc_82824260;
	l_r5.u64 = r22.u64;
	cr6.compare<uint32_t>(l_r4.u32, 0, xer);
	if (cr6.eq) goto loc_82824260;
	REX_LOOP_IN(ll_r1, l_r1);
	REX_LOOP_IN(ll_r3, l_r3);
	REX_LOOP_IN(ll_r4, l_r4);
	REX_LOOP_IN(ll_r5, l_r5);
	REX_LOOP_IN(ll_r6, l_r6);
	REX_LOOP_IN(ll_r7, l_r7);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
loc_828241E8:
	REX_LOOP_BARRIER();
	cr6.compare<uint32_t>(ll_r5.u32, 4, xer);
	if (!cr6.lt) goto loc_F0000001;
	ll_r9.s64 = ll_r1.s64 + 80;
	ll_r10.u64 = ((uint32_t)(ll_r6.u32 << 2));
	xer.ca = ll_r6.u32 <= 4;
	r11.u64 = static_cast<uint64_t>(4) - ll_r6.u64;
	ll_r10.u64 = ll_r10.u64 + ll_r9.u64;
	ll_r9.u64 = ll_r6.u64;
	ll_r7.u64 = ((uint32_t)(ll_r5.u32 << 2));
	ll_r8.s64 = ll_r10.s64 + -4;
	ctr.u64 = r11.u64;
loc_82824210:
	REX_LOOP_BARRIER();
	ea = 4 + ll_r8.u32;
	r11.u64 = REX_LOAD_U32(ea);
	ll_r8.u32 = ea;
	ll_r10.u64 = ll_r7.u64 | ll_r9.u64;
	ll_r9.s64 = ll_r9.s64 + 1;
	ll_r3.u64 = ll_r7.u64 | r11.u64;
	r11.u64 = ((uint32_t)(ll_r10.u32 << 4));
	ll_r10.u64 = ((uint32_t)(ll_r3.u32 << 4));
	r11.u64 = r11.u64 + r30.u64;
	ll_r10.u64 = ll_r10.u64 + r30.u64;
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 0);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(r11.u32, 0, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 4);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(r11.u32, 4, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(r11.u32, 8, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(ll_r10.u32, 12);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(r11.u32, 12, temp.u32);
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82824210;
	ll_r5.s64 = ll_r5.s64 + 1;
	cr6.compare<uint32_t>(ll_r5.u32, ll_r4.u32, xer);
	if (cr6.lt) goto loc_828241E8;
	REX_LOOP_OUT(ll_r3, l_r3);
	REX_LOOP_OUT(ll_r5, l_r5);
	REX_LOOP_OUT(ll_r7, l_r7);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	goto loc_F0000002;
loc_F0000001:
	REX_LOOP_OUT(ll_r3, l_r3);
	REX_LOOP_OUT(ll_r5, l_r5);
	REX_LOOP_OUT(ll_r7, l_r7);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
	goto loc_82824260;
loc_F0000002:
loc_82824260:
	cr6.compare<uint32_t>(l_r4.u32, 4, xer);
	if (!cr6.lt) goto loc_828242E0;
	r11.u64 = ((uint32_t)(l_r4.u32 << 2));
	l_r10.s64 = l_r1.s64 + 80;
	l_r6.u64 = l_r4.u64;
	l_r5.u64 = r11.u64 + l_r10.u64;
	REX_LOOP_IN(ll_r5, l_r5);
	REX_LOOP_IN(ll_r6, l_r6);
	REX_LOOP_IN(ll_r7, l_r7);
	REX_LOOP_IN(ll_r8, l_r8);
	REX_LOOP_IN(ll_r9, l_r9);
	REX_LOOP_IN(ll_r10, l_r10);
loc_82824278:
	REX_LOOP_BARRIER();
	r11.s64 = 4;
	ll_r10.u64 = REX_LOAD_D_U32(ll_r5.u32, 0);
	ll_r9.u64 = r22.u64;
	ll_r8.u64 = ((uint32_t)(ll_r10.u32 << 2));
	ll_r7.u64 = ((uint32_t)(ll_r6.u32 << 2));
	ctr.u64 = r11.u64;
loc_82824290:
	REX_LOOP_BARRIER();
	r11.u64 = ll_r8.u64 | ll_r9.u64;
	ll_r10.u64 = ll_r9.u64 | ll_r7.u64;
	r11.u64 = ((uint32_t)(r11.u32 << 4));
	ll_r10.u64 = ((uint32_t)(ll_r10.u32 << 4));
	r11.u64 = r11.u64 + r30.u64;
	ll_r10.u64 = ll_r10.u64 + r30.u64;
	ll_r9.s64 = ll_r9.s64 + 1;
	fb0.u32 = REX_LOAD_D_U32(r11.u32, 0);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r10.u32, 0, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(r11.u32, 4);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r10.u32, 4, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(r11.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r10.u32, 8, temp.u32);
	fb0.u32 = REX_LOAD_D_U32(r11.u32, 12);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r10.u32, 12, temp.u32);
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82824290;
	ll_r6.s64 = ll_r6.s64 + 1;
	ll_r5.s64 = ll_r5.s64 + 4;
	cr6.compare<uint32_t>(ll_r6.u32, 4, xer);
	if (cr6.lt) goto loc_82824278;
	REX_LOOP_OUT(ll_r5, l_r5);
	REX_LOOP_OUT(ll_r6, l_r6);
	REX_LOOP_OUT(ll_r7, l_r7);
	REX_LOOP_OUT(ll_r8, l_r8);
	REX_LOOP_OUT(ll_r9, l_r9);
	REX_LOOP_OUT(ll_r10, l_r10);
loc_828242E0:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 252);
	cr6.compare<int32_t>(r11.s32, 0, xer);
	if (!cr6.eq) goto loc_828242FC;
	r11.u64 = REX_LOAD_D_U32(r31.u32, 92);
	l_r5.u64 = r22.u64;
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	if (cr6.eq) goto loc_82824300;
loc_828242FC:
	l_r5.u64 = r21.u64;
loc_82824300:
	r11.u64 = REX_LOAD_D_U32(r31.u32, 144);
	l_r4.u64 = r30.u64;
	l_r3.u64 = r28.u64;
	ctr.u64 = r11.u64;
	if (REX_PROMOTED_IS(ctr.u32, 0x828345B0u)) [[likely]] { REX_PROMOTED_CALL(0x828345B0, sub_828345B0); { SA2_SYNC_OUT(); sub_828345B0(ctx, base); SA2_SYNC_IN(); } } else if (REX_PROMOTED_IS(ctr.u32, 0x82833B68u)) { REX_PROMOTED_CALL(0x82833B68, sub_82833B68); { SA2_SYNC_OUT(); sub_82833B68(ctx, base); SA2_SYNC_IN(); } } else { { SA2_SYNC_OUT(); REX_CALL_INDIRECT_FUNC_AT(0x82823E08, 3, ctr.u32); SA2_SYNC_IN(); } }
	r11.u64 = REX_LOAD_D_U32(r31.u32, 136);
	r29.s64 = r29.s64 + 4;
	r30.s64 = r30.s64 + 256;
	r28.u64 = r11.u64 + r28.u64;
	r11.u64 = REX_LOAD_D_U32(r31.u32, 204);
	cr6.compare<uint32_t>(r29.u32, r11.u32, xer);
	if (cr6.lt) goto loc_828241C4;
loc_82824330:
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 228);
	r11.s64 = r26.s64 + 8;
	l_r8.u64 = REX_LOAD_D_U32(r31.u32, 224);
	l_r9.u64 = REX_LOAD_D_U32(r31.u32, 248);
	l_r10.s64 = int64_t(l_r10.s32) * int64_t(l_r8.s32);
	l_r10.u64 = ((uint32_t)(l_r10.u32 << 3));
	l_r10.u64 = l_r10.u64 + l_r9.u64;
	cr6.compare<uint32_t>(r11.u32, l_r10.u32, xer);
	if (!cr6.lt) goto loc_8282437C;
	r11.u64 = REX_LOAD_D_U32(r11.u32, 0);
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	if (!cr6.eq) goto loc_8282437C;
	r11.u64 = REX_LOAD_D_U32(r26.u32, 12);
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	if (!cr6.eq) goto loc_8282437C;
	r11.u64 = REX_LOAD_D_U32(r26.u32, 4);
	REX_STORE_D_U32_C(r26.u32, 4, r22.u32);
	REX_STORE_D_U32_C(r26.u32, 12, r11.u32);
	goto loc_82824394;
loc_8282437C:
	l_r4.s64 = 612433920;
	l_r3.u64 = REX_LOAD_D_U32(r26.u32, 4);
	{ SA2_SYNC_OUT(); __imp__sub_828074B8(ctx, base); SA2_SYNC_IN(); }
	r11.u64 = REX_LOAD_D_U32(r31.u32, 244);
	r11.s64 = r11.s64 + -1;
	REX_STORE_D_U32_C(r31.u32, 244, r11.u32);
loc_82824394:
	REX_STORE_D_U32_C(r26.u32, 4, r22.u32);
loc_82824398:
	l_r1.s64 = l_r1.s64 + 192;
	{ SA2_SYNC_OUT(); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_texel_store
