#!/usr/bin/env python3
"""Generates app/src/renut_engine/gpu/sa2_setter_marks_tags.h: for each of Direct3D's dirty tags (the device's
m_Pending words 2-4, D3DTAGCOLLECTION), the delta chain's chunks a write under that tag may have changed
(sa2_renut_setter_marks, sa2_setter_marks.h).

The game's own code sets render, sampler and fetch state inline (Direct3D's inline setters compiled into it) and sets
the field's tag with it, or the GPU would never get the new state; the out-of-line setters of the Direct3D range
(0x82540000-0x82570000, the device's call tables among them) are the same code. So each tag bit stands for the fields
those setters write with it: a straight-line reading of each setter's PPC (the comments in the recompiled code) finds
the tag constants it ORs into words 2-4 (std of (ld of the same word) | constant) and the device offsets it stores to
(the device: r3 at entry, and its copies by mr; the register save helpers keep r3), and every offset of a setter counts
for every bit it sets. A fetch constant's tag (word 3, bit 31 - n) also stands for its 24 bytes at 0x480 + 24 n (the
sampler setters and the stream and texture setters compute the bit, or take it from their caller). A bit no setter
names stands for every render state chunk (kStateChunks). The setters the static reading cannot follow (an unknown tag
value) are hooked whole (sa2_setter_marks.cpp), and the 32nd-draw check (sa2_renut_setter_marks_check) finds any
write this misses.

  python tools/d3d/setter_marks_tags.py [--print]"""
import glob
import os
import re
import sys
from collections import defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
GEN = ROOT + '/app/generated/default'
OUT = ROOT + '/app/src/renut_engine/gpu/sa2_setter_marks_tags.h'
MASK = (1 << 64) - 1
SNAPSHOT = 13088


def compared(off):  # the delta chain's chunks that sa2_renut_snapshot_deltas_narrow compares
    return 0x480 <= off < 0x780 or (0x2780 <= off < SNAPSHOT and not (11008 <= off < 12224))


def chunk_index(off):  # queued.cpp's chunk index: 30 head chunks, then the tail from 0x2780
    return off // 64 if off < 0x780 else 30 + (off - 0x2780) // 64


def rotl(v, n):
    n %= 64
    return ((v << n) | (v >> (64 - n))) & MASK


def mask_to(me):  # PPC rldicr's mask: bits 0..me from the msb
    return MASK ^ ((1 << (63 - me)) - 1)


def read_setters():
    out = {}
    for f in sorted(glob.glob(GEN + '/sa2_recomp.*.cpp')):
        lines = open(f, encoding='utf-8', errors='ignore').read().split('\n')
        i = 0
        while i < len(lines):
            m = re.match(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$', lines[i])
            if not m or not (0x82540000 <= int(m.group(1), 16) < 0x82570000):
                i += 1
                continue
            name = m.group(1)
            ppc = []
            i += 1
            while i < len(lines) and lines[i] != '}':
                s = lines[i].strip()
                if s.startswith('// ') and not s.startswith('// ---'):
                    ppc.append(s[3:])
                i += 1
            regs, dev = {}, {'r3'}
            tags, offs = defaultdict(int), set()
            for ins in ppc:
                op, _, rest = ins.partition(' ')
                a = [x.strip() for x in rest.split(',')] if rest else []

                def based(x):
                    mm = re.match(r'(-?\d+)\((r\d+)\)$', x)
                    return int(mm.group(1)) if mm and mm.group(2) in dev else None
                if op == 'mr':
                    (dev.add if a[1] in dev else dev.discard)(a[0])
                    regs[a[0]] = regs.get(a[1])
                    continue
                if op == 'bl' and a and re.match(r'0x827ef[d-e]', a[0]):
                    continue  # __savegprlr / __restgprlr
                if op.startswith('bl') and op != 'blr':
                    regs, _ = {}, dev.discard('r3')
                    continue
                if op == 'ld' and len(a) == 2 and based(a[1]) in (16, 24, 32):
                    regs[a[0]] = ('t', based(a[1]) // 8, 0)
                    if a[0] in dev:
                        dev.discard(a[0])
                    continue
                if op.startswith('st') and not op.endswith('x') and len(a) == 2 and based(a[1]) is not None:
                    off, s = based(a[1]), regs.get(a[0])
                    if op == 'std' and off in (16, 24, 32):
                        tags[off // 8] |= s[2] if s and s[0] == 't' and s[1] == off // 8 else MASK
                    else:
                        offs.add(off)
                    continue
                if not a or op.startswith(('st', 'cmp', 'tw', 'b')):
                    continue
                dev.discard(a[0])
                s = regs.get(a[1]) if len(a) > 1 else None
                if op == 'li':
                    regs[a[0]] = ('c', int(a[1], 0) & MASK)
                elif op == 'lis':
                    regs[a[0]] = ('c', (int(a[1], 0) << 16) & MASK)
                elif op in ('ori', 'oris'):
                    v = int(a[2], 0) << (16 if op == 'oris' else 0)
                    regs[a[0]] = (('c', s[1] | v) if s and s[0] == 'c' else ('t', s[1], s[2] | v) if s and s[0] == 't'
                                  else None)
                elif op == 'rldicr' and s and s[0] == 'c':
                    regs[a[0]] = ('c', rotl(s[1], int(a[2])) & mask_to(int(a[3])))
                elif op == 'sldi' and s and s[0] == 'c':
                    regs[a[0]] = ('c', (s[1] << int(a[2])) & MASK)
                elif op == 'or':
                    x, y = regs.get(a[1]), regs.get(a[2])
                    if x and y and {x[0], y[0]} == {'t', 'c'}:
                        t, c = (x, y) if x[0] == 't' else (y, x)
                        regs[a[0]] = ('t', t[1], t[2] | c[1])
                    elif x and y and x[0] == y[0] == 'c':
                        regs[a[0]] = ('c', x[1] | y[1])
                    else:
                        regs[a[0]] = None
                else:
                    regs[a[0]] = None
            if tags or offs:
                out[name] = (dict(tags), offs)
            i += 1
    return out


def main():
    setters = read_setters()
    lines_of_bit = defaultdict(set)   # (word, bit from the lsb) -> compared lines
    private_of_bit = defaultdict(bool)  # a setter of the bit stores to the private words (11008-12223)
    for name, (tags, offs) in setters.items():
        lines = {o & ~63 for o in offs if compared(o)}
        private = any(11008 <= o < 12224 for o in offs)
        for w, v in tags.items():
            if v == MASK:
                continue  # an unknown value: the setter is hooked whole
            for b in range(64):
                if v >> b & 1:
                    lines_of_bit[(w, b)] |= lines
                    private_of_bit[(w, b)] |= private
    # A bit stands for its setters' compared lines; a bit whose setters store only to the private words (which the
    # render thread never reads) for none; any other (no setter, or setters whose stores this reading cannot place:
    # through the ring pointer) for every render state chunk.
    named = {k for k in set(lines_of_bit) | set(private_of_bit) if lines_of_bit[k] or private_of_bit[k]}
    for n in range(32):  # fetch constant n: word 3, bit 31 - n
        k = (3, 31 - n)
        named.add(k)
        lines_of_bit[k] |= {o & ~63 for o in range(0x480 + 24 * n, 0x480 + 24 * n + 24)}
    # SetStreamSource (sub_82552408) sets bit 19 of word 2 when a stream's stride changes, and stores the stride at
    # 0x3268 + stream through device + stream, which this reading cannot place: the stride's chunk.
    lines_of_bit[(2, 19)].add(0x3240)
    state = sorted({o for o in range(0x2780, 0x2B00, 64)} | {o for o in range(0x3000, 0x3200, 64)})
    rows = []
    for w in (2, 3, 4):
        for b in range(64):
            k = (w, b)
            lines = sorted(lines_of_bit[k]) if k in named else state
            idx = sorted({chunk_index(o) for o in lines if compared(o)})
            words = [0, 0]
            for i in idx:
                words[i // 64] |= 1 << (i % 64)
            rows.append((w, b, k in named, lines, words))
    if '--print' in sys.argv:
        for w, b, known, lines, _ in rows:
            print(f'w{w} b{b:2d} {"setters" if known else "unknown"}: {" ".join(f"0x{x:X}" for x in lines)}')
        return
    with open(OUT, 'w', newline='\n') as o:
        o.write('// Generated by tools/d3d/setter_marks_tags.py (do not edit): per dirty tag (m_Pending words 2-4, the bit\n'
                '// from the least significant), the delta chain\'s chunks a write under it may have changed (chunk index i:\n'
                '// bit i % 64 of word i / 64; sa2_setter_marks.h). A tag the Direct3D range\'s setters never name stands for\n'
                '// every render state chunk.\n#pragma once\n\n#include <cstdint>\n\nnamespace sa2::renut {\n\n'
                'inline constexpr uint64_t kTagChunks[3 * 64][2] = {\n')
        for w, b, known, lines, words in rows:
            note = ' '.join(f'0x{x:X}' for x in lines if compared(x)) if known else 'unknown: the render state'
            o.write(f'    {{0x{words[0]:016X}ull, 0x{words[1]:016X}ull}},  // word {w} bit {b}: {note}\n')
        o.write('};\n\n}  // namespace sa2::renut\n')
    print(f'{OUT}: {sum(1 for r in rows if r[2])} tags named by setters, {len(setters)} setters read')


if __name__ == '__main__':
    main()
