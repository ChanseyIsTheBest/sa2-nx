#!/usr/bin/env python3
"""vivapinata-nx (2026-11-11, the GPU optimisation work): one frame of a PC run's [drawlog] (banjo_renut_diag_draw_log_s)
as passes: runs of draws to the same colour target and depth target, with their draw count, primitives, blend states,
pixel shaders and the alpha test.

  python tools/drawlog_passes.py LOG [--frame N] [--shaders]
"""
import argparse
import collections
import re

DRAW = re.compile(r'\[drawlog\] frame (\d+) draw (\d+): vs (\w+) ps (\w+) prim (\d+) count (\d+) -> (?:indices|vertices)? ?(\d+)?.*?'
                  r'target (\S+) fmt (-?\d+) x(\d+) depth (-?\d+) dc (\w+) blend (\w+) mask (\w+) a2c (\d+) alpha (\d+)')


def main():
    a = argparse.ArgumentParser()
    a.add_argument('log')
    a.add_argument('--frame', type=int)
    a.add_argument('--shaders', action='store_true')
    args = a.parse_args()
    frames = collections.OrderedDict()
    for line in open(args.log, encoding='utf-8', errors='replace'):
        m = DRAW.search(line)
        if m:
            frames.setdefault(int(m.group(1)), []).append(m.groups())
    if not frames:
        raise SystemExit('no [drawlog] draws')
    f = args.frame if args.frame is not None else list(frames)[-1]
    draws = frames[f]
    print('frame %d: %d draws (frames logged: %s)' % (f, len(draws), ', '.join(map(str, frames))))
    passes = []
    for d in draws:
        key = (d[7], d[8], d[9], d[10])  # target, fmt, samples, depth
        if not passes or passes[-1]['key'] != key:
            passes.append({'key': key, 'draws': [], 'first': int(d[1])})
        passes[-1]['draws'].append(d)
    for i, p in enumerate(passes):
        t, fmt, x, depth = p['key']
        ds = p['draws']
        blends = collections.Counter(d[12] for d in ds)
        ps = collections.Counter(d[3][:8] for d in ds)
        prims = sum(int(d[5]) for d in ds)
        alpha = collections.Counter(d[15] for d in ds)
        print('%3d  draws %4d-%4d (%4d)  target %-10s fmt %3s x%s depth %3s  indices %7d  blend %s  alpha %s' % (
            i, p['first'], p['first'] + len(ds) - 1, len(ds), t, fmt, x, depth, prims,
            ' '.join('%s:%d' % kv for kv in blends.most_common(3)), ' '.join('%s:%d' % kv for kv in alpha.most_common(2))))
        if args.shaders:
            print('      ps ' + ' '.join('%s:%d' % kv for kv in ps.most_common(8)))


if __name__ == '__main__':
    main()
