#!/usr/bin/env python3
"""banjo-nx (2026-10-20): write the variants (the text plume_deko3d compiles: the shader with its specialization
defines) the pipeline lists make of some shaders, for uam_ops.py / difftest_game.py.

    python tools/difftest/dump_variants.py --list pipeline_list.bin [--list ...] [--glsl DIR] --out DIR HASH [HASH ...]

HASH: ps_<16 hex> or vs_<16 hex>. Each variant goes to <out>/<prefix>_<hash>_<n>.glsl, most-used first is not known
(the lists do not count draws): in list order. Game data: write under the build tree or the scratchpad only.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import deko3d_shader_stats as stats  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', action='append', required=True)
    ap.add_argument('--glsl', default=os.path.join(stats.ROOT, 'out', 'deko3d_shaders', 'glsl_opt'))
    ap.add_argument('--out', required=True)
    ap.add_argument('names', nargs='+')
    a = ap.parse_args()
    variants, _ = stats.collect(a.glsl, a.list, True)
    os.makedirs(a.out, exist_ok=True)
    for name in a.names:
        prefix, h = name.split('_', 1)
        found = variants.get((prefix, h.upper()), [])
        for n, (_, stage, text) in enumerate(found):
            path = os.path.join(a.out, f'{prefix}_{h.upper()}_{n}.glsl')
            open(path, 'wb').write(text)
        print(name, len(found), 'variants')


if __name__ == '__main__':
    main()
