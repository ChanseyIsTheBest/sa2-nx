#!/usr/bin/env python3
"""katamari-nx (2026-10-30): matches Renut's hooked Direct3D functions (written at N&B's addresses, banjo-nx title
update 3) to Beautiful Katamari's functions by their field signature, for the hook map (docs/katamari-notes.md,
"Renut's renderer at Katamari's addresses"). The method of feedingfrenzy2-nx and zuma-nx (their scripts were one-offs,
only the outputs are kept there): a function's signature is what does not depend on register allocation or
scheduling:
  - the multiset of its instructions with registers removed: mnemonic plus the displacement of a D-form load / store,
    the immediate of li / lis / addi / ori / cmpwi / cmplwi / andi., the shift and mask of rlwinm;
  - the imports it calls (by name) and how many calls it makes;
  - its size.
Similarity is the weighted Jaccard index of the multisets (0..1), with the imports counted three times.

  python tools/renut_sigmatch.py --nb <banjo-nx app/generated> --game app/generated --hooks app/src/renut_engine/gpu/d3d/hooks
         [--top 3] [--out out/renut_map/sigmatch.txt] [--lo 0x82400000 --hi 0x82600000]

Output: one line per hook: Renut's name, N&B's address, the best matches in this game with their scores.
"""
import argparse
import collections
import glob
import os
import re

FUNC = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)')
COMMENT = re.compile(r'^\t// (\S.*)$')
CALL = re.compile(r'\b(sub_[0-9A-F]{8}|__imp__\w+)\(ctx, base\)')
DFORM = re.compile(r'^(l[bhwd]z?u?|lha|lwa|st[bhwd]u?|lfs|lfd|stfs|stfd|lmw|stmw)\s+\w+,(-?\d+)\((r\d+)\)')
IMM = re.compile(r'^(li|lis|addi|addis|ori|oris|cmpwi|cmplwi|cmpdi|cmpldi|andi\.|andis\.|xori|subfic|mulli)\s+(?:cr\d,)?[^,]+(?:,[^,]+)?,(-?\d+)$')
RLW = re.compile(r'^(rlwinm|rlwimi|rldicl|rldicr|clrlwi|clrldi|srwi|slwi|srawi)\.?\s+\w+,\w+,(.*)$')
HOOK = re.compile(r'BANJO_RENUT_HOOK(?:_SHARED)?\((sub_[0-9A-F]{8}),\s*(\w+)\);\s*(?://\s*(\S+))?')
HOOK_RAW = re.compile(r'BANJO_RENUT_HOOK_RAW(?:_SHARED)?\((sub_[0-9A-F]{8})\)\s*\{?\s*(?://\s*(\S+))?')


def load_tree(gen):
    """{function: (features Counter, imports Counter, calls, size, [instruction texts])}"""
    out = {}
    for path in glob.glob(os.path.join(gen, '*recomp*.cpp')):
        cur = None
        feats = imports = None
        calls = 0
        ins = []
        for line in open(path, errors='replace'):
            m = FUNC.match(line)
            if m:
                cur = m.group(1)
                feats, imports, calls, ins = collections.Counter(), collections.Counter(), 0, []
                continue
            if cur is None:
                continue
            if line.startswith('}'):
                out[cur] = (feats, imports, calls, len(ins), ins)
                cur = None
                continue
            m = COMMENT.match(line)
            if m:
                t = m.group(1).strip()
                if t.startswith(('PPC_', '0x', 'ERROR')):
                    continue
                ins.append(t)
                op = t.split()[0]
                d = DFORM.match(t)
                if d:
                    feats[f'{d.group(1)} {d.group(2)}' + ('(r1)' if d.group(3) == 'r1' else '')] += 1
                    continue
                i = IMM.match(t)
                if i:
                    feats[f'{i.group(1)} {i.group(2)}'] += 1
                    continue
                r = RLW.match(t)
                if r:
                    feats[f'{r.group(1)} {r.group(2)}'] += 1
                    continue
                feats[op] += 1
                continue
            for c in CALL.findall(line):
                calls += 1
                if c.startswith('__imp__'):
                    imports[c[7:]] += 1
    return out


def score(a, b):
    fa, ia, ca, na, _ = a
    fb, ib, cb, nb, _ = b
    inter = sum((fa & fb).values()) + 3 * sum((ia & ib).values())
    union = sum((fa | fb).values()) + 3 * sum((ia | ib).values())
    s = inter / union if union else 0.0
    if na and nb:
        s *= 0.85 + 0.15 * min(na, nb) / max(na, nb)
    return s


def load_hooks(folder):
    hooks = []
    for path in sorted(glob.glob(os.path.join(folder, '*.cpp'))):
        for line in open(path, encoding='utf-8', errors='replace'):
            m = HOOK.search(line)
            if m:
                hooks.append((m.group(1), m.group(3) or m.group(2), os.path.basename(path)))
                continue
            m = HOOK_RAW.search(line)
            if m:
                hooks.append((m.group(1), m.group(2) or '?', os.path.basename(path)))
    return hooks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--nb', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--hooks', required=True)
    ap.add_argument('--top', type=int, default=3)
    ap.add_argument('--lo', default='0x82000000')
    ap.add_argument('--hi', default='0x83000000')
    ap.add_argument('--out', default=None)
    ap.add_argument('--only', default=None, help='comma-separated N&B functions')
    a = ap.parse_args()
    nb = load_tree(a.nb)
    game = load_tree(a.game)
    lo, hi = int(a.lo, 16), int(a.hi, 16)
    cands = {k: v for k, v in game.items() if lo <= int(k[4:], 16) < hi}
    hooks = load_hooks(a.hooks)
    if a.only:
        want = set(a.only.split(','))
        hooks = [h for h in hooks if h[0] in want] or [(w, '?', '-') for w in want]
    lines = []
    for sub, name, src in hooks:
        if sub not in nb:
            lines.append(f'{name:44s} {sub}  (not in the N&B tree)  [{src}]')
            continue
        best = sorted(((score(nb[sub], v), k) for k, v in cands.items()), reverse=True)[:a.top]
        lines.append(f'{name:44s} {sub} n={nb[sub][3]:<4d} ' + '  '.join(f'{k}={s:.2f}(n={cands[k][3]})' for s, k in best)
                     + f'  [{src}]')
    text = '\n'.join(lines) + '\n'
    if a.out:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        open(a.out, 'w').write(text)
    print(text)


if __name__ == '__main__':
    main()
