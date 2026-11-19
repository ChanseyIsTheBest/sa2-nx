#!/usr/bin/env python3
"""banjo-nx (2026-10-28, the title update): maps guest addresses of the base executable to the title update's, with
tools/map_functions.py's maps, and rewrites files with them.

An address is
- a function start: the function map's new start;
- inside a function (a hook, a call site, a jump-table case): the containing function's pair, and the instruction
  there found by aligning the two functions' instruction lists (the generator's `// mnemonic operands` comments, one
  a 4-byte instruction): the full text with immediates blanked first, then mnemonics only; the instruction at the
  result must have the same mnemonic ('exact'), or the address is reported ('unsure');
- data (outside the code): the data map.
Every rewrite is listed with its kind and confidence; the unmapped and unsure are reported, never guessed silently.

  python tools/remap_addresses.py --maps <dir> --old-gen <dir> --new-gen <dir> [--test-hooks old.toml new.toml]
         [--rewrite file ...] [--dry-run] [--report out.tsv]
"""
import argparse
import bisect
import difflib
import glob
import json
import os
import re
import sys


def gen_file(gen, suffix):
    """renut-ultimate (2026-11-19, port 186; found by sasrt-nx's sync): the generator names its files after the project
    (<project>_pch.h, <project>_init.cpp, <project>_recomp.N.cpp): renut_ in the base, a port's own name otherwise."""
    for name in sorted(os.listdir(gen)):
        m = re.fullmatch(r'(\w+)_recomp\.\d+\.cpp', name)
        if m:
            return os.path.join(gen, m.group(1) + '_' + suffix)
    return os.path.join(gen, 'renut_' + suffix)


FUNC = re.compile(r'^DEFINE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\) \{$')
TABLE = re.compile(r'\{ 0x([0-9A-Fa-f]{8}), ([A-Za-z_][A-Za-z0-9_]*) \}')
INSN = re.compile(r'^\s*// ([a-z][a-z0-9.+\-]*)(?:\s+(.*))?$')
ADDR = re.compile(r'0x8[0-9a-f]{7}', re.I)
IMM = re.compile(r'(?<![rfv])-?\b(?:0x[0-9a-f]+|\d+)\b', re.I)


class Code:
    """One generated tree's functions: starts, and instruction lists on demand."""

    def __init__(self, gen_dir, sigs):
        self.gen_dir = gen_dir
        self.starts = sorted(int(a, 16) for a in sigs)
        self.sigs = sigs
        self.insns = None
        names = {}
        with open(gen_file(gen_dir, 'init.cpp'), encoding='utf-8', errors='replace') as f:
            for m in TABLE.finditer(f.read()):
                names[m.group(2)] = m.group(1).upper()
        self.names = names

    def load(self, wanted):
        self.insns = {}
        for path in glob.glob(os.path.join(self.gen_dir, '*.cpp')):
            cur = None
            with open(path, encoding='utf-8', errors='replace') as f:
                for line in f:
                    m = FUNC.match(line)
                    if m:
                        n = m.group(1)
                        a = self.names.get(n) or (n[4:] if n.startswith('sub_') else None)
                        cur = a if a in wanted else None
                        if cur:
                            self.insns[cur] = []
                        continue
                    if cur is None:
                        continue
                    i = INSN.match(line)
                    if i:
                        self.insns[cur].append((i.group(1), i.group(2) or ''))

    def containing(self, addr):
        k = bisect.bisect_right(self.starts, addr) - 1
        return '%08X' % self.starts[k] if k >= 0 else None


def norm(insn):
    mnem, ops = insn
    return mnem + ' ' + IMM.sub('#', ADDR.sub('A', ops))


def align_index(old_list, new_list, k):
    """The new index of old instruction k, and how sure ('exact', 'unsure')."""
    for key in (norm, lambda x: x[0]):
        a, b = [key(x) for x in old_list], [key(x) for x in new_list]
        sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
        for tag, i0, i1, j0, j1 in sm.get_opcodes():
            if i0 <= k < i1 and tag in ('equal', 'replace') and (tag == 'equal' or i1 - i0 == j1 - j0):
                kk = j0 + (k - i0)
                if kk < len(new_list) and new_list[kk][0] == old_list[k][0]:
                    return kk, 'exact' if tag == 'equal' else 'replaced'
    return None, 'unsure'


class Mapper:
    def __init__(self, maps, old_gen, new_gen):
        self.fmap = json.load(open(os.path.join(maps, 'func_map.json')))
        self.dmap = json.load(open(os.path.join(maps, 'data_map.json')))
        self.old = Code(old_gen, json.load(open(os.path.join(maps, 'base_sigs.json'))))
        self.new = Code(new_gen, json.load(open(os.path.join(maps, 'tu_sigs.json'))))
        self.lo, self.hi = self.old.starts[0], self.old.starts[-1] + 0x40000
        self.rescue_names()
        # Hand-checked mappings (manual.json: {"funcs": {old: new}, "addrs": {old: new}}), each read in both listings.
        self.manual = {}
        mp = os.path.join(maps, 'manual.json')
        if os.path.exists(mp):
            j = json.load(open(mp))
            for o, n in j.get('funcs', {}).items():
                self.fmap[o.upper()] = [n.upper(), 'manual', 0, 0, None]
            self.manual = {int(o, 16): int(n, 16) for o, n in j.get('addrs', {}).items()}

    def rescue_names(self):
        """A function both lists name the same that the map left out (map_functions.py keeps only anchors in address
        order, and the TU reordered some libraries: XACT), taken when its body agrees: the instruction counts within
        15 % and the mnemonic histograms at least 0.6 alike. 'name' in the map's status."""
        targets = {v[0] for v in self.fmap.values()}
        added = 0
        for n, a in self.old.names.items():
            if n.startswith('sub_') or a in self.fmap:
                continue
            b = self.new.names.get(n)
            if not b or b in targets or a not in self.old.sigs or b not in self.new.sigs:
                continue
            ca, cb = self.old.sigs[a][2], self.new.sigs[b][2]
            if abs(ca - cb) > 0.15 * max(ca, cb):
                continue
            ha, hb = self.old.sigs[a][5], self.new.sigs[b][5]
            common = sum(min(ha.get(k, 0), hb.get(k, 0)) for k in set(ha) | set(hb))
            if common < 0.6 * max(sum(ha.values()), sum(hb.values())):
                continue
            self.fmap[a] = [b, 'name', ca, cb, None]
            targets.add(b)
            added += 1
        self.rescued = added

    def prepare(self, addrs):
        need_old, need_new = set(), set()
        for a in addrs:
            if '%08X' % a in self.fmap or not (self.lo <= a < self.hi):
                continue
            f = self.old.containing(a)
            if f in self.fmap:
                need_old.add(f)
                need_new.add(self.fmap[f][0])
        self.old.load(need_old)
        self.new.load(need_new)

    def map(self, a):
        """(new address or None, kind, note)."""
        key = '%08X' % a
        if a in self.manual:
            return self.manual[a], 'manual', 'hand-checked'
        if key in self.fmap:
            nf, st = self.fmap[key][0], self.fmap[key][1]
            return int(nf, 16), 'start', st
        if not (self.lo <= a < self.hi):
            if key in self.dmap:
                y, votes, rivals = self.dmap[key]
                return int(y, 16), 'data', '%d votes%s' % (votes, ', %d rival' % rivals if rivals else '')
            return None, 'data', 'unmapped'
        f = self.old.containing(a)
        if f not in self.fmap:
            return None, 'inside', 'function %s unmapped' % f
        nf, st = self.fmap[f][0], self.fmap[f][1]
        k = (a - int(f, 16)) // 4
        ol, nl = self.old.insns.get(f), self.new.insns.get(nf)
        if ol is None or nl is None or k >= len(ol):
            return None, 'inside', 'no instructions for %s' % f
        if st == 'same':
            return int(nf, 16) + (a - int(f, 16)), 'inside', 'same function'
        kk, how = align_index(ol, nl, k)
        if kk is None:
            return None, 'inside', 'unsure in %s (%s)' % (f, st)
        return int(nf, 16) + kk * 4, 'inside', '%s in %s (%s): %s' % (how, f, st, norm(ol[k]))


TOKEN = re.compile(r'(?<![0-9A-Za-z_])(sub_|0x|rex_\w*?_|)(8[2-9A-Fa-f][0-9A-Fa-f]{6})(?![0-9A-Za-z])')
REGTOK = re.compile(r'\b([rf])(\d+)\b')


def register_map(m, addr, regs, window=40):
    """The new names of a hook's registers: votes from the aligned instructions around it (operand by operand)."""
    f = m.old.containing(addr)
    if f not in m.fmap:
        return None
    if int(f, 16) == addr and all(r in ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r1') for r in regs):
        return {r: r for r in regs}         # a hook at a function's entry: argument registers are the ABI's
    nf = m.fmap[f][0]
    ol, nl = m.old.insns.get(f), m.new.insns.get(nf)
    if not ol or not nl:
        return None
    k = (addr - int(f, 16)) // 4
    out = {}
    for reg in regs:
        votes = {}
        for kk in range(max(0, k - window), min(len(ol), k + window)):
            nk, how = align_index(ol, nl, kk)
            if nk is None or nl[nk][0] != ol[kk][0]:
                continue
            a = REGTOK.findall(ol[kk][1])
            b = REGTOK.findall(nl[nk][1])
            if len(a) != len(b):
                continue
            for (ka, na_), (kb, nb_) in zip(a, b):
                if ka + na_ == reg and ka == kb:
                    w = 3 if abs(kk - k) <= 3 else 1
                    votes[kb + nb_] = votes.get(kb + nb_, 0) + w
        if not votes:
            return None
        out[reg] = max(votes, key=votes.get)
    return out


def remap_hooks(m, ours_path, upstream_path, out_path):
    """Our hook file re-keyed: hooks upstream has too are taken as upstream wrote them (checked by hand there); ours
    get their address, jump targets and registers mapped. Upstream's hooks we do not have are listed, not added."""
    up = {}
    for blk in re.split(r'\n(?=\[\[)', open(upstream_path, encoding='utf-8').read()):
        n = re.search(r'name\s*=\s*"([^"]+)"', blk)
        if n:
            up[n.group(1)] = blk.strip()
    text = open(ours_path, encoding='utf-8').read()
    blocks = re.split(r'\n(?=\[\[)', text)
    addrs = []
    for blk in blocks:
        for mm in TOKEN.finditer(blk):
            addrs.append(int(mm.group(2), 16))
    m.prepare(addrs)
    out, report = [], []
    for blk in blocks:
        n = re.search(r'name\s*=\s*"([^"]+)"', blk)
        if not n or '[[' not in blk:
            out.append(blk)
            continue
        name = n.group(1)
        if name in up:
            out.append('\n' + up[name] + '\n')
            report.append('%-34s upstream' % name)
            continue
        a = re.search(r'address\s*=\s*(0x[0-9A-Fa-f]+)', blk)
        addr = int(a.group(1), 16)
        new_blk, ok, notes = blk, True, []

        def sub(mm):
            nonlocal ok
            x = int(mm.group(2), 16)
            got, kind, note = m.map(x)
            if got is None:
                ok = False
                notes.append('%08X: %s' % (x, note))
                return mm.group(0)
            notes.append('%08X->%08X %s' % (x, got, note[:60]))
            return mm.group(1) + '%08X' % got
        new_blk = TOKEN.sub(sub, new_blk)
        rl = re.search(r'registers\s*=\s*\[([^\]]*)\]', blk)
        if rl:
            regs = re.findall(r'"([^"]+)"', rl.group(1))
            if regs:
                rm = register_map(m, addr, regs)
                if rm is None:
                    ok = False
                    notes.append('registers %s: no votes' % regs)
                else:
                    new_list = ', '.join('"%s"' % rm[r] for r in regs)
                    new_blk = new_blk.replace(rl.group(0), 'registers = [%s]' % new_list)
                    if any(rm[r] != r for r in regs):
                        notes.append('registers %s -> %s' % (regs, [rm[r] for r in regs]))
        if not ok:
            new_blk = '\n'.join('# TU: UNMAPPED, hook disabled: ' + l if l.strip() else l for l in blk.strip().split('\n'))
            new_blk = '\n' + new_blk + '\n'
        out.append(new_blk)
        report.append('%-34s %s  %s' % (name, 'mapped' if ok else 'UNMAPPED', '; '.join(notes)))
    with open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(x.strip('\n') for x in out) + '\n')
    for r in report:
        print(r)
    missing = [n for n in up if n not in text]
    print('upstream hooks not in ours (their code is upstream\'s):', missing)


def hooks(path):
    out = {}
    for blk in re.split(r'\n(?=\[\[)', open(path, encoding='utf-8').read()):
        a = re.search(r'address\s*=\s*(0x[0-9A-Fa-f]+)', blk)
        n = re.search(r'name\s*=\s*"([^"]+)"', blk)
        if a and n:
            out[n.group(1)] = int(a.group(1), 16)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--maps', required=True)
    ap.add_argument('--old-gen', required=True)
    ap.add_argument('--new-gen', required=True)
    ap.add_argument('--test-hooks', nargs=2)
    ap.add_argument('--hooks', nargs=3, metavar=('OURS', 'UPSTREAM', 'OUT'))
    ap.add_argument('--rewrite', nargs='*', default=[])
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--report')
    ap.add_argument('--exclude', help='a file of hex values (one per line, or the 2nd column of a tsv) to leave alone: '
                    'values a file already has in their title-update form (reNut upstream\'s own edits)')
    a = ap.parse_args()
    exclude = set()
    if a.exclude:
        for line in open(a.exclude, encoding='utf-8'):
            cols = line.split('	')
            v = (cols[1] if len(cols) > 1 else cols[0]).strip()
            v = re.sub(r'^(0x|sub_|loc_|dword_|byte_|word_|unk_|off_)', '', v)
            if re.fullmatch(r'[0-9a-fA-F]{8}', v):
                exclude.add(int(v, 16))
    m = Mapper(a.maps, a.old_gen, a.new_gen)
    if a.hooks:
        remap_hooks(m, *a.hooks)
        return
    if a.test_hooks:
        o, u = hooks(a.test_hooks[0]), hooks(a.test_hooks[1])
        m.prepare(list(o.values()))
        ok = 0
        for n, addr in o.items():
            if n not in u:
                continue
            got, kind, note = m.map(addr)
            good = got == u[n]
            ok += good
            print('%-34s %08X -> %s  upstream %08X  %s  %s' % (n, addr, '%08X' % got if got else 'NONE', u[n],
                                                                'OK' if good else 'MISS', note))
        print('ok %d of %d' % (ok, sum(1 for n in o if n in u)))
        return
    texts = {}
    addrs = set()
    for p in a.rewrite:
        t = open(p, encoding='utf-8', errors='replace').read()
        texts[p] = t
        for mm in TOKEN.finditer(t):
            addrs.add(int(mm.group(2), 16))
    addrs -= exclude
    m.prepare(addrs)
    table = {x: m.map(x) for x in addrs}
    for x in exclude:
        table[x] = (None, 'kept', 'excluded: already the title update\'s')
    rows = []
    for p, t in texts.items():
        def sub(mm):
            x = int(mm.group(2), 16)
            got, kind, note = table[x]
            if got is None:
                return mm.group(0)
            s = mm.group(2)
            new = ('%08X' % got) if s.isupper() or s.isdigit() else ('%08x' % got)
            return mm.group(1) + new
        out = TOKEN.sub(sub, t)
        for mm in TOKEN.finditer(t):
            x = int(mm.group(2), 16)
            got, kind, note = table[x]
            rows.append((p, '%08X' % x, '%08X' % got if got else '-', kind, note))
        if not a.dry_run and out != t:
            with open(p, 'w', encoding='utf-8', newline='') as f:
                f.write(out)
    unm = sum(1 for v in table.values() if v[0] is None)
    print('%d addresses: %d mapped, %d not' % (len(table), len(table) - unm, unm))
    if a.report:
        with open(a.report, 'w', encoding='utf-8') as f:
            for r in rows:
                f.write('\t'.join(r) + '\n')


if __name__ == '__main__':
    main()
