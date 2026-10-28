"""Writes app/src/cpu/hot/n_slot_copies.h: sub_82139750 with its six word-copy loops as one copy each."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = fl.copy_text('82139750')
labels = ['loc_8213979C', 'loc_821397E4', 'loc_8213982C', 'loc_82139874', 'loc_821398BC', 'loc_82139904']
setup = ('\t\tconst uint32_t n = ctr.u32;  // the words (the copy\'s counter)\n'
         '\t\tconst uint64_t from = uint32_t(l_r6.u32 + 4), to = uint32_t(l_r8.u32 + 4), bytes = 4ull * n;\n')
cond = ('n != 0 && ::sa2::hot::fast_loop::Flat(from, bytes) && ::sa2::hot::fast_loop::Flat(to, bytes) && '
        '::sa2::hot::fast_loop::Apart(from, bytes, to, bytes)')
fast = r'''			// Apart: the copy's word-by-word forward copy is a memcpy of the bytes (each word stored as loaded).
			std::memcpy(Raw(base, uint32_t(to)), Raw(base, uint32_t(from)), size_t(bytes));
			ea = uint32_t(to + bytes - 4);
			r11.u64 = REX_LOAD_U32(uint32_t(from + bytes - 4));  // the last word (the source is not written)
			l_r6.u32 = uint32_t(from + bytes - 4);
			l_r8.u32 = ea;
			ctr.u64 = ctr.u64 - n;  // the copy's counter after its n decrements
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(to), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
'''
for k, label in enumerate(labels):
    expect = f'''{label}:
	REX_LOOP_BARRIER();
	ea = 4 + l_r6.u32;
	r11.u64 = REX_LOAD_U32(ea);
	l_r6.u32 = ea;
	ea = 4 + l_r8.u32;
	REX_STORE_U32_C(ea, r11.u32);
	l_r8.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto {label};
'''
    src = fl.wrap_loop(src, label, f'\tif (ctr.u32 != 0) goto {label};\n', expect, setup, cond, fast,
                       [('uint32_t(to)', 'uint32_t(bytes)')], ['l_r6', 'l_r8', 'r11', 'ctr', 'ea'], 5,
                       f'sub_82139750 fast slot copy {k + 1}', f'check{k + 1}')
src = src.replace('#pragma once\n', '#pragma once\n\n#include <cstring>\n', 1)

header = r'''// sa2-nx (2026-10-28): hand-written sub_82139750 (0.47 % of the console session): for each of six flagged slots
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
'''
fl.finish(src, '82139750', 'n_slot_copies', header, 'n_slot_copies.h')
