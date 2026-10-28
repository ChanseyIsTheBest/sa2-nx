// sa2-nx (2026-10-28): hand-written sub_8214BBD8, the keyframe search (a binary search of a key table by int(f1),
// then the two neighbours and the blend factor). Made from its localized copy (app/generated/hot/n_8214BBD8.h, with
// its limited write-backs), which stays its reference: re-run tools/hand_natives/make_key_search.py after the copy
// changes.
//
// Optimisation: the search loop's branch on the key compare (taken or not about half the time in a binary search: a
// mispredict on the A57 at most steps) as two conditional selects of the new bounds.
//
// Audit: the loop computes the same mid (low32(hi + lo) >> 1), loads the same key and leaves the same lo (r11) and hi
// (l_r6): r11 = mid unless key < table[mid], l_r6 = mid when it is, as the copy's two branches. The loop's exit test
// is the copy's (unsigned hi - lo > 1). The copy's cr6 after the loop is overwritten by the cmplw that follows it
// before any read (loc_8214BC2C), and xer is only read by compare: neither is observable. Every other statement is
// the copy's. The guard compares every register (Cmp::kAll) and the written ranges.
#pragma once


namespace sa2::hot::n_key_search {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_f1 = ctx.f1;
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
#define SA2_SYNC_OUT() do { ctx.f1 = l_f1; ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f1 = ctx.f1; l_f7 = ctx.f7; l_f8 = ctx.f8; l_f9 = ctx.f9; l_f10 = ctx.f10; l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCRegister f0{};
	PPCRegister temp{};
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -16, r30.u64));
	REX_DEAD_SAVE(REX_STORE_D_U64(l_r1.u32, -8, r31.u64));
	r11.s64 = 0;
	l_r6.u64 = l_r5.u64;
	cr6.compare<uint32_t>(l_r5.u32, 1, xer);
	if (!cr6.gt) goto loc_8214BC2C;
	f0.s64 = rex::ppc::fctidz(l_f1.f64);
	REX_STORE_D_U64(l_r1.u32, -32, f0.u64);
	r31.u64 = REX_LOAD_D_U32(l_r1.u32, -28);
loc_8214BBFC:
	REX_LOOP_BARRIER();
	l_r10.u64 = l_r6.u64 + r11.u64;
	l_r10.u64 = (l_r10.u32 >> 1);
	r30.s64 = int64_t(l_r10.s32) * int64_t(l_r4.s32);
	r30.u64 = REX_LOAD_U32(r30.u32 + l_r3.u32);
	{
		// sa2-nx (2026-10-28): the copy's branch on the key compare as two selects (exact: the same new bounds).
		const bool lower = r31.u32 < r30.u32;  // cr6.lt of cmplw r31,r30
		l_r6.u64 = lower ? l_r10.u64 : l_r6.u64;
		r11.u64 = lower ? r11.u64 : l_r10.u64;
	}
	l_r10.u64 = l_r6.u64 - r11.u64;
	if (l_r10.u32 > 1u) goto loc_8214BBFC;  // cr6.gt of cmplwi r10,1
loc_8214BC2C:
	l_r10.s64 = int64_t(r11.s32) * int64_t(l_r4.s32);
	l_r6.s64 = l_r5.s64 + -1;
	l_r10.u64 = l_r10.u64 + l_r3.u64;
	cr6.compare<uint32_t>(r11.u32, l_r6.u32, xer);
	if (!cr6.lt) goto loc_8214BC4C;
	r11.s64 = r11.s64 + 1;
	r11.s64 = int64_t(r11.s32) * int64_t(l_r4.s32);
	l_r3.u64 = r11.u64 + l_r3.u64;
loc_8214BC4C:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 0);
	l_r6.u64 = REX_LOAD_D_U32(l_r3.u32, 0);
	REX_STORE_D_U32_C(l_r7.u32, 0, l_r10.u32);
	l_r10.u64 = l_r6.u64 - r11.u64;
	cr0.compare<int32_t>(l_r10.s32, 0, xer);
	REX_STORE_D_U32_C(l_r8.u32, 0, l_r3.u32);
	if (!cr0.gt) goto loc_8214BCA4;
	l_r10.s64 = l_r10.s32;
	l_r8.u64 = r11.u64 & 0xFFFFFFFF;
	REX_STORE_D_U64(l_r1.u32, -32, l_r10.u64);
	f0.u64 = REX_LOAD_D_U64(l_r1.u32, -32);
	REX_STORE_D_U64(l_r1.u32, -32, l_r8.u64);
	l_f13.u64 = REX_LOAD_D_U64(l_r1.u32, -32);
	l_f11.f64 = double(f0.s64);
	l_f12.f64 = double(l_f13.s64);
	ctx.fpscr.disableFlushMode();
	l_f9.f64 = double(float(l_f11.f64));
	l_f10.f64 = double(float(l_f12.f64));
	l_f8.f64 = double(float(l_f1.f64 - l_f10.f64));
	l_f7.f64 = REX_FDIVS(l_f8.f64, l_f9.f64);
	temp.f32 = float(l_f7.f64);
	REX_STORE_D_U32_C(l_r9.u32, 0, temp.u32);
	r30.u64 = REX_LOAD_D_U64(l_r1.u32, -16);
	r31.u64 = REX_LOAD_D_U64(l_r1.u32, -8);
	{ do { ctx.f7 = l_f7; ctx.f8 = l_f8; ctx.f9 = l_f9; ctx.f10 = l_f10; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r3 = l_r3; ctx.r6 = l_r6; ctx.r8 = l_r8; ctx.r10 = l_r10; } while (0); return; }
loc_8214BCA4:
	r11.u64 = r11.u64 & 0xFFFFFFFF;
	REX_STORE_D_U64(l_r1.u32, -32, r11.u64);
	f0.u64 = REX_LOAD_D_U64(l_r1.u32, -32);
	l_f13.f64 = double(f0.s64);
	ctx.fpscr.disableFlushMode();
	l_f12.f64 = double(float(l_f13.f64));
	l_f11.f64 = double(float(l_f1.f64 - l_f12.f64));
	temp.f32 = float(l_f11.f64);
	REX_STORE_D_U32_C(l_r9.u32, 0, temp.u32);
	r30.u64 = REX_LOAD_D_U64(l_r1.u32, -16);
	r31.u64 = REX_LOAD_D_U64(l_r1.u32, -8);
	{ do { ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r3 = l_r3; ctx.r6 = l_r6; ctx.r10 = l_r10; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_key_search
