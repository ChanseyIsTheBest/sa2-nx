#!/usr/bin/env python3
"""renut-ultimate (2026-11-16, port 144; tw2de_nx's fnmadd / fnmsub fix, sdk/include/rex/ppc/fpu_fma.h): the negated
fused multiply-adds through a macro, so that the sign of zero and NaN can be the PowerPC's.

The generator writes fnmadd / fnmsub as  X.f64 = -std::fma(A, B, C);  and tools/fma_single.py the single forms as
(-REX_FMADDS(A, B, C)) (tools/single_floats.py: (-REX_FMADDS_KNOWN(A, B, C))). GCC folds those negations into the
AArch64 FNMADD / FMSUB, which give +0 where the PowerPC gives -0 (a * c = +0 and b = +0 in fnmsub) and negate a NaN
result, which the PowerPC does not. This step writes each as REX_FNEG_FUSED(<the fused call>):
    -std::fma(A, B, C)          ->  REX_FNEG_FUSED(std::fma(A, B, C))
    (-REX_FMADDS(A, B, C))      ->  REX_FNEG_FUSED(REX_FMADDS(A, B, C))          (and REX_FMADDS_KNOWN)
    double(float(-std::fma(..)))->  double(float(REX_FNEG_FUSED(std::fma(..))))  (fma_single.py off)
REX_FNEG_FUSED is (-(x)) by default, the same expression as before (the same code: off by default, the user's word);
the CMake option TW2DE_FNMADD_SIGN (REX_FNMADD_SIGN_EXACT) makes it rex::ppc::FusedNeg: the fused result kept in its
register by an empty asm, then negated unless NaN (one more instruction or two a site). Katamari's tree: 272 sites.

Runs after tools/fma_single.py and before the copies (tools/localize_native.py), so a copy and the statements its guard
compares it with take the same form. --restore writes the forms back (post_codegen.py runs it first, so a re-run of the
steps sees the generator's forms). Idempotent.
  python tools/fnm_sign.py --gen app/generated [--check] [--restore]
"""
import argparse
import glob
import os
import re
import sys

OPERAND = r'(-?[A-Za-z_][\w.]*)'
ARGS = OPERAND + r', ' + OPERAND + r', ' + OPERAND
# The double form's statement: `= -std::fma(A, B, C);`
DOUBLE = re.compile(r'= -std::fma\(' + ARGS + r'\);')
# The single forms after fma_single.py / single_floats.py.
MACRO = re.compile(r'\(-(REX_FMADDS(?:_KNOWN)?)\(' + ARGS + r'\)\)')
# The single form when fma_single.py did not run.
SINGLE = re.compile(r'double\(float\(-std::fma\(' + ARGS + r'\)\)\)')
# What this step writes (for --restore).
UNDO_DOUBLE = re.compile(r'= REX_FNEG_FUSED\(std::fma\(' + ARGS + r'\)\);')
UNDO_MACRO = re.compile(r'REX_FNEG_FUSED\((REX_FMADDS(?:_KNOWN)?)\(' + ARGS + r'\)\)')
UNDO_SINGLE = re.compile(r'double\(float\(REX_FNEG_FUSED\(std::fma\(' + ARGS + r'\)\)\)\)')

MARKER = '// renut-ultimate port 144: the negated fused multiply-adds (tools/fnm_sign.py)'
MACROS = MARKER + '''
#ifndef REX_FNEG_FUSED
#if defined(REX_FNMADD_SIGN_EXACT)
#include <rex/ppc/fpu_fma.h>
#define REX_FNEG_FUSED(x) rex::ppc::FusedNeg((x))
#else
#define REX_FNEG_FUSED(x) (-(x))
#endif
#endif  // REX_FNEG_FUSED
// (end of the macros of tools/fnm_sign.py)
'''


def rewrite(text):
    n = [0]

    def count(s):
        n[0] += 1
        return s

    text = DOUBLE.sub(lambda m: count('= REX_FNEG_FUSED(std::fma(%s, %s, %s));' % m.groups()), text)
    text = MACRO.sub(lambda m: count('REX_FNEG_FUSED(%s(%s, %s, %s))' % m.groups()), text)
    text = SINGLE.sub(lambda m: count('double(float(REX_FNEG_FUSED(std::fma(%s, %s, %s))))' % m.groups()), text)
    return text, n[0]


def restore(text):
    n = [0]

    def count(s):
        n[0] += 1
        return s

    text = UNDO_DOUBLE.sub(lambda m: count('= -std::fma(%s, %s, %s);' % m.groups()), text)
    text = UNDO_SINGLE.sub(lambda m: count('double(float(-std::fma(%s, %s, %s)))' % m.groups()), text)
    text = UNDO_MACRO.sub(lambda m: count('(-%s(%s, %s, %s))' % m.groups()), text)
    return text, n[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    ap.add_argument('--restore', action='store_true', help='write the original forms back')
    a = ap.parse_args()
    total = files = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        new, n = (restore if a.restore else rewrite)(text)
        if n:
            total += n
            files += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    if not a.check and not a.restore:
        pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
        if len(pchs) != 1:
            sys.exit('ERROR (post-codegen patch): fnm_sign: no single *_pch.h in %s' % a.gen)
        text = open(pchs[0], encoding='utf-8').read()
        if MARKER not in text:
            anchor = '// rexglue ports: vcfsx / vcfux with a scale as one fixed-point conversion (tools/vec_cvt_fixed.py)'
            if anchor not in text:
                sys.exit('ERROR (post-codegen patch): fnm_sign: the vec_cvt_fixed macros are not in %s' % pchs[0])
            text = text.replace(anchor, MACROS + anchor, 1)
            open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    print('fnm_sign%s: %d negated fused multiply-adds %s (%d files)' % (
        ' (check only)' if a.check else '', total, 'restored' if a.restore else 'as REX_FNEG_FUSED', files))
    return 0


if __name__ == '__main__':
    sys.exit(main())
