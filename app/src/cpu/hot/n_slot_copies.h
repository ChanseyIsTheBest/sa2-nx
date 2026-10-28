// sa2-nx (2026-10-28): hand-written sub_82139750 (0.47 % of the console session): for each of six flagged slots
// (flags at [r5-8 .. r5+12]), copies the slot's five words from r9 to the next 20-byte record at r10+384 and counts it
// in [r10+624]. Made from its localized copy (app/generated/hot/n_82139750.h), which stays its reference: re-run 
// tools/hand_natives/make_slot_copies.py after the copy changes (it checks the six loops it replaces).
//
// Optimisation: each copy loop moves a word at a time (a byte-swapped load and store and a card mark each); the fast
// copy is one memcpy of the bytes and the cards of the record once.
//
// Audit: a fast copy applies when its source and destination ranges (n words from r6+4 and r8+4) are apart and each
// is one host range without a 4 GB wrap: then the copy's forward word copy (a word stored as loaded, unchanged by the
// two byte swaps) is a memcpy, its last loaded word is the source's last (not written), and r6 / r8 / ea / ctr end as
// its last iteration leaves them (r6 and r8 by their low halves, as the copy's 32-bit updates). The cards are the
// pages of the stores' first bytes (four bytes apart over the record), zeroed after the data. Otherwise the copy's loop
// runs. The count's reload and store after each copy are the copy's.
//
// Unguarded like its bulk copy; in verify mode (sa2_cpu_verify) each fast copy checks itself against the copy's loop
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

#include <cstring>


namespace sa2::hot::n_slot_copies {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_r4 = ctx.r4;
  PPCRegister l_r5 = ctx.r5;
  PPCRegister l_r6 = ctx.r6;
  PPCRegister l_r7 = ctx.r7;
  PPCRegister l_r8 = ctx.r8;
  PPCRegister l_r9 = ctx.r9;
  PPCRegister l_r10 = ctx.r10;
#define SA2_SYNC_OUT() do { ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	PPCRegister ctr{};
	PPCXERRegister xer{};
	PPCCRRegister cr0{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	uint32_t ea{};
	::sa2::hot::fast_loop::Check* check6 = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check5 = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check4 = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check3 = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check2 = nullptr;  // verify mode only
	::sa2::hot::fast_loop::Check* check1 = nullptr;  // verify mode only
	l_r10.s64 = -2088435712;
	r11.s64 = 0;
	l_r10.s64 = l_r10.s64 + 13312;
	l_r4.s64 = 2;
	l_r5.s64 = l_r10.s64 + 8;
	l_r9.s64 = l_r10.s64 + 148;
	REX_STORE_D_U32_C(l_r10.u32, 624, r11.u32);
loc_8213976C:
	REX_LOOP_BARRIER();
	l_r8.u64 = REX_LOAD_D_U32(l_r5.u32, -8);
	cr6.compare<int32_t>(l_r8.s32, 0, xer);
	if (cr6.eq) goto loc_821397B4;
	l_r8.u64 = ((uint32_t)(r11.u32 << 2));
	l_r7.s64 = l_r10.s64 + 384;
	l_r8.u64 = r11.u64 + l_r8.u64;
	r11.s64 = 5;
	l_r8.u64 = ((uint32_t)(l_r8.u32 << 2));
	l_r6.s64 = l_r9.s64 + -24;
	l_r8.u64 = l_r8.u64 + l_r7.u64;
	l_r8.s64 = l_r8.s64 + -4;
	ctr.u64 = r11.u64;
	{
		// sa2-nx (2026-10-28): sub_82139750 fast slot copy 1 where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the words (the copy's counter)
		const uint64_t from = uint32_t(l_r6.u32 + 4), to = uint32_t(l_r8.u32 + 4), bytes = 4ull * n;
		if (n != 0 && ::sa2::hot::fast_loop::Flat(from, bytes) && ::sa2::hot::fast_loop::Flat(to, bytes) && ::sa2::hot::fast_loop::Apart(from, bytes, to, bytes)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check1 = new ::sa2::hot::fast_loop::Check{};
				check1->Add(uint32_t(to), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check1, base);
				check1->in[0] = l_r6.u64;
				check1->in[1] = l_r8.u64;
				check1->in[2] = r11.u64;
				check1->in[3] = ctr.u64;
				check1->in[4] = uint64_t(ea);
			}
			// Apart: the copy's word-by-word forward copy is a memcpy of the bytes (each word stored as loaded).
			std::memcpy(Raw(base, uint32_t(to)), Raw(base, uint32_t(from)), size_t(bytes));
			ea = uint32_t(to + bytes - 4);
			r11.u64 = REX_LOAD_U32(uint32_t(from + bytes - 4));  // the last word (the source is not written)
			l_r6.u32 = uint32_t(from + bytes - 4);
			l_r8.u32 = ea;
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(to), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check1) [[likely]] goto loc_8213979C_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check1->out[0] = l_r6.u64;
			check1->out[1] = l_r8.u64;
			check1->out[2] = r11.u64;
			check1->out[3] = ctr.u64;
			check1->out[4] = uint64_t(ea);
			::sa2::hot::fast_loop::Swap(*check1, base);
			l_r6.u64 = check1->in[0];
			l_r8.u64 = check1->in[1];
			r11.u64 = check1->in[2];
			ctr.u64 = check1->in[3];
			ea = uint32_t(check1->in[4]);
		}
	}
loc_8213979C:
	REX_LOOP_BARRIER();
	ea = 4 + l_r6.u32;
	r11.u64 = REX_LOAD_U32(ea);
	l_r6.u32 = ea;
	ea = 4 + l_r8.u32;
	REX_STORE_U32_C(ea, r11.u32);
	l_r8.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_8213979C;
	if (check1) [[unlikely]] {
		uint64_t want_[5];
		want_[0] = l_r6.u64;
		want_[1] = l_r8.u64;
		want_[2] = r11.u64;
		want_[3] = ctr.u64;
		want_[4] = uint64_t(ea);
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check1, base, want_, 5, "sub_82139750 fast slot copy 1", checks_);
	}
loc_8213979C_done:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 624);
	r11.s64 = r11.s64 + 1;
	REX_STORE_D_U32_C(l_r10.u32, 624, r11.u32);
loc_821397B4:
	l_r8.u64 = REX_LOAD_D_U32(l_r5.u32, -4);
	cr6.compare<int32_t>(l_r8.s32, 0, xer);
	if (cr6.eq) goto loc_821397FC;
	l_r8.u64 = ((uint32_t)(r11.u32 << 2));
	l_r7.s64 = l_r10.s64 + 384;
	l_r8.u64 = r11.u64 + l_r8.u64;
	r11.s64 = 5;
	l_r8.u64 = ((uint32_t)(l_r8.u32 << 2));
	l_r6.s64 = l_r9.s64 + -4;
	l_r8.u64 = l_r8.u64 + l_r7.u64;
	l_r8.s64 = l_r8.s64 + -4;
	ctr.u64 = r11.u64;
	{
		// sa2-nx (2026-10-28): sub_82139750 fast slot copy 2 where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the words (the copy's counter)
		const uint64_t from = uint32_t(l_r6.u32 + 4), to = uint32_t(l_r8.u32 + 4), bytes = 4ull * n;
		if (n != 0 && ::sa2::hot::fast_loop::Flat(from, bytes) && ::sa2::hot::fast_loop::Flat(to, bytes) && ::sa2::hot::fast_loop::Apart(from, bytes, to, bytes)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check2 = new ::sa2::hot::fast_loop::Check{};
				check2->Add(uint32_t(to), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check2, base);
				check2->in[0] = l_r6.u64;
				check2->in[1] = l_r8.u64;
				check2->in[2] = r11.u64;
				check2->in[3] = ctr.u64;
				check2->in[4] = uint64_t(ea);
			}
			// Apart: the copy's word-by-word forward copy is a memcpy of the bytes (each word stored as loaded).
			std::memcpy(Raw(base, uint32_t(to)), Raw(base, uint32_t(from)), size_t(bytes));
			ea = uint32_t(to + bytes - 4);
			r11.u64 = REX_LOAD_U32(uint32_t(from + bytes - 4));  // the last word (the source is not written)
			l_r6.u32 = uint32_t(from + bytes - 4);
			l_r8.u32 = ea;
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(to), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check2) [[likely]] goto loc_821397E4_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check2->out[0] = l_r6.u64;
			check2->out[1] = l_r8.u64;
			check2->out[2] = r11.u64;
			check2->out[3] = ctr.u64;
			check2->out[4] = uint64_t(ea);
			::sa2::hot::fast_loop::Swap(*check2, base);
			l_r6.u64 = check2->in[0];
			l_r8.u64 = check2->in[1];
			r11.u64 = check2->in[2];
			ctr.u64 = check2->in[3];
			ea = uint32_t(check2->in[4]);
		}
	}
loc_821397E4:
	REX_LOOP_BARRIER();
	ea = 4 + l_r6.u32;
	r11.u64 = REX_LOAD_U32(ea);
	l_r6.u32 = ea;
	ea = 4 + l_r8.u32;
	REX_STORE_U32_C(ea, r11.u32);
	l_r8.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_821397E4;
	if (check2) [[unlikely]] {
		uint64_t want_[5];
		want_[0] = l_r6.u64;
		want_[1] = l_r8.u64;
		want_[2] = r11.u64;
		want_[3] = ctr.u64;
		want_[4] = uint64_t(ea);
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check2, base, want_, 5, "sub_82139750 fast slot copy 2", checks_);
	}
loc_821397E4_done:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 624);
	r11.s64 = r11.s64 + 1;
	REX_STORE_D_U32_C(l_r10.u32, 624, r11.u32);
loc_821397FC:
	l_r8.u64 = REX_LOAD_D_U32(l_r5.u32, 0);
	cr6.compare<int32_t>(l_r8.s32, 0, xer);
	if (cr6.eq) goto loc_82139844;
	l_r8.u64 = ((uint32_t)(r11.u32 << 2));
	l_r7.s64 = l_r10.s64 + 384;
	l_r8.u64 = r11.u64 + l_r8.u64;
	r11.s64 = 5;
	l_r8.u64 = ((uint32_t)(l_r8.u32 << 2));
	l_r6.s64 = l_r9.s64 + 16;
	l_r8.u64 = l_r8.u64 + l_r7.u64;
	l_r8.s64 = l_r8.s64 + -4;
	ctr.u64 = r11.u64;
	{
		// sa2-nx (2026-10-28): sub_82139750 fast slot copy 3 where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the words (the copy's counter)
		const uint64_t from = uint32_t(l_r6.u32 + 4), to = uint32_t(l_r8.u32 + 4), bytes = 4ull * n;
		if (n != 0 && ::sa2::hot::fast_loop::Flat(from, bytes) && ::sa2::hot::fast_loop::Flat(to, bytes) && ::sa2::hot::fast_loop::Apart(from, bytes, to, bytes)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check3 = new ::sa2::hot::fast_loop::Check{};
				check3->Add(uint32_t(to), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check3, base);
				check3->in[0] = l_r6.u64;
				check3->in[1] = l_r8.u64;
				check3->in[2] = r11.u64;
				check3->in[3] = ctr.u64;
				check3->in[4] = uint64_t(ea);
			}
			// Apart: the copy's word-by-word forward copy is a memcpy of the bytes (each word stored as loaded).
			std::memcpy(Raw(base, uint32_t(to)), Raw(base, uint32_t(from)), size_t(bytes));
			ea = uint32_t(to + bytes - 4);
			r11.u64 = REX_LOAD_U32(uint32_t(from + bytes - 4));  // the last word (the source is not written)
			l_r6.u32 = uint32_t(from + bytes - 4);
			l_r8.u32 = ea;
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(to), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check3) [[likely]] goto loc_8213982C_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check3->out[0] = l_r6.u64;
			check3->out[1] = l_r8.u64;
			check3->out[2] = r11.u64;
			check3->out[3] = ctr.u64;
			check3->out[4] = uint64_t(ea);
			::sa2::hot::fast_loop::Swap(*check3, base);
			l_r6.u64 = check3->in[0];
			l_r8.u64 = check3->in[1];
			r11.u64 = check3->in[2];
			ctr.u64 = check3->in[3];
			ea = uint32_t(check3->in[4]);
		}
	}
loc_8213982C:
	REX_LOOP_BARRIER();
	ea = 4 + l_r6.u32;
	r11.u64 = REX_LOAD_U32(ea);
	l_r6.u32 = ea;
	ea = 4 + l_r8.u32;
	REX_STORE_U32_C(ea, r11.u32);
	l_r8.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_8213982C;
	if (check3) [[unlikely]] {
		uint64_t want_[5];
		want_[0] = l_r6.u64;
		want_[1] = l_r8.u64;
		want_[2] = r11.u64;
		want_[3] = ctr.u64;
		want_[4] = uint64_t(ea);
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check3, base, want_, 5, "sub_82139750 fast slot copy 3", checks_);
	}
loc_8213982C_done:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 624);
	r11.s64 = r11.s64 + 1;
	REX_STORE_D_U32_C(l_r10.u32, 624, r11.u32);
loc_82139844:
	l_r8.u64 = REX_LOAD_D_U32(l_r5.u32, 4);
	cr6.compare<int32_t>(l_r8.s32, 0, xer);
	if (cr6.eq) goto loc_8213988C;
	l_r8.u64 = ((uint32_t)(r11.u32 << 2));
	l_r7.s64 = l_r10.s64 + 384;
	l_r8.u64 = r11.u64 + l_r8.u64;
	r11.s64 = 5;
	l_r8.u64 = ((uint32_t)(l_r8.u32 << 2));
	l_r6.s64 = l_r9.s64 + 36;
	l_r8.u64 = l_r8.u64 + l_r7.u64;
	l_r8.s64 = l_r8.s64 + -4;
	ctr.u64 = r11.u64;
	{
		// sa2-nx (2026-10-28): sub_82139750 fast slot copy 4 where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the words (the copy's counter)
		const uint64_t from = uint32_t(l_r6.u32 + 4), to = uint32_t(l_r8.u32 + 4), bytes = 4ull * n;
		if (n != 0 && ::sa2::hot::fast_loop::Flat(from, bytes) && ::sa2::hot::fast_loop::Flat(to, bytes) && ::sa2::hot::fast_loop::Apart(from, bytes, to, bytes)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check4 = new ::sa2::hot::fast_loop::Check{};
				check4->Add(uint32_t(to), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check4, base);
				check4->in[0] = l_r6.u64;
				check4->in[1] = l_r8.u64;
				check4->in[2] = r11.u64;
				check4->in[3] = ctr.u64;
				check4->in[4] = uint64_t(ea);
			}
			// Apart: the copy's word-by-word forward copy is a memcpy of the bytes (each word stored as loaded).
			std::memcpy(Raw(base, uint32_t(to)), Raw(base, uint32_t(from)), size_t(bytes));
			ea = uint32_t(to + bytes - 4);
			r11.u64 = REX_LOAD_U32(uint32_t(from + bytes - 4));  // the last word (the source is not written)
			l_r6.u32 = uint32_t(from + bytes - 4);
			l_r8.u32 = ea;
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(to), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check4) [[likely]] goto loc_82139874_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check4->out[0] = l_r6.u64;
			check4->out[1] = l_r8.u64;
			check4->out[2] = r11.u64;
			check4->out[3] = ctr.u64;
			check4->out[4] = uint64_t(ea);
			::sa2::hot::fast_loop::Swap(*check4, base);
			l_r6.u64 = check4->in[0];
			l_r8.u64 = check4->in[1];
			r11.u64 = check4->in[2];
			ctr.u64 = check4->in[3];
			ea = uint32_t(check4->in[4]);
		}
	}
loc_82139874:
	REX_LOOP_BARRIER();
	ea = 4 + l_r6.u32;
	r11.u64 = REX_LOAD_U32(ea);
	l_r6.u32 = ea;
	ea = 4 + l_r8.u32;
	REX_STORE_U32_C(ea, r11.u32);
	l_r8.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82139874;
	if (check4) [[unlikely]] {
		uint64_t want_[5];
		want_[0] = l_r6.u64;
		want_[1] = l_r8.u64;
		want_[2] = r11.u64;
		want_[3] = ctr.u64;
		want_[4] = uint64_t(ea);
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check4, base, want_, 5, "sub_82139750 fast slot copy 4", checks_);
	}
loc_82139874_done:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 624);
	r11.s64 = r11.s64 + 1;
	REX_STORE_D_U32_C(l_r10.u32, 624, r11.u32);
loc_8213988C:
	l_r8.u64 = REX_LOAD_D_U32(l_r5.u32, 8);
	cr6.compare<int32_t>(l_r8.s32, 0, xer);
	if (cr6.eq) goto loc_821398D4;
	l_r8.u64 = ((uint32_t)(r11.u32 << 2));
	l_r7.s64 = l_r10.s64 + 384;
	l_r8.u64 = r11.u64 + l_r8.u64;
	r11.s64 = 5;
	l_r8.u64 = ((uint32_t)(l_r8.u32 << 2));
	l_r6.s64 = l_r9.s64 + 56;
	l_r8.u64 = l_r8.u64 + l_r7.u64;
	l_r8.s64 = l_r8.s64 + -4;
	ctr.u64 = r11.u64;
	{
		// sa2-nx (2026-10-28): sub_82139750 fast slot copy 5 where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the words (the copy's counter)
		const uint64_t from = uint32_t(l_r6.u32 + 4), to = uint32_t(l_r8.u32 + 4), bytes = 4ull * n;
		if (n != 0 && ::sa2::hot::fast_loop::Flat(from, bytes) && ::sa2::hot::fast_loop::Flat(to, bytes) && ::sa2::hot::fast_loop::Apart(from, bytes, to, bytes)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check5 = new ::sa2::hot::fast_loop::Check{};
				check5->Add(uint32_t(to), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check5, base);
				check5->in[0] = l_r6.u64;
				check5->in[1] = l_r8.u64;
				check5->in[2] = r11.u64;
				check5->in[3] = ctr.u64;
				check5->in[4] = uint64_t(ea);
			}
			// Apart: the copy's word-by-word forward copy is a memcpy of the bytes (each word stored as loaded).
			std::memcpy(Raw(base, uint32_t(to)), Raw(base, uint32_t(from)), size_t(bytes));
			ea = uint32_t(to + bytes - 4);
			r11.u64 = REX_LOAD_U32(uint32_t(from + bytes - 4));  // the last word (the source is not written)
			l_r6.u32 = uint32_t(from + bytes - 4);
			l_r8.u32 = ea;
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(to), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check5) [[likely]] goto loc_821398BC_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check5->out[0] = l_r6.u64;
			check5->out[1] = l_r8.u64;
			check5->out[2] = r11.u64;
			check5->out[3] = ctr.u64;
			check5->out[4] = uint64_t(ea);
			::sa2::hot::fast_loop::Swap(*check5, base);
			l_r6.u64 = check5->in[0];
			l_r8.u64 = check5->in[1];
			r11.u64 = check5->in[2];
			ctr.u64 = check5->in[3];
			ea = uint32_t(check5->in[4]);
		}
	}
loc_821398BC:
	REX_LOOP_BARRIER();
	ea = 4 + l_r6.u32;
	r11.u64 = REX_LOAD_U32(ea);
	l_r6.u32 = ea;
	ea = 4 + l_r8.u32;
	REX_STORE_U32_C(ea, r11.u32);
	l_r8.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_821398BC;
	if (check5) [[unlikely]] {
		uint64_t want_[5];
		want_[0] = l_r6.u64;
		want_[1] = l_r8.u64;
		want_[2] = r11.u64;
		want_[3] = ctr.u64;
		want_[4] = uint64_t(ea);
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check5, base, want_, 5, "sub_82139750 fast slot copy 5", checks_);
	}
loc_821398BC_done:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 624);
	r11.s64 = r11.s64 + 1;
	REX_STORE_D_U32_C(l_r10.u32, 624, r11.u32);
loc_821398D4:
	l_r8.u64 = REX_LOAD_D_U32(l_r5.u32, 12);
	cr6.compare<int32_t>(l_r8.s32, 0, xer);
	if (cr6.eq) goto loc_8213991C;
	l_r8.u64 = ((uint32_t)(r11.u32 << 2));
	l_r7.s64 = l_r10.s64 + 384;
	l_r8.u64 = r11.u64 + l_r8.u64;
	r11.s64 = 5;
	l_r8.u64 = ((uint32_t)(l_r8.u32 << 2));
	l_r6.s64 = l_r9.s64 + 76;
	l_r8.u64 = l_r8.u64 + l_r7.u64;
	l_r8.s64 = l_r8.s64 + -4;
	ctr.u64 = r11.u64;
	{
		// sa2-nx (2026-10-28): sub_82139750 fast slot copy 6 where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = ctr.u32;  // the words (the copy's counter)
		const uint64_t from = uint32_t(l_r6.u32 + 4), to = uint32_t(l_r8.u32 + 4), bytes = 4ull * n;
		if (n != 0 && ::sa2::hot::fast_loop::Flat(from, bytes) && ::sa2::hot::fast_loop::Flat(to, bytes) && ::sa2::hot::fast_loop::Apart(from, bytes, to, bytes)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check6 = new ::sa2::hot::fast_loop::Check{};
				check6->Add(uint32_t(to), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check6, base);
				check6->in[0] = l_r6.u64;
				check6->in[1] = l_r8.u64;
				check6->in[2] = r11.u64;
				check6->in[3] = ctr.u64;
				check6->in[4] = uint64_t(ea);
			}
			// Apart: the copy's word-by-word forward copy is a memcpy of the bytes (each word stored as loaded).
			std::memcpy(Raw(base, uint32_t(to)), Raw(base, uint32_t(from)), size_t(bytes));
			ea = uint32_t(to + bytes - 4);
			r11.u64 = REX_LOAD_U32(uint32_t(from + bytes - 4));  // the last word (the source is not written)
			l_r6.u32 = uint32_t(from + bytes - 4);
			l_r8.u32 = ea;
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(to), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check6) [[likely]] goto loc_82139904_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check6->out[0] = l_r6.u64;
			check6->out[1] = l_r8.u64;
			check6->out[2] = r11.u64;
			check6->out[3] = ctr.u64;
			check6->out[4] = uint64_t(ea);
			::sa2::hot::fast_loop::Swap(*check6, base);
			l_r6.u64 = check6->in[0];
			l_r8.u64 = check6->in[1];
			r11.u64 = check6->in[2];
			ctr.u64 = check6->in[3];
			ea = uint32_t(check6->in[4]);
		}
	}
loc_82139904:
	REX_LOOP_BARRIER();
	ea = 4 + l_r6.u32;
	r11.u64 = REX_LOAD_U32(ea);
	l_r6.u32 = ea;
	ea = 4 + l_r8.u32;
	REX_STORE_U32_C(ea, r11.u32);
	l_r8.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_82139904;
	if (check6) [[unlikely]] {
		uint64_t want_[5];
		want_[0] = l_r6.u64;
		want_[1] = l_r8.u64;
		want_[2] = r11.u64;
		want_[3] = ctr.u64;
		want_[4] = uint64_t(ea);
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check6, base, want_, 5, "sub_82139750 fast slot copy 6", checks_);
	}
loc_82139904_done:
	r11.u64 = REX_LOAD_D_U32(l_r10.u32, 624);
	r11.s64 = r11.s64 + 1;
	REX_STORE_D_U32_C(l_r10.u32, 624, r11.u32);
loc_8213991C:
	xer.ca = l_r4.u32 > 0;
	l_r4.s64 = l_r4.s64 + -1;
	cr0.compare<int32_t>(l_r4.s32, 0, xer);
	l_r5.s64 = l_r5.s64 + 24;
	l_r9.s64 = l_r9.s64 + 120;
	if (!cr0.eq) goto loc_8213976C;
	{ do { ctx.r4 = l_r4; ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_slot_copies
