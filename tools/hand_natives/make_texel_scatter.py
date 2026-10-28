"""Writes app/src/cpu/hot/n_texel_store.h: sub_82823E08 (a texel span into the block cache) with a fast scatter loop."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = fl.copy_text('82823E08')
label, end = 'loc_82824108', '\tif (ctr.u32 != 0) goto loc_82824108;\n'
expect = '''loc_82824108:
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
'''
assert src.count('\tl_r7.u64 = l_r9.u32 & 0x3;\n') == 1  # r7: the span's x within its block, < 4

setup = ('\t\tconst uint32_t n = ctr.u32;                            // the texels (the copy\'s counter)\n'
         '\t\tconst uint32_t cache = REX_LOAD_D_U32(r26.u32, 4);     // the block cache\n'
         '\t\t// The texels\' slots: 16 bytes at cache + 16 * (((r11 & ~3) | r7) << 2 | (r11 & 3)) for r11 in [a, a + n):\n'
         '\t\t// within [cache + 64 * (a & ~3), cache + 64 * ((a + n - 1) & ~3) + 256) while the index stays below 2^28.\n'
         '\t\tconst uint64_t a = r11.u32, last = a + n - 1;\n'
         '\t\tconst uint64_t lo = uint64_t(cache) + 64 * (a & ~3ull), hi = uint64_t(cache) + 64 * (last & ~3ull) + 256;\n')
cond = ('n != 0 && last < 0xFFFFFFFFull && 4 * (last & ~3ull) + 15 < (1ull << 28) && (r11.u64 >> 32) == 0 && '
        'll_r7.u64 < 4 && hi <= 0x100000000ull && ::sa2::hot::fast_loop::Apart(lo, hi - lo, r26.u32 + 4ull, 4)')
fast = r'''			uint32_t left = n;
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
'''
ranges = [('uint32_t(lo)', 'uint32_t(hi - lo)')]
regs = ['ll_r6', 'll_r8', 'll_r9', 'll_r10', 'r11', 'ctr', 'f0', 'fb0', 'ea', 'temp']
src = fl.wrap_loop(src, label, end, expect, setup, cond, fast, ranges, regs, 7, 'sub_82823E08 fast texel scatter',
                   'check')

header = r'''// sa2-nx (2026-10-28): hand-written sub_82823E08 (0.66 % of the console session; 0.7 % of the game thread in
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
'''
fl.finish(src, '82823E08', 'n_texel_store', header, 'n_texel_store.h')
