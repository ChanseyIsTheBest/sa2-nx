#!/usr/bin/env python3
"""Find code addresses the game builds in registers (lis + addi / ori) that land in unclaimed code gaps.

  usage: python3 tools/code_pointer_scan.py [--gen app/generated] [--gaps out/gaps/candidates.toml]

Port 167 (2026-11-18; the lesson of ds1-nx's chapter 3 crash, its commit d8acf12): tools/pointer_scan.py finds code
pointers stored in DATA; a pointer the code makes itself is never stored in the image. ds1-nx's game registered 347
static destructors at run time through thunks of the form

    lis r11, 0x82C1 ; addi r3, r11, imm ; b <atexit>

so the CRT's exit table, filled while the game ran, named functions the code generator had never made; three of them
were called at a level load and ended the game ("Call to invalid or unregistered function"). The code generator's own
scan for such pairs (sdk/src/codegen/phase_discover.cpp functionPointerScan) is disabled for its false positives; this
tool lists only the addresses that fall inside a gap find_gaps.py reports, for gap_classify validation, as
pointer_scan does for the data side.

Input: the generated C++ (one '\\t// <asm>' comment per instruction) and the candidate list tools/find_gaps.py writes
(out/gaps/candidates.toml). A lis value is followed within one function, and forgotten at a label (a block that may be
reached another way) or when its register is written by anything else the scan does not know as harmless.
"""
import argparse
import bisect
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RE_FUNC = re.compile(r'^DEFINE_REX_FUNC\((?:sub_)?([0-9A-Fa-f]{8})\)')
RE_INSN = re.compile(r'^\t// ([a-z.]+)\s*(.*)$')
RE_GAP = re.compile(r'"(0x[0-9A-Fa-f]+)" = \{ \}\s*#\s*(\d+) bytes')
RE_REG = re.compile(r'^r(\d+)$')


def parse_int(s):
    s = s.strip()
    return int(s, 16) if s.lower().startswith(('0x', '-0x')) else int(s)


def load_gaps(path):
    gaps = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = RE_GAP.search(line)
        if m:
            gaps.append((int(m.group(1), 16), int(m.group(2))))
    gaps.sort()
    return gaps


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated'))
    ap.add_argument('--gaps', default=os.path.join(ROOT, 'out', 'gaps', 'candidates.toml'))
    a = ap.parse_args()
    if not os.path.exists(a.gaps):
        sys.exit('%s does not exist: run tools/find_gaps.py first' % a.gaps)
    gaps = load_gaps(a.gaps)
    starts = [g[0] for g in gaps]

    def gap_of(v):
        i = bisect.bisect_right(starts, v) - 1
        if i >= 0 and gaps[i][0] <= v < gaps[i][0] + gaps[i][1]:
            return gaps[i]
        return None

    hits = {}
    files = sorted(f for f in os.listdir(a.gen) if f.endswith('.cpp'))
    for name in files:
        func = None
        lis = {}
        with open(os.path.join(a.gen, name), encoding='utf-8', errors='replace') as fh:
            for line in fh:
                if line.startswith('DEFINE_REX_FUNC('):
                    m = RE_FUNC.match(line)
                    func = int(m.group(1), 16) if m else None
                    lis = {}
                    continue
                if func is None:
                    continue
                if line.startswith('loc_'):
                    lis = {}
                    continue
                m = RE_INSN.match(line)
                if not m:
                    continue
                op, args = m.group(1), [x.strip() for x in m.group(2).split(',')]
                if op == 'lis' and len(args) == 2 and RE_REG.match(args[0]):
                    try:
                        lis[args[0]] = (parse_int(args[1]) & 0xFFFF) << 16
                    except ValueError:
                        lis.pop(args[0], None)
                    continue
                if op in ('addi', 'ori') and len(args) == 3 and args[1] in lis:
                    try:
                        imm = parse_int(args[2])
                    except ValueError:
                        imm = None
                    if imm is not None:
                        hi = lis[args[1]]
                        v = ((hi + imm) & 0xFFFFFFFF) if op == 'addi' else (hi | (imm & 0xFFFF))
                        if not (v & 3) and gap_of(v):
                            hits.setdefault(v, set()).add(func)
                    lis.pop(args[0], None)  # the target now holds the address, not a lis value
                    continue
                # Any other write of a tracked register forgets it (the first operand of most forms is the target).
                if args and args[0] in lis:
                    lis.pop(args[0], None)
    for v in sorted(hits):
        g = gap_of(v)
        refs = sorted(hits[v])
        print('0x%08X gap=0x%08X+%d size=%d refs=%d first_ref=sub_%08X' % (v, g[0], v - g[0], g[1], len(refs), refs[0]))
    print('# %d distinct code addresses built in registers that land in gaps' % len(hits), file=sys.stderr)


if __name__ == '__main__':
    main()
