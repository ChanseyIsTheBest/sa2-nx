#!/usr/bin/env python3
"""blur-nx (2026-11-11, docs/vector-ideas-2.md VX-20 and VX-21; the user: "do all the exact ones"): the vector word
shifts as whole-vector NEON shifts.
VX-20. The generator writes vsrw / vsraw (and the 128 forms) as four scalar lane statements:
    D.u32[0] = A.u32[0] >> (B.u8[0] & 0x1F);  ...  D.u32[3] = A.u32[3] >> (B.u8[12] & 0x1F);     (.s32 for vsraw)
four narrow stores into the register's union followed, at its next use, by a 16-byte load the A57 cannot forward from
them. Here: one statement, simde_mm_store_si128(D, REX_VSRW(A, B)) (REX_VSRAW for vsraw): on the AArch64 USHL / SSHL by
the negated count (lane & 31). Exact: B.u8[4k] is the low byte of lane k (little-endian host), and (byte & 0x1F) is
(lane & 31); every lane is computed from the inputs as they were, which is what the lane statements compute even
when D is A or B (lane k reads only lane k of A and B, written by no earlier lane statement).
VX-21. vslw is written as a block whose count is already masked:
    simde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi32(0x1F));
    simde_mm_store_si128((simde__m128i*)D.u8, simde_mm_sllv_epi32(a, shift));
simde's NEON sllv also compares the count with 32 and masks the result: dead for a count of 0..31. Here REX_SLLV_31,
a plain USHL. Exact for counts 0..31, which is what the mask leaves.
Off the AArch64 the macros are the original arithmetic. REX_VEC_SHIFT_SCALAR (compile time) gives the original forms.
The app's forms self-check compares them on the device (banjo_forms_selfcheck.cpp, VecShifts).
  python tools/vec_shift_whole.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import re
import sys

LANE = r'(?P<ind{k}>\s*)(?P<d{k}>[\w.]+)\.(?P<t{k}>u32|s32)\[{k}\] = (?P<a{k}>[\w.]+)\.(?P=t{k})\[{k}\] >> \((?P<b{k}>[\w.]+)\.u8\[{b}\] & 0x1F\);\n'
SRW = re.compile(''.join(LANE.format(k=k, b=4 * k) for k in range(4)))
SLLV = re.compile(r'(simde__m128i shift = simde_mm_and_si128\(b, simde_mm_set1_epi32\(0x1F\)\);\n\s*'
                  r'simde_mm_store_si128\(\(simde__m128i\*\)[\w.]+\.u8, )simde_mm_sllv_epi32\(a, shift\)\);')
MARKER = '// rexglue ports: the vector word shifts as whole-vector shifts (tools/vec_shift_whole.py)'
END = '// (end of the macros of tools/vec_shift_whole.py)\n'
MACROS = MARKER + '''
#ifndef REX_VSRW
#include <rex/ppc/vec_batch1.h>
#define REX_VSRW(A, B) rex::ppc::VecSrw(simde_mm_load_si128((const simde__m128i*)(A).u8), simde_mm_load_si128((const simde__m128i*)(B).u8))
#define REX_VSRAW(A, B) rex::ppc::VecSraw(simde_mm_load_si128((const simde__m128i*)(A).u8), simde_mm_load_si128((const simde__m128i*)(B).u8))
#define REX_SLLV_31(a, n) rex::ppc::VecSllv31((a), (n))
#endif  // REX_VSRW
''' + END


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    n = {'u32': 0, 's32': 0, 'sllv': 0, 'mixed': 0}
    changed = 0

    def srw(m):
        d, t, aa, b = m.group('d0'), m.group('t0'), m.group('a0'), m.group('b0')
        if any(m.group('d%d' % k) != d or m.group('t%d' % k) != t or m.group('a%d' % k) != aa or m.group('b%d' % k) != b
               for k in range(4)):
            n['mixed'] += 1
            return m.group(0)
        n[t] += 1
        mac = 'REX_VSRW' if t == 'u32' else 'REX_VSRAW'
        return '%ssimde_mm_store_si128((simde__m128i*)%s.u8, %s(%s, %s));\n' % (m.group('ind0'), d, mac, aa, b)

    def sllv(m):
        n['sllv'] += 1
        return m.group(1) + 'REX_SLLV_31(a, shift));'

    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        new = SRW.sub(srw, text)
        new = SLLV.sub(sllv, new)
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vec_shift_whole: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    if MACROS not in text and not a.check:
        anchor = '#define REX_LOAD_U8(x)'
        if MARKER in text:
            start = text.index(MARKER)
            text = text[:start] + MACROS + text[text.index(END, start) + len(END):]
        elif anchor in text:
            text = text.replace(anchor, MACROS + anchor, 1)
        else:
            sys.exit('ERROR (post-codegen patch): vec_shift_whole: no REX_LOAD_U8 in %s' % pchs[0])
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    print('vec_shift_whole%s: vsrw %d, vsraw %d as one vector shift; vslw %d without the dead clamp; %d lane groups '
          'not uniform (kept); %d files' % (' (check only)' if a.check else '', n['u32'], n['s32'], n['sllv'],
                                            n['mixed'], changed))


if __name__ == '__main__':
    main()
