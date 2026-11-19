#!/usr/bin/env python3
"""blur-nx (2026-11-11, CPU batch 2: docs/vector-ideas-2.md VX-29 and VX-02; the user: "do all the exact ones"): the
XDK's vector constants built at run time, folded where the source is a known splat.

The XDK builds 1.0 as vupkd3d128 type 0 of a zero register, and 0.5 / 0.25 / 2.0 ... as vcuxwfp128 / vcsxwfp128 of a
vspltisw. GCC folds neither (the probe of 2026-11-11: the table lookup `tbx` of a zero vector with its table load,
and `movi` + `ucvtf #n`). In the copies' text:
    simde_mm_store_si128((simde__m128i*)Z.u32, simde_mm_set1_epi32(int(0x0)));        vspltisw Z,0
    REX_VUPKD3D_COLOR(vTemp, Z);                                                       vupkd3d128 D,Z,0
      -> every lane (Z's byte | 0x3F800000) = 0x3F800000
    simde_mm_store_si128((simde__m128i*)S.u32, simde_mm_set1_epi32(int(K)));           vspltisw S,k
    simde_mm_store_ps(D.f32, REX_VCFUX_N(simde_mm_load_si128((simde__m128i*)S.u32), n)); vcuxwfp128 D,S,n
      -> every lane float(uint32(K)) * 2^-n, rounded to nearest (the conversion's one rounding: the scale by a power
         of two moves no significant bit); likewise REX_VCFSX_N with int32(K), and the unscaled
         rex::ppc::simde_mm_cvtepu32_ps_ / simde_mm_cvtepi32_ps.
Each becomes a store of the constant: simde_mm_set1_epi32(int(BITS)) (one movi / fmov). The splat source is a known
constant only in a straight stretch since its splat (no label, branch, block, call; no other statement naming it but
reads), followed through plain copies (`X = Y;`). A folded value is never a denormal or out of range (|K| < 2^32,
n <= 31), so the flush mode does not matter, as for the conversions themselves (VX-01).

Only the copies' f_ bodies (app/generated/hot/n_*.h), so the guard's original statements (o_) keep the run-time forms
and a guarded PC run compares both. BLUR_CONST_FOLD_OFF (environment): nothing.
  python tools/vec_const_fold.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import re
import struct
import sys

FUNC_START = re.compile(r'^(?:\[\[gnu::always_inline\]\] )?static (?:inline )?void ([fo])_sub_[0-9A-F]+\(.*\{\s*$')
REG = r'(?:ll_|l_)?v\d+|vTemp'
SPLAT = re.compile(r'^(?P<ind>\s*)simde_mm_store_si128\(\(simde__m128i\*\)(?P<d>' + REG + r')\.u(?:8|32), '
                   r'simde_mm_set1_epi32\(int\((?P<k>0x[0-9A-Fa-f]+|-?\d+)\)\)\);\s*$')
UPKD3D = re.compile(r'^(?P<ind>\s*)REX_VUPKD3D_COLOR\((?P<d>' + REG + r'), (?P<s>' + REG + r')\);\s*$')
CVT_N = re.compile(r'^(?P<ind>\s*)simde_mm_store_ps\((?P<d>' + REG + r')\.f32, REX_VCF(?P<su>S|U)X_N\(simde_mm_load_si128\('
                   r'\(simde__m128i\*\)(?P<s>' + REG + r')\.u32\), (?P<n>\d+)\)\);\s*$')
CVT_0 = re.compile(r'^(?P<ind>\s*)simde_mm_store_ps\((?P<d>' + REG + r')\.f32, (?P<f>rex::ppc::simde_mm_cvtepu32_ps_|simde_mm_cvtepi32_ps)\('
                   r'simde_mm_load_si128\(\(simde__m128i\*\)(?P<s>' + REG + r')\.u32\)\)\);\s*$')
COPY = re.compile(r'^\s*(?P<d>' + REG + r') = (?P<s>' + REG + r');\s*$')
BREAK = re.compile(r'\b(?:if|else|goto|return|switch|case|for|while|do|break|continue)\b|[{}]|^\s*[A-Za-z_]\w*:\s*$|'
                   r'\bsub_[0-9A-Fa-f]{8}\b|Native\(|REX_CALL|PPC_CALL|setjmp|longjmp|REX_LOOP_')
STOP_CHECK = 'if (s.Stopped()) return;'
DEST = re.compile(r'^\s*simde_mm_store_(?:si128|ps|pd)\((?:\(simde__m128i\*\))?(?P<d>[\w.]+?)\.(?:u8|u16|u32|u64|f32|f64), ')
ASSIGN = re.compile(r'^\s*(?P<d>[\w.\[\]]+)\s*=(?!=)')


def f32_bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def converted(k, signed, n):
    """Bits of float(k) * 2^-n, k a 32-bit pattern read as int32 or uint32, rounded once to nearest even."""
    v = k & 0xFFFFFFFF
    if signed and v >= 0x80000000:
        v -= 1 << 32
    # float(v) of a 32-bit integer is exact in a double, struct rounds it to the nearest float (ties to even)
    b = f32_bits(float(v))
    if n and b & 0x7FFFFFFF:
        exp = (b >> 23) & 0xFF
        assert exp - n >= 1, 'a denormal result'  # never: |v| >= 1 here and n <= 31
        b = (b & ~(0xFF << 23)) | ((exp - n) << 23)
    return b


def mentions(r, line):
    return re.search(r'(?<![\w.])' + re.escape(r) + r'\b', line) is not None


def destination(line):
    m = DEST.match(line)
    if m:
        return m.group('d')
    m = ASSIGN.match(line)
    if m:
        return m.group('d').split('.')[0].split('[')[0]
    return None


def transform_body(lines, stats):
    known = {}
    for i, l in enumerate(lines):
        s = l.strip()
        if not s or s.startswith('//') or s == STOP_CHECK or re.match(r'^s\.Hit\(\d+\);$', s):
            continue
        if BREAK.search(l):
            known.clear()
            continue
        m = SPLAT.match(l)
        if m:
            known[m.group('d')] = int(m.group('k'), 0) & 0xFFFFFFFF
            continue
        m = UPKD3D.match(l)
        if m and known.get(m.group('s')) == 0:
            lines[i] = '%ssimde_mm_store_si128((simde__m128i*)%s.u32, simde_mm_set1_epi32(int(0x3F800000)));  // vupkd3d128 0 of zero (tools/vec_const_fold.py)' % (m.group('ind'), m.group('d'))
            known[m.group('d')] = 0x3F800000
            stats['one'] += 1
            continue
        m = CVT_N.match(l) or CVT_0.match(l)
        if m and m.group('s') in known:
            if 'su' in m.groupdict() and m.group('su'):
                signed, n = m.group('su') == 'S', int(m.group('n'))
            else:
                signed, n = m.group('f') == 'simde_mm_cvtepi32_ps', 0
            k = known[m.group('s')]
            if n > 31:
                continue
            bits = converted(k, signed, n)
            lines[i] = '%ssimde_mm_store_si128((simde__m128i*)%s.u32, simde_mm_set1_epi32(int(0x%08X)));  // vc%sxwfp %d of 0x%X (tools/vec_const_fold.py)' % (
                m.group('ind'), m.group('d'), bits, 's' if signed else 'u', n, k)
            known[m.group('d')] = bits
            stats['cvt'] += 1
            continue
        m = COPY.match(l)
        if m:
            src = m.group('s')
            known.pop(m.group('d'), None)
            if src in known:
                known[m.group('d')] = known[src]
            continue
        # anything else: a register it may write is no longer known
        dest = destination(l)
        for r in list(known):
            if not mentions(r, l):
                continue
            if dest is not None and dest != r and dest.split('.')[0] != r and not ('&' + r) in l.replace(' ', ''):
                continue  # read only
            del known[r]
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (its hot/ copies)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    if os.environ.get('BLUR_CONST_FOLD_OFF'):
        print('vec_const_fold: off (BLUR_CONST_FOLD_OFF)')
        return 0
    # the arithmetic, against values worked out by hand
    assert converted(1, False, 1) == 0x3F000000 and converted(1, False, 2) == 0x3E800000
    assert converted(3, True, 0) == 0x40400000 and converted(0xFFFFFFFF, True, 0) == 0xBF800000
    assert converted(0xFFFFFFFF, False, 0) == 0x4F800000 and converted(0, False, 5) == 0
    assert converted(0x01000001, False, 0) == 0x4B800000  # 16777217 rounds to even
    stats = {'one': 0, 'cvt': 0}
    changed = 0
    for path in sorted(glob.glob(os.path.join(a.gen, 'hot', 'n_*.h'))):
        text = open(path, encoding='utf-8', newline='').read()
        if 'tools/vec_const_fold.py' in text:
            continue  # already done
        if '\r' in text:
            sys.exit('ERROR (post-codegen patch): vec_const_fold: %s has CR LF line ends' % path)
        lines = text.split('\n')
        out = []
        start = kind = None
        before = dict(stats)
        for l in lines:
            if start is None:
                m = FUNC_START.match(l)
                out.append(l)
                if m:
                    start, kind = len(out), m.group(1)
            else:
                if l == '}':
                    if kind == 'f':
                        out[start:] = transform_body(out[start:], stats)
                    start = None
                out.append(l)
        if stats != before:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print('vec_const_fold: %d vupkd3d128 of zero as 1.0, %d conversions of a known splat as constants, in %d copies' % (
        stats['one'], stats['cvt'], changed))
    return 0


if __name__ == '__main__':
    sys.exit(main())
