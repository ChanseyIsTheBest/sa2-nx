import re
import sys

# Frame-weighted render-thread ms of the on / off halves of "[renut A/B]" lines (renut.log), all periods and the busy
# ones (both halves above a threshold), per target. Usage: ab_sum.py renut.log [busy_ms]
pat = re.compile(r'\[renut A/B\] .*?\(target (\d+)[^)]*\): on ([0-9.]+) fps, render thread ([0-9.]+) ms a frame, GPU '
                 r'[0-9.]+ ms a frame \(([0-9.]+) s\); off ([0-9.]+) fps, render thread ([0-9.]+) ms a frame, GPU [0-9.]+ ms '
                 r'a frame \(([0-9.]+) s\)')
busy = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
acc = {}
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = pat.search(line)
    if not m:
        continue
    t = int(m.group(1))
    fps_on, ms_on, s_on, fps_off, ms_off, s_off = map(float, m.groups()[1:])
    f_on, f_off = fps_on * s_on, fps_off * s_off
    a = acc.setdefault(t, {'all': [0, 0, 0, 0, 0], 'busy': [0, 0, 0, 0, 0]})
    for key in ('all',) + (('busy',) if ms_on > busy and ms_off > busy else ()):
        v = a[key]
        v[0] += ms_on * f_on
        v[1] += f_on
        v[2] += ms_off * f_off
        v[3] += f_off
        v[4] += 1
for t, a in sorted(acc.items()):
    for key in ('all', 'busy'):
        v = a[key]
        if v[1] and v[3]:
            on, off = v[0] / v[1], v[2] / v[3]
            print('target %d %-4s: %3d periods, on %.2f ms (%d frames), off %.2f ms (%d frames): %+.1f %%' %
                  (t, key, v[4], on, v[1], off, v[3], 100.0 * (on - off) / off))
