#!/usr/bin/env python3
"""banjo-nx (2026-11-20): a thread's sampled time per function, per draw, from a stack-sampled rex_profile.log and the
renut log of the same run: two builds on the same scene compared function by function in absolute terms (shares alone
hide a uniform slowdown).

Each window's samples of the thread (user PCs and kernel waits) are its wall time (the window's own length, "==== 10.0
s"); a function's time is its PCs' share of those samples times that length, summed over the windows from --from on.
The draws are the renut log's 10 s lines ("[renut] 10.0 s: N game frames (F fps) ...; D draws") for the same windows.

The two logs are aligned by the clock (2026-11-20, second version): the profile's windows start at the process's start,
which is the renut log's first line (its time stamp), and each renut line covers the seconds it names before its own stamp; a
window takes each line's draws in proportion to the part of the line's span inside the window. --skew S moves the
profile's start S seconds later against the log's first line.

  python tools/profile_per_draw.py PROFILE ELF RENUT_LOG [PROFILE ELF RENUT_LOG] [--from 70] [--thread 0x2C/0x2] [--top 30]
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profile_threads as pt  # noqa: E402

LINE = re.compile(r'^\[(\d+)-(\d+)-(\d+) (\d+):(\d+):([0-9.]+)\].*?\[renut\] (\d+\.\d) s: (\d+) game frames .*?; (\d+) draws')
STAMP = re.compile(r'^\[(\d+)-(\d+)-(\d+) (\d+):(\d+):([0-9.]+)\]')
WIN_LEN = re.compile(r'^==== ([0-9.]+) s \| mode: \w+ \| game ([0-9.]+) fps')


def windows_of(profile, table, key):
    """[(seconds, fps, Counter of microseconds by function)] for the thread, one per window."""
    out = []
    cur = None
    samples = 0
    for line in open(profile, encoding='utf-8', errors='replace'):
        m = WIN_LEN.match(line)
        if m:
            out.append((float(m.group(1)), float(m.group(2)), collections.Counter()))
            cur = None
            samples = 0
            continue
        if not out:
            continue
        m = pt.THREAD.match(line)
        if m:
            k = '%s/%s' % (m.group(4).upper().replace('0X', '0x'), m.group(6).lower())
            cur = k == key
            continue
        if not cur:
            continue
        m = pt.SAMPLES.match(line)
        if m:
            samples = int(m.group(1))
            continue
        m = pt.PC.match(line)
        if m and samples:
            name = re.sub(r' \[clone [^]]*\]', '', pt.name_of(table, int(m.group(1), 16)))  # LTO's clones
            secs, _, funcs = out[-1]
            funcs[name] += int(m.group(2)) / samples * secs * 1e6
    return out


def stamp_seconds(m):
    """Seconds of the day (a run crossing midnight gains a day)."""
    return int(m.group(4)) * 3600 + int(m.group(5)) * 60 + float(m.group(6))


def lines_of(log):
    """[(start, end, frames, draws)] per renut 10 s line, in seconds from the log's first stamp."""
    out = []
    first = None
    day = 0.0
    last = None
    for line in open(log, encoding='utf-8', errors='replace'):
        m = STAMP.match(line)
        if not m:
            continue
        t = stamp_seconds(m)
        if first is None:
            first = t
        if last is not None and t + day < last - 3600:
            day += 86400
        last = t + day
        m = LINE.match(line)
        if m:
            secs = float(m.group(7))
            end = t + day - first
            out.append((end - secs, end, int(m.group(8)), int(m.group(9))))
    return out


def run(profile, elf, log, t0, key, skew):
    table = pt.symbols(elf)
    wins = windows_of(profile, table, key)
    lines = lines_of(log)
    funcs = collections.Counter()
    draws = frames = 0.0
    used = 0
    t = skew
    for secs, _, f in wins:
        start, end = t, t + secs
        t = end
        if start < t0 + skew or secs <= 0:
            continue
        got = False
        for a, b, lframes, ldraws in lines:
            part = min(b, end) - max(a, start)
            if part > 0:
                draws += ldraws * part / (b - a)
                frames += lframes * part / (b - a)
                got = True
        if not got:
            continue  # past the log's end (the log stopped first)
        funcs.update(f)
        used += 1
    return funcs, used, draws, frames, len(wins), len(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--from', dest='t0', type=float, default=70)
    ap.add_argument('--thread', default='0x2C/0x2')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--skew', type=float, default=0.0, help="the profile's start this many seconds after the log's first line")
    a = ap.parse_args()
    runs = []
    for i in range(0, len(a.files), 3):
        prof, elf, log = a.files[i:i + 3]
        funcs, used, draws, frames, nwins, nlines = run(prof, elf, log, a.t0, a.thread, a.skew)
        runs.append((prof, funcs, used, draws))
        total = sum(funcs.values())
        print('== %s: %d of %d windows from %.0f s against %d renut lines: %.0f draws (%.0f frames, %.1f draws a '
              'frame); user code %.2f us a draw' % (prof, used, nwins, a.t0, nlines, draws, frames,
                                                    draws / max(1, frames), total / max(1, draws)))
    names = collections.Counter()
    for _, funcs, _, draws in runs:
        for f, us in funcs.items():
            names[f] = max(names[f], us / max(1, draws))
    print('%-70s ' % 'us a draw' + ' '.join('%10s' % ('run %d' % (i + 1)) for i in range(len(runs))))
    for f, _ in names.most_common(a.top):
        print('%-70s ' % f[:70] + ' '.join('%10.3f' % (r[1].get(f, 0) / max(1, r[3])) for r in runs))


if __name__ == '__main__':
    main()
