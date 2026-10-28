"""Writes app/src/cpu/hot/n_colour_strips.h: sub_8214F120 (position + colour strips, strided) with a fast vertex loop.
  python make_colour_strips.py [copy path]   (default: app/generated/hot/n_8214F120.h)"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = open(sys.argv[1], encoding='utf-8').read() if len(sys.argv) > 1 else fl.copy_text('8214F120')
label, end = 'loc_8214F190', '\tif (!cr0.eq) goto loc_8214F190;\n'
expect = '''loc_8214F190:
	REX_LOOP_BARRIER();
	r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
	xer.ca = ll_r5.u32 > 0;
	ll_r5.s64 = ll_r5.s64 + -1;
	cr0.compare<int32_t>(ll_r5.s32, 0, xer);
	ll_r8.u64 = REX_LOAD_D_U32(r31.u32, 0);
	r30.u64 = r28.u64 + r30.u64;
	ll_r7.s64 = r11.s16;
	ll_r9.u64 = ll_r10.u64 + ll_r8.u64;
	r11.u64 = ((uint32_t)(ll_r7.u32 << 5));
	r11.u64 = r11.u64 + r29.u64;
	fb0.u32 = REX_LOAD_D_U32(r11.u32, 8);
	f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);
	ctx.fpscr.disableFlushMode();
	ll_f13.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
	fb12.u32 = REX_LOAD_D_U32(r11.u32, 0);
	ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
	temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
	REX_STORE_U32_C(ll_r10.u32 + ll_r8.u32, temp.u32);
	ea = 4 + ll_r9.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r9.u32 = ea;
	temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
	REX_STORE_D_U32_C(ll_r9.u32, 4, temp.u32);
	ll_r10.u64 = REX_LOAD_D_U32(r31.u32, 20);
	ll_r10.s64 = ll_r10.s64 + 12;
	REX_STORE_D_U32_C(r31.u32, 20, ll_r10.u32);
	ll_r6.u64 = REX_LOAD_D_U32(r11.u32, 24);
	ll_r9.u64 = REX_LOAD_D_U32(r31.u32, 0);
	REX_STORE_U32_C(ll_r10.u32 + ll_r9.u32, ll_r6.u32);
	r11.u64 = REX_LOAD_D_U32(r31.u32, 20);
	ll_r10.s64 = r11.s64 + 4;
	REX_STORE_D_U32_C(r31.u32, 20, ll_r10.u32);
	if (!cr0.eq) goto loc_8214F190;
'''
assert src.count('\tl_r10.u64 = REX_LOAD_D_U32(r31.u32, 20);\n') == 1  # the cursor, loaded before the loop

setup = ('\t\tconst uint32_t n = ll_r5.u32;                        // the strip\'s vertices (the loop\'s counter)\n'
         '\t\tconst uint32_t buf = REX_LOAD_D_U32(r31.u32, 0);     // the vertex buffer\n'
         '\t\tconst uint64_t cur0 = ll_r10.u64;                    // its cursor [r31+20], loaded above\n'
         '\t\tconst uint64_t dst = uint64_t(buf) + cur0, bytes = 16ull * n;\n')
cond = ('n != 0 && cur0 + bytes < 0x100000000ull && ::sa2::hot::fast_loop::Flat(dst, bytes) && '
        '::sa2::hot::fast_loop::Apart(dst, bytes, r31.u32, 24)')
fast = r'''			uint64_t cur = cur0;
			ctx.fpscr.disableFlushMode();
			do {
				REX_LOOP_BARRIER();
				r11.u64 = REX_LOAD_D_U16(r30.u32, 0);
				ll_r5.s64 = ll_r5.s64 + -1;
				ll_r8.u64 = buf;
				r30.u64 = r28.u64 + r30.u64;
				ll_r7.s64 = r11.s16;
				ll_r9.u64 = ll_r10.u64 + ll_r8.u64;
				r11.u64 = ((uint32_t)(ll_r7.u32 << 5));
				r11.u64 = r11.u64 + r29.u64;
				fb0.u32 = REX_LOAD_D_U32(r11.u32, 8);
				ll_f13.f64 = double(REX_LOAD_D_F32(r11.u32, 4));
				fb12.u32 = REX_LOAD_D_U32(r11.u32, 0);
				ll_f12.f64 = rex::ppc::SingleToDoublePure(fb12.u32);
				temp.u32 = rex::ppc::QuietSingleBits(fb12.u32);
				REX_STORE_U32(ll_r10.u32 + ll_r8.u32, temp.u32);
				ea = 4 + ll_r9.u32;
				temp.f32 = float(ll_f13.f64);
				REX_STORE_U32(ea, temp.u32);
				ll_r9.u32 = ea;
				temp.u32 = rex::ppc::QuietSingleBits(fb0.u32);
				REX_STORE_D_U32(ll_r9.u32, 4, temp.u32);
				ll_r10.u64 = cur + 12;  // the copy's [r31+20] + 12, stored back
				ll_r6.u64 = REX_LOAD_D_U32(r11.u32, 24);
				ll_r9.u64 = buf;
				REX_STORE_U32(ll_r10.u32 + ll_r9.u32, ll_r6.u32);
				r11.u64 = cur + 12;  // the copy's reload of [r31+20]
				cur += 16;
				ll_r10.u64 = cur;  // r11 + 4, stored back
			} while (ll_r5.s32 != 0);
			f0.f64 = rex::ppc::SingleToDoublePure(fb0.u32);  // the copy's f0 of the last vertex
			REX_STORE_D_U32_C(r31.u32, 20, uint32_t(cur));
#if !defined(REX_STORE_CARDS_OFF)
			Card(base, uint32_t(dst), uint32_t(bytes - 3));  // the pages of the stores' first bytes
#endif
'''
ranges = [('uint32_t(dst)', 'uint32_t(bytes)'), ('r31.u32 + 20', '4')]
regs = ['ll_r5', 'll_r6', 'll_r7', 'll_r8', 'll_r9', 'll_r10', 'll_f12', 'll_f13', 'r11', 'r30', 'f0', 'fb0', 'fb12',
        'ea', 'temp']
src = fl.wrap_loop(src, label, end, expect, setup, cond, fast, ranges, regs, 11, 'sub_8214F120 fast vertex loop',
                   'check')

header = r'''// sa2-nx (2026-10-28): hand-written sub_8214F120: a model's triangle strips into the vertex buffer of the descriptor
// at 0x8389F0F8 (r31), 16 bytes a vertex (x, y, z and the colour of the 32-byte vertex its index names), from an
// index stream of stride r28: the position-and-colour sibling of sub_8214E3F0 and sub_8214F028 (hot/n_strip_builder.h,
// hot/n_strided_strips.h). Made from its localized copy (app/generated/hot/n_8214F120.h), which stays its reference:
// re-run tools/hand_natives/make_colour_strips.py after the copy changes (it checks the loop it replaces).
//
// Optimisation: the copy reloads the buffer base [r31+0] twice and the cursor [r31+20] twice a vertex, stores the
// cursor back twice and marks a card at its six stores; the fast loop holds the base and the cursor, stores the cursor
// once and marks the vertices' cards once.
//
// Audit: it applies when the count n != 0, the cursor (loaded above) does not wrap, and the vertices [dst, dst + 16n)
// neither wrap nor cross the hosts' physical-range offset nor overlap the descriptor [r31, r31+24): then the copy's
// reloads read what the fast loop holds and its intermediate cursor stores are overwritten by the last. Every other
// statement is the copy's, in its order (vertex and stream loads, the four stores). The registers it leaves (r5-r10,
// f12, f13, r11, r30, f0) are the copy's last vertex's; cr0 / xer are its dead locals; the mode is set once (nothing
// in the loop changes it). The cards are the pages of the stores' first bytes (four bytes apart over the range).
// Otherwise the copy's loop runs.
//
// Unguarded like its copy (it calls sub_82139B48 / sub_82139C90); in verify mode (sa2_cpu_verify) the fast loop
// checks itself against the copy's (hot/fast_loop_check.h).
'''
fl.finish(src, '8214F120', 'n_colour_strips', header, 'n_colour_strips.h')
