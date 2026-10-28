"""Writes app/src/cpu/hot/n_vertex24_strips.h: sub_8214E130 (strips of 24-byte vertices) with a fast vertex loop."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = fl.copy_text('8214E130')
label, end = 'loc_8214E1BC', '\tif (!cr0.eq) goto loc_8214E1BC;\n'
expect = '''loc_8214E1BC:
	REX_LOOP_BARRIER();
	l_r9.u64 = REX_LOAD_D_U16(r30.u32, 0);
	l_r5.s64 = 24;
	ea = 2 + r30.u32;
	r29.u64 = REX_LOAD_U16(ea);
	r30.u32 = ea;
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 0);
	l_r8.s64 = l_r9.s16;
	l_r3.u64 = r11.u64 + l_r10.u64;
	r11.u64 = ((uint32_t)(l_r8.u32 << 5));
	ea = 2 + r30.u32;
	r28.u64 = REX_LOAD_U16(ea);
	r30.u32 = ea;
	l_r4.u64 = r11.u64 + r26.u64;
	r30.s64 = r30.s64 + 2;
	{ ctx.r3 = l_r3; ctx.r4 = l_r4; if (!SA2_MEMCPY_INLINE(24)) { ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_827F05C0(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } }
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	l_r10.u64 = REX_LOAD_D_U32(r31.u32, 0);
	cr6.compare<int32_t>(r27.s32, 0, xer);
	r11.s64 = r11.s64 + 24;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	if (cr6.eq) goto loc_8214E23C;
	l_r9.s64 = r29.s16;
	l_r8.s64 = r28.s16;
	xer.ca = (l_r9.s32 < 0) & ((l_r9.u32 & 0xF) != 0);
	l_r7.s64 = l_r9.s32 >> 4;
	xer.ca = (l_r8.s32 < 0) & ((l_r8.u32 & 0xF) != 0);
	l_r6.s64 = l_r8.s32 >> 4;
	l_r5.s64 = l_r7.s16;
	l_r4.s64 = l_r6.s16;
	l_r3.s64 = l_r5.s32;
	l_r9.s64 = l_r4.s32;
	REX_STORE_D_U64(l_r1.u32, 80, l_r3.u64);
	f0.u64 = REX_LOAD_D_U64(l_r1.u32, 80);
	REX_STORE_D_U64(l_r1.u32, 88, l_r9.u64);
	l_f13.u64 = REX_LOAD_D_U64(l_r1.u32, 88);
	l_f12.f64 = double(f0.s64);
	l_f11.f64 = double(l_f13.s64);
	goto loc_8214E25C;
loc_8214E23C:
	l_r6.s64 = r29.s16;
	l_r7.s64 = r28.s16;
	REX_STORE_D_U64(l_r1.u32, 104, l_r6.u64);
	l_f13.u64 = REX_LOAD_D_U64(l_r1.u32, 104);
	REX_STORE_D_U64(l_r1.u32, 96, l_r7.u64);
	f0.u64 = REX_LOAD_D_U64(l_r1.u32, 96);
	l_f11.f64 = double(f0.s64);
	l_f12.f64 = double(l_f13.s64);
loc_8214E25C:
	ctx.fpscr.disableFlushMode();
	l_f10.f64 = double(float(l_f12.f64));
	l_r9.u64 = r11.u64 + l_r10.u64;
	l_f9.f64 = double(float(l_f11.f64));
	xer.ca = r25.u32 > 0;
	r25.s64 = r25.s64 + -1;
	cr0.compare<int32_t>(r25.s32, 0, xer);
	l_f8.f64 = double(float(l_f10.f64 * f31.f64));
	temp.f32 = float(l_f8.f64);
	REX_STORE_U32_C(r11.u32 + l_r10.u32, temp.u32);
	l_f7.f64 = double(float(l_f9.f64 * f31.f64));
	temp.f32 = float(l_f7.f64);
	REX_STORE_D_U32_C(l_r9.u32, 4, temp.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	r11.s64 = r11.s64 + 8;
	REX_STORE_D_U32_C(r31.u32, 20, r11.u32);
	if (!cr0.eq) goto loc_8214E1BC;
'''
assert src.count('\tr11.u64 = REX_LOAD_D_U32(r31.u32, 20);\nloc_8214E1BC:\n') == 1  # the cursor, just loaded
assert src.count('\tr31.s64 = r11.s64 + -3848;\n') == 1

setup = ('\t\tconst uint32_t n = r25.u32;                         // the strip\'s vertices (the loop\'s counter)\n'
         '\t\tconst uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);    // the vertex buffer\n'
         '\t\tconst uint64_t cur0 = r11.u64;                      // its cursor [r31+20], loaded just above\n'
         '\t\tconst uint64_t dst = uint64_t(buf) + cur0, bytes = 32ull * n, frame = l_r1.u32;\n')
cond = ('n != 0 && cur0 + bytes < 0x100000000ull && ::sa2::hot::fast_loop::Flat(dst, bytes) && '
        '::sa2::hot::fast_loop::Apart(dst, bytes, r31.u32, 24) && ::sa2::hot::fast_loop::Apart(dst, bytes, frame + 80, 32)')
fast = r'''			const bool shift = r27.s32 != 0;  // the copy's per-vertex test of r27 (cr6.eq), the same at every vertex
			uint64_t cur = cur0;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				l_r9.u64 = REX_LOAD_D_U16(r30.u32, 0);
				l_r5.s64 = 24;
				ea = 2 + r30.u32;
				r29.u64 = REX_LOAD_U16(ea);
				r30.u32 = ea;
				l_r10.u64 = buf;
				l_r8.s64 = l_r9.s16;
				l_r3.u64 = r11.u64 + l_r10.u64;
				r11.u64 = ((uint32_t)(l_r8.u32 << 5));
				ea = 2 + r30.u32;
				r28.u64 = REX_LOAD_U16(ea);
				r30.u32 = ea;
				l_r4.u64 = r11.u64 + r26.u64;
				r30.s64 = r30.s64 + 2;
				// The vertex's 24 bytes, as the copy copies them (inline, or by the call where it does not inline).
				{ ctx.r3 = l_r3; ctx.r4 = l_r4; if (!SA2_MEMCPY_INLINE(24)) { ctx.r5 = l_r5; ctx.r6 = l_r6; ctx.r7 = l_r7; ctx.r8 = l_r8; ctx.r9 = l_r9; ctx.r10 = l_r10; sub_827F05C0(ctx, base); l_r1 = ctx.r1; l_r3 = ctx.r3; l_r4 = ctx.r4; l_r5 = ctx.r5; l_r6 = ctx.r6; l_r7 = ctx.r7; l_r8 = ctx.r8; l_r9 = ctx.r9; l_r10 = ctx.r10; } }
				r11.u64 = cur + 24;  // the copy's [r31+20] + 24, stored back
				l_r10.u64 = buf;
				if (shift) {
					l_r9.s64 = r29.s16;
					l_r8.s64 = r28.s16;
					l_r7.s64 = l_r9.s32 >> 4;
					l_r6.s64 = l_r8.s32 >> 4;
					l_r5.s64 = l_r7.s16;
					l_r4.s64 = l_r6.s16;
					l_r3.s64 = l_r5.s32;
					l_r9.s64 = l_r4.s32;
					f0.u64 = l_r3.u64;     // the copy's store and load of [r1+80]
					l_f13.u64 = l_r9.u64;  // and of [r1+88]
					l_f12.f64 = double(f0.s64);
					l_f11.f64 = double(l_f13.s64);
				} else {
					l_r6.s64 = r29.s16;
					l_r7.s64 = r28.s16;
					l_f13.u64 = l_r6.u64;  // the copy's store and load of [r1+104]
					f0.u64 = l_r7.u64;     // and of [r1+96]
					l_f11.f64 = double(f0.s64);
					l_f12.f64 = double(l_f13.s64);
				}
				l_f10.f64 = double(float(l_f12.f64));
				l_r9.u64 = r11.u64 + l_r10.u64;
				l_f9.f64 = double(float(l_f11.f64));
				r25.s64 = r25.s64 + -1;
				l_f8.f64 = double(float(l_f10.f64 * f31.f64));
				temp.f32 = float(l_f8.f64);
				REX_STORE_U32(r11.u32 + l_r10.u32, temp.u32);
				l_f7.f64 = double(float(l_f9.f64 * f31.f64));
				temp.f32 = float(l_f7.f64);
				REX_STORE_D_U32(l_r9.u32, 4, temp.u32);
				cur += 32;
				r11.u64 = cur;  // the copy's [r31+20] + 8, stored back
			} while (r25.s32 != 0);
			// The last vertex's frame slots, the cursor, and the cards of the texture coordinates' stores (the copy's
			// marked stores: two a vertex, 24 bytes apart at most, so every page from the first to the last has one).
			if (shift) {
				REX_STORE_D_U64(l_r1.u32, 80, l_r3.u64);
				REX_STORE_D_U64(l_r1.u32, 88, l_r9.u64);
			} else {
				REX_STORE_D_U64(l_r1.u32, 104, l_r6.u64);
				REX_STORE_D_U64(l_r1.u32, 96, l_r7.u64);
			}
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst + 24), uint32_t(bytes - 27));
#endif
'''
ranges = [('uint32_t(dst)', 'uint32_t(bytes)'), ('r31.u32 + 20', '4'), ('uint32_t(frame + 80)', '32'),
          ('uint32_t(frame - 8)', '8')]
regs = ['l_r3', 'l_r4', 'l_r5', 'l_r6', 'l_r7', 'l_r8', 'l_r9', 'l_r10', 'l_f7', 'l_f8', 'l_f9', 'l_f10', 'l_f11',
        'l_f12', 'l_f13', 'r11', 'r25', 'r28', 'r29', 'r30', 'l_r1', 'f0', 'ea', 'temp']
src = fl.wrap_loop(src, label, end, expect, setup, cond, fast, ranges, regs, 21,
                   'sub_8214E130 fast vertex loop', 'check')

header = r'''// sa2-nx (2026-10-28): hand-written sub_8214E130 (1.3 % of the game thread in console gameplay; 2.4 % on the PC
// profile): a model's triangle strips into the vertex buffer of the descriptor at 0x8389F0F8 (r31), 32 bytes a
// vertex: the 24-byte vertex its index names (copied as memcpy: inline, or by the call where the copy calls), then its
// two texture coordinates (s16, >> 4 when r6 != 0, times 1/255). The 24-byte-vertex sibling of sub_8214E3F0
// (hot/n_strip_builder.h). Made from its localized copy (app/generated/hot/n_8214E130.h), which stays its reference:
// re-run tools/hand_natives/make_vertex24_strips.py after the copy changes (it checks the loop it replaces).
//
// Optimisation: the copy's vertex loop reloads the buffer base [r31+0] twice and the cursor [r31+20] twice a vertex,
// stores the cursor back twice, marks a card at its three stores, and moves the coordinates to double through two
// frame slots. The fast loop holds the base and the cursor, stores the cursor once, converts directly, writes the last
// vertex's frame slots once and marks the coordinates' cards once.
//
// Audit: it applies when the count n != 0, the vertices [dst, dst + 32n) (dst = base + cursor) neither wrap nor cross
// the hosts' physical-range offset, nor overlap the descriptor [r31, r31+24) or the frame slots [r1+80, r1+112), and
// the cursor does not wrap: then the copy's reloads read what the fast loop holds (nothing it stores, the 24-byte copy
// included, lands on them; the copy's memcpy writes [dst..] and, inline, the dead slot [r1-8], as here), and its
// intermediate cursor and slot stores are overwritten by the last. Every other statement is the copy's, in its order:
// the vertex copy (inline or the call, with its conditions), the strip stream and vertex loads and the coordinate
// stores (so a buffer overlapping them reads what the copy reads). The registers the loop leaves are the copy's last
// vertex's; cr0 / cr6 / xer are its locals, written before any read after the loop. disableFlushMode runs once (the
// loop changes no mode: a call, when taken, is memcpy). The cards: the coordinates' stores (two a vertex) mark the pages
// of their first bytes, [dst + 24, dst + 32n - 4] (no gap of 4096), zeroed after the data; the cursor's as the copy's.
// Otherwise the copy's loop runs.
//
// Unguarded like its copy; in verify mode (sa2_cpu_verify) the fast loop checks itself against the copy's
// (hot/fast_loop_check.h).
'''
fl.finish(src, '8214E130', 'n_vertex24_strips', header, 'n_vertex24_strips.h')
