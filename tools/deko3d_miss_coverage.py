#!/usr/bin/env python3
"""banjo-nx (2026-11-20): how many of the variants a run fell back for (banjo_deko3d_log_misses' "missed (generic
meanwhile) run-time variant" lines, or any "run-time variant" line) a --export-variants list holds: a check of
tools/deko3d_variant_cache.py's derivation against what the renderer asks for.

  python tools/deko3d_miss_coverage.py <log with the lines> <export list> [<another export list> ...] [--glsl DIR]
      [--nearest]   for each variant the lists lack: the closest listed variant of the same shader (fewest words
                    differing) and the words that differ, then the differing words counted over all of them (what
                    the cache tool's derivation does not guess)
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deko3d_variant_cache as dvc  # noqa: E402

RT = re.compile(r'(missed \(generic meanwhile\) )?run-time variant \(stage (\d+), \d+ bytes, base (\d+) bytes '
                r'([0-9a-f]{16})\): (.*)$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('exports', nargs='+')
    ap.add_argument('--glsl', default=os.path.join(dvc.ROOT, 'out', 'deko3d_shaders', 'glsl_opt'))
    ap.add_argument('--nearest', action='store_true')
    a = ap.parse_args()
    by_size = {}
    for n in os.listdir(a.glsl):
        by_size.setdefault(os.path.getsize(os.path.join(a.glsl, n)), []).append(n)
    fnv = {}
    misses = set()
    unmatched = 0
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = RT.search(line)
        if not m:
            continue
        size, want = int(m.group(3)), int(m.group(4), 16)
        name = None
        for n in by_size.get(size, []):
            if n not in fnv:
                fnv[n] = dvc.fnv1a64(open(os.path.join(a.glsl, n), 'rb').read())
            if fnv[n] == want:
                name = n
                break
        if not name:
            unmatched += 1
            continue
        pairs = []
        for item in [x.strip() for x in m.group(5).split(';') if x.strip()]:
            k, v = item.split()
            pairs.append('%s=%d' % (k, int(v.rstrip('u'))))
        misses.add('%s %s' % (name[:-5], ','.join(pairs)))
    print('%d distinct run-time variants in %s (%d not matched to a GLSL file)' % (len(misses), a.log, unmatched))
    for path in a.exports:
        have = set()
        for line in open(path, encoding='utf-8', errors='replace'):
            parts = line.rstrip('\n').split(' ', 1)
            have.add('%s %s' % (parts[0], parts[1] if len(parts) > 1 else ''))
        hit = misses & have
        print('  %s: %d variants, holds %d of them (%.1f %%)' % (path, len(have), len(hit),
                                                              100.0 * len(hit) / max(1, len(misses))))
        if a.nearest:
            nearest(misses - hit, have)


def words(spec):
    return dict(item.split('=') for item in spec.split(',') if item)


def nearest(missing, have):
    by_stem = {}
    for v in have:
        stem, spec = v.split(' ', 1)
        by_stem.setdefault(stem, []).append(words(spec))
    tally = collections.Counter()
    for v in sorted(missing):
        stem, spec = v.split(' ', 1)
        want = words(spec)
        best = None
        for cand in by_stem.get(stem, []):
            diff = [(k, cand.get(k), want.get(k)) for k in sorted(set(cand) | set(want), key=int)
                    if cand.get(k) != want.get(k)]
            if best is None or len(diff) < len(best):
                best = diff
        if best is None:
            print('    %s: no listed variant of this shader' % stem)
            tally['(shader not listed)'] += 1
            continue
        print('    %s: %s' % (stem, '; '.join('%s %s -> %s' % d for d in best)))
        for k, have_v, want_v in best:
            tally['%s %s -> %s' % (k, have_v, want_v)] += 1
    print('  differing words over %d variants:' % len(missing))
    for k, n in tally.most_common(40):
        print('    %4d  %s' % (n, k))


if __name__ == '__main__':
    main()
