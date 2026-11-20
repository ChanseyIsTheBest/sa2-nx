#!/usr/bin/env python3
"""banjo-nx (2026-11-19): the reads behind tools/cc_exit_census.py's sites. For each indirect call site after which an
EXIT_DEAD register (reg_liveness.py) is read before it is written, the first instructions that read it (a forward walk
from the call that stops where the register is written), grouped by form, so each kind can be judged once.

  python tools/cc_exit_reads.py [--gen app/generated] [--top 60] [--examples 4]
"""
import collections
import os
import re
import sys

sys.dont_write_bytecode = True
TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
sys.argv_saved = list(sys.argv)
import cc_exit_census as cc  # noqa: E402  (runs the census itself first, as a module)

TOP = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 60
EX = int(sys.argv[sys.argv.index('--examples') + 1]) if '--examples' in sys.argv else 4
rl, lds = cc.rl, cc.lds


def reg_name(b):
    return ('r%d' % b) if b < 32 else ('f%d' % (b - 32)) if b < 64 else ('v%d' % (b - 64))


forms = collections.Counter()
where = collections.defaultdict(list)
for addr, (path, lines) in cc.bodies.items():
    nodes = rl.parse_function(lines)
    if not any(node.indirect for node in nodes):
        continue
    comments = [c for c in (lds.COMMENT.match(l) for l in lines) if c]
    if len(comments) != len(nodes):
        continue
    dus = [cc.du(c) for c in comments]
    for i, node in enumerate(nodes):
        if not node.indirect:
            continue
        for b in range(192):
            bit = 1 << b
            if not (rl.EXIT_DEAD & bit):
                continue
            # forward walk: the first reads of this register before a write of it
            seen, stack, reads = set(), list(node.succ), []
            while stack:
                j = stack.pop()
                if j in seen or j >= len(nodes):
                    continue
                seen.add(j)
                d, u = dus[j]
                if nodes[j].call or nodes[j].indirect:
                    u = 0
                if u & bit:
                    reads.append(j)
                    continue
                if d & bit:
                    continue
                stack.extend(nodes[j].succ)
            for j in reads:
                c = comments[j]
                mn = c.group(1)
                ops = re.sub(r'\b(r|f|v)\d+\b', lambda m: m.group(0) if m.group(0) == reg_name(b) else m.group(1) + 'N',
                             c.group(2))
                key = '%s %s  [reads %s]' % (mn, ops, reg_name(b))
                forms[key] += 1
                if len(where[key]) < EX:
                    where[key].append('%08X@%d->%d' % (addr, i, j))
print('forms of reads after indirect calls (EXIT_DEAD registers): %d forms, %d reads' % (len(forms),
                                                                                       sum(forms.values())))
for key, k in forms.most_common(TOP):
    print('%5d  %-60s %s' % (k, key[:60], ', '.join(where[key])))
