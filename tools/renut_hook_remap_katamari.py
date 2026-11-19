#!/usr/bin/env python3
"""katamari-nx (2026-10-30): moves Renut's hook table (app/src/renut_engine/gpu/d3d/hooks/*.cpp, at N&B's title-update-3
addresses) to Beautiful Katamari's functions. One-off per hook state of the tree; docs/katamari-notes.md, "Renut's
renderer at Katamari's addresses", has the evidence.

Sources, in this order:
  1. tools/renut_hook_map_katamari.txt: `<N&B sub> <this game's address or -> <evidence>` lines written by hand
     (they win over everything else; `-` = leave unhooked);
  2. the state tables, pair by pair (out/renut_map/state_tables.txt, tools/renut_state_tables.py);
  3. tools/renut_sigmatch.py's best match (out/renut_map/sigmatch_all.txt) when it scores >= --min (0.75) and beats the
     second by --margin (0.05).
A hook that none of them maps becomes `BANJO_RENUT_UNHOOKED(sub_<N&B>, ...)` (compiled, registered nowhere: an N&B
address is another function here). Two hooks may not map to one address: the second is left unhooked and reported.

  python -P tools/renut_hook_remap_katamari.py [--dry-run]
"""
import argparse
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOOKS = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu', 'd3d', 'hooks')
LINE = re.compile(r'^(\s*)BANJO_RENUT_(HOOK|HOOK_SHARED|HOOK_RAW|HOOK_RAW_SHARED)\((sub_[0-9A-F]{8})([,)])(.*)$')
MATCH = re.compile(r'(sub_[0-9A-F]{8})=([0-9.]+)')


def load():
    hand = {}
    path = os.path.join(ROOT, 'tools', 'renut_hook_map_katamari.txt')
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            if not line.strip() or line.startswith('#'):
                continue
            nb, game, why = line.split(None, 2)
            hand[nb] = (None if game == '-' else f'sub_{game[-8:].upper()}', why.strip())
    states = {}
    for line in open(os.path.join(ROOT, 'out', 'renut_map', 'state_tables.txt')):
        if line.startswith('#'):
            continue
        kind, i, gs, nb, game, j = line.split()
        states[f'sub_{nb}'] = (f'sub_{game}', f'{kind} state {i} {gs}ter (state tables, this game index {j})')
    sig = {}
    for line in open(os.path.join(ROOT, 'out', 'renut_map', 'sigmatch_all.txt')):
        p = line.split()
        if len(p) < 3 or not p[1].startswith('sub_'):
            continue
        sig[p[1]] = [(m.group(1), float(m.group(2))) for m in MATCH.finditer(line)]
    return hand, states, sig


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--min', type=float, default=0.75)
    ap.add_argument('--margin', type=float, default=0.05)
    a = ap.parse_args()
    hand, states, sig = load()
    used = {}
    report = []
    counts = {'hand': 0, 'state': 0, 'sig': 0, 'unhooked': 0}
    for name in sorted(os.listdir(HOOKS)):
        if not name.endswith('.cpp'):
            continue
        path = os.path.join(HOOKS, name)
        text = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in text else '\n'
        lines = text.split(nl)
        changed = False
        for k, line in enumerate(lines):
            m = LINE.match(line)
            if not m:
                continue
            indent, kind, nb, sep, rest = m.groups()
            target, why, src = None, None, None
            if nb in hand:
                target, why = hand[nb]
                src = 'hand'
            elif nb in states:
                target, why = states[nb]
                src = 'state'
            elif nb in sig and sig[nb]:
                best = sig[nb]
                second = best[1][1] if len(best) > 1 else 0.0
                if best[0][1] >= a.min and best[0][1] - second >= a.margin:
                    target, why = best[0][0], f'signature {best[0][1]:.2f} (next {second:.2f})'
                    src = 'sig'
                else:
                    why = f'signature too weak or ambiguous ({best[0][0]} {best[0][1]:.2f}, next {second:.2f})'
            else:
                why = 'no match'
            if target and target in used:
                why = f'{target} already taken by {used[target]}'
                target = None
            if target:
                used[target] = nb
                counts[src] += 1
                # banjo-nx's own renderer hooks (src/me/banjo_me_hooks.cpp), which called Renut's side of a shared
                # address, are not built here: a shared hook is an ordinary one.
                kind = {'HOOK_SHARED': 'HOOK', 'HOOK_RAW_SHARED': 'HOOK_RAW'}.get(kind, kind)
                new = f'{indent}BANJO_RENUT_{kind}({target}{sep}{rest}'
                tag = f'  // katamari-nx: N&B {nb}, {why}'
                if '// katamari-nx:' not in new:
                    new += tag
            else:
                counts['unhooked'] += 1
                new = f'{indent}BANJO_RENUT_UNHOOKED_{kind}({nb}{sep}{rest}'
                if '// katamari-nx:' not in new:
                    new += f'  // katamari-nx: not hooked ({why})'
            report.append(f'{name:14s} {nb} -> {target or "-":12s} {why}')
            if new != line:
                lines[k] = new
                changed = True
        if changed and not a.dry_run:
            open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
    out = os.path.join(ROOT, 'out', 'renut_map', 'remap_report.txt')
    open(out, 'w').write('\n'.join(report) + '\n')
    print(counts, 'report:', out)


if __name__ == '__main__':
    main()
