#!/usr/bin/env python3
"""banjo-nx (2026-11-20): two GLSL trees (vs_/ps_<HASH>.glsl), each shader's generic form (no constant) compiled with
the same uam, compared by Maxwell instructions and GPRs (tools/deko3d_shader_stats.py's DKSH reader). For "is this
tree's code bigger than that one's" across the whole game, without the pipeline lists' 53,000 variants.

  python tools/deko3d_glsl_compare.py <base glsl dir> <new glsl dir> [--uam EXE] [--jobs 12] [--top 30] [--json out]
"""
import argparse
import concurrent.futures
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deko3d_shader_stats as dss  # noqa: E402


def job(args):
    name, path, uam = args
    stage = 'frag' if name.startswith('ps_') else 'vert'
    _, st, err = dss.compile_stats((name, stage, open(path, 'rb').read()), uam=uam)
    return name, st, err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('new')
    ap.add_argument('--uam', default=dss.UAM)
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--json', default=None)
    a = ap.parse_args()
    dss.TABLE = dss.opcode_table()  # the stats tool builds it in its main
    names = sorted(set(os.listdir(a.base)) & set(os.listdir(a.new)))
    names = [n for n in names if n.endswith('.glsl')]
    work = [(n, os.path.join(d, n), a.uam) for n in names for d in (a.base, a.new)]
    res = {}
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for i, (n, st, err) in enumerate(ex.map(job, work)):
            res.setdefault(n, []).append(st)
            if i % 1000 == 0:
                print('  %d/%d' % (i, len(work)), flush=True)
    rows = []
    for n in names:
        b, w = res[n]
        if not b or not w:
            continue
        rows.append((n, b['instr'], w['instr'], b['gprs'], w['gprs'], b['classes'], w['classes']))
    for kind in ('vs_', 'ps_'):
        sel = [r for r in rows if r[0].startswith(kind)]
        bi = sum(r[1] for r in sel)
        wi = sum(r[2] for r in sel)
        bg = sum(r[3] for r in sel) / max(1, len(sel))
        wg = sum(r[4] for r in sel) / max(1, len(sel))
        grew = sum(1 for r in sel if r[2] > r[1])
        shrank = sum(1 for r in sel if r[2] < r[1])
        print('%s %d shaders: instructions %d -> %d (%+.1f %%); mean GPRs %.1f -> %.1f; %d grew, %d shrank' % (
            kind, len(sel), bi, wi, 100.0 * (wi - bi) / max(1, bi), bg, wg, grew, shrank))
        classes = sorted(set().union(*[set(r[5]) | set(r[6]) for r in sel]) if sel else [])
        print('   by class: ' + ', '.join('%s %d->%d' % (c, sum(r[5].get(c, 0) for r in sel),
                                                         sum(r[6].get(c, 0) for r in sel)) for c in classes))
    print('largest growth (instructions, base -> new, GPRs):')
    for r in sorted(rows, key=lambda r: r[1] - r[2])[:0] or sorted(rows, key=lambda r: r[2] - r[1], reverse=True)[:a.top]:
        print('  %-26s %5d -> %5d (%+5d)  gprs %3d -> %3d' % (r[0], r[1], r[2], r[2] - r[1], r[3], r[4]))
    if a.json:
        json.dump({r[0]: {'base': [r[1], r[3]], 'new': [r[2], r[4]]} for r in rows}, open(a.json, 'w'))


if __name__ == '__main__':
    main()
