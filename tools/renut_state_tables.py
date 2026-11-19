#!/usr/bin/env python3
"""katamari-nx (2026-10-30): finds Beautiful Katamari's Direct3D render and sampler state tables (records of {getter,
setter, default}, 12 bytes each, that D3D_InitializeApiState copies into the device) by N&B's defaults, and pairs the
two games' getters and setters by index (feedingfrenzy2-nx's method: "found by Peggle 1's defaults at stride 12").

  python tools/renut_state_tables.py --nb-image <banjo-nx out/renut_image.bin> --image out/renut_image.bin
         [--out out/renut_map/state_tables.txt]

Output lines: `render|sampler <index> get|set <N&B address> <this game's address>`, then the alignment it used.
"""
import argparse
import os
import struct

BASE = 0x82000000
NB_RENDER, NB_RENDER_N = 0x82D8A948, 101   # app/src/renut_engine/gpu/d3d/d3d_native_state.cpp (title update 3)
NB_SAMPLER, NB_SAMPLER_N = 0x82D8AE28, 20


def words(img, addr, n):
    return struct.unpack_from('>%dI' % n, img, addr - BASE)


def table(img, addr, n):
    w = words(img, addr, 3 * n)
    return [(w[3 * i], w[3 * i + 1], w[3 * i + 2]) for i in range(n)]


def is_code(v):
    return 0x82000000 <= v < 0x83000000 and not v & 3


def find(img, nb, min_run=8):
    """Every place where at least min_run consecutive records look like {code, code, default} with N&B's defaults
    in order (allowing states one game has and the other lacks)."""
    defaults = [d for _, _, d in nb]
    hits = []
    for off in range(0, len(img) - 12 * min_run, 4):
        g, s, d = struct.unpack_from('>III', img, off)
        if not (is_code(g) and is_code(s)) or d != defaults[0]:
            continue
        run, i = 0, 0
        p = off
        while p + 12 <= len(img) and i < len(defaults):
            g, s, d = struct.unpack_from('>III', img, p)
            if not (is_code(g) and is_code(s)):
                break
            if d == defaults[i]:
                run += 1
            i += 1
            p += 12
        if run >= min_run:
            hits.append((BASE + off, run, (p - off) // 12))
    return hits


def align(nb, game):
    """Pairs N&B's records with this game's by default value in order (a small edit distance on the defaults)."""
    a = [d for _, _, d in nb]
    b = [d for _, _, d in game]
    n, m = len(a), len(b)
    dp = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        for j in range(m - 1, -1, -1):
            dp[i][j] = max(dp[i + 1][j], dp[i][j + 1], dp[i + 1][j + 1] + (1 if a[i] == b[j] else 0))
    pairs, i, j = [], 0, 0
    while i < n and j < m:
        if a[i] == b[j] and dp[i][j] == dp[i + 1][j + 1] + 1:
            pairs.append((i, j))
            i += 1
            j += 1
        elif dp[i + 1][j] >= dp[i][j + 1]:
            i += 1
        else:
            j += 1
    return pairs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--nb-image', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--out', default=None)
    # renut-ultimate (port 69, 2026-11-10, blur-nx): another reference than N&B's title update 3, such as the base's own
    # (Katamari: --ref-render 82AD8E48 --ref-sampler 82AD9308, docs/katamari-notes.md); the same counts.
    ap.add_argument('--ref-render', type=lambda v: int(v, 16), default=NB_RENDER)
    ap.add_argument('--ref-sampler', type=lambda v: int(v, 16), default=NB_SAMPLER)
    a = ap.parse_args()
    nbimg = open(a.nb_image, 'rb').read()
    img = open(a.image, 'rb').read()
    out = []
    for kind, addr, n in (('render', a.ref_render, NB_RENDER_N), ('sampler', a.ref_sampler, NB_SAMPLER_N)):
        nb = table(nbimg, addr, n)
        hits = find(img, nb, 8 if kind == 'render' else 6)
        print(f'{kind}: N&B 0x{addr:08X} x{n}; candidates', [(f'0x{h:08X}', r, l) for h, r, l in hits[:6]])
        if not hits:
            continue
        start, run, length = max(hits, key=lambda h: h[1])
        # The table runs while records look like {code, code, value}.
        cnt = 0
        while cnt < n + 16:
            g, s, d = struct.unpack_from('>III', img, start - BASE + 12 * (cnt))
            if not (is_code(g) and is_code(s)):
                break
            cnt += 1
        game = table(img, start, cnt)
        pairs = align(nb, game)
        print(f'{kind}: this game 0x{start:08X} x{cnt}, {len(pairs)} records paired by default')
        paired_nb = {i for i, _ in pairs}
        paired_game = {j for _, j in pairs}
        for i, j in pairs:
            out.append(f'{kind} {i} get {nb[i][0]:08X} {game[j][0]:08X} {j}')
            out.append(f'{kind} {i} set {nb[i][1]:08X} {game[j][1]:08X} {j}')
        for i in range(n):
            if i not in paired_nb:
                out.append(f'# {kind} N&B {i} (default 0x{nb[i][2]:08X}, set {nb[i][1]:08X}) has no partner')
        for j in range(cnt):
            if j not in paired_game:
                out.append(f'# {kind} this game {j} (default 0x{game[j][2]:08X}, set {game[j][1]:08X}) has no partner')
    if a.out:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        open(a.out, 'w').write('\n'.join(out) + '\n')
    print('\n'.join(l for l in out if l.startswith('#')))


if __name__ == '__main__':
    main()
