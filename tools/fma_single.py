#!/usr/bin/env python3
"""blur-nx (2026-11-12, the user: "Please do the fmadds change"): the single-precision fused multiply-adds rounded once,
as the Xenon rounds them.

The generator writes fmadds / fmsubs / fnmadds / fnmsubs as
    X.f64 = double(float(std::fma(A.f64, B.f64, C.f64)));        (fmsubs: -C.f64; fnmadds / fnmsubs: -std::fma)
a double fused multiply-add rounded again to single: two roundings, where the Xenon rounds the exact result once. This
step writes them as
    X.f64 = REX_FMADDS(A.f64, B.f64, C.f64);                      (fnmadds / fnmsubs: (-REX_FMADDS(...)))
with REX_FMADDS = rex::ppc::FmaddsSingle (sdk/include/rex/ppc/fma_single.h): one single-precision fused multiply-add
when the three inputs are floats (the Xenon's result exactly), the double form otherwise. Negating the result commutes
with the rounding, so the negated forms keep their bits. tools/single_floats.py (sasrt-nx) does the same for the plain
operations, where the two roundings never differ; the fused ones differ in rare cases (about one operation in a few
hundred million for random inputs, by one unit in the last place), so this one is a choice, the user's (2026-11-12).
REX_FMADDS_DOUBLE_ROUNDING (CMake BLUR_FMADDS_SINGLE=OFF) gives the double forms back.

The statements stay where they are (the flush mode at each is the generator's: the scalar FPU's). Run before the machine
copies (tools/localize_native.py), so a copy and the original statements the guard compares it with have the same form;
the hand-written natives that emulate these instructions use REX_FMADDS too. Idempotent.
  python tools/fma_single.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import re
import sys

OPERAND = r'(-?[A-Za-z_][\w.]*)'
FORM = re.compile(r'double\(float\((-?)std::fma\(' + OPERAND + r', ' + OPERAND + r', ' + OPERAND + r'\)\)\)')

MARKER = '// blur-nx ports: the single-precision fused multiply-adds rounded once (tools/fma_single.py)'
MACROS = MARKER + '''
#ifndef REX_FMADDS
#include <rex/ppc/fma_single.h>
#define REX_FMADDS(a, b, c) rex::ppc::FmaddsSingle((a), (b), (c))
#endif  // REX_FMADDS
// (end of the macros of tools/fma_single.py)
'''


def rewrite(text):
    count = [0]

    def repl(m):
        count[0] += 1
        call = 'REX_FMADDS(%s, %s, %s)' % (m.group(2), m.group(3), m.group(4))
        return '(-%s)' % call if m.group(1) else call

    return FORM.sub(repl, text), count[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    if os.environ.get('BLUR_FMA_SINGLE_OFF'):
        print('fma_single: off (BLUR_FMA_SINGLE_OFF)')
        return 0
    total = files = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        new, n = rewrite(text)
        if n:
            total += n
            files += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    if not a.check:
        pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
        if len(pchs) != 1:
            sys.exit('ERROR (post-codegen patch): fma_single: no single *_pch.h in %s' % a.gen)
        text = open(pchs[0], encoding='utf-8').read()
        if MARKER not in text:
            anchor = '// rexglue ports: vcfsx / vcfux with a scale as one fixed-point conversion (tools/vec_cvt_fixed.py)'
            if anchor not in text:
                sys.exit('ERROR (post-codegen patch): fma_single: the vec_cvt_fixed macros are not in %s' % pchs[0])
            text = text.replace(anchor, MACROS + anchor, 1)
            open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    print('fma_single%s: %d single-precision fused multiply-adds rounded once (%d files)' % (
        ' (check only)' if a.check else '', total, files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
