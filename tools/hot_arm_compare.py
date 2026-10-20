#!/usr/bin/env python3
# sa2-nx: copied from banjo-nx on 2026-10-19 (its codegen round), adapted to sa2-nx's tree.
"""banjo-nx: the AArch64 code of each localized native copy against the recompiled original it replaces, weighted by
how often each part really runs (docs/enhancements.md, "Hot guest functions as localized copies").

Nothing here runs on a console. What it does instead:
  1. a PC session with banjo_hot_count = true counts, for every copy, how often each block of statements (a straight
     run between labels and jumps) is entered: app/out/pc-me/logs/hot_counts.txt. Those counts do not depend on the
     processor: the copy and the original take the same path through the same statements;
  2. this tool compiles, with the Switch compiler and the flags of the Switch build (app/out/switch/build.ninja) plus
     line tables (-g1, which does not change the code), the generated files that hold the originals and
     app/src/cpu/banjo_hot_guest.cpp, which holds the copies, into a scratch folder;
  3. every AArch64 instruction of an original (__imp__<function> and the callees its copy contains) and of a copy
     (banjo::hot::n_<id>::Native and the callee copies left out of line) is given the count of the block its source
     line belongs to (app/generated/hot/n_<id>.blocks.json); code inlined from another function of the copy counts
     in proportion to its call site.
The sums, divided by the number of calls, are the instructions, context loads / stores, other memory loads / stores
and host stack traffic one call executes on the A57, for both versions. The hook in front of a copy adds its own
few instructions per call (listed), and a hooked function can no longer be inlined into its callers (not measured).

  python tools/hot_arm_compare.py --scratch <folder> [--counts <hot_counts.txt>] [--jobs 6] [--json out.json] [names]
Without --counts every block counts once (a static comparison).
"""
import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = r'C:\devkitPro\devkitA64\bin'
GXX = os.path.join(BIN, 'aarch64-none-elf-g++.exe')
OBJDUMP = os.path.join(BIN, 'aarch64-none-elf-objdump.exe')
NM = os.path.join(BIN, 'aarch64-none-elf-nm.exe')
GEN = os.path.join(ROOT, 'app', 'generated')
HOT = os.path.join(GEN, 'hot')
LIST = os.path.join(ROOT, 'app', 'src', 'cpu', 'hot', 'localized.txt')
NINJA = os.path.join(ROOT, 'app', 'out', 'switch', 'build.ninja')
COUNTS = os.path.join(ROOT, 'app', 'out', 'pc-me', 'logs', 'hot_counts.txt')
LOAD = re.compile(r'^(ldr|ldp|ldur|ldrb|ldrh|ldrsw|ldrsb|ldrsh|ldurb|ldurh|ldursw|ldursb|ldursh|ld1|ldnp)')
STORE = re.compile(r'^(str|stp|stur|strb|strh|sturb|sturh|st1|stnp)')
CALLER_SAVED = {f'x{i}' for i in range(0, 19)}
RECORD = re.compile(r'^(.+):(\d+)(?: \(discriminator \d+\))?$')
INLINED = re.compile(r'^inlined by (.+):(\d+) \((.*)\)$')
INSN = re.compile(r'^\s*([0-9a-f]+):\t(\S+)\s*(.*)$')
RELOC = re.compile(r'^\s+[0-9a-f]+: R_AARCH64_(?:CALL26|JUMP26)\s+(\S+)')
# Calls the compiler keeps off the hot path: the slow halves of the any-mode float helpers (a denormal or a
# NaN), the trap helper, an indirect call's fallback lookup.
COLD = re.compile(r'Slow|ppc_trap|ResolveIndirectFunction|Mismatch|__cxa|abort')
KEYS = ['insns', 'ctx_loads', 'ctx_stores', 'mem_loads', 'mem_stores', 'sp_loads', 'sp_stores', 'calls', 'fpcr']


def ninja_flags(obj):
    text = open(NINJA, encoding='utf-8', errors='replace').read()
    m = re.search(r'^build [^\n]*' + re.escape(obj) + r':[^\n]*\n((?:  [^\n]*\n)+)', text, re.M)
    if not m:
        return None
    vals = {}
    for line in m.group(1).splitlines():
        k, _, v = line.strip().partition(' = ')
        vals[k] = v
    return vals


def command(vals, source, out, extra=()):
    flags = []
    skip = False
    for f in shlex.split(vals['FLAGS'], posix=True):
        if skip:
            skip = False
            continue
        if f == '-include':  # the build's precompiled header: the plain header instead (the same text)
            skip = True
            continue
        if f == '-Winvalid-pch':
            continue
        flags.append(f)
    return ([GXX] + shlex.split(vals.get('DEFINES', ''), posix=True) + shlex.split(vals.get('INCLUDES', ''), posix=True)
            + flags + ['-g1', '-w'] + list(extra) + ['-c', source, '-o', out])


def compile_all(scratch, files, jobs):
    """The generated files that hold the originals, and the copies' file, with line tables. Up to date ones stay."""
    os.makedirs(scratch, exist_ok=True)
    recomp = ninja_flags('renut_recomp.0.cpp.obj')
    hot = ninja_flags('banjo_hot_guest.cpp.obj')
    hot_extra = []
    if hot is None:  # the Switch build folder was configured before the file existed: its flags from a neighbour
        hot = ninja_flags('banjo_reverb_native.cpp.obj')
        hot_extra = ['-fno-math-errno', '-fomit-frame-pointer']
    if recomp is None or hot is None:
        sys.exit('hot_arm_compare: app/out/switch/build.ninja has no rule for the game code (configure the Switch build)')
    pch = ['-include', os.path.join(GEN, 'sa2_pch.h')]
    tasks = []
    for f in sorted(files):
        src = os.path.join(GEN, f)
        out = os.path.join(scratch, f + '.o')
        if not os.path.isfile(out) or os.path.getmtime(out) < os.path.getmtime(src):
            tasks.append((command(recomp, src, out, pch), out))
    src = os.path.join(ROOT, 'app', 'src', 'cpu', 'banjo_hot_guest.cpp')
    out = os.path.join(scratch, 'banjo_hot_guest.o')
    newest = max([os.path.getmtime(src)] + [os.path.getmtime(os.path.join(HOT, x)) for x in os.listdir(HOT)
                                           if x.endswith(('.h', '.inc'))])
    if not os.path.isfile(out) or os.path.getmtime(out) < newest:
        tasks.append((command(hot, src, out, hot_extra), out))

    def run(task):
        r = subprocess.run(task[0], capture_output=True, text=True)
        return task[1], r.returncode, r.stderr[-2000:]

    if tasks:
        print(f'hot_arm_compare: compiling {len(tasks)} file(s) with the Switch compiler into {scratch} ...',
              file=sys.stderr)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for out, code, err in pool.map(run, tasks):
            if code:
                sys.exit(f'hot_arm_compare: compiling {out} failed:\n{err}')
    return os.path.join(scratch, 'banjo_hot_guest.o')


def symbol_table(obj):
    raw = subprocess.run([NM, '--defined-only', obj], capture_output=True, text=True).stdout
    dem = subprocess.run([NM, '-C', '--defined-only', obj], capture_output=True, text=True).stdout
    out = []
    for m, d in zip(raw.splitlines(), dem.splitlines()):
        mp, dp = m.split(None, 2), d.split(None, 2)
        if len(mp) == 3 and len(dp) == 3 and mp[1] in 'TtWw':
            out.append((mp[2], dp[2]))
    return out


_DISASSEMBLY = {}


def disassemble_many(obj, symbols):
    """Reads the code of several functions of one object in one objdump run (its line tables are read once; each
    function is a section of its own: -ffunction-sections)."""
    cache = _DISASSEMBLY.setdefault(obj, {})
    wanted = [s for s in symbols if s not in cache]
    for k in range(0, len(wanted), 60):
        part = wanted[k:k + 60]
        args = [OBJDUMP, '-dlr', '--inlines', '--no-show-raw-insn']
        for s in part:
            # .text.hot.<name> for the functions tools/hot_layout.py declares hot (package 27)
            args += ['-j', '.text.' + s, '-j', '.text.hot.' + s]
        out = subprocess.run(args + [obj], capture_output=True, text=True, errors='replace').stdout
        current = None
        chain = []
        fresh = True  # no instruction since the chain's first line was read
        for line in out.splitlines():
            m = RELOC.match(line)
            if m:
                if current:
                    current[-1] = current[-1][:4] + (m.group(1),)
                continue
            m = INSN.match(line)
            if m:
                if current is not None:
                    current.append((int(m.group(1), 16), m.group(2), m.group(3).split('//')[0].strip(), chain, ''))
                fresh = False
                continue
            m = re.match(r'^[0-9a-f]+ <(.+)>:$', line)
            if m:
                current = cache.setdefault(m.group(1), [])
                chain = []
                continue
            m = INLINED.match(line)
            if m:
                if not fresh:
                    # objdump repeats only the "inlined by" lines when the innermost line stays and the way it was
                    # inlined changes: a new chain under the same innermost line.
                    chain = chain[:1]
                    fresh = True
                chain = chain + [(os.path.basename(m.group(1)), int(m.group(2)))]
                continue
            m = RECORD.match(line)
            if m and ('/' in m.group(1) or '\\' in m.group(1)):
                chain = [(os.path.basename(m.group(1)), int(m.group(2)))]
                fresh = True
        for s in part:
            cache.setdefault(s, [])


def disassemble(obj, symbol):
    """[(address, mnemonic, operands, [(file, line) innermost first])]"""
    disassemble_many(obj, [symbol])
    return _DISASSEMBLY[obj][symbol]


def classify(insns):
    """Per instruction: which counters it adds to. The context register: x0 at entry, followed through register
    moves and saves on the host stack (linear in address order: an estimate, good where the compiler keeps the
    context in one callee-saved register, which it does in every function with a call)."""
    holders = {'x0'}
    slots = set()
    out = []
    for a, mn, ops, _, _ in insns:
        kinds = ['insns']
        if mn in ('bl', 'blr'):
            kinds.append('calls')
            holders -= CALLER_SAVED
            out.append(kinds)
            continue
        if mn == 'msr' and 'fpcr' in ops:
            kinds.append('fpcr')
        load, store = LOAD.match(mn), STORE.match(mn)
        if load or store:
            m = re.search(r'\[(\w+)(?:,\s*#(-?\w+))?', ops)
            base = m.group(1) if m else ''
            offset = m.group(2) if m and m.group(2) else '0'
            regs = [r.strip() for r in ops.split('[')[0].split(',') if r.strip()]
            if base == 'sp':
                kind = 'sp'
                if store and regs and regs[0] in holders and mn in ('str', 'stur'):
                    slots.add(offset)
                elif store and mn == 'stp':
                    try:
                        o = int(offset, 0)
                        for k, r in enumerate(regs[:2]):
                            if r in holders:
                                slots.add(str(o + 8 * k) if o + 8 * k else '0')
                    except ValueError:
                        pass
                elif load and mn in ('ldr', 'ldur') and offset in slots and regs and re.fullmatch(r'x\d+', regs[0]):
                    holders.add(regs[0])
                    kinds.append('sp_loads')
                    out.append(kinds)
                    continue
            else:
                kind = 'ctx' if base in holders else 'mem'
            kinds.append(f'{kind}_{"loads" if load else "stores"}')
        if mn == 'mov':
            parts = [p.strip() for p in ops.split(',')]
            if len(parts) == 2 and parts[1] in holders and re.fullmatch(r'x\d+', parts[0]):
                holders.add(parts[0])
                out.append(kinds)
                continue
        if not store and not mn.startswith(('cmp', 'cmn', 'tst', 'b', 'cb', 'tb', 'ret', 'msr', 'nop', 'prfm', 'fcmp',
                                            'ccmp', 'fccmp')):
            for r in ([p.strip() for p in ops.split('[')[0].split(',')][:2] if mn.startswith('ldp')
                      else [ops.split(',')[0].strip()]):
                m = re.fullmatch(r'[xw](\d+)', r)
                if m:
                    holders.discard('x' + m.group(1))
        out.append(kinds)
    return out


class Blocks:
    """One copy's map: which block a line of the header or of a generated file belongs to, each block's count."""
    others = {}

    def __init__(self, rid, counts):
        d = json.load(open(os.path.join(HOT, f'n_{rid}.blocks.json'), encoding='utf-8'))
        self.root = d['root']
        self.members = d['members']
        self.entry = d['entry']
        self.header = (f'n_{rid}.h', {int(k): v for k, v in d['header'].items()})
        self.original = {f: {int(k): v for k, v in lines.items()} for f, lines in d['original'].items()}
        self.member_of_block = {}
        self.blocks = d['blocks']
        # A block belongs to the member whose entry block is the largest one not above it (blocks are numbered in
        # the order the members are written).
        entries = sorted((b, m) for m, b in self.entry.items())
        for b in range(d['blocks']):
            self.member_of_block[b] = [m for e, m in entries if e <= b][-1]
        self.hits = counts.get(self.root) if counts is not None else None
        self.files = d['original']

    def count(self, block):
        if self.hits is None:
            return 1.0
        return float(self.hits.get(block, 0))

    def block(self, file, line, copy):
        if copy:
            if file == self.header[0]:
                return self.header[1].get(line)
            m = re.fullmatch(r'n_(\w+)\.h', file)
            if not m:
                return None
            # The compiler keeps one of several identical functions (-fipa-icf): a callee's copy can be the one in
            # another function's header. The same statements, so the same blocks, counted from that member's first.
            if m.group(1) not in Blocks.others:
                try:
                    Blocks.others[m.group(1)] = Blocks(m.group(1), None)
                except OSError:
                    Blocks.others[m.group(1)] = None
            other = Blocks.others[m.group(1)]
            if other is None:
                return None
            b = other.header[1].get(line)
            if b is None:
                return None
            member = other.member_of_block[b]
            if member not in self.entry:
                return None
            return self.entry[member] + (b - other.entry[member])
        return self.original.get(file, {}).get(line)

    def weight(self, chain, copy):
        """The count of an instruction from its line and the lines it was inlined through; and the (callee, call
        site block) pairs of that chain (for the callees' out-of-line versions)."""
        own = [b for b in (self.block(f, l, copy) for f, l in chain) if b is not None]
        if not own:
            return None, []
        w = self.count(own[0])
        sites = []
        for k in range(1, len(own)):
            inner_member = self.member_of_block[own[k - 1]]
            if self.member_of_block[own[k]] == inner_member and own[k] == own[k - 1]:
                continue
            entry = self.count(self.entry[inner_member])
            sites.append((inner_member, own[k]))
            w = w * self.count(own[k]) / entry if entry else 0.0
        return w, sites


def cold_instructions(insns):
    """Addresses of the instructions in machine basic blocks that call a cold helper (COLD): the slow path of a
    float helper, a trap. A source line's count says how often the statement ran, not how often its slow path did;
    the compiler moves those blocks out of the way and so does this count, for the original and the copy alike."""
    if not insns:
        return set()
    addrs = [a for a, _, _, _, _ in insns]
    inside = set(addrs)
    leaders = {addrs[0]}
    for k, (a, mn, ops, _, _) in enumerate(insns):
        branch = mn == 'b' or mn.startswith(('b.', 'cb', 'tb')) or mn in ('ret', 'br')
        if branch:
            m = re.match(r'(?:.*,\s*)?([0-9a-f]+) <', ops)
            if m and int(m.group(1), 16) in inside:
                leaders.add(int(m.group(1), 16))
            if k + 1 < len(insns):
                leaders.add(addrs[k + 1])
    cold = set()
    block = []
    is_cold = False
    for a, mn, ops, _, target in insns:
        if a in leaders and block:
            if is_cold:
                cold.update(block)
            block, is_cold = [], False
        block.append(a)
        if mn == 'bl' and COLD.search(target or ops):
            is_cold = True
    if is_cold:
        cold.update(block)
    return cold


def measure(obj_of, blocks, copy, symbols):
    """Weighted counters over the given (object, symbol, member) list."""
    total = {k: 0.0 for k in KEYS}
    static = 0
    unattributed = 0
    inlined_sites = {}  # member -> set of call site blocks where it was inlined
    per_symbol = []
    for obj, symbol, member in symbols:
        insns = disassemble(obj, symbol)
        kinds = classify(insns)
        cold = cold_instructions(insns)
        sums = {k: 0.0 for k in KEYS}
        last = None
        for (a, mn, ops, chain, _), ks in zip(insns, kinds):
            if a in cold:
                continue  # off the hot path: the statement's count is not this code's count
            w, sites = blocks.weight(chain, copy)
            if w is None:
                # No line of this function: code of a helper header or of the compiler. With the nearest line before.
                if last is None:
                    w, sites = blocks.count(blocks.entry[member]) if member in blocks.entry else 0.0, []
                    unattributed += 1
                else:
                    w, sites = last
            last = (w, sites)
            for m, site in sites:
                inlined_sites.setdefault(m, set()).add(site)
            for k in ks:
                sums[k] += w
        static += len(insns)
        per_symbol.append((member, symbol, sums))
    # A member that is inlined at some call sites runs out of line only for the calls from the other sites.
    for member, symbol, sums in per_symbol:
        scale = 1.0
        entry = blocks.count(blocks.entry[member]) if member in blocks.entry else 0.0
        if member in inlined_sites and entry and blocks.hits is not None and member != blocks.root:
            inlined = sum(blocks.count(s) for s in inlined_sites[member])
            scale = max(0.0, entry - inlined) / entry
        for k in KEYS:
            total[k] += sums[k] * scale
    return total, static, unattributed


def hook_cost(obj, symbols, name):
    """Instructions of the hook up to its jump into the copy (the path of an unguarded call)."""
    n = 0
    for a, mn, ops, _, target in disassemble(obj, name):
        n += 1
        if mn == 'b' and 'Native' in (target or ops):
            return n
    return n


def names_of_list():
    names = []
    for line in open(LIST, encoding='utf-8'):
        line = line.strip()
        if line and not line.startswith('#'):
            first = line.split()[0]
            names.append('sub_' + first.upper() if re.fullmatch(r'[0-9A-Fa-f]{8}', first) else first)
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('names', nargs='*')
    ap.add_argument('--scratch', required=True)
    ap.add_argument('--counts', nargs='?', const=COUNTS)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--json')
    a = ap.parse_args()
    names = [n if not re.fullmatch(r'[0-9A-Fa-f]{8}', n) else 'sub_' + n.upper() for n in a.names] or names_of_list()
    counts = None
    if a.counts:
        counts = {}
        for line in open(a.counts, encoding='utf-8'):
            if line.startswith('#'):
                continue
            f, b, n = line.split()
            counts.setdefault(f, {})[int(b)] = int(n)
    maps = {}
    files = set()
    for name in names:
        rid = name[4:] if re.fullmatch(r'sub_[0-9A-F]{8}', name) else name
        maps[name] = Blocks(rid, counts)
        files.update(maps[name].files)
    hot_obj = compile_all(a.scratch, files, a.jobs)
    hot_symbols = symbol_table(hot_obj)
    tables = {}
    out = []
    ids = {(n[4:] if re.fullmatch(r'sub_[0-9A-F]{8}', n) else n) for n in names}
    disassemble_many(hot_obj, [raw for raw, dem in hot_symbols
                               if raw in names or (re.search(r'banjo::hot::n_(\w+?)::', dem) and
                                                   re.search(r'banjo::hot::n_(\w+?)::', dem).group(1) in ids and
                                                   ('::Native(' in dem or 'banjo::hot::Fast' in dem))])
    for name in names:
        b = maps[name]
        rid = name[4:] if re.fullmatch(r'sub_[0-9A-F]{8}', name) else name
        calls = b.count(b.entry[b.root]) if (counts is None or b.hits is not None) else 0
        if counts is not None and not calls:
            out.append({'name': name, 'calls': 0})
            print(f'{name}: not called in the counted session')
            continue
        # The original: __imp__<member> (and the pieces the compiler split off) for every function of the copy.
        originals = []
        for member in b.members:
            f = None
            for file, lines in b.original.items():
                if b.entry[member] in lines.values():
                    f = file
                    break
            obj = os.path.join(a.scratch, f + '.o')
            if obj not in tables:
                tables[obj] = symbol_table(obj)
            for raw, _ in tables[obj]:
                if raw == '__imp__' + member or raw.startswith('__imp__' + member + '.'):
                    originals.append((obj, raw, member))
        copies = []
        for raw, dem in hot_symbols:
            if f'banjo::hot::n_{rid}::' not in dem:
                continue
            if '::Native(' in dem:
                copies.append((hot_obj, raw, b.root))
            elif 'banjo::hot::Fast' in dem and '::f_' in dem:
                copies.append((hot_obj, raw, re.search(r'::f_(\w+?)<', dem).group(1)))
        for obj in {o for o, _, _ in originals}:
            disassemble_many(obj, [s for o, s, _ in originals if o == obj])
        # Callee copies the compiler folded with an identical one of another function's header: follow the calls.
        demangled = dict(hot_symbols)
        seen = {raw for _, raw, _ in copies}
        work = list(copies)
        while work:
            _, raw, _ = work.pop()
            for _, mn, _, _, target in disassemble(hot_obj, raw):
                if mn in ('bl', 'b') and target and target not in seen:
                    m = re.search(r'banjo::hot::n_\w+?::f_(\w+?)<banjo::hot::Fast>', demangled.get(target, ''))
                    if m and m.group(1) in b.entry:
                        seen.add(target)
                        copies.append((hot_obj, target, m.group(1)))
                        work.append(copies[-1])
        orig, orig_static, _ = measure(None, b, False, originals)
        copy, copy_static, _ = measure(None, b, True, copies)
        hook = hook_cost(hot_obj, hot_symbols, name)
        row = {'name': name, 'calls': calls, 'hook': hook, 'original_static': orig_static, 'copy_static': copy_static,
               'original': {k: orig[k] / calls for k in KEYS}, 'copy': {k: copy[k] / calls for k in KEYS},
               'members': len(b.members),
               'copies_out_of_line': sorted({m for _, _, m in copies if m != b.root})}
        out.append(row)
        o, c = row['original'], row['copy']
        gain = (1 - (c['insns'] + hook) / o['insns']) * 100 if o['insns'] else 0.0
        print(f'{name}: {calls:.0f} calls counted; per call, original -> copy: instructions {o["insns"]:.1f} -> '
              f'{c["insns"]:.1f} (+{hook} hook: {gain:+.1f} %), context loads {o["ctx_loads"]:.1f} -> {c["ctx_loads"]:.1f}, '
              f'context stores {o["ctx_stores"]:.1f} -> {c["ctx_stores"]:.1f}, other loads {o["mem_loads"]:.1f} -> '
              f'{c["mem_loads"]:.1f}, other stores {o["mem_stores"]:.1f} -> {c["mem_stores"]:.1f}, host stack '
              f'{o["sp_loads"] + o["sp_stores"]:.1f} -> {c["sp_loads"] + c["sp_stores"]:.1f}, calls {o["calls"]:.2f} -> '
              f'{c["calls"]:.2f}; static size {orig_static} -> {copy_static}')
    if a.json:
        json.dump(out, open(a.json, 'w', encoding='utf-8'), indent=1)


if __name__ == '__main__':
    main()
