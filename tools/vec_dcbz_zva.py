#!/usr/bin/env python3
"""blur-nx (2026-11-11, docs/vector-store-ideas.md VS-05; the user: "do all the exact ones"): a guest dcbzl's 128-byte
zero as two DC ZVA.
The generator writes dcbzl (and dcbz128) as `memset((void*)REX_RAW_ADDR(ea), 0, 128);` on a 128-byte aligned ea. GCC
inlines that as stores of zero registers: the A57 reads each line from memory before it overwrites it (a write
allocate), so a zeroed 128 bytes costs 128 bytes of reads too. Blur's audio jobs zero their 1 KB work blocks with runs
of dcbzl (sub_8230AD50, the hottest function of the console race, has no hand native) and are memory-bound
(blur-nx's docs/progress-log.md, 2026-11-11). DC ZVA zeroes a whole block in the cache without reading it: on the A57 the block is
64 bytes (DCZID_EL0 = 4), so two of them zero the guest line. Horizon allows it at EL0 (newlib's memset uses it).
REX_DCBZ128(ea) does that when rex_zva64_ok (set at start-up from DCZID_EL0: the block 64 bytes and ZVA permitted),
otherwise the memset. The asm statements name the 64 bytes each writes as their output, so the compiler knows exactly
what memory changed. Exact: the same 128 bytes become zero. The 32-byte dcbz stays a memset (smaller than a ZVA block).
Off the AArch64 REX_DCBZ128 is the memset. REX_DCBZ_MEMSET (compile time) gives the memset back everywhere.
  python tools/vec_dcbz_zva.py --gen app/generated [--check] [--restore]
"""
import argparse
import glob
import os
import sys

OLD = 'memset((void*)REX_RAW_ADDR(ea), 0, 128);'
NEW = 'REX_DCBZ128(ea);'
MARKER = '// rexglue ports: a guest dcbzl as two DC ZVA (tools/vec_dcbz_zva.py)'
END = '// (end of the macros of tools/vec_dcbz_zva.py)\n'
MACROS = MARKER + '''
#ifndef REX_DCBZ128
#include <rex/ppc/vec_batch1.h>
#define REX_DCBZ128(ea) rex::ppc::Dcbz128((void*)REX_RAW_ADDR(ea))
#endif  // REX_DCBZ128
''' + END


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    ap.add_argument('--restore', action='store_true',
                    help='the memsets back (post_codegen.py, first, with the other late steps: store_cards.py finds '
                         'the dcbz by the memset, tools/localize_native.py knows its store)')
    a = ap.parse_args()
    total = changed = 0
    if a.restore:
        for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))) + sorted(glob.glob(os.path.join(a.gen, 'hot', 'n_*.h'))):
            text = open(path, encoding='utf-8').read()
            n = text.count(NEW)
            if n:
                total += n
                changed += 1
                if not a.check:
                    open(path, 'w', encoding='utf-8', newline='\n').write(text.replace(NEW, OLD))
        print('vec_dcbz_zva --restore: %d DC ZVA pairs back to memsets (%d files)' % (total, changed))
        return
    # After tools/store_cards.py (which finds the dcbz by this memset's text to put its card in front): the generated
    # functions and the machine copies (hot/n_*.h), the files store_cards.py reads.
    files = sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))) + sorted(glob.glob(os.path.join(a.gen, 'hot', 'n_*.h')))
    for path in files:
        text = open(path, encoding='utf-8').read()
        n = text.count(OLD)
        if n:
            total += n
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(text.replace(OLD, NEW))
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vec_dcbz_zva: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    if MACROS not in text and not a.check:
        anchor = '#define REX_LOAD_U8(x)'
        if MARKER in text:
            start = text.index(MARKER)
            text = text[:start] + MACROS + text[text.index(END, start) + len(END):]
        elif anchor in text:
            text = text.replace(anchor, MACROS + anchor, 1)
        else:
            sys.exit('ERROR (post-codegen patch): vec_dcbz_zva: no REX_LOAD_U8 in %s' % pchs[0])
        open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    print('vec_dcbz_zva%s: %d 128-byte dcbz as two DC ZVA (%d files)' % (' (check only)' if a.check else '', total, changed))


if __name__ == '__main__':
    main()
