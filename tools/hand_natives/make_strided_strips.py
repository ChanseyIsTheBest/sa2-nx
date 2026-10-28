"""Writes app/src/cpu/hot/n_strided_strips.h: sub_8214F028 (position strips, strided index stream), fast vertex loop."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = fl.copy_text('8214F028')
label, end = 'loc_8214F0C4', '\tif (!cr0.eq) goto loc_8214F0C4;\n'
expect = '''loc_8214F0C4:
	REX_LOOP_BARRIER();
	ll_r10.u64 = REX_LOAD_D_U16(r30.u32, 0);
	xer.ca = ll_r7.u32 > 0;
	ll_r7.s64 = ll_r7.s64 + -1;
	cr0.compare<int32_t>(ll_r7.s32, 0, xer);
	ll_r8.u64 = REX_LOAD_D_U32(r31.u32, 0);
	r30.u64 = r28.u64 + r30.u64;
	ll_r9.s64 = ll_r10.s16;
	ll_r10.u64 = ll_r8.u64 + r11.u64;
	ll_r9.u64 = ((uint32_t)(ll_r9.u32 << 5));
	ll_r9.u64 = ll_r9.u64 + r29.u64;
	fb0.u32 = REX_LOAD_D_U32(ll_r9.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	ctx.fpscr.disableFlushMode();
	ll_f13.f64 = double(REX_LOAD_D_F32(ll_r9.u32, 4));
	fb12.u32 = REX_LOAD_D_U32(ll_r9.u32, 0);
	ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_U32_C(ll_r8.u32 + r11.u32, temp.u32);
	ea = 4 + ll_r10.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r10.u32 = ea;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r10.u32, 4, temp.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	r11.s64 = r11.s64 + 12;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	if (!cr0.eq) goto loc_8214F0C4;
'''
# r11 at the loop: the cursor [r31+20], loaded (empty descriptor) or computed and stored (open one) just above.
for line in ('\tr11.u64 = REX_LOAD_D_U32(r31.u32, 20);\n\tcr6.compare<uint32_t>(l_r8.u32, 0, xer);\n',
             '\tr11.u64 = l_r10.u64 + r11.u64;\n\tREX_STORE_D_U32_C(r31.u32, 20, r11.u32);\n\tgoto loc_8214F0B8;\n'):
    assert src.count(line) == 1, line

setup = ('\t\tconst uint32_t n = ll_r7.u32;                        // the strip\'s vertices (the loop\'s counter)\n'
         '\t\tconst uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);     // the vertex buffer\n'
         '\t\tconst uint64_t cur0 = r11.u64;                       // its cursor, [r31+20] when below 4 GB\n'
         '\t\tconst uint64_t dst = uint64_t(buf) + cur0, bytes = 12ull * n;\n')
cond = ('n != 0 && cur0 + bytes < 0x100000000ull && ::sa2::hot::fast_loop::Flat(dst, bytes) && '
        '::sa2::hot::fast_loop::Apart(dst, bytes, r31.u32, 24)')
fast = r'''			uint64_t cur = cur0;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				ll_r10.u64 = REX_LOAD_D_U16(r30.u32, 0);
				ll_r7.s64 = ll_r7.s64 + -1;
				ll_r8.u64 = buf;
				r30.u64 = r28.u64 + r30.u64;
				ll_r9.s64 = ll_r10.s16;
				ll_r10.u64 = ll_r8.u64 + r11.u64;
				ll_r9.u64 = ((uint32_t)(ll_r9.u32 << 5));
				ll_r9.u64 = ll_r9.u64 + r29.u64;
				fb0.u32 = REX_LOAD_D_U32(ll_r9.u32, 8);
				ll_f13.f64 = double(REX_LOAD_D_F32(ll_r9.u32, 4));
				fb12.u32 = REX_LOAD_D_U32(ll_r9.u32, 0);
				ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
				temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
				REX_STORE_U32(ll_r8.u32 + r11.u32, temp.u32);
				ea = 4 + ll_r10.u32;
				temp.f32 = float(ll_f13.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r10.u32 = ea;
				temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
				REX_STORE_D_U32(ll_r10.u32, 4, temp.u32);
				cur += 12;
				r11.u64 = cur;  // the copy's [r31+20] + 12, stored back
			} while (ll_r7.s32 != 0);
			f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);  // the copy's f0 of the last vertex
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
'''
ranges = [('uint32_t(dst)', 'uint32_t(bytes)'), ('r31.u32 + 20', '4')]
regs = ['ll_r7', 'll_r8', 'll_r9', 'll_r10', 'll_f12', 'll_f13', 'r30', 'r11', 'f0', 'fb0', 'fb12', 'ea', 'temp']
src = fl.wrap_loop(src, label, end, expect, setup, cond, fast, ranges, regs, 9, 'sub_8214F028 fast vertex loop',
                   'check')

header = r'''// sa2-nx (2026-10-28): hand-written sub_8214F028: a model's triangle strips into the vertex buffer of the descriptor
// at 0x8389F0F8 (r31), positions only, 12 bytes a vertex, from an index stream of stride r28 (the strided sibling of
// sub_8214E2B0, hot/n_position_strips.h). Made from its localized copy (app/generated/hot/n_8214F028.h), which stays
// its reference: re-run tools/hand_natives/make_strided_strips.py after the copy changes (it checks the loop it
// replaces).
//
// Optimisation, audit: as hot/n_position_strips.h (the buffer base and the cursor held; the cursor stored once; the
// vertices' cards marked once). It applies when the count n != 0, the cursor r11 ([r31+20], loaded or just stored) is
// below 4 GB and does not wrap, and the vertices [dst, dst + 12n) neither wrap nor cross the hosts' physical-range
// offset nor overlap the descriptor [r31, r31+24). The registers the loop leaves (r7-r10, f12, f13, r30, r11, f0) are
// the copy's last vertex's; cr0 / xer are its dead locals. Otherwise the copy's loop runs.
//
// Unguarded like its copy (it calls sub_82139C90); in verify mode (sa2_cpu_verify) the fast loop checks itself
// against the copy's (hot/fast_loop_check.h).
'''
fl.finish(src, '8214F028', 'n_strided_strips', header, 'n_strided_strips.h')
