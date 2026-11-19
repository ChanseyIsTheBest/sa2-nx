#!/usr/bin/env python3
"""banjo-nx (package 27, codegen round): the generated functions the console's profile samples, together in the ELF.

The game code is about 60 MB of .text, written in the generator's order. The console profile of the game threads
(package 21b's diagnostics and fragments runs, 8,323 samples, out/cpu_proposals/prof_steady) puts its samples in
about 600 pages: every hot function on a page of its own among cold ones, which the A57's instruction TLB (48 entries
at the first level) and its 48 KB instruction cache keep missing. A declaration with __attribute__((hot)) in front of
each function app/config/hot_functions.txt names makes GCC put it in .text.hot.<name>, which devkitA64's switch.ld
gathers in one run (`*(.text.hot .text.hot.*)`) ahead of the rest of .text.

Exact: where the code is placed (and GCC's optimisation of those functions within -O3's rules), not what it does.
Only hot: unsampled functions are not marked cold, as cold also means optimised for size and the profile does not
cover every level. REX_HOT_LAYOUT_OFF (CMake BANJO_GEN_HOT_LAYOUT=OFF) leaves the declarations empty.

  hot_layout.py --gen <app/generated> [--list app/config/hot_functions.txt] [--restore]
  hot_layout.py --make-list <game_pcs.json> --out app/config/hot_functions.txt [--min 1]
"""
import glob
import json
import os
import re
import sys


def gen_file(gen, suffix):
    """renut-ultimate (2026-11-19, port 186; found by sasrt-nx's sync): the generator names its files after the project
    (<project>_pch.h, <project>_init.cpp, <project>_recomp.N.cpp): renut_ in the base, a port's own name otherwise."""
    for name in sorted(os.listdir(gen)):
        m = re.fullmatch(r'(\w+)_recomp\.\d+\.cpp', name)
        if m:
            return os.path.join(gen, m.group(1) + '_' + suffix)
    return os.path.join(gen, 'renut_' + suffix)


MARK = '// banjo-nx: hot (tools/hot_layout.py)'
FUNC_RE = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
PCH_MACRO = '''
// banjo-nx (package 27): tools/hot_layout.py. A function the console's profile samples is declared hot first, so GCC
// places it in .text.hot.*, which switch.ld gathers ahead of the rest of .text.
#ifndef REX_HOT_DECL
#if defined(__GNUC__) && !defined(__clang__) && !defined(REX_HOT_LAYOUT_OFF)
#define REX_HOT_DECL(name) extern "C" void __imp__##name(PPCContext& __restrict, uint8_t*) __attribute__((hot))
#else
#define REX_HOT_DECL(name) static_assert(true)
#endif
#endif
'''


def make_list(pcs_path, out, minimum):
    pcs = json.load(open(pcs_path))
    totals = {}
    for sym, d in pcs.items():
        name = sym[7:] if sym.startswith('__imp__') else sym
        name = name.split('.')[0]
        totals[name] = totals.get(name, 0) + sum(d.values())
    names = sorted(n for n, s in totals.items() if s >= minimum)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# banjo-nx (tools/hot_layout.py): the generated functions the console profile of the game threads samples\n'
                f'# (package 21b, out/cpu_proposals/prof_steady/game_pcs.json, at least {minimum} sample), placed together.\n')
        for n in names:
            f.write(n + '\n')
    print(f'hot_layout: {len(names)} functions listed')


def main():
    args = sys.argv[1:]
    if '--make-list' in args:
        make_list(args[args.index('--make-list') + 1], args[args.index('--out') + 1],
                  int(args[args.index('--min') + 1]) if '--min' in args else 1)
        return
    gen = args[args.index('--gen') + 1]
    root = os.path.abspath(os.path.join(gen, '..', '..'))
    listed = args[args.index('--list') + 1] if '--list' in args else os.path.join(root, 'app', 'config', 'hot_functions.txt')
    hot = set()
    if os.path.isfile(listed):
        hot = {l.strip() for l in open(listed, encoding='utf-8') if l.strip() and not l.startswith('#')}
    restore = '--restore' in args
    pch = gen_file(gen, 'pch.h')
    if not restore and os.path.isfile(pch):
        p = open(pch, encoding='utf-8').read()
        if 'REX_HOT_DECL' not in p:
            open(pch, 'w', encoding='utf-8', newline='\n').write(p + PCH_MACRO)
    marked = 0
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        lines = open(path, encoding='utf-8').read().split('\n')
        out = []
        for l in lines:
            if l.endswith(MARK):
                continue  # an earlier run's declaration: made again below where it still applies
            m = FUNC_RE.match(l)
            if m and not restore and m.group(1) in hot:
                out.append(f'REX_HOT_DECL({m.group(1)}); {MARK}')
                marked += 1
            out.append(l)
        if out != lines:
            open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    if not restore:
        print(f'hot_layout: {marked} functions declared hot (of {len(hot)} listed)')


if __name__ == '__main__':
    main()
