"""renut-ultimate (2026-11-12, port 121; sasrt-nx's batch_cc2 vec_abi_census.py on the base): which vector registers the
guest functions read on entry, the pointer-reached ones above all. tools/reg_liveness.py takes v0 and v32-v63 as never
read there (the 360 convention: v1 is the first vector argument and the result, not v2). Run it on each game before
trusting that: the last line must say none. Beautiful Katamari (2026-11-12): none (a raw run's 47 hits were vspltish,
which reg_liveness's defuse takes as reading its destination, and lvlx / lvrx, which write the whole register in the
generated code: both counted here as writes).

  python tools/cc_entry_census.py [--gen app/generated] [--image out/renut_image.bin]
"""
import collections
import os
import re
import sys

sys.dont_write_bytecode = True
TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import dead_ctx_defs as dcd  # noqa: E402
import leaf_dead_stores as lds  # noqa: E402
import reg_liveness as rl  # noqa: E402

GEN = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else os.path.join(ROOT, 'app', 'generated')
IMAGE = sys.argv[sys.argv.index('--image') + 1] if '--image' in sys.argv else os.path.join(ROOT, 'out', 'renut_image.bin')
# Forms that write the whole destination without reading it (whatever reg_liveness's conservative defuse says).
FULL_WRITE = re.compile(r'^(lvlx|lvlx128|lvrx|lvrx128|vupkd3d128|vspltis[bhw]|vspltis[bhw]128)$')
SELF = re.compile(r'^(vxor|vxor128|vsubuwm|vsububm|vsubuhm|vandc|vandc128)$')  # of a register with itself

rl._ns['MID_ASM_HOOKS'] = lds.load_mid_asm_hooks(GEN)
named = dcd.named_functions(GEN)
bodies = dcd.all_functions(GEN, named)
taken = lds.address_taken(GEN, IMAGE, set(bodies), bodies)
ptr = {a for a, r in taken.items() if all(x in ('data word', 'lis pair') for x in r)}
VOLV = rl.V(0) | rl.V(1) | sum(rl.V(n) for n in range(2, 14)) | sum(rl.V(n) for n in range(32, 64))
ALLV = sum(rl.V(n) for n in range(128))
entry, ptr_entry, after = collections.Counter(), collections.Counter(), collections.Counter()
examples = collections.defaultdict(list)
entry_funcs = after_sites = skipped = 0
for addr, (path, lines) in bodies.items():
    nodes = rl.parse_function(lines)
    comments = [c for c in (lds.COMMENT.match(l) for l in lines) if c]
    if len(comments) != len(nodes):
        skipped += 1
        continue
    du = []
    for c in comments:
        mn = c.group(1).rstrip('.')
        ops = [o.strip() for o in c.group(2).split(',')]
        try:
            d, u = rl.defuse(c.group(1), c.group(2))
        except Exception:
            d, u = 0, rl.regs(c.group(2))
        if u == rl.ALL or (u & ALLV) == ALLV:
            u = rl.regs(c.group(2))
        if (FULL_WRITE.match(mn) or (SELF.match(mn) and len(ops) == 3 and ops[1] == ops[2])) and ops:
            dv = rl.regs(ops[0]) & ALLV
            u &= ~dv
            d |= dv
        du.append((d & ALLV, u & ALLV))
    n = len(nodes)
    live = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = 0
            for s in nodes[i].succ:
                out |= live[s]
            d, u = du[i]
            new = ((out & ~(d | VOLV)) | u) if (nodes[i].call or nodes[i].indirect) else ((out & ~d) | u)
            if new != live[i]:
                live[i] = new
                changed = True
    ue = live[0] if n else 0
    entry_funcs += bool(ue)
    for k in range(128):
        if ue >> (64 + k) & 1:
            entry['v%d' % k] += 1
            if addr in ptr:
                ptr_entry['v%d' % k] += 1
                if len(examples['v%d' % k]) < 4:
                    examples['v%d' % k].append('%08X' % addr)
    for i, node in enumerate(nodes):
        if not (node.call or node.indirect) or (node.call and node.tail):
            continue
        a = 0
        for s in node.succ:
            a |= live[s]
        a &= VOLV
        after_sites += bool(a)
        for k in range(128):
            if a >> (64 + k) & 1:
                after['v%d' % k] += 1
print('functions %d (%d not one node a comment); reading a vector register on entry: %d' % (
    len(bodies), skipped, entry_funcs))
print('  by register:', entry.most_common(24))
print('call sites with a volatile vector register read right after: %d' % after_sites)
print('  by register:', after.most_common(24))
print('pointer-reached functions %d; vector registers read on entry:' % len(ptr), ptr_entry.most_common(30))
bad = {r: (k, examples[r]) for r, k in ptr_entry.items() if r == 'v0' or 32 <= int(r[1:]) <= 63}
print('pointer-reached functions reading v0 or v32-v63 on entry (must be none):', bad or 'none')
