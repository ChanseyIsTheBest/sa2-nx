#!/usr/bin/env python3
"""katamari-nx (2026-10-30): the `bl` / `b` instructions of an image dump that reach the given addresses (reads the
image only, so it is safe during a build that rewrites app/generated).

  python tools/ppc_callers.py 0x824E18D0 [0x...] [--image out/renut_image.bin] [--code 82210000:82A05350]
"""
import argparse
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('targets', nargs='+')
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'renut_image.bin'))
    ap.add_argument('--code', default='82210000:82A05350')
    a = ap.parse_args()
    img = open(a.image, 'rb').read()
    lo, hi = (int(x, 16) for x in a.code.split(':'))
    want = {int(t, 16) for t in a.targets}
    for off in range(lo - BASE, min(hi - BASE, len(img) - 3), 4):
        w = struct.unpack_from('>I', img, off)[0]
        if w >> 26 != 18:
            continue
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        addr = BASE + off
        t = (li if w & 2 else addr + li) & 0xFFFFFFFF
        if t in want:
            print(f'{addr:08X} {"bl" if w & 1 else "b "} {t:08X}')


if __name__ == '__main__':
    main()
