#!/usr/bin/env python3
"""banjo-nx: hottest functions of a Switch run by ELF symbol (logs/rex/rex_profile.log).

Like tools/switch_hot_functions.py, but names every address from the ELF's own symbol table instead of the link
map's .text.<function> sections, so code built without -ffunction-sections (Mesa/NVK, libnx, assembly) gets a name
too instead of "?". Needs the ELF of the SAME build (builds/<date>/renut.elf) and devkitA64's nm.

  python tools/switch_hot_symbols.py rex_profile.log --elf builds/<date>/renut.elf [--top 40] [--thread "GPU ring"]
      [--from-report 3] [--group]

--group adds up the samples per area (game code, renderer, driver, C library, ...).
"""
import argparse
import bisect
import collections
import re
import subprocess

NM = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-nm.exe'


def load_symbols(elf):
    out = subprocess.run([NM, '-n', '--defined-only', '-C', elf], capture_output=True, text=True, errors='replace').stdout
    starts, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) < 3 or parts[1] not in 'tTwW':
            continue
        starts.append(int(parts[0], 16))
        names.append(parts[2])
    return starts, names


def area(name):
    if name.startswith('sub_8') or name.startswith('rex_') or name.startswith('__imp__'):
        return 'game code (recompiled)'
    if 'masseffect::native' in name or 'me::native' in name or name.startswith('XXH'):
        return 'renderer'
    if name.startswith('nvk_') or name.startswith('nv50_') or name.startswith('nvc0_') or name.startswith('vk_') or \
            name.startswith('nouveau_') or name.startswith('nil_') or name.startswith('mme_') or 'nvkmd' in name or \
            name.startswith('util_') or name.startswith('_mesa') or name.startswith('nv_push') or \
            name.startswith('P_') or name.startswith('nvk'):
        return 'driver (Mesa/NVK)'
    if name.startswith('rex::') or name.startswith('banjo') or 'banjo_' in name:
        return 'SDK and app'
    if name.startswith('mem') or name.startswith('_malloc') or name.startswith('_free') or name.startswith('__') or \
            name.startswith('str') or name.startswith('_'):
        return 'C library'
    return 'other host code'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--elf', required=True)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--thread', default='')
    ap.add_argument('--from-report', type=int, default=1)
    ap.add_argument('--group', action='store_true')
    args = ap.parse_args()
    starts, names = load_symbols(args.elf)
    per_thread = collections.defaultdict(collections.Counter)
    report = 0
    thread = None
    pc_re = re.compile(r'pc (image\+)?0x([0-9a-f]+) x(\d+)')
    for line in open(args.log, encoding='utf-8', errors='replace'):
        if line.startswith('===='):
            report += 1
            thread = None
            continue
        # The 10 s histogram's sections carry the thread id ('-- thread 9093 "GPU ring native": CPU 95.6% ...'); the
        # ones without it are the stacks taken during hitches, a different sample.
        m = re.match(r'\s*-- thread (\d+ )?"([^"]*)"', line)
        if m:
            thread = m.group(2) if m.group(1) else None
            continue
        if report < args.from_report or thread is None:
            continue
        m = pc_re.search(line)
        if not m:
            continue
        count = int(m.group(3))
        if not m.group(1):
            per_thread[thread]['(outside the image)'] += count
            continue
        offset = int(m.group(2), 16)
        i = bisect.bisect_right(starts, offset) - 1
        per_thread[thread][names[i] if i >= 0 else '?'] += count
    total = sum(sum(c.values()) for c in per_thread.values())
    print('%d busy samples in %d threads (reports %d on)' % (total, len(per_thread), args.from_report))
    for name, c in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
        print('  %5.1f%%  %s' % (100.0 * sum(c.values()) / max(1, total), name))
    for name, c in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
        if args.thread and args.thread not in name:
            continue
        n = sum(c.values())
        print('\n== thread "%s" (%d samples) ==' % (name, n))
        if args.group:
            groups = collections.Counter()
            for symbol, count in c.items():
                groups[area(symbol)] += count
            for g, count in groups.most_common():
                print('  %5.1f%%  %6d  %s' % (100.0 * count / n, count, g))
        for symbol, count in c.most_common(args.top):
            print('  %5.1f%%  %6d  %s' % (100.0 * count / n, count, symbol[:150]))
        if not args.thread:
            break


if __name__ == '__main__':
    main()
