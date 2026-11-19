#!/usr/bin/env python3
"""blur-nx (2026-11-11, docs/vector-store-ideas.md VS-08; the user: "do all the exact ones"): a guest dcbt / dcbtst
prefetches the whole 128-byte guest line.
The Xenon's cache line is 128 bytes, the A57's 64. The generator's REX_PREFETCH(x, write) is one host prefetch of
address x: half of the line the guest asked for. Blur's audio kernels (sub_822EA500, sub_822EA3C0, ...) issue eight
dcbt per 1 KB source, one per 128 bytes, so half of each source went unprefetched. This pass defines REX_PREFETCH
ahead of the generator's (which is #ifndef-guarded) as two host prefetches, of the 128-byte line holding x (x & ~127,
the line the Xenon would fetch) and of its second half. A prefetch is a hint: it cannot fault and changes no state, so
the program's results are unchanged. REX_PREFETCH_HALF_LINE (compile time) gives the generator's one prefetch back.
  python tools/vec_prefetch_line.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import sys

MARKER = '// rexglue ports: a guest dcbt prefetches the whole 128-byte line (tools/vec_prefetch_line.py)'
END = '// (end of the macros of tools/vec_prefetch_line.py)\n'
MACROS = MARKER + '''
#if !defined(REX_PREFETCH) && !defined(REX_PREFETCH_HALF_LINE)
#define REX_PREFETCH(x, write)                                                              \\
  do {                                                                                      \\
    const char* rex_prefetch_line_ = (const char*)REX_RAW_ADDR((u32)(x) & ~127u);           \\
    __builtin_prefetch(rex_prefetch_line_, (write));                                        \\
    __builtin_prefetch(rex_prefetch_line_ + 64, (write));                                   \\
  } while (0)
#endif
''' + END
ANCHOR = '// dcbt / dcbtst with the generator\'s dcbt_prefetch option'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    sites = 0
    for path in glob.glob(os.path.join(a.gen, '*_recomp.*.cpp')):
        sites += open(path, encoding='utf-8').read().count('REX_PREFETCH(')
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vec_prefetch_line: no single *_pch.h in %s' % a.gen)
    text = open(pchs[0], encoding='utf-8').read()
    state = 'already in place'
    if MACROS not in text:
        if MARKER in text:
            start = text.index(MARKER)
            text = text[:start] + MACROS + text[text.index(END, start) + len(END):]
        elif ANCHOR in text:
            text = text.replace(ANCHOR, MACROS + ANCHOR, 1)
        else:
            sys.exit('ERROR (post-codegen patch): vec_prefetch_line: the generator\'s REX_PREFETCH comment not found')
        state = 'macro written'
        if not a.check:
            open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    print('vec_prefetch_line%s: %s; %d REX_PREFETCH sites in the generated code (the copies take the same macro)'
          % (' (check only)' if a.check else '', state, sites))


if __name__ == '__main__':
    main()
