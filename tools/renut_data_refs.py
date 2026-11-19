#!/usr/bin/env python3
"""katamari-nx (2026-10-30): pairs the guest data addresses Renut hard-codes (N&B title update 3) with Beautiful
Katamari's, through the functions already paired (out/renut_map/pairs_*.txt): each function's data references
(`lis rX,hi` then `addi / lwz / lfs / stw ... lo(rX)`) in order, N&B's beside this game's.

  python tools/renut_data_refs.py --nb <banjo-nx app/generated> --game <tree> --want 0x82DA3478,0x82161B30 [--pairs ...]
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from renut_sigmatch import load_tree  # noqa: E402

LIS = re.compile(r'^lis (r\d+),(-?\d+)$')
USE = re.compile(r'^(\w+\.?) (?:\w+,)?(-?\d+)\((r\d+)\)$|^(addi|ori) (r\d+),(r\d+),(-?\d+)$')


def refs(ins):
    hi = {}
    out = []
    for t in ins:
        m = LIS.match(t)
        if m:
            hi[m.group(1)] = (int(m.group(2)) << 16) & 0xFFFFFFFF
            continue
        m = USE.match(t)
        if not m:
            continue
        if m.group(1):
            off, reg = int(m.group(2)), m.group(3)
        else:
            reg, off = m.group(6), int(m.group(7))
        if reg in hi:
            out.append((hi[reg] + off) & 0xFFFFFFFF)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nb', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--want', required=True)
    ap.add_argument('--pairs', nargs='+', required=True)
    a = ap.parse_args()
    want = {int(x, 16) for x in a.want.split(',')}
    nb = load_tree(a.nb)
    game = load_tree(a.game)
    for path in a.pairs:
        for line in open(path):
            p = line.split()
            if len(p) < 2 or line.startswith('#') or p[0] not in nb or p[1] not in game:
                continue
            rx, ry = refs(nb[p[0]][4]), refs(game[p[1]][4])
            hit = [x for x in rx if x in want]
            if hit:
                print(f'{p[0]} -> {p[1]} {p[2] if len(p) > 2 else ""}: wanted {[hex(x) for x in hit]}')
                print(f'   N&B : {" ".join(hex(x) for x in rx[:24])}')
                print(f'   game: {" ".join(hex(x) for x in ry[:24])}')


if __name__ == '__main__':
    main()
