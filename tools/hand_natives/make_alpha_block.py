"""Writes app/src/cpu/hot/n_alpha_block.h: the localized copy of sub_82833938 (DXT5 block) with fast loops."""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = fl.copy_text('82833938')
for line in ('\tr11.s64 = 1;\n\tl_r10.s64 = l_r1.s64 + 116;\n', '\tl_r9.s64 = r31.s64 + -4;\n',
             '\tl_r9.s64 = r31.s64 + 124;\n'):
    assert src.count(line) == 1, line


def palette(label, top):
    """The interpolated alpha entries: `top` 7 (six between the endpoints) or 5 (four)."""
    if top == 7:
        expect = f'''{label}:
	REX_LOOP_BARRIER();
	xer.ca = r11.u32 <= 7;
	ll_r9.u64 = static_cast<uint64_t>(7) - r11.u64;
	REX_FLUSH_KNOWN_OFF();
	ll_f13.f64 = double(REX_LOAD_D_F32(ll_r1.u32, 116));
	ll_r8.u64 = r11.u64 & 0xFFFFFFFF;
	ll_f12.f64 = double(REX_LOAD_D_F32(ll_r1.u32, 112));
	ll_r9.u64 = ll_r9.u64 & 0xFFFFFFFF;
	REX_STORE_D_U64(ll_r1.u32, 80, ll_r8.u64);
	ll_f11.u64 = REX_LOAD_D_U64(ll_r1.u32, 80);
	REX_STORE_D_U64(ll_r1.u32, 88, ll_r9.u64);
	ll_f10.u64 = REX_LOAD_D_U64(ll_r1.u32, 88);
	ll_f11.f64 = double(ll_f11.s64);
	r11.s64 = r11.s64 + 1;
	ll_f11.f64 = double(float(ll_f11.f64));
	ll_f10.f64 = double(ll_f10.s64);
	ll_f13.f64 = REX_FMULS(ll_f11.f64, ll_f13.f64);
	ll_f11.f64 = double(float(ll_f10.f64));
	ll_f13.f64 = double(float(std::fma(ll_f11.f64, ll_f12.f64, ll_f13.f64)));
	ll_f13.f64 = REX_FMULS(ll_f13.f64, f0.f64);
	ea = 4 + ll_r10.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r10.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto {label};
'''
        body = '''				ll_r9.u64 = static_cast<uint64_t>(7) - r11.u64;
				ll_f13.f64 = double(a116);
				ll_r8.u64 = r11.u64 & 0xFFFFFFFF;
				ll_f12.f64 = double(a112);
				ll_r9.u64 = ll_r9.u64 & 0xFFFFFFFF;
				ll_f11.u64 = ll_r8.u64;  // the copy's store and load of [r1+80]
				ll_f10.u64 = ll_r9.u64;  // and of [r1+88]
				ll_f11.f64 = double(ll_f11.s64);
				r11.s64 = r11.s64 + 1;
				ll_f11.f64 = double(float(ll_f11.f64));
				ll_f10.f64 = double(ll_f10.s64);
				ll_f13.f64 = REX_FMULS(ll_f11.f64, ll_f13.f64);
				ll_f11.f64 = double(float(ll_f10.f64));
				ll_f13.f64 = double(float(std::fma(ll_f11.f64, ll_f12.f64, ll_f13.f64)));
				ll_f13.f64 = REX_FMULS(ll_f13.f64, f0.f64);
'''
        slots = ('\t\t\tREX_STORE_D_U64(ll_r1.u32, 80, ll_r8.u64);\n'
                 '\t\t\tREX_STORE_D_U64(ll_r1.u32, 88, ll_r9.u64);\n')
    else:
        expect = f'''{label}:
	REX_LOOP_BARRIER();
	ll_r8.u64 = r11.u64 & 0xFFFFFFFF;
	REX_FLUSH_KNOWN_OFF();
	ll_f13.f64 = double(REX_LOAD_D_F32(ll_r1.u32, 116));
	xer.ca = r11.u32 <= 5;
	ll_r9.u64 = static_cast<uint64_t>(5) - r11.u64;
	ll_f12.f64 = double(REX_LOAD_D_F32(ll_r1.u32, 112));
	REX_STORE_D_U64(ll_r1.u32, 88, ll_r8.u64);
	r11.s64 = r11.s64 + 1;
	ll_r9.u64 = ll_r9.u64 & 0xFFFFFFFF;
	REX_STORE_D_U64(ll_r1.u32, 80, ll_r9.u64);
	ll_f11.u64 = REX_LOAD_D_U64(ll_r1.u32, 88);
	ll_f11.f64 = double(ll_f11.s64);
	ll_f10.u64 = REX_LOAD_D_U64(ll_r1.u32, 80);
	ll_f10.f64 = double(ll_f10.s64);
	ll_f11.f64 = double(float(ll_f11.f64));
	ll_f10.f64 = double(float(ll_f10.f64));
	ll_f13.f64 = REX_FMULS(ll_f11.f64, ll_f13.f64);
	ll_f13.f64 = double(float(std::fma(ll_f10.f64, ll_f12.f64, ll_f13.f64)));
	ll_f13.f64 = REX_FMULS(ll_f13.f64, f0.f64);
	ea = 4 + ll_r10.u32;
	temp.f32 = float(ll_f13.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r10.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto {label};
'''
        body = '''				ll_r8.u64 = r11.u64 & 0xFFFFFFFF;
				ll_f13.f64 = double(a116);
				ll_r9.u64 = static_cast<uint64_t>(5) - r11.u64;
				ll_f12.f64 = double(a112);
				r11.s64 = r11.s64 + 1;
				ll_r9.u64 = ll_r9.u64 & 0xFFFFFFFF;
				ll_f11.u64 = ll_r8.u64;  // the copy's store and load of [r1+88]
				ll_f11.f64 = double(ll_f11.s64);
				ll_f10.u64 = ll_r9.u64;  // and of [r1+80]
				ll_f10.f64 = double(ll_f10.s64);
				ll_f11.f64 = double(float(ll_f11.f64));
				ll_f10.f64 = double(float(ll_f10.f64));
				ll_f13.f64 = REX_FMULS(ll_f11.f64, ll_f13.f64);
				ll_f13.f64 = double(float(std::fma(ll_f10.f64, ll_f12.f64, ll_f13.f64)));
				ll_f13.f64 = REX_FMULS(ll_f13.f64, f0.f64);
'''
        slots = ('\t\t\tREX_STORE_D_U64(ll_r1.u32, 88, ll_r8.u64);\n'
                 '\t\t\tREX_STORE_D_U64(ll_r1.u32, 80, ll_r9.u64);\n')
    setup = ('\t\tconst uint32_t n = ctr.u32;  // the entries (the copy\'s counter)\n'
             '\t\tconst uint64_t first = uint32_t(ll_r10.u32 + 4), frame = ll_r1.u32;  // the first entry\'s address\n')
    cond = ('n != 0 && ::sa2::hot::fast_loop::Flat(first, 4ull * n) && '
            '::sa2::hot::fast_loop::Apart(first, 4ull * n, frame + 80, 16) && '
            '::sa2::hot::fast_loop::Apart(first, 4ull * n, frame + 112, 8)')
    fast = ('\t\t\t// The endpoints [r1+116], [r1+112], reloaded by the copy at every entry (no entry lands on them).\n'
            '\t\t\tconst float a116 = REX_LOAD_D_F32(ll_r1.u32, 116), a112 = REX_LOAD_D_F32(ll_r1.u32, 112);\n'
            '\t\t\tuint32_t left = n;\n\t\t\tREX_FLUSH_KNOWN_OFF();\n\t\t\tdo {\n\t\t\t\tREX_LOOP_BARRIER();\n' + body +
            '\t\t\t\tea = 4 + ll_r10.u32;\n\t\t\t\ttemp.f32 = float(ll_f13.f64);\n\t\t\t\tREX_STORE_U32(ea, temp.u32);\n'
            '\t\t\t\tll_r10.u32 = ea;\n\t\t\t} while (--left != 0);\n'
            '\t\t\tctr.u64 = ctr.u64 - n;  // the copy\'s counter after its n decrements\n'
            '\t\t\t// The last entry\'s frame slots, and the cards of the entries\' pages (a store\'s first byte each).\n'
            + slots +
            '#if !defined(REX_STORE_CARDS_OFF)\n\t\t\tCard(base, uint32_t(first), 4 * (n - 1) + 1);\n#endif\n')
    regs = ['ll_f10', 'll_f11', 'll_f12', 'll_f13', 'll_r8', 'll_r9', 'll_r10', 'r11', 'ctr', 'ea', 'temp']
    return expect, setup, cond, fast, [('uint32_t(first)', '4 * n'), ('ll_r1.u32 + 80', '16')], regs


def texels(label):
    """Eight texels' alpha: entry (3 bits each of r11) to the alpha word of each 16-byte texel."""
    expect = f'''{label}:
	REX_LOOP_BARRIER();
	ll_r10.u64 = ((uint32_t)(r11.u32 << 2) & 0x1Cu);
	ll_r8.s64 = ll_r1.s64 + 112;
	r11.u64 = (r11.u32 >> 3);
	REX_FLUSH_KNOWN_OFF();
	f0.f64 = double(REX_LOAD_F32(ll_r10.u32 + ll_r8.u32));
	ea = 16 + ll_r9.u32;
	temp.f32 = float(f0.f64);
	REX_STORE_U32_C(ea, temp.u32);
	ll_r9.u32 = ea;
	--ctr.u64;
	if (ctr.u32 != 0) goto {label};
'''
    setup = ('\t\tconst uint32_t n = ctr.u32;  // the texels (the copy\'s counter)\n'
             '\t\tconst uint64_t first = uint32_t(ll_r9.u32 + 16), frame = ll_r1.u32;  // the first alpha word\n')
    cond = ('n != 0 && ::sa2::hot::fast_loop::Flat(first, 16ull * (n - 1) + 4) && '
            '::sa2::hot::fast_loop::Apart(first, 16ull * (n - 1) + 4, frame + 112, 32)')
    fast = ('\t\t\t// The eight entries [r1+112..143], reloaded by the copy at every texel (no texel lands on them).\n'
            '\t\t\tfloat entry[8];\n'
            '\t\t\tfor (uint32_t k = 0; k < 8; ++k) entry[k] = REX_LOAD_F32(uint32_t(ll_r1.u32 + 112 + 4 * k));\n'
            '\t\t\tuint32_t left = n;\n\t\t\tREX_FLUSH_KNOWN_OFF();\n\t\t\tdo {\n\t\t\t\tREX_LOOP_BARRIER();\n'
            '\t\t\t\tll_r10.u64 = ((uint32_t)(r11.u32 << 2) & 0x1Cu);\n'
            '\t\t\t\tll_r8.s64 = ll_r1.s64 + 112;\n'
            '\t\t\t\tr11.u64 = (r11.u32 >> 3);\n'
            '\t\t\t\tf0.f64 = double(entry[ll_r10.u32 >> 2]);\n'
            '\t\t\t\tea = 16 + ll_r9.u32;\n'
            '\t\t\t\ttemp.f32 = float(f0.f64);\n'
            '\t\t\t\tREX_STORE_U32(ea, temp.u32);\n'
            '\t\t\t\tll_r9.u32 = ea;\n'
            '\t\t\t} while (--left != 0);\n'
            '\t\t\tctr.u64 = ctr.u64 - n;  // the copy\'s counter after its n decrements\n'
            '#if !defined(REX_STORE_CARDS_OFF)\n'
            '\t\t\tCard(base, uint32_t(first), 16 * (n - 1) + 1);  // the pages of the stores\' first bytes\n'
            '#endif\n')
    regs = ['ll_r8', 'll_r9', 'll_r10', 'r11', 'ctr', 'f0', 'ea', 'temp']
    return expect, setup, cond, fast, [('uint32_t(first)', '16 * (n - 1) + 4')], regs


for label, top, ptr in (('loc_82833A1C', 7, 'check6'), ('loc_82833A7C', 5, 'check4')):
    expect, setup, cond, fast, ranges, regs = palette(label, top)
    src = fl.wrap_loop(src, label, f'\tif (ctr.u32 != 0) goto {label};\n', expect, setup, cond, fast, ranges, regs, 9,
                       f'sub_82833938 fast alpha palette ({top - 1} entries)', ptr)
for label, ptr in (('loc_82833B04', 'check_lo'), ('loc_82833B40', 'check_hi')):
    expect, setup, cond, fast, ranges, regs = texels(label)
    src = fl.wrap_loop(src, label, f'\tif (ctr.u32 != 0) goto {label};\n', expect, setup, cond, fast, ranges, regs, 6,
                       f'sub_82833938 fast alpha texels ({"0-7" if ptr == "check_lo" else "8-15"})', ptr)

header = r'''// sa2-nx (2026-10-28): hand-written sub_82833938, a DXT5 block to 16 RGBA float texels (2.6 % of the game thread in
// console gameplay, round-6 renut profile): it byte-swaps the block's 16-bit words to the stack, decodes the colour
// half with sub_82833568 (hot/n_dxt1_block.h), builds the eight-entry alpha palette on the stack ([r1+112..143]: the
// two endpoints, then six interpolated entries, or four and 0.0 / 1.0), and writes each texel's alpha (3-bit index)
// to [r3 + 12 + 16k]. Made from its localized copy (app/generated/hot/n_82833938.h), which stays its reference:
// re-run tools/hand_natives/make_alpha_block.py after the copy changes (it checks the four loops it replaces statement
// by statement).
//
// Optimisation: every store of the copy's loops marks a card, a byte store that may alias anything, so GCC reloads
// after each: the palette loops reload the endpoints [r1+112], [r1+116] and move the counters to double through two
// frame slots at every entry, and the texel loops reload the palette entry from the stack. The fast loops hold the
// endpoints and the palette in registers, convert directly, store the last entry's frame slots once, and mark the
// cards once.
//
// Audit: a palette loop applies when its entries [r10+4, r10+4+4n) neither wrap nor overlap the frame slots [r1+80,
// r1+96) or the endpoints [r1+112, r1+120) (r10 = r1 + 116: they never do); a texel loop when its alpha words neither
// wrap nor overlap the palette [r1+112, r1+144). Then the copy's reloads read what the fast loop holds, its frame slot
// round trips give back the values stored, and its intermediate slot stores are overwritten by the last. Every other
// statement is the copy's, in its order; the registers each loop leaves (f10-f13, r8-r10, r11, ctr; r8-r10, r11, ctr,
// f0) are the copy's last iteration's; xer is the copy's dead local (only its carry is written). REX_FLUSH_KNOWN_OFF
// runs once a loop (nothing in a loop changes the mode). The cards are the pages of every store's first byte (stores
// 4 or 16 bytes apart: every page between the first and the last has one), zeroed after the data. Otherwise the
// copy's loop runs.
//
// Unguarded like its copy (it calls sub_82833568); in verify mode (sa2_cpu_verify) each fast loop checks itself
// against the copy's (hot/fast_loop_check.h).
'''
fl.finish(src, '82833938', 'n_alpha_block', header, 'n_alpha_block.h')
