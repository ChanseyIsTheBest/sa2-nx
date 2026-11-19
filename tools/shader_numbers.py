#!/usr/bin/env python3
"""banjo-nx: the container behind a shader number of the logs ("PS n536", "VS n768"; docs/shader.md).

A shader's number is its position in banjo_shaders.mesp, whose entries are sorted by the XXH3-64 of the original
container (masseffect_shader_library.cpp: Smaller), identical containers once. Adding shaders shifts the numbers
after them, so name the package the log came from: its input folder, minus the containers added later.

Usage: python tools/shader_numbers.py [--containers DIR] [--without NAME,...] N [N ...]
       python tools/shader_numbers.py [--containers DIR] [--without NAME,...] --name NAME [NAME ...]
  DIR defaults to out/shaders_prev/in (the library build's input). E.g. the nineteenth package's numbers:
  --without ps_db727b9b18f73fa3b,vs_d5cc2e40fc642589d (added in the twentieth).
Needs the xxhash module (pip install xxhash).
"""
import argparse
import pathlib

import xxhash

ROOT = pathlib.Path(__file__).resolve().parent.parent


def library_order(folder, without):
    first = {}
    for path in sorted(pathlib.Path(folder).glob('*.bin')):
        if path.stem in without:
            continue
        data = path.read_bytes()
        first.setdefault((xxhash.xxh3_64_intdigest(data), data), path.stem)
    return [first[k] for k in sorted(first)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--containers', default=str(ROOT / 'out' / 'shaders_prev' / 'in'))
    ap.add_argument('--without', default='')
    ap.add_argument('--name', action='store_true', help='the arguments are container names: print their numbers')
    ap.add_argument('items', nargs='+')
    a = ap.parse_args()
    order = library_order(a.containers, set(x for x in a.without.split(',') if x))
    print('%d shaders' % len(order))
    if a.name:
        index = {n: i for i, n in enumerate(order)}
        for n in a.items:
            print(n, index.get(n.removesuffix('.bin'), 'not in the library'))
    else:
        for n in a.items:
            k = int(n)
            print(k, order[k] if 0 <= k < len(order) else 'out of range')


if __name__ == '__main__':
    main()
