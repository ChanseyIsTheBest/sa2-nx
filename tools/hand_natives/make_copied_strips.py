"""Writes app/src/cpu/hot/n_copied_strips.h: sub_8214EF60 (strips of copied 24-byte vertices) with a fast vertex loop."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = fl.copy_text('8214EF60')
label, end = 'loc_8214EFD0', '\tif (!cr0.eq) goto loc_8214EFD0;\n'
copy_call = ('{ ctx.r3 = l_r3; ctx.r4 = l_r4; if (!SA2_MEMCPY_INLINE(24)) { ctx.r5 = l_r5; ctx.r8 = l_r8; ctx.r9 = l_r9; '
             'ctx.r10 = l_r10; sub_827F05C0(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; '
             'l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } }')
expect = f'''loc_8214EFD0:
	REX_LOOP_BARRIER();
	l_r9.u64 = REX_LOAD_D_U16(r31.u32, 0);
	l_r5.s64 = 24;
	l_r10.u64 = REX_LOAD_D_U32(r29.u32, 0);
	l_r8.s64 = l_r9.s16;
	l_r3.u64 = r11.u64 + l_r10.u64;
	r11.u64 = ((uint32_t)(l_r8.u32 << 5));
	l_r4.u64 = r11.u64 + r28.u64;
	{copy_call}
	r11.u64 = REX_LOAD_D_U32(r29.u32, 20);
	xer.ca = r30.u32 > 0;
	r30.s64 = r30.s64 + -1;
	cr0.compare<int32_t>(r30.s32, 0, xer);
	r11.s64 = r11.s64 + 24;
	r31.u64 = r27.u64 + r31.u64;
	REX_STORE_D_U32_C(r29.u32, 20, r11.u32);
	if (!cr0.eq) goto loc_8214EFD0;
'''
assert src.count('\tr11.u64 = REX_LOAD_D_U32(r29.u32, 20);\nloc_8214EFD0:\n') == 1  # the cursor, just loaded
assert src.count('\tr29.s64 = r11.s64 + -3848;\n') == 1

setup = ('\t\tconst uint32_t n = r30.u32;                          // the strip\'s vertices (the loop\'s counter)\n'
         '\t\tconst uint32_t buf = REX_LOAD_D_U32(r29.u32, 0);     // the vertex buffer\n'
         '\t\tconst uint64_t cur0 = r11.u64;                       // its cursor [r29+20], loaded just above\n'
         '\t\tconst uint64_t dst = uint64_t(buf) + cur0, bytes = 24ull * n;\n')
cond = ('n != 0 && cur0 + bytes < 0x100000000ull && ::sa2::hot::fast_loop::Flat(dst, bytes) && '
        '::sa2::hot::fast_loop::Apart(dst, bytes, r29.u32, 24)')
fast = f'''			uint64_t cur = cur0;
			do {{
				REX_LOOP_BARRIER();
				l_r9.u64 = REX_LOAD_D_U16(r31.u32, 0);
				l_r5.s64 = 24;
				l_r10.u64 = buf;
				l_r8.s64 = l_r9.s16;
				l_r3.u64 = r11.u64 + l_r10.u64;
				r11.u64 = ((uint32_t)(l_r8.u32 << 5));
				l_r4.u64 = r11.u64 + r28.u64;
				// The vertex's 24 bytes, as the copy copies them (inline, or by the call where it does not inline).
				{copy_call}
				r30.s64 = r30.s64 + -1;
				cur += 24;
				r11.u64 = cur;  // the copy's [r29+20] + 24, stored back
				r31.u64 = r27.u64 + r31.u64;
			}} while (r30.s32 != 0);
			REX_STORE_D_U32_C(r29.u32, 20, uint32_t(cur));  // the cursor, and its card, once
'''
ranges = [('uint32_t(dst)', 'uint32_t(bytes)'), ('r29.u32 + 20', '4'), ('uint32_t(l_r1.u32 - 8)', '8')]
regs = ['l_r1', 'l_r3', 'l_r4', 'l_r5', 'l_r8', 'l_r9', 'l_r10', 'r11', 'r30', 'r31']
src = fl.wrap_loop(src, label, end, expect, setup, cond, fast, ranges, regs, 10, 'sub_8214EF60 fast vertex loop',
                   'check')

header = r'''// sa2-nx (2026-10-28): hand-written sub_8214EF60: a model's triangle strips into the vertex buffer of the descriptor
// at 0x8389F0F8 (r29), each vertex the 24 bytes of the 32-byte vertex its index names (copied as memcpy: inline, or by
// the call where the copy calls), from an index stream of stride r27. Made from its localized copy
// (app/generated/hot/n_8214EF60.h), which stays its reference: re-run tools/hand_natives/make_copied_strips.py after the
// copy changes (it checks the loop it replaces).
//
// Optimisation: the copy reloads the buffer base [r29+0] and the cursor [r29+20] at every vertex and stores the cursor
// back with its card; the fast loop holds them and stores the cursor once.
//
// Audit: it applies when the count n != 0, the cursor (loaded just above) does not wrap, and the vertices [dst, dst +
// 24n) neither wrap nor cross the hosts' physical-range offset nor overlap the descriptor [r29, r29+24): then the
// copy's reloads read what the fast loop holds (its 24-byte copies write [dst..] and, inline, the dead slot [r1-8],
// as here), and its intermediate cursor stores are overwritten by the last. Every other statement is the copy's, in
// its order (the vertex copy with its conditions and its call). The registers it leaves are the copy's last vertex's;
// cr0 / xer are its dead locals. The cursor's card is marked as the copy's last. Otherwise the copy's loop runs.
//
// Unguarded like its copy (it calls sub_82139C90); in verify mode (sa2_cpu_verify) the fast loop checks itself
// against the copy's (hot/fast_loop_check.h).
'''
fl.finish(src, '8214EF60', 'n_copied_strips', header, 'n_copied_strips.h')
