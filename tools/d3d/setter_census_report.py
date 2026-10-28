#!/usr/bin/env python3
"""Sums the setter census of a census build's log (sa2_diag_d3d_census; sa2_d3d_api.cpp): per Direct3D function, its
calls and the device lines (64 bytes) its calls changed; at the draws, the delta chunks changed and those no
census-bracketed call changed (inline writes by the game's own code).

  python tools/d3d/setter_census_report.py out/pc/logs/sa2_NNN.log [--top 40]"""
import re
import sys
from collections import defaultdict

log = open(sys.argv[1], encoding='utf-8', errors='replace').read()
top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 40

funcs = defaultdict(lambda: {'calls': 0, 'nested': 0, 'changes': 0.0, 'lines': set()})
for m in re.finditer(r'\[d3d census\] sub_([0-9A-F]{8}): (\d+) calls \((\d+) nested\), [\d.]+ PM4 words/call, \d+ segments; '
                     r'device lines changed ([\d.]+)/call:([^;]*);', log):
    f = funcs[m.group(1)]
    calls = int(m.group(2))
    f['calls'] += calls
    f['nested'] += int(m.group(3))
    f['changes'] += float(m.group(4)) * calls
    if m.group(5).strip() != 'none':
        f['lines'] |= {int(x) for x in re.findall(r'\+(\d+)', m.group(5))}

draws = chunks = inline = 0.0
by_chunk = defaultdict(lambda: [0, 0])
for m in re.finditer(r'\[d3d census\] draws (\d+): ([\d.]+) chunks changed a draw, ([\d.]+) of them by no census call '
                     r'\(inline writes\); inline by chunk:(.*)', log):
    n = int(m.group(1))
    draws += n
    chunks += float(m.group(2)) * n
    inline += float(m.group(3)) * n
    for c in re.finditer(r'\+(\d+) x(\d+) \(of (\d+)\)', m.group(4)):
        by_chunk[int(c.group(1))][0] += int(c.group(2))
        by_chunk[int(c.group(1))][1] += int(c.group(3))

print(f'draws {draws:.0f}: {chunks / max(draws, 1):.2f} chunks changed a draw (of the 40 compared), '
      f'{inline / max(draws, 1):.2f} by inline writes ({100 * inline / max(chunks, 1):.1f} %)')
print('\ninline writes by chunk (device offset: draws with an unattributed change, of draws with a change):')
for off, (n, of) in sorted(by_chunk.items(), key=lambda kv: -kv[1][0])[:30]:
    print(f'  +{off} (0x{off:X}): {n} of {of}')
print(f'\nDirect3D functions that change device lines, by calls (top {top}):')
rows = sorted(((f['calls'], a, f) for a, f in funcs.items() if f['lines']), reverse=True)
for calls, a, f in rows[:top]:
    lines = sorted(f['lines'])
    shown = ' '.join(f'0x{x:X}' for x in lines[:10]) + (' ...' if len(lines) > 10 else '')
    print(f'  sub_{a}: {calls} calls, {f["changes"] / max(calls, 1):.2f} lines changed a call; lines {shown}')
print(f'\n{len(funcs)} functions called, {len(rows)} of them changed device lines')
