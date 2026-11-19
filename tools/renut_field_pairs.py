#!/usr/bin/env python3
"""katamari-nx (2026-10-30): prints the device displacements (>= --min) of paired functions side by side, in order, for
reading the fields that tools/renut_field_diff.py cannot pair by position (different instruction counts).

  python tools/renut_field_pairs.py --nb <banjo-nx app/generated> --game <tree> sub_NB:sub_GAME [...] [--min 10944]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from renut_sigmatch import load_tree  # noqa: E402
from renut_field_diff import disps  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nb', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--min', type=int, default=10944)
    ap.add_argument('pairs', nargs='+')
    a = ap.parse_args()
    nb = load_tree(a.nb)
    game = load_tree(a.game)
    for p in a.pairs:
        x, y = p.split(':')
        dx = [f'{o}{v}' for o, v in disps(nb[x][4]) if v >= a.min]
        dy = [f'{o}{v}' for o, v in disps(game[y][4]) if v >= a.min]
        print(f'== {x} (N&B): {" ".join(dx)}')
        print(f'== {y} (game): {" ".join(dy)}')


if __name__ == '__main__':
    main()
