#!/usr/bin/env python3
"""banjo-nx (2026-11-20): a thread's sampled time per draw by SOURCE function, inlined ones included: two builds that
PGO and LTO inlined differently compared function by function (a name in the symbol table hides everything inlined
into it: renut-nx 1.0.2's ConvertVertices was inside FlushPending). Needs an ELF with DWARF (-g; the ultimate build:
BANJO_SWITCH_RENUT_DEBUG_INFO=ON, which does not change the code).

Each sampled PC of the thread is named with addr2line -f -i -C: the innermost function (the source function whose code
the PC is in) and the outermost (the symbol it was inlined into). The time is the PC's share of the window's samples
times the window's length (tools/profile_per_draw.py's windows and its clock alignment with the renut log), divided by
the draws of the same span.

  python tools/profile_inline.py PROFILE ELF RENUT_LOG [PROFILE ELF RENUT_LOG] [--from 70] [--thread 0x2C/0x2]
         [--top 40] [--by inner|outer|file] [--to SECONDS]
"""
import argparse
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import profile_threads as pt  # noqa: E402
import profile_per_draw as ppd  # noqa: E402

ADDR2LINE = r'C:\devkitPro\devkitA64\bin\aarch64-none-elf-addr2line.exe'


def pcs_of(profile, key, t0, t1):
    """{pc: microseconds} over the windows from t0 to t1 (seconds from the profile's start), and the windows' span."""
    out = collections.Counter()
    cur = None
    samples = 0
    t = 0.0
    secs = 0.0
    span = []
    for line in open(profile, encoding='utf-8', errors='replace'):
        m = ppd.WIN_LEN.match(line)
        if m:
            secs = float(m.group(1))
            start = t
            t += secs
            use = start >= t0 and (t1 is None or t <= t1)
            if use:
                span.append((start, t))
            cur = None
            samples = 0
            continue
        if not span or span[-1][1] != t:
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
            out[int(m.group(1), 16)] += int(m.group(2)) / samples * secs * 1e6
    return out, span


def names(elf, pcs):
    """{pc: [(function, file:line), ...]} innermost first."""
    pcs = sorted(pcs)
    text = '\n'.join('0x%x' % p for p in pcs) + '\n'
    r = subprocess.run([ADDR2LINE, '-f', '-i', '-C', '-a', '-e', elf], input=text, capture_output=True, text=True,
                       errors='replace')
    out = {}
    cur = None
    lines = r.stdout.splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith('0x'):
            cur = int(ln, 16)
            out[cur] = []
            i += 1
            continue
        fn = ln
        loc = lines[i + 1] if i + 1 < len(lines) else '??:0'
        out[cur].append((fn, loc))
        i += 2
    return out


def short(fn):
    fn = re.sub(r'\(anonymous namespace\)::', '', fn)
    fn = re.sub(r' \[clone [^]]*\]', '', fn)
    return re.sub(r'\(.*$', '', fn)


def draws_over(log, span):
    lines = ppd.lines_of(log)
    draws = frames = 0.0
    for start, end in span:
        for a, b, lframes, ldraws in lines:
            part = min(b, end) - max(a, start)
            if part > 0:
                draws += ldraws * part / (b - a)
                frames += lframes * part / (b - a)
    return draws, frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--from', dest='t0', type=float, default=70)
    ap.add_argument('--to', dest='t1', type=float, default=None)
    ap.add_argument('--thread', default='0x2C/0x2')
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--by', choices=('inner', 'outer', 'file'), default='inner')
    a = ap.parse_args()
    runs = []
    for i in range(0, len(a.files), 3):
        prof, elf, log = a.files[i:i + 3]
        pcs, span = pcs_of(prof, a.thread, a.t0, a.t1)
        named = names(elf, pcs)
        draws, frames = draws_over(log, span)
        acc = collections.Counter()
        for pc, us in pcs.items():
            chain = named.get(pc) or [('??', '??:0')]
            if a.by == 'inner':
                k = short(chain[0][0])
            elif a.by == 'outer':
                k = short(chain[-1][0])
            else:
                k = re.sub(r':\d+.*$', '', chain[0][1]).replace('\\', '/').split('/')[-1]
            acc[k] += us
        total = sum(acc.values())
        print('== %s: %d windows, %.0f draws (%.0f frames, %.0f a frame); %.2f us a draw of user code' % (
            prof, len(span), draws, frames, draws / max(1, frames), total / max(1, draws)))
        runs.append((acc, draws))
    keys = collections.Counter()
    for acc, draws in runs:
        for k, us in acc.items():
            keys[k] = max(keys[k], us / max(1, draws))
    print('%-64s ' % ('us a draw by ' + a.by) + ' '.join('%9s' % ('run %d' % (i + 1)) for i in range(len(runs))))
    for k, _ in keys.most_common(a.top):
        print('%-64s ' % k[:64] + ' '.join('%9.3f' % (acc.get(k, 0) / max(1, d)) for acc, d in runs))


if __name__ == '__main__':
    main()
