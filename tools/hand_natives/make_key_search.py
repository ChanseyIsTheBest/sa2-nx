"""Writes app/src/cpu/hot/n_key_search.h: the localized copy of sub_8214BBD8 with its binary search branchless."""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/') + '/'
src = open(ROOT + 'app/generated/hot/n_8214BBD8.h', encoding='utf-8').read()
old = """	cr6.compare<uint32_t>(r31.u32, r30.u32, xer);
	if (cr6.lt) goto loc_8214BC1C;
	r11.u64 = l_r10.u64;
	goto loc_8214BC20;
loc_8214BC1C:
	l_r6.u64 = l_r10.u64;
loc_8214BC20:
	l_r10.u64 = l_r6.u64 - r11.u64;
	cr6.compare<uint32_t>(l_r10.u32, 1, xer);
	if (cr6.gt) goto loc_8214BBFC;
"""
new = """	{
		// sa2-nx (2026-10-28): the copy's branch on the key compare as two selects (exact: the same new bounds).
		const bool lower = r31.u32 < r30.u32;  // cr6.lt of cmplw r31,r30
		l_r6.u64 = lower ? l_r10.u64 : l_r6.u64;
		r11.u64 = lower ? r11.u64 : l_r10.u64;
	}
	l_r10.u64 = l_r6.u64 - r11.u64;
	if (l_r10.u32 > 1u) goto loc_8214BBFC;  // cr6.gt of cmplwi r10,1
"""
assert src.count(old) == 1
body = src.replace(old, new)
assert body.count('namespace sa2::hot::n_8214BBD8') == 2
body = body.replace('namespace sa2::hot::n_8214BBD8', 'namespace sa2::hot::n_key_search')
start = body.index('#pragma once')
header = """// sa2-nx (2026-10-28): hand-written sub_8214BBD8, the keyframe search (a binary search of a key table by int(f1),
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
"""
open(ROOT + 'app/src/cpu/hot/n_key_search.h', 'w', encoding='utf-8', newline='\n').write(header + body[start:])
print('n_key_search.h written')
