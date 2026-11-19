#!/usr/bin/env python3
"""sa2-nx-ult (2026-11-20): candidates for the game's read of a stack word it never wrote (sa2-nx 1.0.6, docs/cpu-rounds.md
"The audit" item 1: with the prologue saves of the non-volatile registers removed, the two-player demo's characters
came out tinted; the saves write zeros into frames, and some function reads a frame word it never initialised).

For every generated function (the PowerPC instructions in the `// mnemonic operands` comments of app/generated):
  - its frame size N from `stwu r1,-N(r1)` (a function without one has no frame of its own);
  - every load from its own frame, `l?? rD,K(r1)` with 0 < K < N, after the stwu (the back chain at 0 and the caller's
    area from N up are not its own);
  - every store to its own frame (`st?? rS,K(r1)`, including stmw / stfd / stvx forms with an r1 base and offset),
    and every frame address it takes (`addi rX,r1,K`: a callee or a pointer may write there).
A load at an offset of the frame that the function neither stores to (of the load's width, anywhere in the function)
nor takes the address of is a candidate: what it reads is whatever an earlier call at the same stack depth left
there. Path order is not followed (a store anywhere in the function counts), so this lists fewer candidates than
exist, not more; vector forms with an index register are not followed either.

  python tools/sa2_stale_frame_reads.py [--gen app/generated] [--top 60]
"""
import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUNC = re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\((?:__imp__)?(sub_[0-9A-F]{8})\)')
INSN = re.compile(r'^\s*// ([a-z][a-z0-9.]*) (.*)$')
MEM = re.compile(r'^(r\d+|f\d+|v\d+),(-?\d+)\(r1\)$')
LOAD = re.compile(r'^(lbz|lhz|lha|lwz|lwa|ld|lfs|lfd)u?$')
STORE = re.compile(r'^(stb|sth|stw|std|stfs|stfd|stfiwx)u?$')
WIDTH = {'lbz': 1, 'lhz': 2, 'lha': 2, 'lwz': 4, 'lwa': 4, 'ld': 8, 'lfs': 4, 'lfd': 8,
         'stb': 1, 'sth': 2, 'stw': 4, 'std': 8, 'stfs': 4, 'stfd': 8}


def scan(path):
    out = []
    name = None
    insns = []

    def finish():
        if not name:
            return
        frame = None
        loads, stored, taken = [], set(), set()
        for i, (m, ops) in enumerate(insns):
            if m == 'stwu' and ops.startswith('r1,') and ops.endswith('(r1)') and frame is None:
                k = int(ops.split(',')[1].split('(')[0])
                if k < 0:
                    frame = -k
                continue
            if frame is None:
                continue
            if m == 'stmw':
                mm = re.match(r'^r(\d+),(-?\d+)\(r1\)$', ops)
                if mm:
                    first, k = int(mm.group(1)), int(mm.group(2))
                    for b in range(k, k + 4 * (32 - first)):
                        stored.add(b)
                continue
            mm = MEM.match(ops)
            if mm:
                k = int(mm.group(2))
                base = m.rstrip('u') if m.endswith('u') and m not in ('lhau',) else m
                if LOAD.match(m) and 0 < k < frame:
                    loads.append((i, m, k, WIDTH.get(base, 4)))
                elif STORE.match(m):
                    for b in range(k, k + WIDTH.get(base, 4)):
                        stored.add(b)
                continue
            if m == 'addi':
                parts = ops.split(',')
                if len(parts) == 3 and parts[1] == 'r1':
                    try:
                        taken.add(int(parts[2]))
                    except ValueError:
                        pass
        if frame is None:
            return
        for i, m, k, w in loads:
            if any(b in stored for b in range(k, k + w)):
                continue
            if any(t <= k < t + 1024 for t in taken):  # a taken address at or below it may cover it (an array, a struct)
                continue
            out.append((name, m, k, frame, os.path.basename(path)))

    for line in open(path, encoding='utf-8', errors='replace'):
        fm = FUNC.match(line)
        if fm:
            finish()
            name, insns = fm.group(1), []
            continue
        im = INSN.match(line)
        if im and name:
            insns.append((im.group(1), im.group(2).replace(' ', '')))
    finish()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated'))
    ap.add_argument('--top', type=int, default=60)
    a = ap.parse_args()
    found = []
    files = sorted(glob.glob(os.path.join(a.gen, 'renut_recomp.*.cpp')))
    if not files:
        sys.exit('no generated files in ' + a.gen)
    for f in files:
        found += scan(f)
    funcs = sorted({x[0] for x in found})
    print(f'{len(found)} loads of a frame word the function never stores to (nor takes the address of), in '
          f'{len(funcs)} functions of {len(files)} files')
    for name, m, k, frame, f in found[:a.top]:
        print(f'  {name}  {m} {k}(r1)  frame {frame}  {f}')


if __name__ == '__main__':
    main()
