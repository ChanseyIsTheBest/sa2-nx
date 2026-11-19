#!/usr/bin/env python3
"""blur-nx (2026-11-11, docs/vector-store-ideas.md VS-27; the user: "do all the exact ones"): vcfsx / vcfux with a
scale as one fixed-point conversion.
The generator writes `vcfsx vD, vB, n` (and vcsxwfp128 / vcuxwfp128 with a scale) as a conversion followed by a
multiply by 2^-n:
    simde_mm_mul_ps(simde_mm_cvtepi32_ps(X), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x38800000))))
    simde_mm_mul_ps(rex::ppc::simde_mm_cvtepu32_ps_(X), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x3F000000))))
On the AArch64 that is SCVTF/UCVTF, then FMUL by a constant register. SCVTF/UCVTF with #fbits does both in one
instruction: REX_VCFSX_N(X, n) / REX_VCFUX_N(X, n).
Exact: the conversion rounds to nearest, and multiplying by 2^-n only changes the exponent; for a nonzero 32-bit
integer and n <= 31 the result is at least 2^-31, a normal number, so it cannot underflow and the flush mode cannot
matter; zero stays +0. The fixed-point form rounds x / 2^n once, which is the same value. Off the AArch64 the macros are
the original text. The app's forms self-check compares both forms for every n this pass writes (banjo_forms_selfcheck.cpp,
VecCvtFixed).
  python tools/vec_cvt_fixed.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import re
import sys

ARG = r'(simde_mm_load_si128\(\(simde__m128i\*\)[\w.]+\))'
CONST = r'simde_mm_castsi128_ps\(simde_mm_set1_epi32\(int\((0x[0-9A-Fa-f]{8})\)\)\)'
SIGNED = re.compile(r'simde_mm_mul_ps\(simde_mm_cvtepi32_ps\(' + ARG + r'\), ' + CONST + r'\)')
UNSIGNED = re.compile(r'simde_mm_mul_ps\(rex::ppc::simde_mm_cvtepu32_ps_\(' + ARG + r'\), ' + CONST + r'\)')
ANY = re.compile(r'simde_mm_mul_ps\((?:simde_mm_cvtepi32_ps|rex::ppc::simde_mm_cvtepu32_ps_)\(')
MARKER = '// rexglue ports: vcfsx / vcfux with a scale as one fixed-point conversion (tools/vec_cvt_fixed.py)'
END = '// (end of the macros of tools/vec_cvt_fixed.py)\n'
MACROS = MARKER + '''
#ifndef REX_VCFSX_N
#include <rex/ppc/vec_batch1.h>
#define REX_VCFSX_N(x, n) rex::ppc::VcfsxN<n>(x)
#define REX_VCFUX_N(x, n) rex::ppc::VcfuxN<n>(x)
#endif  // REX_VCFSX_N
''' + END


def scale_of(hexword):
    """n for a constant 2^-n (1 <= n <= 31), else None."""
    v = int(hexword, 16)
    if v & 0x807FFFFF:
        return None
    n = 127 - (v >> 23)
    return n if 1 <= n <= 31 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    counts = {'s': {}, 'u': {}}
    left = changed = 0

    def repl(kind):
        def f(m):
            n = scale_of(m.group(2))
            if n is None:
                return m.group(0)
            counts[kind][n] = counts[kind].get(n, 0) + 1
            return ('REX_VCFSX_N(%s, %d)' if kind == 's' else 'REX_VCFUX_N(%s, %d)') % (m.group(1), n)
        return f

    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        new = SIGNED.sub(repl('s'), text)
        new = UNSIGNED.sub(repl('u'), new)
        left += len(ANY.findall(new))
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vec_cvt_fixed: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    if MACROS not in text and not a.check:
        anchor = '#define REX_LOAD_U8(x)'
        if MARKER in text:
            start = text.index(MARKER)
            text = text[:start] + MACROS + text[text.index(END, start) + len(END):]
        elif anchor in text:
            text = text.replace(anchor, MACROS + anchor, 1)
        else:
            sys.exit('ERROR (post-codegen patch): vec_cvt_fixed: no REX_LOAD_U8 in %s' % pchs[0])
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    fmt = lambda d: ', '.join('2^-%d x%d' % (n, c) for n, c in sorted(d.items())) or 'none'
    print('vec_cvt_fixed%s: signed %d (%s), unsigned %d (%s); %d files; %d scaled conversions left in another form'
          % (' (check only)' if a.check else '', sum(counts['s'].values()), fmt(counts['s']),
             sum(counts['u'].values()), fmt(counts['u']), changed, left))


if __name__ == '__main__':
    main()
