#!/usr/bin/env python3
"""banjo-nx (2026-11-20): the "[renut A/B]" lines of a renut log summed per target, both halves weighted by their
frames: fps, render thread ms a frame, GPU ms a frame, CPU ms a frame (all threads, when logged), draws a frame, and
the render thread's and the GPU's microseconds a draw (the halves may catch different scenes; per draw evens that out
in part). tools/ab_sum.py has the render thread only.

  python tools/ab_halves.py renut_00N.log [--from SECONDS]   (--from: lines stamped that long after the log's first)
      [--pairs [TOL]]   only the lines whose halves had draws a frame within TOL of each other (default 0.15: the
                        same scene), split into heavy (500 draws a frame or more) and light lines
"""
import argparse
import re

HALF = (r'([0-9.]+) fps, render thread ([0-9.]+) ms a frame, GPU ([0-9.]+) ms a frame \(([0-9.]+) s\)'
        r'(?:, CPU ([0-9.]+) ms a frame \(all threads\))?(?:, ([0-9.]+) draws a frame, render thread [0-9.]+ us a draw)?')
LINE = re.compile(r'^\[[0-9-]+ (\d+):(\d+):([0-9.]+)\].*?\[renut A/B\] (.+?) \(target (\d+)[^)]*\): on ' + HALF +
                  r'; off ' + HALF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--from', dest='t0', type=float, default=0.0)
    ap.add_argument('--pairs', type=float, nargs='?', const=0.15, default=None)
    a = ap.parse_args()
    acc = {}
    names = {}
    first = None
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = LINE.match(line)
        if not m:
            continue
        t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))
        first = t if first is None else first
        if t - first < a.t0:
            continue
        target = int(m.group(5))
        if a.pairs is not None:
            d_on, d_off = m.group(11), m.group(17)
            if not d_on or not d_off or float(d_off) <= 0:
                continue
            d_on, d_off = float(d_on), float(d_off)
            if abs(d_on / d_off - 1.0) > a.pairs:
                continue
            target = (target, 'heavy' if (d_on + d_off) / 2 >= 500 else 'light')
        names[target] = m.group(4)
        v = acc.setdefault(target, {'on': [0.0] * 8, 'off': [0.0] * 8, 'n': 0})
        v['n'] += 1
        for half, base in (('on', 6), ('off', 12)):
            fps, rt, gpu, secs = (float(m.group(base + i)) for i in range(4))
            cpu = m.group(base + 4)
            draws = m.group(base + 5)
            frames = fps * secs
            if frames <= 0:
                continue
            s = v[half]
            s[0] += frames
            s[1] += secs
            s[2] += rt * frames
            s[3] += gpu * frames
            if cpu:
                s[4] += float(cpu) * frames
                s[5] += frames
            if draws:
                s[6] += float(draws) * frames
                s[7] += frames
    for target in sorted(acc):
        v = acc[target]
        print('target %s (%s), %d lines:' % (target, names[target], v['n']))
        rows = {}
        for half in ('on', 'off'):
            s = v[half]
            if not s[0]:
                continue
            fr = s[0]
            dpf = s[6] / s[7] if s[7] else 0.0
            rt, gpu = s[2] / fr, s[3] / fr
            rows[half] = (fr / s[1], rt, gpu, s[4] / s[5] if s[5] else 0.0, dpf,
                          1000.0 * rt / dpf if dpf else 0.0, 1000.0 * gpu / dpf if dpf else 0.0)
            print('  %-3s %6.2f fps | render %6.2f ms | GPU %6.2f ms | CPU %6.1f ms | %6.0f draws | render %5.2f us '
                  '| GPU %5.2f us a draw  (%.0f frames, %.0f s)' % ((half,) + rows[half] + (fr, s[1])))
        if 'on' in rows and 'off' in rows:
            on, off = rows['on'], rows['off']
            pct = lambda i: 100.0 * (on[i] - off[i]) / off[i] if off[i] else 0.0  # noqa: E731
            print('  on vs off: fps %+.1f %%, render %+.1f %%, GPU %+.1f %%, CPU %+.1f %%, draws %+.1f %%, '
                  'render/draw %+.1f %%, GPU/draw %+.1f %%' % tuple(pct(i) for i in range(7)))


if __name__ == '__main__':
    main()
