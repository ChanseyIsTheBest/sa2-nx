#!/usr/bin/env python3
"""blur-nx (2026-11-16, the 15 ms batch, CG-26 of docs/codegen-ideas-3.md): the compiled files that write the guest's
FPSCR, which keep -ftrapping-math when the rest of the game code gets -fno-trapping-math (app/CMakeLists.txt,
BLUR_NO_TRAPPING_MATH).

Why only these: -fno-trapping-math lets GCC compute a float operation earlier than the code does (out of a branch or a
loop). sa2-nx's audit kept it off because the generated code switched the host's flush mode in between (an
`msr fpcr` the compiler does not know a float operation depends on). Under BLUR_FPCR_ONE_MODE that switch is gone; the
only `msr fpcr` left in game code is the guest's own FPSCR write (mtfsf / mtfsfi / mtfsb0 / mtfsb1:
ctx.fpscr.storeFromGuest, which sets the rounding mode). A float operation moved across it would round in the other
mode, so the functions that write the FPSCR keep the old option: every generated file and every copy shard that has one.

Writes app/generated/fp_env_files.txt: one source per line, relative to app/ (generated/renut_recomp.N.cpp,
src/cpu/banjo_hot_shard_K.cpp, src/cpu/banjo_hot_guest.cpp). Rewritten only when it changes: CMake re-runs its
configure when it does (CMAKE_CONFIGURE_DEPENDS). Run by tools/post_codegen.py last, on the final text (after the
inline leaves, which copy small functions into other files).

  python tools/fp_env_files.py [--gen app/generated] [--print]
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
WRITE = 'fpscr.storeFromGuest('
INCLUDE = re.compile(r'^#include "(n_[0-9A-F]{8}\.h)"', re.M)


def has_write(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return WRITE in f.read()


def scan(gen):
    files = []
    for name in sorted(os.listdir(gen)):
        if re.fullmatch(r'\w+_recomp\.\d+\.cpp', name) and has_write(  # port 186: any project's
            os.path.join(gen, name)):
            files.append('generated/' + name)
    hot = os.path.join(gen, 'hot')
    if os.path.isdir(hot):
        writers = {n for n in os.listdir(hot) if n.startswith('n_') and n.endswith('.h') and
                   has_write(os.path.join(hot, n))}
        for name in sorted(os.listdir(hot)):
            if not name.endswith('.inc'):
                continue
            m = re.fullmatch(r'banjo_hot_bulk_(\d+)\.inc', name)
            if m:
                source = 'src/cpu/banjo_hot_shard_%s.cpp' % m.group(1)
            elif name == 'banjo_hot_list.inc':
                source = 'src/cpu/banjo_hot_guest.cpp'
            else:
                continue
            text = open(os.path.join(hot, name), encoding='utf-8', errors='replace').read()
            if any(h in writers for h in INCLUDE.findall(text)):
                files.append(source)
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', default=GEN)
    ap.add_argument('--print', action='store_true')
    args = ap.parse_args()
    files = scan(args.gen)
    text = ''.join(f + '\n' for f in files)
    out = os.path.join(args.gen, 'fp_env_files.txt')
    try:
        old = open(out, encoding='utf-8').read()
    except OSError:
        old = None
    if old != text:
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    print('fp_env_files: %d files write the guest FPSCR (they keep -ftrapping-math)' % len(files))
    if args.print:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
