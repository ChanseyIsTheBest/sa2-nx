#!/usr/bin/env python3
"""pacman-nx (2026-11-02): which accepted entries of app/overrides.toml are not functions but labels in the middle of
one. tools/pointer_fixpoint.py and tools/gap_sweep.py accept what decodes as a complete listing; three shapes are still
mid-function code, and declaring one splits its parent (the parent's branch becomes a tail call, and its registers
are read from zeroed locals under non_volatile_as_local):

  SEH    starts with `mr r8,r8`, MSVC's scope marker: the pointer is in a __try scope table (0x820FDFA4);
  FALL   the word before it is neither a terminator nor padding: the previous instruction runs into it (0x821A4750,
         `li r3,8` then `bl`, found by the gap sweep in an exception filter);
  BRANCH a conditional branch in the image targets it (a function's start is reached by bl, b or a pointer).

  python tools/override_labels_check.py [--image out/renut_image.bin]
"""
import argparse
import os
import re
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'renut_image.bin'))
    ap.add_argument('--code', default='820F0000:82458B7C')
    a = ap.parse_args()
    img = open(a.image, 'rb').read()
    lo, hi = (int(x, 16) for x in a.code.split(':'))
    word = lambda x: struct.unpack_from('>I', img, x - BASE)[0]
    cond = {}
    for pc in range(lo, hi, 4):
        w = word(pc)
        if w >> 26 == 16:  # bc
            d = w & 0xFFFC
            d = d - 0x10000 if d & 0x8000 else d
            if not w & 2:
                cond.setdefault(pc + d, []).append(pc)
    text = open(os.path.join(ROOT, 'app', 'overrides.toml'), encoding='utf-8').read()
    bad = 0
    for m in re.finditer(r'^"0x([0-9A-Fa-f]{8})" = \{[^}]*\}(.*)$', text, re.M):
        x = int(m.group(1), 16)
        why = []
        if word(x) == 0x7D084378:
            why.append('SEH')
        p = word(x - 4)
        terminator = (p in (0x4E800020, 0x4E800420, 0, 0x60000000) or p >> 26 == 18 and not p & 1
                      or (p >> 26 == 16 and not p & 1 and (p >> 21) & 0x14 == 0x14))
        if not terminator:
            why.append(f'FALL(prev {p:08X})')
        if x in cond:
            why.append('BRANCH from ' + ','.join(f'{c:08X}' for c in cond[x][:3]))
        if why:
            bad += 1
            print(f'0x{x:08X} {" ".join(why)} |{m.group(2)[:70]}')
    print(f'{bad} suspicious of {len(re.findall(chr(94) + chr(34) + "0x", text, re.M))}')


if __name__ == '__main__':
    main()
