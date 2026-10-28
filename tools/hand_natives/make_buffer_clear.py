"""Writes app/src/cpu/hot/n_buffer_clear.h: sub_82919200 (clears up to 16 float buffers) with a fast fill loop."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = fl.copy_text('82919200')
label, end = 'loc_82919258', '\tif (cr6.lt) goto loc_82919258;\n'
expect = '''loc_82919258:
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
'''
# Entered with r11 = 1 and r6 = [r3+72] > 1 just loaded; r10 = the buffer (its first word just stored).
entry = '''	l_r6.u64 = REX_LOAD_D_U32(l_r3.u32, 72);
	cr6.compare<uint32_t>(l_r6.u32, 1, xer);
	if (!cr6.gt) goto loc_8291926C;
'''
assert src.count(entry) == 1

setup = ('\t\tconst uint32_t n = l_r6.u32;   // the buffer\'s length [r3+72], just loaded (the loop\'s bound)\n'
         '\t\tconst uint32_t k0 = r11.u32;  // the copy\'s index (1)\n'
         '\t\tconst uint64_t first = uint32_t(l_r10.u32 + 4), words = n > k0 ? n - k0 : 1, bytes = 4 * words;\n')
cond = ('n > k0 && (r11.u64 >> 32) == 0 && ::sa2::hot::fast_loop::Flat(first, bytes) && '
        '::sa2::hot::fast_loop::Apart(first, bytes, l_r3.u32 + 72ull, 4)')
fast = r'''			// The copy stores float(f0) word by word while its index < [r3+72], reloaded after each store (no store
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
'''
regs = ['l_r6', 'l_r10', 'r11', 'f0', 'ea', 'temp']
src = fl.wrap_loop(src, label, end, expect, setup, cond, fast, [('uint32_t(first)', 'uint32_t(bytes)')], regs, 4,
                   'sub_82919200 fast buffer fill', 'check')
src = src.replace('#pragma once\n', '#pragma once\n\n#include <cstring>\n', 1)

header = r'''// sa2-nx (2026-10-28): hand-written sub_82919200, the guest function with the most executed instructions of the
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
'''
fl.finish(src, '82919200', 'n_buffer_clear', header, 'n_buffer_clear.h')
