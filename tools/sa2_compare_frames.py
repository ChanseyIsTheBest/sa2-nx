#!/usr/bin/env python3
"""sa2-nx's lockstep frame comparison (the gate of every change to the code generation and the renderer).

Two recordings of sa2_video_s (src/sa2_capture.cpp): each has video_<s>s.hash (the XXH3-64 of every frame at full
resolution, RGB, with "Press START" and the loading spinner zeroed), .frames (the game frame of each, UINT64_MAX when
unknown) and .rgba / .txt (the frames at half resolution). This pairs the two runs' frames by game frame and counts the
identical ones; with --out it writes the first differing pairs side by side (A | B | 4x difference) from the .rgba.

Two runs of one build differ only in frames whose picture depends on wall-clock time (sa2-nx: the loading screen's
53 frames, its ring and Omochao animated on the loading thread's timing). A change is exact when it adds no other.

  python tools/sa2_compare_frames.py <capture folder or .hash of A> <... of B> [--name video_20s] [--out <folder>]
         [--max-pictures 8] [--labels]
"""
import argparse
import glob
import os
import struct
import sys

UNKNOWN = 0xFFFFFFFFFFFFFFFF


def resolve(path, name):
    if os.path.isdir(path):
        if name:
            base = os.path.join(path, name)
        else:
            hashes = sorted(glob.glob(os.path.join(path, 'video_*s.hash')))
            if len(hashes) != 1:
                sys.exit(f'{path}: {len(hashes)} recordings, name one with --name')
            base = hashes[0][:-5]
    else:
        base = os.path.splitext(path)[0]
    return base


def load(base):
    hashes = open(base + '.hash', 'rb').read()
    labels = open(base + '.frames', 'rb').read()
    n = min(len(hashes), len(labels)) // 8
    h = struct.unpack(f'<{n}Q', hashes[:n * 8])
    lab = struct.unpack(f'<{n}Q', labels[:n * 8])
    return h, lab


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--name', default='')
    ap.add_argument('--out', default='')
    ap.add_argument('--max-pictures', type=int, default=8)
    ap.add_argument('--labels', action='store_true', help='print every differing game frame')
    args = ap.parse_args()
    base_a, base_b = resolve(args.a, args.name), resolve(args.b, args.name)
    ha, la = load(base_a)
    hb, lb = load(base_b)
    by_label_b = {}
    for i, (h, lab) in enumerate(zip(hb, lb)):
        if lab != UNKNOWN:
            by_label_b.setdefault(lab, (h, i))
    same, differ, missing, unknown = 0, [], 0, 0
    for i, (h, lab) in enumerate(zip(ha, la)):
        if lab == UNKNOWN:
            unknown += 1
            continue
        other = by_label_b.get(lab)
        if other is None:
            missing += 1
            continue
        if other[0] == h:
            same += 1
        else:
            differ.append((lab, i, other[1]))
    pairs = same + len(differ)
    print(f'A: {len(ha)} frames ({base_a}); B: {len(hb)} frames ({base_b})')
    print(f'{pairs} frames in both by game frame: {same} identical, {len(differ)} differ; '
          f'{missing} of A not in B, {unknown} of A without a label')
    if differ:
        runs, start, prev = [], differ[0][0], differ[0][0]
        for lab, _, _ in differ[1:]:
            if lab != prev + 1:
                runs.append((start, prev))
                start = lab
            prev = lab
        runs.append((start, prev))
        print('differing game frames: ' + ', '.join(f'{s}' if s == e else f'{s}-{e}' for s, e in runs[:40]) +
              (' ...' if len(runs) > 40 else ''))
        if args.labels:
            for lab, i, j in differ:
                print(f'  frame {lab}: A #{i}, B #{j}')
    if args.out and differ:
        try:
            import numpy as np
            from PIL import Image
        except ImportError:
            sys.exit('--out needs numpy and Pillow')
        os.makedirs(args.out, exist_ok=True)

        def frames(base):
            w, h, _fps, _count = open(base + '.txt').read().split()[:4]
            w, h = int(w), int(h)
            n = os.path.getsize(base + '.rgba') // (w * h * 4)
            return np.memmap(base + '.rgba', np.uint8, 'r', shape=(n, h, w, 4))

        fa, fb = frames(base_a), frames(base_b)
        for lab, i, j in differ[:args.max_pictures]:
            if i >= len(fa) or j >= len(fb):
                continue
            a = fa[i][..., :3].astype(np.int16)
            b = fb[j][..., :3].astype(np.int16)
            d = np.clip(np.abs(a - b) * 4, 0, 255).astype(np.uint8)
            row = np.concatenate([a.astype(np.uint8), b.astype(np.uint8), d], axis=1)
            Image.fromarray(row).save(os.path.join(args.out, f'frame_{lab}.png'))
        print(f'pictures of the first {min(len(differ), args.max_pictures)} differing frames in {args.out}')


if __name__ == '__main__':
    main()
