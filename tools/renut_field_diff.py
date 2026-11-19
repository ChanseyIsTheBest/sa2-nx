#!/usr/bin/env python3
"""katamari-nx (2026-10-30): compares the device fields (D-form displacements) of paired functions in N&B's and this
game's generated trees, for Renut's device layout (renut_device_offsets.py's question: did a field move between the
XDK builds?). Pairs come from out/renut_map/state_tables.txt and/or a list of "nb game" pairs.

  python tools/renut_field_diff.py --nb <banjo-nx app/generated> --game app/generated [--pairs file] [--states]

For each pair: the displacements only one side uses (N&B -> this game, when the instruction sequence is the same
length and position), and a summary of offset deltas across all pairs.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from renut_sigmatch import load_tree  # noqa: E402

DISP = re.compile(r'^(l[bhwd]z?u?|lha|lwa|st[bhwd]u?|lfs|lfd|stfs|stfd)\s+\w+,(-?\d+)\((r\d+)\)')


def disps(ins):
    out = []
    for t in ins:
        m = DISP.match(t)
        if m and m.group(3) != 'r1':
            out.append((m.group(1), int(m.group(2))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nb', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--pairs', default=None)
    ap.add_argument('--states', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()
    nb = load_tree(a.nb)
    game = load_tree(a.game)
    pairs = []
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if a.states:
        for line in open(os.path.join(root, 'out', 'renut_map', 'state_tables.txt')):
            if line.startswith('#'):
                continue
            kind, i, gs, x, y, _ = line.split()
            pairs.append((f'sub_{x}', f'sub_{y}', f'{kind}{i}{gs}'))
    if a.pairs:
        for line in open(a.pairs):
            p = line.split()
            if len(p) >= 2 and not line.startswith('#'):
                pairs.append((p[0], p[1], p[2] if len(p) > 2 else ''))
    deltas = collections.Counter()
    moved = collections.defaultdict(set)
    same = diff = 0
    for x, y, tag in pairs:
        if x not in nb or y not in game:
            print(f'{tag}: {x} or {y} missing')
            continue
        dx, dy = disps(nb[x][4]), disps(game[y][4])
        if dx == dy:
            same += 1
            continue
        diff += 1
        if len(dx) == len(dy) and all(p[0] == q[0] for p, q in zip(dx, dy)):
            for p, q in zip(dx, dy):
                if p[1] != q[1]:
                    deltas[q[1] - p[1]] += 1
                    moved[p[1]].add(q[1])
            if a.verbose:
                print(f'{tag} {x} {y}: ' + ', '.join(f'{p[1]}->{q[1]}' for p, q in zip(dx, dy) if p != q))
        else:
            print(f'{tag} {x} {y}: shapes differ: N&B {dx[:12]} / game {dy[:12]}')
    print(f'{same} pairs with the same displacements, {diff} different')
    print('deltas:', deltas.most_common(20))
    for k in sorted(moved):
        print(f'  N&B +{k} -> game {sorted(moved[k])}')


if __name__ == '__main__':
    main()
