#!/usr/bin/env python3
"""sasrt-nx (2026-11-12, docs/calling-convention-plan.md, stage 1): whole-program register liveness of the guest code,
vector registers included, for the localized copies' call sites.

tools/leaf_dead_stores.py's analysis (UE to a fixed point over the call graph, per call site the registers live after
it, LiveOut to a fixed point, everything live for functions with an address outside direct calls) with three changes:
  - the masks hold the vector registers too (bits: r0-r31 0-31, f0-f31 32-63, v0-v127 64-191), with what each VMX /
    VMX128 instruction reads and writes (forms that also read their destination, or that this table does not know,
    read it: vmaddfp128 / vmaddcfp128 / vnmsubfp128 use vD as an input, vsel128 uses it as the mask, vrlimi128 and
    vpkd3d128 insert into it; the left / right / element loads keep the rest of the register);
  - an indirect call reads everything but the scratch vector registers v0, v32-v63 (THE assumption: a function
    reached through a pointer follows the calling convention, under which those are undefined on entry; the 360
    compiler cannot know the callers of such a function, LTCG included). General and float registers: everything, as
    in leaf_dead_stores.py;
  - the save / restore helpers read (save) or write (restore) the registers their name says, and r0, r11, r12.
Output (out/reg_liveness.json, with the tree it belongs to): per function its LiveOut (all banks) and per direct call
site (by the index of its guest instruction in the function) the registers that need not be handed to the callee:
neither read by the callee before it writes them (UE) nor live after the call (read later in the caller before being
written, or live after the caller returns). tools/localize_native.py leaves those stores out of a real call's hand-over
and leaves vector write-backs no caller reads out of a return (both exact: what nobody reads).

  python tools/reg_liveness.py --gen app/generated [--image out/renut_image.bin] [--check]

renut-ultimate (2026-11-12, port 121, sasrt-nx's batch_cc2: docs/ultimate-fixes.md row 121): main() follows the base's
leaf_dead_stores.py (the named guest functions analysed through tools/dead_ctx_defs.py, the border stubs resolved, the
hand natives of app/src/cpu/hot/hand.txt), and the base parser's mid-asm hook blocks (hook_block) take this file's
masks.
"""
import argparse
import collections
import inspect
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import leaf_dead_stores as lds  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_JSON = os.path.join(ROOT, 'out', 'reg_liveness.json')
NBITS = 192
ALL = (1 << NBITS) - 1


def R(n):
    return 1 << n


def F(n):
    return 1 << (32 + n)


def V(n):
    return 1 << (64 + n)


VMASK = sum(V(n) for n in range(128))
# The vector registers no function reached through a pointer reads on entry: v0 and v32-v63. NOT v1: in this game's
# code v1 is the first vector argument and the vector result (2026-11-12 census, scratchpad vec_abi_census.py: of
# the 45,146 pointer-reached functions, 100 read v1 on entry, 40 v2, 8 v3, none v0 or v32-v63; v1 is also the vector
# most read right after a call). Stage 1 had v1 here until 2026-11-12 16:30 (abCC1's build).
SCRATCH_V = V(0) | sum(V(n) for n in range(32, 64))
# sasrt-nx (2026-11-12): what a trap instruction reads besides its operands. ppc_trap (the generated pch) only logs and
# continues; the service traps (types 20, 26) read r3 / r4 for their debug string. Was ALL (leaf_dead_stores.py's
# rule), which made every function with a divide-by-zero check read every register. SASRT_REG_LIVENESS_TRAP_ALL=1: ALL.
TRAP_USE = (R(3) | R(4)) if os.environ.get('SASRT_REG_LIVENESS_TRAP_ALL', '0') != '1' else ALL
INDIRECT_USE = ALL & ~SCRATCH_V
# sasrt-nx (2026-11-12, stage 1b): calls through an app or SDK hook (a C++ replacement of the guest function) and calls
# of kernel imports / named functions follow the same convention: they read their arguments, never the scratch vector
# registers. SASRT_REG_LIVENESS_HOOKS_ABI=0: everything live at them, as leaf_dead_stores.py.
HOOKS_ABI = os.environ.get('SASRT_REG_LIVENESS_HOOKS_ABI', '1') != '0'
HOOK_USE = INDIRECT_USE if HOOKS_ABI else ALL
# sasrt-nx (2026-11-12, stage 1c): the exit side of the same convention. A function entered through a pointer (its
# address outside direct calls) or by no direct call at all returns to a caller that follows the convention: after the
# return it reads no volatile register but a result: v0, v5-v13, v32-v63, r0, r5-r12, f0 and f5-f13 are then dead
# at its exit (LiveOut; r3/r4, f1-f4 and v1-v4 kept as results, with a margin); a direct callee's LiveOut stays what its
# callers read (tools/localize_native.py's write-backs and the guard's skip set take this LiveOut for r/f too).
# SASRT_REG_LIVENESS_EXIT_ABI=0: everything live there, as before.
EXIT_ABI = os.environ.get('SASRT_REG_LIVENESS_EXIT_ABI', '1') != '0'
EXIT_DEAD = (SCRATCH_V | sum(V(n) for n in range(5, 14)) | R(0) | sum(R(n) for n in range(5, 13)) | F(0)
             | sum(F(n) for n in range(5, 14))) if EXIT_ABI else 0
REG = re.compile(r'(?<![\w])([rfv])(\d+)(?![\w])')


def regs(text):
    m = 0
    for kind, n in REG.findall(text):
        n = int(n)
        if kind == 'r' and n < 32:
            m |= R(n)
        elif kind == 'f' and n < 32:
            m |= F(n)
        elif kind == 'v' and n < 128:
            m |= V(n)
    return m


def reg_names(mask):
    out = []
    for b in range(NBITS):
        if mask >> b & 1:
            out.append('r%d' % b if b < 32 else 'f%d' % (b - 32) if b < 64 else 'v%d' % (b - 64))
    return out


# Vector instructions that write their first operand and read the others (the forms of this game's tree, 2026-11-12).
VPURE = set('''vspltw128 vmulfp128 vmaddfp vor128 vpermwi128 vnmsubfp vsel vmsum3fp128 vsubfp128 vperm128 vspltisw128
vaddfp128 vcmpeqfp128 vrsqrtefp128 vmrghw128 vmrglw128 vxor128 vcsxwfp128 vor vslw128 vmsum4fp128 vsldoi128 vcmpgtfp128
vcmpeqfp vand128 vcmpequw128 vrefp vandc128 vcfpsxws128 vmaxfp128 vminfp128 vspltw vupkhsb128 vavgub vupklsb128 vsubsws
vrefp128 vsplth vcmpgefp128 vmrghh vcuxwfp128 vexptefp128 vspltisw vsubfp vpkswss128 vmaxsw vaddshs vadduwm vrfin128
vaddfp vsraw128 vsldoi vadduws vmrglb vlogefp128 vsububs vsrw128 vpkshus vmrghb vrsqrtefp vsl vsr vcfpuxws128 vperm
vrfim128 vpkshus128 vcmpgtfp vpkuhum128 vrfiz128 vcmpbfp128 vmrglh vcfux vcfsx vctuxs vsubuws vslw vminfp vspltisb
vmaxfp vcmpgtuw vrlw128 vcmpequw vspltb vslb vrfiz vrfin vrfim vcmpgefp vand vavguw vxor vnor vnor128 vandc'''.split())
VLOAD_FULL = {'lvx', 'lvx128', 'lvxl', 'lvxl128', 'lvsl', 'lvsl128', 'lvsr', 'lvsr128'}


def defuse(mn, operands):
    """(written, read) masks of one guest instruction, all banks; unknown forms read everything."""
    base = mn.rstrip('.').rstrip('+-')
    ops = [o.strip() for o in operands.split(',')] if operands.strip() else []
    if lds.TRAPS.match(base) or base == 'trap':
        # sasrt-nx (2026-11-12): a trap reads its operands; ppc_trap itself only logs (TRAP_USE: r3 / r4 for the
        # service traps' string). leaf_dead_stores.py says ALL, which made every function with a divide-by-zero check
        # read every register.
        return 0, regs(operands) | TRAP_USE
    w, u = lds.defuse(mn, operands)
    if u == lds.ALL:
        u = ALL
    if base.startswith('v') and ops:
        d = regs(ops[0]) & VMASK
        rest = regs(','.join(ops[1:]))
        if base in VPURE:
            return w | d, u | rest
        return w | d, u | rest | d
    if base in lds.VLOADS and ops:
        d = regs(ops[0]) & VMASK
        return (w | d, u) if base in VLOAD_FULL else (w | d, u | d)
    if base.startswith('stv') and ops:
        return w, u | (regs(ops[0]) & VMASK)
    if base == 'mfvscr' and ops:
        return w | (regs(ops[0]) & VMASK), u
    if base == 'mtvscr' and ops:
        return w, u | (regs(ops[0]) & VMASK)
    return w, u


SAVE_NAME = re.compile(r'^__(save|rest)(vmx|gprlr|fpr)_(\d+)$')


def save_helper(name):
    """(use, defn) of a save / restore helper call by its name: the registers it saves (reads) or restores (writes)."""
    m = SAVE_NAME.match(name)
    use = R(0) | R(11) | R(12)
    if not m:
        return ALL, 0
    kind, bank, n = m.group(1), m.group(2), int(m.group(3))
    if bank == 'gprlr':
        rs = sum(R(k) for k in range(n, 32))
    elif bank == 'fpr':
        rs = sum(F(k) for k in range(n, 32))
    else:
        rs = sum(V(k) for k in range(n, 32 if n < 32 else 128))
    return (use | rs, 0) if kind == 'save' else (use, 0)  # (a restore writes them: no kill claimed, conservative)


# parse_function of leaf_dead_stores.py, with this file's masks: the same text with two changes (an indirect call reads
# INDIRECT_USE, a trap everything; a save / restore helper what its name says). Asserted so a change there is noticed.
_SRC = inspect.getsource(lds.parse_function)
_OLD_INDIRECT = ("                if 'REX_CALL_INDIRECT' in line or 'REX_PROMOTED' in line or 'ppc_trap' in line:\n"
                 "                    node.use = ALL\n")
_NEW_INDIRECT = ("                if 'ppc_trap' in line:\n"
                 "                    node.use |= TRAP_USE\n"
                 "                elif 'REX_CALL_INDIRECT' in line or 'REX_PROMOTED' in line:\n"
                 "                    node.use |= INDIRECT_USE\n"
                 "                    node.indirect = True\n")


class NodeX(lds.Node):
    """leaf_dead_stores.Node with two more fields: the instruction makes an indirect (or promoted) call (stage 1d);
    the promoted call's other targets after the first ((address, known) each; the first is node.call)."""
    __slots__ = ('indirect', 'extra')

    def __init__(self, use, defn):
        super().__init__(use, defn)
        self.indirect = False
        self.extra = ()


_OLD_SAVE = ("                    if SAVE_HELPERS.match(name):\n"
             "                        node.use |= R(0) | R(11) | R(12)\n")
_NEW_SAVE = ("                    if SAVE_HELPERS.match(name):\n"
             "                        node.use |= save_helper(name)[0]\n")
assert _SRC.count(_OLD_INDIRECT) == 1 and _SRC.count(_OLD_SAVE) == 1, 'leaf_dead_stores.parse_function changed'
_ns = dict(lds.__dict__)
_OLD_NAMED = ("                    else:\n"
              "                        node.use = ALL\n")
_NEW_NAMED = ("                    else:\n"
              "                        node.use |= HOOK_USE\n")
assert _SRC.count(_OLD_NAMED) == 1, 'leaf_dead_stores.parse_function changed (named calls)'
_SRC = _SRC.replace(_OLD_NAMED, _NEW_NAMED)
_ns.update(TRAP_USE=TRAP_USE, ALL=ALL, regs=regs, defuse=defuse, R=R, INDIRECT_USE=INDIRECT_USE, save_helper=save_helper,
           HOOK_USE=HOOK_USE, Node=NodeX)
exec(compile(_SRC.replace(_OLD_INDIRECT, _NEW_INDIRECT).replace(_OLD_SAVE, _NEW_SAVE), lds.__file__, 'exec'), _ns)
parse_function = _ns['parse_function']
# renut-ultimate (port 121): the base's parse_function marks a call inside a mid-asm hook's block as reading ALL
# (leaf_dead_stores.hook_block): compiled again in this namespace so that ALL is this file's (vector registers too).
exec(compile(inspect.getsource(lds.hook_block), lds.__file__, 'exec'), _ns)


def const_ctr(comments, i):
    """The constant target of the indirect call / jump at instruction i (comments: (mnemonic, operands) per
    instruction), or None: mtctr rS, with rS = lis hi then addi / ori lo, all in the instructions just before."""
    src = None
    j = i - 1
    while j >= 0 and i - j <= 12:
        mn, ops = comments[j]
        if mn == 'mtctr' and len(ops) == 1:
            src = ops[0]
            break
        if mn.startswith('b') or (ops and ops[0] == 'r12' and False):
            return None
        j -= 1
    if src is None:
        return None
    lo = None
    j -= 1
    while j >= 0 and i - j <= 16:
        mn, ops = comments[j]
        if mn.startswith('b'):
            return None
        if ops and ops[0] == src:
            if lo is None and mn in ('addi', 'ori') and len(ops) == 3 and ops[1] == src:
                try:
                    v = int(ops[2], 0)
                except ValueError:
                    return None
                lo = v if mn == 'addi' else v & 0xFFFF
            elif lo is not None and mn == 'lis' and len(ops) == 2:
                try:
                    hi = int(ops[1], 0)
                except ValueError:
                    return None
                return ((hi << 16) + lo) & 0xFFFFFFFF
            else:
                return None
        j -= 1
    return None


def node_of_lines(lines):
    """For a function body's lines (with the instruction comments), the guest instruction index each line belongs to
    (None before the first): the node numbering of parse_function (one node per instruction comment)."""
    out = []
    cur = None
    for line in lines:
        if lds.COMMENT.match(line):
            cur = 0 if cur is None else cur + 1
        out.append(cur)
    return out


def tree_stamp(gen):
    """The generated tree's state: every *_recomp.N.cpp file's size and modification time (localize_native.py takes the
    analysis only for the tree it was made from)."""
    import glob
    parts = []
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        st = os.stat(path)
        parts.append('%s:%d:%d' % (os.path.basename(path), st.st_size, st.st_mtime_ns))
    return '|'.join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'renut_image.bin'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()

    # renut-ultimate (port 121): the base's leaf_dead_stores.py procedure. The guest functions the generator names are
    # analysed too and a call of one is a call edge (tools/dead_ctx_defs.py); gotos to the border stubs are followed to
    # their real target (parse_function's resolve_stubs).
    import dead_ctx_defs as dcd
    named = dcd.named_functions(a.gen)
    hooked_named = dcd.hooked_names(set(named))
    bodies = dcd.all_functions(a.gen, named)
    starts = set(bodies)
    taken = lds.address_taken(a.gen, a.image, starts, bodies)
    _ns['MID_ASM_HOOKS'] = lds.load_mid_asm_hooks(a.gen)  # (the copied parser reads its own namespace's)
    parsed = {addr: parse_function(lines) for addr, (_, lines) in bodies.items()}
    for addr, (_, lines) in bodies.items():
        dcd.fix_named_calls(parsed[addr], dcd.chunks_of(lines), named, hooked_named)
    # Stage 1c/1d's guard rail: an indirect call or jump whose target is a constant built in the same straight-line run
    # (lis + addi / ori -> mtctr -> bctr / bctrl: 2026-11-12 census, ~514 of the ~1,120 unresolved bctr) reaches its
    # target like a direct branch, so nothing says the target follows the convention: it counts as an unknown direct
    # call of that target (its UE bound by HOOK_USE as before, its LiveOut everything), never as a pointer call.
    n_const = 0
    for addr, (_, lines) in bodies.items():
        nodes = parsed[addr]
        if not any(node.indirect for node in nodes):
            continue
        comments = [lds.COMMENT.match(l) for l in lines]
        comments = [(c.group(1).rstrip('.'), [o.strip() for o in c.group(2).split(',')]) for c in comments if c]
        for i, node in enumerate(nodes):
            if not node.indirect or node.call or i >= len(comments):
                continue
            target = const_ctr(comments, i)
            if target is not None and target in parsed:
                node.call = (target, False)
                node.indirect = False
                n_const += 1
    print('reg_liveness: %d indirect calls / jumps of a constant target counted as unknown direct calls' % n_const)

    # Calls through hooks that run the original's statements or a native that reads what the original reads: as
    # leaf_dead_stores.py counts them (its SASRT_HOT_HOOK_NATIVE natives and the localized copies' lists).
    # renut-ultimate (port 121): the base's hand-written natives, app/src/cpu/hot/hand.txt (the address first on each
    # line), as its leaf_dead_stores.py reads them.
    hand = os.path.join(ROOT, 'app', 'src', 'cpu', 'hot', 'hand.txt')
    natives = set(int(m.group(1), 16) for m in re.finditer(r'^([0-9A-Fa-f]{8})\b',
                                                             open(hand, encoding='utf-8').read(), re.M))
    listed = set()
    for path in lds.COPY_LISTS:
        try:
            for line in open(path, encoding='utf-8', errors='ignore'):
                m = re.match(r'\s*(?:sub_)?([0-9A-Fa-f]{8})\b', line)
                if m:
                    listed.add(int(m.group(1), 16))
        except OSError:
            pass
    # sasrt-nx (2026-11-12, stage 1c): only image pointers to a function (a data word: vtables, tables; a lis pair: an
    # address built in guest code) make its callers the convention's; a function an app source names (a hook, a hand
    # native), the entry point, or one a named (unparsed) function calls can be called by code that reads anything
    # after it. The exit bound and the copies rule below take only the first kind.
    # (Two lists name functions without calling them: app/indirect_promotions.txt, the generator's promoted indirect
    # calls, which the tree shows as REX_PROMOTED calls; app/src/hot/localized.txt, the copy list unused since
    # 2026-11-08, which leaf_dead_stores.py's scan still reads.)
    # renut-ultimate (port 121): the base's leaf_dead_stores.py reads neither list as naming a function (ports 74 and
    # 108: the copy lists and app/indirect_promotions.txt, the hand natives' drafts), so only the two image kinds.
    POINTER_REASONS = ('data word', 'lis pair')

    def pointer_only(addr):
        return all(r in POINTER_REASONS for r in taken.get(addr, ()))

    # A call through the hook of a localized copy runs the copy (this tree's own statements, guard-checked) or the
    # original: it reads and writes what the original does. leaf_dead_stores.py counts it as known only for copies
    # not in `taken`; a copy reached through image pointers too is the same copy for its direct callers.
    copies = {addr for addr in listed if addr in starts and pointer_only(addr)}
    # sasrt-nx (2026-11-12 18:40): a promoted call with several targets (`if (REX_PROMOTED_IS(..a..)) { sub_a(...) }
    # else if (REX_PROMOTED_IS(..b..)) { sub_b(...) } ...`) calls each of them: all are call sites. The parser keeps
    # only the first (node.call); the others go to node.extra. Without them a later target's LiveOut lacked this
    # caller's after-set (guard: r8 of sub_826A1D38 through its second target 823FE090).
    n_extra = 0
    for addr, (_, lines) in bodies.items():
        nodes = parsed[addr]
        if not any(node.indirect and node.call for node in nodes):
            continue
        idx = node_of_lines(lines)
        for line, k in zip(lines, idx):
            if k is None or k >= len(nodes) or 'REX_PROMOTED_IS' not in line:
                continue
            node = nodes[k]
            found = [(int(m.group(2), 16), bool(m.group(1))) for m in lds.CALL.finditer(line)]
            if node.call:
                found = [t for t in found if t[0] != node.call[0]]
            more = tuple(t for t in dict.fromkeys(found))
            if more:
                node.extra = tuple(node.extra) + more
                n_extra += len(more)
    print('reg_liveness: %d further targets of multi-target promoted calls counted as call sites' % n_extra)
    for nodes in parsed.values():
        for node in nodes:
            if node.call and not node.call[1] and (node.call[0] in natives or node.call[0] in copies):
                node.call = (node.call[0], True)
            if node.extra:
                node.extra = tuple((t, kn or t in natives or t in copies) for t, kn in node.extra)

    # 1. UE to a fixed point.
    summary = {}
    for addr, nodes in parsed.items():
        undefined = lds_forward(nodes)
        local = 0
        calls = []
        for node, state in zip(nodes, undefined):
            local |= node.use & state
            if node.call:
                calls.append((node.call, state))
            for t in node.extra:
                calls.append((t, state))
        summary[addr] = (local, calls)
    ue = {addr: s[0] for addr, s in summary.items()}
    callers_of = collections.defaultdict(set)
    for addr, (_, calls) in summary.items():
        for (callee, known), _ in calls:
            callers_of[callee].add(addr)
    work = set(summary)
    while work:
        addr = work.pop()
        local, calls = summary[addr]
        new = local
        for (callee, known), state in calls:
            new |= (ue.get(callee, ALL) if known else HOOK_USE) & state
        if new != ue[addr]:
            ue[addr] = new
            work |= callers_of[addr]

    def use_of(node):
        use = node.use
        if node.call:
            callee, known = node.call
            use |= ue.get(callee, ALL) if known else HOOK_USE
        for callee, known in node.extra:
            use |= ue.get(callee, ALL) if known else HOOK_USE
        return use

    # 2. Per call site: (A, T) as leaf_dead_stores.py (live after = A | LiveOut(caller) & T).
    site_at = {}                                   # caller -> [(node index, callee, known, A, T)]
    sites = collections.defaultdict(list)
    tails = collections.defaultdict(set)
    direct = collections.Counter()
    ind_at = {}                                    # caller -> [(node index, A, T)] of its indirect calls (stage 1d)
    for addr, nodes in parsed.items():
        if not any(node.call or node.indirect for node in nodes):
            continue
        live_a = lds_backward(nodes, use_of, 0)
        live_t = lds_backward(nodes, lambda node: 0, ALL)
        here = []
        ind = []
        for i, node in enumerate(nodes):
            if node.indirect:
                ind_a = 0
                ind_t = ALL if node.exit else 0
                for s in node.succ:
                    ind_a |= live_a[s]
                    ind_t |= live_t[s]
                ind.append((i, ind_a, ind_t))
            if not node.call:
                continue
            callee, known = node.call
            direct[callee] += 1
            # (leaf_dead_stores.py's rule: a tail call passes the caller's LiveOut on through `tails`, not as T)
            out_a = 0
            out_t = ALL if (node.exit and not node.tail) else 0
            for s in node.succ:
                out_a |= live_a[s]
                out_t |= live_t[s]
            # For this site's own hand-over: after a tail call the caller returns, so its LiveOut is live after it.
            here.append((i, callee, known, out_a, ALL if node.tail else out_t))
            if not known:
                continue
            if node.tail:
                tails[callee].add(addr)
            if not node.tail or node.succ:
                sites[callee].append((addr, out_a, out_t))
            for t, kn in node.extra:
                direct[t] += 1
                if not kn:
                    continue
                if node.tail:
                    tails[t].add(addr)
                if not node.tail or node.succ:
                    sites[t].append((addr, out_a, out_t))
        site_at[addr] = here
        if ind:
            ind_at[addr] = ind

    # 3. LiveOut to a fixed point.
    live_out = {}
    for addr, nodes in parsed.items():
        if addr in taken:
            live_out[addr] = ALL & ~EXIT_DEAD if pointer_only(addr) else ALL
        elif not direct[addr]:
            thunk = len(nodes) == 4 and nodes[3].tail
            live_out[addr] = 0 if thunk else ALL
        else:
            live_out[addr] = 0
    for nodes in parsed.values():
        for node in nodes:
            if node.call and not node.call[1] and node.call[0] in live_out:
                live_out[node.call[0]] = ALL
            for t, kn in node.extra:
                if not kn and t in live_out:
                    live_out[t] = ALL
    callees_of = collections.defaultdict(set)
    for callee, lst in sites.items():
        for caller, _, _ in lst:
            callees_of[caller].add(callee)
    for callee, callers in tails.items():
        for caller in callers:
            callees_of[caller].add(callee)
    work = set(addr for addr in parsed if live_out[addr] != ALL)
    while work:
        addr = work.pop()
        if live_out[addr] == ALL:
            continue
        new = live_out[addr]
        for caller, out_a, out_t in sites.get(addr, ()):
            new |= out_a | (live_out.get(caller, ALL) & out_t)
        for caller in tails.get(addr, ()):
            new |= live_out.get(caller, ALL)
        if new != live_out[addr]:
            live_out[addr] = new
            work |= callees_of[addr]

    # 4. Per call site, what need not be handed over: not in the callee's UE, not live after the call.
    out_sites = {}
    skipped = collections.Counter()
    n_sites = 0
    for addr, here in site_at.items():
        rows = []
        for i, callee, known, out_a, out_t in here:
            n_sites += 1
            if not known and not HOOKS_ABI:
                continue
            after = out_a | (live_out.get(addr, ALL) & out_t)
            skip = ALL & ~((ue.get(callee, ALL) if known else HOOK_USE) | after)
            if skip:
                rows.append([i, '%08X' % callee, '%x' % skip])
                for b in range(NBITS):
                    if skip >> b & 1:
                        skipped['r' if b < 32 else 'f' if b < 64 else 'v'] += 1
        if rows:
            out_sites['%08X' % addr] = rows
    # sasrt-nx (2026-11-12, stage 1d): an indirect call (and the fallback of a promoted one) reaches a function through
    # a pointer, which reads no scratch vector register on entry (INDIRECT_USE): those dead after the call need not be
    # handed over.
    out_ind = {}
    n_ind = 0
    for addr, ind in ind_at.items():
        rows = []
        for i, out_a, out_t in ind:
            n_ind += 1
            skip = SCRATCH_V & ~(out_a | (live_out.get(addr, ALL) & out_t))
            if skip:
                rows.append([i, '%x' % skip])
                skipped['iv'] += bin(skip).count('1')
        if rows:
            out_ind['%08X' % addr] = rows
    print('reg_liveness: %d functions, %d direct call sites, %d with registers that need not be handed over '
          '(general %d, float %d, vector %d register-sites); %d functions with an address outside direct calls' % (
              len(parsed), n_sites, sum(len(v) for v in out_sites.values()), skipped['r'], skipped['f'],
              skipped['v'], len(taken)))
    print('reg_liveness: %d indirect call sites, %d with scratch vector registers that need not be handed over (%d '
          'register-sites); exit bound %s' % (n_ind, sum(len(v) for v in out_ind.values()), skipped['iv'],
                                             'on' if EXIT_ABI else 'off'))
    if a.check:
        return
    with open(OUT_JSON + '.tmp', 'w') as f:
        json.dump({'gen': os.path.abspath(a.gen), 'stamp': tree_stamp(a.gen),
                   'live_out': {'%08X' % k: '%x' % v for k, v in sorted(live_out.items())},
                   'sites': out_sites, 'indirect': out_ind}, f)
    os.replace(OUT_JSON + '.tmp', OUT_JSON)


def lds_forward(nodes):
    """leaf_dead_stores.forward_undefined with this file's ALL."""
    n = len(nodes)
    state = [0] * n
    if not n:
        return state
    state[0] = ALL
    work = [0]
    queued = [False] * n
    queued[0] = True
    while work:
        i = work.pop()
        queued[i] = False
        out = state[i] & ~nodes[i].defn
        for s in nodes[i].succ:
            new = state[s] | out
            if new != state[s]:
                state[s] = new
                if not queued[s]:
                    queued[s] = True
                    work.append(s)
    return state


lds_backward = lds.backward  # (no ALL inside: the boundary and the uses come from the caller)

if __name__ == '__main__':
    main()
