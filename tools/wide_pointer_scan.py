#!/usr/bin/env python3
"""pacman-nx (2026-11-02): katamari-nx's "could there be more?" scans of 2026-10-31 as one tool: addresses that can be called
through a pointer but are not function starts.

  1. every `lis` + `addi` / `ori` pair inside a function, with no instruction window (tools/pointer_fixpoint.py looks 32
     instructions ahead; this game's comparator 0x82306978 had its `lis` further away);
  2. every aligned data word outside the code that points into the code;
  3. vcall-thunk shapes with any registers (`lwz rA,0(r3); lwz rB,N(rA); mtctr rB; bctr`).

Each hit that is no function start is classed:
  GAP       in unclaimed bytes (tools/find_gaps.py's sense: no generated function covers it)
  BODY      inside a generated function's range
with the word before it: PAD (padding or a terminator: it can be a function's start) or FALL (an instruction runs into it: a
label, not a function). Jump-table case addresses and scope-table labels show as BODY or FALL.

  python -I tools/wide_pointer_scan.py [--image out/renut_image.bin] [--all]
"""
import argparse
import bisect
import glob
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'renut_image.bin'))
    ap.add_argument('--code', default='820F0000:82458B7C')
    ap.add_argument('--all', action='store_true', help='also the BODY hits')
    a = ap.parse_args()
    img = open(a.image, 'rb').read()
    lo, hi = (int(x, 16) for x in a.code.split(':'))
    word = lambda x: struct.unpack_from('>I', img, x - BASE)[0]
    gen = os.path.join(ROOT, 'app', 'generated')
    starts, ends = [], {}
    for p in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        cur = None
        for line in open(p, errors='replace'):
            m = re.match(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', line)
            if m:
                cur = int(m.group(1), 16)
                starts.append(cur)
                ends[cur] = cur
                continue
            # the listing's last instruction comment of the function gives its extent
            m = re.match(r'\s*// (\S.*)$', line)
            if m and cur is not None:
                ends[cur] += 4
    starts = sorted(set(starts))
    sset = set(starts)

    def covered(x):
        i = bisect.bisect_right(starts, x) - 1
        return i >= 0 and x < ends[starts[i]]

    def before(x):
        p = word(x - 4)
        term = p in (0x4E800020, 0x4E800420, 0, 0x60000000) or (p >> 26 == 18 and not p & 1)
        return 'PAD' if term else 'FALL'

    hits = {}
    # 1. lis + addi / ori in a function, no window
    for i, s in enumerate(starts):
        e = starts[i + 1] if i + 1 < len(starts) else hi
        his = {}
        for pc in range(s, min(e, hi), 4):
            w = word(pc)
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:
                his[rd] = (imm << 16) & 0xFFFFFFFF
            elif op == 14 and ra in his:
                t = (his[ra] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
                if lo <= t < hi and not t & 3:
                    hits.setdefault(t, []).append(f'built at {pc:08X}')
            elif op == 24 and ra in his and rd == ra:
                t = his[ra] | imm
                if lo <= t < hi and not t & 3:
                    hits.setdefault(t, []).append(f'built at {pc:08X}')
    # 2. data words
    for off in range(0, len(img) - 3, 4):
        x = BASE + off
        if lo <= x < hi:
            continue
        v = struct.unpack_from('>I', img, off)[0]
        if lo <= v < hi and not v & 3:
            hits.setdefault(v, []).append(f'data {x:08X}')
    # 3. thunk shapes
    for pc in range(lo, hi - 12, 4):
        a0, a1, a2, a3 = word(pc), word(pc + 4), word(pc + 8), word(pc + 12)
        if (a0 >> 26 == 32 and (a0 >> 16) & 31 == 3 and a0 & 0xFFFF == 0 and a1 >> 26 == 32
                and (a2 & 0xFC1FFFFF) == 0x7C0903A6 and a3 == 0x4E800420):
            hits.setdefault(pc, []).append('thunk shape')
    rows = []
    for t in sorted(hits):
        if t in sset:
            continue
        cls = 'BODY' if covered(t) else 'GAP'
        if cls == 'BODY' and not a.all:
            continue
        rows.append(f'{t:08X} {cls} {before(t):4} {hits[t][0]}' + (f' (+{len(hits[t]) - 1})' if len(hits[t]) > 1 else ''))
    print('\n'.join(rows))
    print(f'{len(rows)} hits ({len(hits)} addresses, {len(starts)} functions)')


if __name__ == '__main__':
    main()
