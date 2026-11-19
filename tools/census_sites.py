#!/usr/bin/env python3
"""Where the game code switches the host flush mode at run time (each switch is an `msr fpcr` on the console).

The PC build counts every switch by the code address it happens at (sa2_diag_runtime_census; rex/sys_counters.h,
CensusSite) and logs the most counted ones as offsets into the executable:
    [census] flush-mode switch sites (cumulative, image offset:count): +1A2B3C:123456 ...
This script takes the last such line of a log, turns each address into its function and generated source line
(llvm-symbolizer on the executable and its PDB), and prints the PowerPC instruction the switch belongs to (the
`// mnemonic operands` comment above the line in the generated file) with the direction of the switch.

  python tools/census_sites.py out/pc_runs/<run>/<run>.log [--exe app/out/pc/renut.exe] [--top 40]
"""
import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# renut-ultimate (2026-11-13): also the PC build's own compiler folder (devkitPro's MSYS2 clang64, CMAKE_CXX_COMPILER).
SYMBOLIZER = [os.path.join(os.path.dirname(ROOT), '_tools', 'llvm', 'bin', 'llvm-symbolizer.exe'),
              'C:/devkitPro/msys2/clang64/bin/llvm-symbolizer.exe', 'llvm-symbolizer']
LINE = re.compile(r'\[census\] flush-mode switch sites.*?:((?: \+[0-9A-F]+:\d+)+)')
# blur-nx (2026-11-12, codegen-ideas-3 CG-13's census): --denormal reads the denormal sites instead: the scalar results
# one flush mode for all code would change (an any-mode cold path's caller, or the switch on that ended an FPU-mode
# region with a denormal operand or an underflow inside).
DENORMAL_LINE = re.compile(r'\[census\] denormal sites \(CG-13.*?:((?: \+[0-9A-F]+:\d+)+)')
PER_SECOND = re.compile(r'\[census\] guest code per second: (\d+) flush-mode switches')
MNEMONIC = re.compile(r'^\s*// (\S+)\s*(.*)$')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--exe', default=os.path.join(ROOT, 'app', 'out', 'pc', 'renut.exe'))  # the base's PC build
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--denormal', action='store_true', help="CG-13's census: the denormal sites")
    a = ap.parse_args()
    line_re = DENORMAL_LINE if a.denormal else LINE
    sites, rates = None, []
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = line_re.search(line)
        if m:
            sites = [(int(x.split(':')[0][1:], 16), int(x.split(':')[1])) for x in m.group(1).split()]
        m = PER_SECOND.search(line)
        if m:
            rates.append(int(m.group(1)))
    if not sites:
        sys.exit('no "[census] flush-mode switch sites" line in %s (run with sa2_diag_runtime_census=true)' % a.log)
    tool = next((t for t in SYMBOLIZER if os.path.isfile(t)), SYMBOLIZER[-1])
    # The address is the return address of the count's call: one byte back is inside the calling line.
    query = '\n'.join('0x%X' % (rva - 1) for rva, _ in sites) + '\n'
    out = subprocess.run([tool, '--obj=' + a.exe, '--relative-address', '--functions=short', '--inlines'],
                         input=query, capture_output=True, text=True).stdout
    blocks = [b.strip().split('\n') for b in out.strip().split('\n\n')]
    total = sum(c for _, c in sites)
    if rates:
        tail = rates[-6:]
        print('switches per second, last reports: %s' % ', '.join(str(r) for r in tail))
    print('%d sites listed, %d switches among them (cumulative)\n' % (len(sites), total))
    by_mnemonic = collections.Counter()
    source_cache = {}
    for (rva, count), block in list(zip(sites, blocks))[:a.top]:
        # With --inlines the last pair is the outermost frame: the generated function and its line.
        function, where = (block[-2], block[-1]) if len(block) >= 2 else ('?', '?')
        inner = block[0] if len(block) >= 4 else ''
        direction = 'on ' if 'enable' in inner else 'off' if 'disable' in inner else '?  '
        m = re.match(r'^(.*):(\d+):\d+$', where)
        mnemonic = '?'
        if m and os.path.isfile(m.group(1)):
            lines = source_cache.setdefault(m.group(1), open(m.group(1), encoding='utf-8', errors='replace').readlines())
            i = int(m.group(2)) - 1
            while i >= 0 and i > int(m.group(2)) - 6:
                mm = MNEMONIC.match(lines[i])
                if mm:
                    mnemonic = (mm.group(1) + ' ' + mm.group(2)).strip()
                    break
                i -= 1
            where = '%s:%s' % (os.path.basename(m.group(1)), m.group(2))
        by_mnemonic[(mnemonic.split()[0], direction.strip())] += count
        print('%9d  %5.1f %%  %s  %-14s %-26s %s' % (count, 100.0 * count / total, direction, function, where, mnemonic))
    print('\nby instruction (of the listed sites):')
    for (mn, direction), count in by_mnemonic.most_common(20):
        print('%9d  %5.1f %%  %-3s %s' % (count, 100.0 * count / total, direction, mn))


if __name__ == '__main__':
    main()
