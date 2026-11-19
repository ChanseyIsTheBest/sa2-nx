#!/usr/bin/env python3
"""banjo-nx (2026-10-28, the title update): maps the functions and the data addresses of one executable's generated
code (the base XEX's) to another's (the title update's), so every address-keyed list, hook and native can be re-keyed.

The title update is a recompile: most functions moved, and thousands came out with other register allocation. So:
1. Anchors, in order of trust (each kept only where it agrees with the order of the ones before: the longest
   increasing subsequence; the linker kept the order):
   - names: the functions both function lists (config/renut_funcs.toml of each version) name the same way, the
     address suffix of a generated name (rex_x_82363ED0) left out, unique in both;
   - shapes that occur once in each executable (tools/func_signatures.py [0]);
   - skeletons (mnemonics only, [3]) that occur once in each.
2. Between two anchors: equal shapes in order, then equal skeletons in order, then a sequence alignment (difflib) of
   the skeletons for the rest; leftovers paired by position when both sides have the same number.
3. A call-graph check of every pair that is not 'same': the share of its calls whose mapped target is a call of the
   new function. Pairs with calls and a share under one half are 'doubtful'.
Status: 'same' (text equal), 'shape' (instructions and registers equal, immediates not), 'skel' (mnemonics equal),
'changed' (paired by place, content differs), 'doubtful'.

Data addresses: in each pair with the same skeleton, the addresses built with lis + addi / ori / a load or store
offset are replayed side by side; each old address votes for the new one at the same place.

  python tools/map_functions.py <old gen> <new gen> <old sigs> <new sigs> <old funcs.toml> <new funcs.toml> <out dir>
Writes func_map.json ({old: [new, status, old count, new count, call share]}) and data_map.json.
"""
import bisect
import collections
import difflib
import glob
import json
import os
import re
import sys

FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$')
INSN = re.compile(r'^\s*// ([a-z][a-z0-9.+\-]*)(?:\s+(.*))?$')
REG = re.compile(r'^r(\d+)$')
MEM = re.compile(r'^(-?\d+)\(r(\d+)\)$')
NAMED = re.compile(r'^(0x[0-9A-Fa-f]+)\s*=\s*\{[^}]*name\s*=\s*"([^"]+)"', re.M)
SUFFIX = re.compile(r'_?(?:sub_)?8[0-9A-F]{7}$', re.I)
STORES = ('stw', 'stb', 'sth', 'std', 'stwu', 'stfs', 'stfd', 'stwx', 'cmpw', 'cmplw', 'cmpwi', 'cmplwi', 'cmpd', 'cmpdi')


def lis_sequence(insns):
    hi = {}
    out = []
    for k, (mnem, ops) in enumerate(insns):
        parts = [p.strip() for p in ops.split(',')] if ops else []
        if mnem == 'lis' and len(parts) == 2 and REG.match(parts[0]):
            try:
                hi[parts[0]] = (int(parts[1], 0) & 0xFFFF) << 16
            except ValueError:
                hi.pop(parts[0], None)
            continue
        if mnem in ('addi', 'addic') and len(parts) == 3 and parts[1] in hi:
            try:
                out.append((k, (hi[parts[1]] + int(parts[2], 0)) & 0xFFFFFFFF))
            except ValueError:
                pass
        elif mnem == 'ori' and len(parts) == 3 and parts[1] in hi:
            try:
                out.append((k, hi[parts[1]] | (int(parts[2], 0) & 0xFFFF)))
            except ValueError:
                pass
        elif len(parts) == 2:
            m = MEM.match(parts[1])
            if m and ('r' + m.group(2)) in hi:
                out.append((k, (hi['r' + m.group(2)] + int(m.group(1))) & 0xFFFFFFFF))
        if parts and REG.match(parts[0]) and mnem not in STORES:
            hi.pop(parts[0], None)
    return out


def instructions(gen_dir, wanted):
    funcs = {}
    for path in glob.glob(os.path.join(gen_dir, '*.cpp')):
        cur = None
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = FUNC.match(line)
                if m:
                    cur = m.group(1) if m.group(1) in wanted else None
                    if cur:
                        funcs[cur] = []
                    continue
                if cur is None:
                    continue
                i = INSN.match(line)
                if i:
                    funcs[cur].append((i.group(1), i.group(2) or ''))
    return funcs


def increasing(pairs):
    """The longest subsequence of (i, j) pairs, sorted by i, that also increases in j."""
    pairs = sorted(pairs)
    tails, tail_idx, prev = [], [], [-1] * len(pairs)
    for n, (_, j) in enumerate(pairs):
        k = bisect.bisect_left(tails, j)
        if k == len(tails):
            tails.append(j)
            tail_idx.append(n)
        else:
            tails[k] = j
            tail_idx[k] = n
        prev[n] = tail_idx[k - 1] if k else -1
    out, n = [], tail_idx[-1] if tail_idx else -1
    while n >= 0:
        out.append(pairs[n])
        n = prev[n]
    return out[::-1]


def names(paths):
    """Names from one or more function lists (';' between paths: renut_funcs.toml and renut_gpu_funcs.toml)."""
    found = collections.defaultdict(list)
    for path in paths.split(';'):
        for m in NAMED.finditer(open(path, encoding='utf-8', errors='replace').read()):
            found[SUFFIX.sub('', m.group(2))].append('%08X' % int(m.group(1), 16))
    return {n: v[0] for n, v in found.items() if len(v) == 1 and n}


def merge_anchors(base, extra):
    """Adds the extra anchors that fall strictly inside the intervals of the base ones (keeping the order)."""
    base = sorted(base)
    bi = [b[0] for b in base]
    keep = []
    for i, j in extra:
        k = bisect.bisect_left(bi, i)
        if k < len(bi) and bi[k] == i:
            continue
        lo = base[k - 1] if k else (-1, -1)
        hi = base[k] if k < len(base) else (10 ** 9, 10 ** 9)
        if lo[1] < j < hi[1]:
            keep.append((i, j))
    return sorted(base + increasing(keep))


def map_functions(old, new, old_names, new_names):
    oa, na = list(old), list(new)
    oidx = {a: i for i, a in enumerate(oa)}
    nidx = {a: j for j, a in enumerate(na)}
    name_pairs = [(oidx[old_names[n]], nidx[new_names[n]]) for n in old_names
                  if n in new_names and old_names[n] in oidx and new_names[n] in nidx]
    anchors = increasing(name_pairs)
    named = len(anchors)
    for field in (0, 3):
        oc = collections.Counter(old[a][field] for a in oa)
        nc = collections.Counter(new[a][field] for a in na)
        npos = {new[a][field]: j for j, a in enumerate(na) if nc[new[a][field]] == 1}
        cand = [(i, npos[old[a][field]]) for i, a in enumerate(oa)
                if oc[old[a][field]] == 1 and old[a][field] in npos]
        anchors = merge_anchors(anchors, cand)
    result = {}

    def status(a, b):
        if old[a][1] == new[b][1]:
            return 'same'
        if old[a][0] == new[b][0]:
            return 'shape'
        if old[a][3] == new[b][3]:
            return 'skel'
        return 'changed'

    bounds = [(-1, -1)] + anchors + [(len(oa), len(na))]
    for (i0, j0), (i1, j1) in zip(bounds, bounds[1:]):
        if 0 <= i0 < len(oa):
            result[oa[i0]] = (na[j0], status(oa[i0], na[j0]))
        gap_o, gap_n = list(range(i0 + 1, i1)), list(range(j0 + 1, j1))
        if not gap_o or not gap_n:
            continue
        matched = {}
        for field in (0, 3):
            used = set(matched.values())
            j = 0
            rest_n = [x for x in gap_n if x not in used]
            for i in [x for x in gap_o if x not in matched]:
                for jj in range(j, len(rest_n)):
                    if new[na[rest_n[jj]]][field] == old[oa[i]][field]:
                        # only in order with what is matched already
                        lo = max([matched[x] for x in matched if x < i], default=-1)
                        hi = min([matched[x] for x in matched if x > i], default=10 ** 9)
                        if lo < rest_n[jj] < hi:
                            matched[i] = rest_n[jj]
                            j = jj + 1
                        break
        # the rest: an ordered alignment by similarity (mnemonic histogram, size), segment by segment between the
        # matches; a pair below kThreshold stays unmatched
        segs = sorted(matched.items())
        sb = [(i0, j0)] + segs + [(i1, j1)]
        for (a0, b0), (a1, b1) in zip(sb, sb[1:]):
            ro, rn = list(range(a0 + 1, a1)), list(range(b0 + 1, b1))
            if not ro or not rn:
                continue
            for i, jn in align(ro, rn, lambda i, j: similarity(old[oa[i]], new[na[j]])):
                matched[i] = jn
        for i, jn in matched.items():
            result[oa[i]] = (na[jn], status(oa[i], na[jn]))
    return result, named, len(anchors)


kThreshold = 0.6


def similarity(a, b):
    ha, hb = a[5], b[5]
    total = a[2] + b[2]
    if not total:
        return 0.0
    diff = sum(abs(ha.get(k, 0) - hb.get(k, 0)) for k in set(ha) | set(hb))
    hist = 1.0 - diff / total
    size = min(a[2], b[2]) / max(a[2], b[2])
    return 0.7 * hist + 0.3 * size


def align(ro, rn, score):
    """The ordered pairing of ro and rn with the highest total of (score - kThreshold) over its pairs (a
    Needleman-Wunsch alignment without gap costs), within a band around the diagonal when the segment is large."""
    n, m = len(ro), len(rn)
    band = None if n * m <= 2_000_000 else max(abs(n - m) + 64, 64)
    best = [[0.0] * (m + 1) for _ in range(n + 1)]
    step = [[0] * (m + 1) for _ in range(n + 1)]  # 1: skip old, 2: skip new, 3: pair
    for x in range(1, n + 1):
        jx = x * m // n if band else 0
        for y in range(1, m + 1):
            if band and abs(y - jx) > band:
                best[x][y] = max(best[x - 1][y], best[x][y - 1])
                step[x][y] = 1 if best[x - 1][y] >= best[x][y - 1] else 2
                continue
            s = score(ro[x - 1], rn[y - 1]) - kThreshold
            pair = best[x - 1][y - 1] + s if s > 0 else -1.0
            up, left = best[x - 1][y], best[x][y - 1]
            if pair >= up and pair >= left:
                best[x][y], step[x][y] = pair, 3
            elif up >= left:
                best[x][y], step[x][y] = up, 1
            else:
                best[x][y], step[x][y] = left, 2
    out = []
    x, y = n, m
    while x and y:
        if step[x][y] == 3:
            out.append((ro[x - 1], rn[y - 1]))
            x, y = x - 1, y - 1
        elif step[x][y] == 1:
            x -= 1
        else:
            y -= 1
    return out[::-1]


def main():
    old_gen, new_gen, old_sigs, new_sigs, old_toml, new_toml, out_dir = sys.argv[1:8]
    old = json.load(open(old_sigs))
    new = json.load(open(new_sigs))
    fmap, named, anchors = map_functions(old, new, names(old_toml), names(new_toml))
    # the call-graph check
    final = {}
    for a, (b, s) in fmap.items():
        calls = old[a][4]
        share = None
        if calls:
            targets = set(new[b][4])
            hits = sum(1 for c in calls if c in fmap and fmap[c][0] in targets)
            share = hits / len(calls)
            if s not in ('same',) and share < 0.5 and len(calls) >= 2:
                s = 'doubtful'
        final[a] = [b, s, old[a][2], new[b][2], share]
    counts = collections.Counter(v[1] for v in final.values())
    print('functions: %d old, %d new; %d name anchors, %d anchors in all; mapped %d: %s; unmapped %d' % (
        len(old), len(new), named, anchors, len(final), dict(counts), len(old) - len(final)))
    json.dump(dict(sorted(final.items())), open(os.path.join(out_dir, 'func_map.json'), 'w'))
    pairs = {a: v[0] for a, v in final.items() if v[1] in ('same', 'shape', 'skel')}
    oi = instructions(old_gen, set(pairs))
    ni = instructions(new_gen, set(pairs.values()))
    lo_code, hi_code = int(min(new), 16), int(max(new), 16) + 0x100000
    votes = collections.defaultdict(collections.Counter)
    for a, b in pairs.items():
        so, sn = lis_sequence(oi.get(a, [])), lis_sequence(ni.get(b, []))
        if len(so) != len(sn):
            continue
        for (k1, x), (k2, y) in zip(so, sn):
            if k1 == k2 and x >= 0x80000000 and not (lo_code <= x < hi_code):
                votes[x][y] += 1
    dmap = {}
    for x, c in votes.items():
        (y, n), = c.most_common(1)
        dmap['%08X' % x] = ['%08X' % y, n, sum(c.values()) - n]
    json.dump(dict(sorted(dmap.items())), open(os.path.join(out_dir, 'data_map.json'), 'w'))
    print('data addresses: %d mapped (%d with rival votes)' % (len(dmap), sum(1 for v in dmap.values() if v[2])))


if __name__ == '__main__':
    main()
