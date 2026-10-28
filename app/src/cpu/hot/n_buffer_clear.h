// sa2-nx (2026-10-28): hand-written sub_82919200, the guest function with the most executed instructions of the
// console session (8.2 %; not on the game thread): for each of the [r3+64] float buffers whose pointers start at r3
// (up to 16), stores [r3+72] back, writes the buffer's first float (the constant at 0x820BF2F8) and fills the rest of
// its [r3+72] floats with the constant at 0x82001630 (0.0), then clears its byte at r3+80+k. Made from its localized
// copy (app/generated/hot/n_82919200.h), which stays its reference: re-run tools/hand_natives/make_buffer_clear.py after
// the copy changes (it checks the loop it replaces).
//
// Optimisation: the copy's fill stores a float at a time, marking its card, checking the float mode and reloading the
// length [r3+72] after each store. The fast fill writes the word pattern over the whole range (one host range: GCC's
// vector stores), marks the range's cards once and checks the mode once.
//
// Audit: it applies when the fill [r10+4, r10+4(n-1)) (n = [r3+72] > 1, the copy's index from 1) is one host range
// without a 4 GB wrap and does not overlap [r3+72]: then every reload of the copy reads n, so it stores n-1 words, each
// float(f0) byte-swapped as REX_STORE_U32 stores it, and leaves r11 = 1 + (n-1), r10 = its last store's address (low
// half, as the copy's 32-bit update), r6 = n, ea = that address; cr6 / xer are its locals. The cards are the pages of
// the stores' first bytes (four bytes apart over the range), zeroed after the data. Otherwise the copy's loop runs.
// The first float, the length's store-back and the byte clear (and their cards) are the copy's statements.
//
// Unguarded (a buffer each: more ranges than the guard takes); in verify mode (sa2_cpu_verify) the fast fill checks
// itself against the copy's loop (hot/fast_loop_check.h).
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


namespace sa2::hot::n_buffer_clear {

inline void Native(PPCContext& ctx, uint8_t* base) {
  PPCRegister l_f13 = ctx.f13;
  PPCRegister l_r3 = ctx.r3;
  PPCRegister l_r6 = ctx.r6;
  PPCRegister l_r7 = ctx.r7;
  PPCRegister l_r8 = ctx.r8;
  PPCRegister l_r9 = ctx.r9;
  PPCRegister l_r10 = ctx.r10;
#define SA2_SYNC_OUT() do { ctx.f13 = l_f13; ctx.r3 = l_r3; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0)
#define SA2_SYNC_IN() do { l_f13 = ctx.f13; l_r3 = ctx.r3; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } while (0)
	REX_FUNC_PROLOGUE();
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister f0{};
	PPCRegister temp{};
	uint32_t ea{};
	::sa2::hot::fast_loop::Check* check = nullptr;  // verify mode only
	r11.u64 = REX_LOAD_D_U32(l_r3.u32, 64);
	l_r7.s64 = 0;
	l_r9.u64 = l_r7.u64;
	cr6.compare<uint32_t>(r11.u32, 0, xer);
	if (cr6.eq) { do { ctx.r7 = l_r7; ctx.r9 = l_r9; } while (0); return; }
	l_r10.s64 = -2113929216;
	l_r6.s64 = -2113142784;
	l_r8.u64 = l_r3.u64;
	cr6.compare<uint32_t>(l_r7.u32, r11.u32, xer);
	ctx.fpscr.disableFlushMode();
	temp.u32 = REX_RO_U32(0x82001630, 0x00000000);
	f0.f64 = double(temp.f32);
	temp.u32 = REX_RO_U32(0x820BF2F8, 0x34000100);
	l_f13.f64 = double(temp.f32);
	if (!cr6.lt) goto loc_82919274;
loc_82919230:
	REX_LOOP_BARRIER();
	cr6.compare<uint32_t>(l_r9.u32, 16, xer);
	if (!cr6.lt) goto loc_82919274;
	l_r10.u64 = REX_LOAD_D_U32(l_r3.u32, 72);
	r11.s64 = 1;
	REX_STORE_D_U32_C(l_r3.u32, 72, l_r10.u32);
	l_r10.u64 = REX_LOAD_D_U32(l_r8.u32, 0);
	REX_FLUSH_KNOWN_OFF();
	temp.f32 = float(l_f13.f64);
	REX_STORE_D_U32_C(l_r10.u32, 0, temp.u32);
	l_r6.u64 = REX_LOAD_D_U32(l_r3.u32, 72);
	cr6.compare<uint32_t>(l_r6.u32, 1, xer);
	if (!cr6.gt) goto loc_8291926C;
	{
		// sa2-nx (2026-10-28): sub_82919200 fast buffer fill where it applies (the header's conditions), else the copy's loop below.
		const uint32_t n = l_r6.u32;   // the buffer's length [r3+72], just loaded (the loop's bound)
		const uint32_t k0 = r11.u32;  // the copy's index (1)
		const uint64_t first = uint32_t(l_r10.u32 + 4), words = n > k0 ? n - k0 : 1, bytes = 4 * words;
		if (n > k0 && (r11.u64 >> 32) == 0 && ::sa2::hot::fast_loop::Flat(first, bytes) && ::sa2::hot::fast_loop::Apart(first, bytes, l_r3.u32 + 72ull, 4)) {
			if (::sa2::enh::VerifyMode()) [[unlikely]] {
				check = new ::sa2::hot::fast_loop::Check{};
				check->Add(uint32_t(first), uint32_t(bytes));
				::sa2::hot::fast_loop::Begin(*check, base);
				check->in[0] = l_r6.u64;
				check->in[1] = l_r10.u64;
				check->in[2] = r11.u64;
				check->in[3] = f0.u64;
				check->in[4] = uint64_t(ea);
				check->in[5] = temp.u64;
			}
			// The copy stores float(f0) word by word while its index < [r3+72], reloaded after each store (no store
			// lands on it): `words` words from first, one host range.
			REX_FLUSH_KNOWN_OFF();
			temp.f32 = float(f0.f64);
			const uint32_t word = __builtin_bswap32(temp.u32);
			uint8_t* const p = Raw(base, uint32_t(first));
			for (uint64_t k = 0; k < words; ++k) std::memcpy(p + 4 * k, &word, 4);
			r11.s64 = r11.s64 + int64_t(words);
			ea = uint32_t(first + bytes - 4);
			l_r10.u32 = ea;
			l_r6.u64 = n;  // the copy's last reload of [r3+72]
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(first), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
			if (!check) [[likely]] goto loc_82919258_done;
			// Verify mode: the fast loop's result aside, the inputs back, then the copy's loop.
			check->out[0] = l_r6.u64;
			check->out[1] = l_r10.u64;
			check->out[2] = r11.u64;
			check->out[3] = f0.u64;
			check->out[4] = uint64_t(ea);
			check->out[5] = temp.u64;
			::sa2::hot::fast_loop::Swap(*check, base);
			l_r6.u64 = check->in[0];
			l_r10.u64 = check->in[1];
			r11.u64 = check->in[2];
			f0.u64 = check->in[3];
			ea = uint32_t(check->in[4]);
			temp.u64 = check->in[5];
		}
	}
loc_82919258:
	REX_LOOP_BARRIER();
	r11.s64 = r11.s64 + 1;
	REX_FLUSH_KNOWN_OFF();
	ea = 4 + l_r10.u32;
	temp.f32 = float(f0.f64);
	REX_STORE_U32_C(ea, temp.u32);
	l_r10.u32 = ea;
	l_r6.u64 = REX_LOAD_D_U32(l_r3.u32, 72);
	cr6.compare<uint32_t>(r11.u32, l_r6.u32, xer);
	if (cr6.lt) goto loc_82919258;
	if (check) [[unlikely]] {
		uint64_t want_[4];
		want_[0] = l_r6.u64;
		want_[1] = l_r10.u64;
		want_[2] = r11.u64;
		want_[3] = f0.u64;
		static std::atomic<uint32_t> checks_{0};
		::sa2::hot::fast_loop::Finish(check, base, want_, 4, "sub_82919200 fast buffer fill", checks_);
	}
loc_82919258_done:
loc_8291926C:
	r11.u64 = l_r9.u64 + l_r3.u64;
	REX_STORE_D_U8_C(r11.u32, 80, l_r7.u8);
loc_82919274:
	r11.u64 = REX_LOAD_D_U32(l_r3.u32, 64);
	l_r9.s64 = l_r9.s64 + 1;
	l_r8.s64 = l_r8.s64 + 4;
	cr6.compare<uint32_t>(l_r9.u32, r11.u32, xer);
	if (cr6.lt) goto loc_82919230;
	{ do { ctx.f13 = l_f13; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; } while (0); return; }
	SA2_SYNC_OUT();
#undef SA2_SYNC_OUT
#undef SA2_SYNC_IN
}

}  // namespace sa2::hot::n_buffer_clear
