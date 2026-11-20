"""renut-ultimate (2026-11-12, port 121): tools/reg_liveness.py's exit bound checked on a game. A pointer-reached
function's copy leaves out the write-backs of reg_liveness.EXIT_DEAD (r0, r5-r12, f0, f5-f13, v0, v5-v13, v32-v63):
safe only if no caller reads one of them after an indirect call before writing it. Lists every such read (calls in
between count as writing nothing, so a read after a later direct call is listed too: look at it). Value-independent
forms do not count: vspltis* (immediates), lvlx / lvrx (whole-register writes in the generated code), vxor / vsub of a
register with itself, `mr rX,rX`. Beautiful Katamari (2026-11-12): 6 of 12,610 sites, all the debug trap
`twi 31,r0,22` past direct calls (ppc_trap only logs; before the `mr` rule also 66 `mr r8,r8`).

  python tools/cc_exit_census.py [--gen app/generated] [--all]

--all (sa2-nx-ult, 2026-11-19): every site, with the guest instructions from the call to the read.
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
NOREAD = re.compile(r'^(vspltis[bhw]|vspltis[bhw]128|lvlx|lvlx128|lvrx|lvrx128)$')
# banjo-nx (2026-11-19, the rebase; a handoff): more forms the generated code writes whole without reading the
# destination (checked in N&B's tree: REX_VUPKD3D_*, simde unpack / shift / convert / estimate stores of the sources
# only), where reg_liveness.defuse counts the destination as read; they count only when the destination is not also a
# source. And the float estimates defuse does not know (it answers "everything"): they read their one source.
NODEST = re.compile(r'^(vupkd3d128|vmrghw|vmrglw|vmrghw128|vmrglw128|vsraw|vsrw|vslw|vctsxs|vctuxs|vcfsx|vcfux|'
                    r'vexptefp|vlogefp|vrefp|vrsqrtefp|vexptefp128|vlogefp128|vrefp128|vrsqrtefp128)$')
FLOAT_EST = re.compile(r'^(fres|frsqrte)$')
SELF = re.compile(r'^(vxor|vxor128|vsubuwm|vsububm|vsubuhm|vandc|vandc128)$')

rl._ns['MID_ASM_HOOKS'] = lds.load_mid_asm_hooks(GEN)
named = dcd.named_functions(GEN)
bodies = dcd.all_functions(GEN, named)


def du(c):
    mn = c.group(1).rstrip('.')
    ops = [o.strip() for o in c.group(2).split(',')]
    if mn.startswith('('):
        return 0, 0  # banjo-nx: a post-codegen tool's note in the comments, "(tools/vec_lvlx_splat.py: ...)", no instruction
    if FLOAT_EST.match(mn) and len(ops) == 2:
        return rl.regs(ops[0]), rl.regs(ops[1])
    if NODEST.match(mn) and ops and ops[0] not in ops[1:]:
        try:
            d, u = rl.defuse(c.group(1), c.group(2))
        except Exception:
            return 0, rl.ALL
        dv = rl.regs(ops[0])
        return d | dv, u & ~dv
    try:
        d, u = rl.defuse(c.group(1), c.group(2))
    except Exception:
        return 0, rl.ALL
    if NOREAD.match(mn) and ops:
        dv = rl.regs(ops[0])
        return d | dv, u & ~dv
    if SELF.match(mn) and len(ops) == 3 and ops[1] == ops[2]:
        return d | rl.regs(ops[0]), u & ~rl.regs(ops[1])
    # banjo-nx (2026-11-19): `subfe rD,rX,rX` is ~rX + rX + CA = CA - 1: the carry's value only, not rX's (N&B: 96 such
    # reads after indirect calls).
    if mn in ('subfe', 'subfeo') and len(ops) == 3 and ops[1] == ops[2]:
        return d, u & ~rl.regs(ops[1])
    # `mr rX,rX` / `or rX,rX,rX` (the compiler's no-op markers): the register keeps its value whatever it is.
    if mn in ('mr', 'or') and ops and all(o == ops[0] for o in ops):
        return d, u & ~rl.regs(ops[0])
    # sa2-nx-ult (2026-11-19, SA2's census): `subfe rD,rA,rA` is ~rA + rA + CA = CA - 1, whatever rA holds (MSVC's
    # idiom for turning the carry into 0 / -1): it reads no value of rA.
    if mn == 'subfe' and len(ops) == 3 and ops[1] == ops[2]:
        return d | rl.regs(ops[0]), u & ~rl.regs(ops[1])
    return d, u


ALL = '--all' in sys.argv  # sa2-nx-ult: list every site (below)
sites = []
hits = collections.Counter()
examples = collections.defaultdict(list)
n_sites = n_bad = 0
for addr, (path, lines) in bodies.items():
    nodes = rl.parse_function(lines)
    if not any(node.indirect for node in nodes):
        continue
    comments = [c for c in (lds.COMMENT.match(l) for l in lines) if c]
    if len(comments) != len(nodes):
        continue
    dus = [du(c) for c in comments]
    n = len(nodes)
    live = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            out = 0
            for s in nodes[i].succ:
                out |= live[s]
            d, u = dus[i]
            if nodes[i].call or nodes[i].indirect:
                u = 0  # a call's own argument reads are not reads after the indirect call
            new = (out & ~d) | (u & rl.EXIT_DEAD)
            if new != live[i]:
                live[i] = new
                changed = True
    for i, node in enumerate(nodes):
        if not node.indirect:
            continue
        n_sites += 1
        after = 0
        for s in node.succ:
            after |= live[s]
        after &= rl.EXIT_DEAD
        if not after:
            continue
        n_bad += 1
        for b in range(192):
            if after >> b & 1:
                reg = ('r%d' % b) if b < 32 else ('f%d' % (b - 32)) if b < 64 else ('v%d' % (b - 64))
                hits[reg] += 1
                if len(examples[reg]) < 3:
                    examples[reg].append('%08X@%d' % (addr, i))
                if ALL:
                    # sa2-nx-ult (2026-11-19): every site, with the guest instructions from the call to the first
                    # instruction that reads the register (in the order of the function's text), to judge each by hand.
                    bit = 1 << b
                    reader = next((k for k in range(i + 1, n) if dus[k][1] & bit), None)
                    end = reader if reader is not None else min(n - 1, i + 6)
                    text = ' | '.join(c.group(1) + ' ' + c.group(2) for c in comments[i:end + 1])
                    sites.append('%08X@%d %s: %s' % (addr, i, reg, text))
print('indirect call sites %d; with a register of EXIT_DEAD read after the call: %d' % (n_sites, n_bad))
for reg, k in hits.most_common(40):
    print('  %-5s %5d  e.g. %s' % (reg, k, ', '.join(examples[reg])))
for s in sites:
    print(s)
