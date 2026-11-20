#!/usr/bin/env python3
"""banjo-nx (2026-11-19): for guest functions, their direct callees and callers with names where the generated tree's
init table has one (a quick "what is this function" for a census site).

  python tools/guest_neighbours.py 82C354A0 [82C395E8 ...] [--gen app/generated]
"""
import collections
import os
import re
import sys

GEN = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else os.path.join('app', 'generated')
want = [a.upper() for a in sys.argv[1:] if re.fullmatch(r'[0-9A-Fa-f]{8}', a)]
names = {}
for f in os.listdir(GEN):
    if f.endswith('_init.cpp'):
        for line in open(os.path.join(GEN, f), encoding='utf-8'):
            m = re.match(r'^\s*\{ 0x([0-9A-F]{8}), (\w+) \},', line)
            if m:
                names[m.group(1)] = m.group(2)
callees = collections.defaultdict(collections.Counter)
callers = collections.defaultdict(collections.Counter)
func_re = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
addr_of = {v: k for k, v in names.items()}
for f in sorted(os.listdir(GEN)):
    if not re.match(r'\w+_recomp\.\d+\.cpp$', f):
        continue
    cur = None
    for line in open(os.path.join(GEN, f), encoding='utf-8'):
        m = func_re.match(line.rstrip('\n'))
        if m:
            n = m.group(1)
            cur = n[4:] if n.startswith('sub_') else addr_of.get(n)
            continue
        if cur and line.startswith('\t// bl 0x'):
            t = line.split('0x')[1].strip().upper().zfill(8)
            callees[cur][t] += 1
            callers[t][cur] += 1


def nm(a):
    n = names.get(a, '')
    return a + ('' if not n or n.startswith('sub_') else ' ' + n)


for a in want:
    print('%s' % nm(a))
    print('  callees:', ', '.join('%s x%d' % (nm(c), k) for c, k in callees[a].most_common(10)) or 'none')
    print('  callers:', ', '.join('%s x%d' % (nm(c), k) for c, k in callers[a].most_common(8)) or 'none (by pointer?)')
