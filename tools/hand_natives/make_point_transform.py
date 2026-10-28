"""Writes app/src/cpu/hot/n_point_transform.h: sub_82147628 (points by a 3x4 matrix) with the matrix held in registers."""
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fastloop as fl

src = fl.copy_text('82147628')
MLOAD = re.compile(r'double\(REX_LOAD_D_F32\(l_r3\.u32, (\d+)\)\)')


def loop_text(label, end):
    i = src.index(f'{label}:\n')
    j = src.index(end) + len(end)
    return src[i:j]


def fast_body(text, label, end, cond_line):
    """The copy's loop with the matrix loads as the held values and plain stores (the cards marked after)."""
    body = text[len(f'{label}:\n'):-len(end)]
    assert body.count('REX_LOOP_BARRIER();') == 1
    n_loads = len(MLOAD.findall(body))
    body = MLOAD.sub(lambda m: f'double(m[{int(m.group(1)) // 4}])', body)
    assert 'l_r3' not in body
    assert not re.search(r'REX_LOAD[^\n]*l_r3', body)
    body = body.replace('REX_STORE_D_U32_C(', 'REX_STORE_D_U32(').replace('REX_STORE_U32_C(', 'REX_STORE_U32(')
    assert '_C(' not in body
    lines = ''.join('\t\t\t\t' + l.lstrip('\t') + '\n' for l in body.splitlines())
    return n_loads, f'\t\t\tdo {{\n{lines}\t\t\t}} while ({cond_line});\n'


matrix = ('\t\t\t// The matrix [r3, r3+48), reloaded by the copy for every point (no output lands on it).\n'
          '\t\t\tfloat m[12];\n'
          '\t\t\tfor (uint32_t k = 0; k < 12; ++k) m[k] = REX_LOAD_D_F32(l_r3.u32, 4 * k);\n')

# The unrolled loop: four points a pass, while r9 (+4 a pass) < r8 (= r6 - 3); outputs [r10+4, r10+4+48g).
main_label, main_end = 'loc_82147648', '\tif (cr6.lt) goto loc_82147648;\n'
main = loop_text(main_label, main_end)
assert main.count('l_r9.s64 = l_r9.s64 + 4;\n\tcr6.compare<uint32_t>(l_r9.u32, l_r8.u32, xer);\n') == 1
n_main, body = fast_body(main, main_label, main_end, 'cr6.lt')
setup = ('\t\t// Passes: the do-while runs once, then while r9 + 4k < r8 (unsigned).\n'
         '\t\tconst uint64_t passes = l_r8.u32 > l_r9.u32 ? (uint64_t(l_r8.u32) - l_r9.u32 + 3) / 4 : 1;\n'
         '\t\tconst uint64_t first = uint32_t(l_r10.u32 + 4), bytes = 48 * passes;\n')
cond = ('::sa2::hot::fast_loop::Flat(first, bytes) && ::sa2::hot::fast_loop::Apart(first, bytes, l_r3.u32, 48) && '
        '::sa2::hot::fast_loop::Flat(l_r3.u32, 48)')
fast = (matrix + body +
        '#if !defined(REX_STORE_CARDS_OFF)\n'
        '\t\t\tCard(base, uint32_t(first), uint32_t(bytes - 3));  // the pages of the stores\' first bytes\n'
        '#endif\n')
regs = ['l_f1', 'l_f2', 'l_f3', 'l_f4', 'l_f5', 'l_f6', 'l_f7', 'l_f8', 'l_f9', 'l_f10', 'l_f11', 'l_f12', 'l_f13',
        'f0', 'l_r9', 'l_r10', 'r11', 'f31', 'ea', 'temp']
src = fl.wrap_loop(src, main_label, main_end, main, setup, cond, fast, [('uint32_t(first)', 'uint32_t(bytes)')], regs,
                   17, 'sub_82147628 fast points (four a pass)', 'check4')

# The remainder: ctr points, outputs [r10+4, r10+4+12n).
rest_label, rest_end = 'loc_82147868', '\tif (ctr.u32 != 0) goto loc_82147868;\n'
rest = loop_text(rest_label, rest_end)
rest_body = rest.replace('\t--ctr.u64;\n', '')
n_rest, body = fast_body(rest_body.replace(rest_end, '') + rest_end, rest_label, rest_end, '--left != 0')
setup = ('\t\tconst uint32_t n = ctr.u32;  // the points (the copy\'s counter)\n'
         '\t\tconst uint64_t first = uint32_t(l_r10.u32 + 4), bytes = 12ull * n;\n')
fast = (matrix + '\t\t\tuint32_t left = n;\n' + body +
        '\t\t\tctr.u64 = ctr.u64 - n;  // the copy\'s counter after its n decrements\n'
        '#if !defined(REX_STORE_CARDS_OFF)\n'
        '\t\t\tCard(base, uint32_t(first), uint32_t(bytes - 3));  // the pages of the stores\' first bytes\n'
        '#endif\n')
cond = ('n != 0 && ::sa2::hot::fast_loop::Flat(first, bytes) && '
        '::sa2::hot::fast_loop::Apart(first, bytes, l_r3.u32, 48) && ::sa2::hot::fast_loop::Flat(l_r3.u32, 48)')
regs = ['l_f1', 'l_f2', 'l_f3', 'l_f4', 'l_f5', 'l_f6', 'l_f7', 'l_f8', 'l_f9', 'l_f10', 'l_f11', 'l_f12', 'l_f13',
        'f0', 'l_r10', 'r11', 'ctr', 'ea', 'temp']
src = fl.wrap_loop(src, rest_label, rest_end, rest, setup, cond, fast, [('uint32_t(first)', 'uint32_t(bytes)')], regs,
                   17, 'sub_82147628 fast points (the rest)', 'check1')
print(f'matrix loads held: {n_main} a pass of four points, {n_rest} a point')

header = r'''// sa2-nx (2026-10-28): hand-written sub_82147628, an array of r6 points (12 bytes each, from r4) by the 3x4 matrix at
// r3 to r5: four points a pass, then the rest one by one. Made from its localized copy (app/generated/hot/n_82147628.h),
// which stays its reference: re-run tools/hand_natives/make_point_transform.py after the copy changes (it takes the
// loops' statements from the copy and checks the forms it rewrites).
//
// Optimisation: every store marks a card, a byte store that may alias anything, so the copy reloads the matrix's 12
// floats for every point (48 loads a pass of four). The fast loops load the matrix once and mark the outputs' cards
// once.
//
// Audit: a fast loop applies when its outputs (the copy's [r10+4] onwards: 48 bytes a pass, 12 a point, contiguous)
// neither wrap at 4 GB nor overlap the matrix [r3, r3+48) (which is one host range): then every matrix load of the
// copy reads what the fast loop holds. Each statement is otherwise the copy's in its order, its point loads and output
// stores included (so outputs over the inputs, in place, read what the copy reads), with plain stores; the registers
// each loop leaves are the copy's last pass's or point's. The cards are the pages of the stores' first bytes (stores
// four bytes apart over the whole range: every page from the first to the last), zeroed after the data. Otherwise the
// copy's loop runs.
//
// Unguarded like its copy (bulk); in verify mode (sa2_cpu_verify) each fast loop checks itself against the copy's
// (hot/fast_loop_check.h).
'''
fl.finish(src, '82147628', 'n_point_transform', header, 'n_point_transform.h')
