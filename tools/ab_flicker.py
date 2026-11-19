#!/usr/bin/env python3
"""Looks for a picture change at the moments an in-run A/B switch flips (banjo_direct_state_ab_s / banjo_ab_target).

A recording made while the switch alternates holds both states of the same scene. If the two states draw the same
picture, the difference between consecutive frames at a flip is like at any other moment (the scene's own motion).
If they do not, the frames at the flips stand out, every N seconds.

    python tools/ab_flicker.py app/out/pc-me/capturas/latest.mp4 --period 2 [--sheet out.png]

The flips are not located from the log: every phase of the period is tried, and the one whose frames differ most
is reported against the median over all phases (a ratio near 1 = the flips look like any other moment). Also printed:
the largest differences with their times. --sheet writes a contact sheet with the frame before and the frame after
each moment of that phase, side by side.
"""
import argparse
import re
import statistics
import subprocess
import sys

FFMPEG = r'C:\ffmpeg\ffmpeg.exe'
FFPROBE = r'C:\ffmpeg\ffprobe.exe'


def frame_differences(path):
    """Average luma (0..255) of |frame n - frame n-1| for every frame, and the frame rate."""
    rate = subprocess.run([FFPROBE, '-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=r_frame_rate',
                           '-of', 'csv=p=0', path], capture_output=True, text=True).stdout.strip()
    num, den = (rate.split('/') + ['1'])[:2]
    fps = float(num) / float(den)
    out = subprocess.run([FFMPEG, '-hide_banner', '-i', path, '-vf',
                          'tblend=all_mode=difference,signalstats,metadata=mode=print:key=lavfi.signalstats.YAVG:file=-',
                          '-an', '-f', 'null', '-'], capture_output=True, text=True)
    values = [float(m) for m in re.findall(r'lavfi\.signalstats\.YAVG=([0-9.]+)', out.stdout)]
    return values, fps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('--period', type=float, default=2.0, help='seconds between flips')
    ap.add_argument('--sheet')
    ap.add_argument('--top', type=int, default=8)
    ap.add_argument('--log', help="the run's log: the flips are then taken from its '[native] A/B: target' lines and "
                                  "the recording's start from its '[captura] recording' line, instead of guessed")
    args = ap.parse_args()

    diffs, fps = frame_differences(args.video)
    if len(diffs) < 10:
        sys.exit('no frames read')
    moving = [d for d in diffs if d > 0.0005]  # a 60 fps recording of a 30 fps game repeats frames
    median = statistics.median(moving) if moving else 0.0
    p90 = sorted(moving)[int(len(moving) * 0.9)] if moving else 0.0
    print(f'{len(diffs)} frames at {fps:g} fps; {len(moving)} with motion; frame difference (luma, 0-255): '
          f'median {median:.3f}, 90th percentile {p90:.3f}, maximum {max(diffs):.3f}')

    period = max(1, round(args.period * fps))
    # A flip lands on one frame; the game frame it shows may be the next recorded one: take the larger of a window.
    window = max(1, round(fps / 15))
    best = None
    for phase in range(period):
        at = []
        for start in range(phase, len(diffs) - window, period):
            at.append(max(diffs[start:start + window + 1]))
        if len(at) < 2:
            continue
        mean = sum(at) / len(at)
        if best is None or mean > best[0]:
            best = (mean, phase, at)
    mean, phase, at = best
    # The same statistic for every phase, to say how unusual the best one is.
    all_means = []
    for ph in range(period):
        v = [max(diffs[s:s + window + 1]) for s in range(ph, len(diffs) - window, period)]
        if len(v) >= 2:
            all_means.append(sum(v) / len(v))
    typical = statistics.median(all_means)
    print(f'period {args.period:g} s = {period} frames: the phase with the largest differences is {phase / fps:.2f} s, '
          f'mean {mean:.3f} over {len(at)} flips; the median over all phases is {typical:.3f} '
          f'(ratio {mean / typical if typical else 0:.2f})')
    print('   at that phase, per flip: ' + ' '.join(f'{v:.2f}' for v in at))
    order = sorted(range(len(diffs)), key=lambda i: -diffs[i])[:args.top]
    print('largest differences: ' + ', '.join(f'{i / fps:.2f} s ({diffs[i]:.2f})' for i in sorted(order)))

    if args.log:
        # The real flips. A flip is made at a Swap; the frame that shows it is recorded up to a few frames later.
        import datetime

        def stamp(line):
            return datetime.datetime.strptime(line[1:24], '%Y-%m-%d %H:%M:%S.%f')

        start, flips = None, []
        for line in open(args.log, encoding='utf-8', errors='replace'):
            if '[captura] recording ' in line and ' to ' in line and start is None:
                start = stamp(line)
            elif '[native] A/B: target' in line:
                flips.append(stamp(line))
        if start is None or not flips:
            sys.exit('--log: no recording start or no flips in the log')
        after = max(2, round(fps / 6))  # 0.17 s
        at_flips = []
        covered = set()
        for f in flips:
            k = round((f - start).total_seconds() * fps)
            if 1 <= k < len(diffs) - after:
                at_flips.append((k, max(diffs[k - 1:k + after])))
                covered.update(range(k - 1, k + after))
        elsewhere = [max(diffs[i:i + after + 1]) for i in range(0, len(diffs) - after) if i not in covered]
        elsewhere.sort()
        e_median = elsewhere[len(elsewhere) // 2]
        e_p90 = elsewhere[int(len(elsewhere) * 0.9)]
        print(f'from the log: {len(at_flips)} flips inside the recording; largest frame difference within '
              f'{after} frames of each: ' + ' '.join(f'{v:.2f}' for _, v in at_flips))
        print(f'   the same window anywhere else: median {e_median:.2f}, 90th percentile {e_p90:.2f}; flips above '
              f'that 90th percentile: {sum(v > e_p90 for _, v in at_flips)} of {len(at_flips)} (10 % expected by '
              f'chance)')

    if args.sheet:
        # Before / after pairs at the best phase.
        selects = []
        for start in range(phase, len(diffs) - window, period):
            peak = max(range(start, start + window + 1), key=lambda i: diffs[i])
            # diffs[k] compares frame k+1 with frame k (tblend drops the first frame).
            selects += [peak, peak + 1]
        expr = '+'.join(f'eq(n\\,{n})' for n in selects[:16])
        cols = 2
        rows = max(1, len(selects[:16]) // 2)
        subprocess.run([FFMPEG, '-y', '-loglevel', 'error', '-i', args.video, '-vf',
                        f"select='{expr}',scale=640:360,tile={cols}x{rows}", '-frames:v', '1', '-vsync', '0',
                        args.sheet])
        print(f'sheet: {args.sheet} (left: before the flip, right: after; {rows} flips)')


if __name__ == '__main__':
    main()
