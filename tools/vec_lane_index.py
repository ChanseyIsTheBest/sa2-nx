#!/usr/bin/env python3
"""blur-nx (2026-11-11): stvewx / stvehx lanes without a trip through memory (sdk/include/rex/ppc/vec_lane.h).
The generator writes the element stores of a vector register as a store of the lane the effective address picks:
    REX_STORE_U32(ea, V.u32[3 - ((ea & 0xF) >> 2)]);    ->   REX_STORE_U32(ea, REX_LANE_U32(V, 3 - ((ea & 0xF) >> 2)));
    REX_STORE_U16(ea, V.u16[7 - ((ea & 0xF) >> 1)]);    ->   REX_STORE_U16(ea, REX_LANE_U16(V, 7 - ((ea & 0xF) >> 1)));
A lane read with a run-time index makes the compiler store the whole register to the stack and load the lane back:
on the A57 that load waits for the wider store (no store forwarding). REX_LANE_U32 / REX_LANE_U16 read both halves
from the register and pick by the index's bits: the same value, no memory. Blur's console race of 2026-11-11: about a
quarter of the samples of sub_8251E6F0, and in four more hot functions (docs/hand-natives-jq012.md).
Exact by construction (vec_lane.h; the app's forms self-check tests every index on the device). Runs before
tools/localize_native.py, so the machine copies take the same form; the guard then compares two forms with the same
helper, which is why the self-check exists. REX_VEC_LANE_MEMORY (compile time) gives the indexed reads back.
  python tools/vec_lane_index.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import re
import sys

LANE32 = re.compile(r'([\w.]+)\.u32\[3 - \(\(ea & 0xF\) >> 2\)\]')
LANE16 = re.compile(r'([\w.]+)\.u16\[7 - \(\(ea & 0xF\) >> 1\)\]')
MARKER = '// rexglue ports: vector lanes by a run-time index without memory (tools/vec_lane_index.py)'
END = '// (end of the macros of tools/vec_lane_index.py)\n'
MACROS = MARKER + '''
#ifndef REX_LANE_U32
#if defined(REX_VEC_LANE_MEMORY)
#define REX_LANE_U32(v, i) ((v).u32[(i)])
#define REX_LANE_U16(v, i) ((v).u16[(i)])
#else
#include <rex/ppc/vec_lane.h>
#define REX_LANE_U32(v, i) rex::ppc::VecLaneU32((v).u32, uint32_t(i))
#define REX_LANE_U16(v, i) rex::ppc::VecLaneU16((v).u16, uint32_t(i))
#endif  // REX_VEC_LANE_MEMORY
#endif  // REX_LANE_U32
''' + END


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    n32 = n16 = changed = already = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        already += text.count('REX_LANE_U32(') + text.count('REX_LANE_U16(')
        new, c32 = LANE32.subn(r'REX_LANE_U32(\1, 3 - ((ea & 0xF) >> 2))', text)
        new, c16 = LANE16.subn(r'REX_LANE_U16(\1, 7 - ((ea & 0xF) >> 1))', new)
        n32 += c32
        n16 += c16
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vec_lane_index: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    if MACROS not in text and not a.check:
        anchor = '#define REX_LOAD_U8(x)'
        if MARKER in text:
            start = text.index(MARKER)
            text = text[:start] + MACROS + text[text.index(END, start) + len(END):]
        elif anchor in text:
            text = text.replace(anchor, MACROS + anchor, 1)
        else:
            sys.exit('ERROR (post-codegen patch): vec_lane_index: no REX_LOAD_U8 in %s' % pchs[0])
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    print('vec_lane_index%s: %d word and %d halfword lanes read from the register (%d files; %d already in that form)'
          % (' (check only)' if a.check else '', n32, n16, changed, already))


if __name__ == '__main__':
    main()
