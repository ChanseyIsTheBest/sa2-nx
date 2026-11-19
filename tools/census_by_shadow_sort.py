import re
import sys
from collections import defaultdict

# The draw-kind census by whether the shadow sort was replaying in that 10 s report: each "[renut] shadow sort" line
# says how many runs it replayed sorted; the census line of the same report (the next "draws by kind" after it, or
# before it: the nearest within the report) is labelled sorted / as recorded. Usage: census_halves.py renut.log
sort_pat = re.compile(r'shadow sort \(banjo_renut_shadow_sort\).*?replayed sorted (\d+) runs \((\d+) draws\)')
kind_pat = re.compile(r'(shadow \(depth target, no pixel shader\)|colour): (\d+) \(([0-9.]+) %\), ([0-9.]+) us a draw')
changed_pat = re.compile(r'changed: ([^;]*)')
events = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = sort_pat.search(line)
    if m:
        events.append(('sort', int(m.group(1)), int(m.group(2))))
    elif 'draws by kind (banjo_renut_diag_draw_kinds)' in line:
        events.append(('kinds', line))
acc = defaultdict(lambda: defaultdict(lambda: [0, 0.0, defaultdict(float)]))
label = None
for e in events:
    if e[0] == 'sort':
        label = 'sorted' if e[1] > 0 else 'recorded'
        continue
    if label is None:
        continue
    for m in kind_pat.finditer(e[1]):
        kind = 'shadow' if m.group(1).startswith('shadow') else 'colour'
        n, us = int(m.group(2)), float(m.group(4))
        a = acc[label][kind]
        a[0] += n
        a[1] += n * us
        cm = changed_pat.search(e[1], m.end())
        for f, pct in re.findall(r'([a-z ]+?) (\d+) %', cm.group(1) if cm else ''):
            a[2][f.strip()] += n * int(pct)
    label = None
for label in ('recorded', 'sorted'):
    for kind in ('shadow', 'colour'):
        n, t, fields = acc[label][kind]
        if n:
            top = sorted(fields.items(), key=lambda x: -x[1])[:8]
            print('%-8s %-6s: %9d draws, %.2f us a draw; changed: %s' % (
                label, kind, n, t / n, ', '.join('%s %.0f %%' % (f, v / n) for f, v in top)))
