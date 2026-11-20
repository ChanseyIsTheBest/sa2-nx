#!/usr/bin/env python3
"""banjo-nx (2026-11-20): a rex_profile.log with stack sampling (switch_profile_stacks) by thread and function.

Sums every window's "pc image+0x... xN" samples per thread (keyed by priority and core mask, so the render thread,
0x2C on core 1, lines up between two builds whose thread ids differ), names them from the NRO's ELF (nm), and prints
each thread's share of the run's samples and its top functions. With two (PROFILE ELF) pairs it prints both side by
side for the threads they share.

  python tools/profile_threads.py PROFILE ELF [--from S] [--to S] [--top 25] [--thread 0x2C/0x2]
  python tools/profile_threads.py PROFILE_A ELF_A PROFILE_B ELF_B --thread 0x2C/0x2
"""
import argparse
import bisect
import collections
import os
import re
import subprocess

NM = next((p for p in (os.path.join(os.environ.get('DEVKITPRO', ''), 'devkitA64', 'bin', 'aarch64-none-elf-nm.exe'),
                        'C:/devkitPro/devkitA64/bin/aarch64-none-elf-nm.exe') if os.path.isfile(p)),
          'aarch64-none-elf-nm')
WIN = re.compile(r'^==== ([0-9.]+) s \| mode: (\w+) \| game ([0-9.]+) fps')
THREAD = re.compile(r'^-- thread (\d+) "([^"]*)": CPU ([0-9.]+)% \| priority (0x[0-9A-Fa-f]+), preferred core (-?\d+), '
                    r'mask (0x[0-9A-Fa-f]+)')
SAMPLES = re.compile(r'^\s+(\d+) samples, ([0-9.]+)% waiting in the kernel')
PC = re.compile(r'^\s+[0-9.]+%\s+pc image\+0x([0-9a-f]+) x(\d+)')


def symbols(elf):
    out = subprocess.run([NM, '-C', '-n', '--defined-only', elf], capture_output=True, text=True,
                         errors='replace').stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'TtWw':
            addrs.append(int(parts[0], 16))
            names.append(parts[2])
    return addrs, names


def name_of(table, off):
    addrs, names = table
    i = bisect.bisect_right(addrs, off) - 1
    return names[i] if i >= 0 else '?'


def load(path, t0, t1):
    """{thread key: {'name', 'cpu': [..], 'samples', 'kernel', 'pcs': Counter}}; windows numbered by order (the
    header's time is each window's length: the run time is the window count x 10 s)."""
    threads = {}
    cur = None
    win = -1
    for line in open(path, encoding='utf-8', errors='replace'):
        m = WIN.match(line)
        if m:
            win += 1
            cur = None
            continue
        t = win * 10.0
        if t < t0 or t > t1:
            continue
        m = THREAD.match(line)
        if m:
            key = '%s/%s' % (m.group(4).upper().replace('0X', '0x'), m.group(6).lower())
            if m.group(2) not in ('?', ''):
                key += ' ' + re.sub(r'[0-9A-F]{8}', '', m.group(2)).strip()
            th = threads.setdefault(key, {'names': collections.Counter(), 'cpu': [], 'samples': 0, 'kernel': 0.0,
                                          'pcs': collections.Counter()})
            th['names'][m.group(2)] += 1
            th['cpu'].append(float(m.group(3)))
            cur = th
            continue
        if cur is None:
            continue
        m = SAMPLES.match(line)
        if m:
            n = int(m.group(1))
            cur['samples'] += n
            cur['kernel'] += n * float(m.group(2)) / 100
            continue
        m = PC.match(line)
        if m:
            cur['pcs'][int(m.group(1), 16)] += int(m.group(2))
    return threads


def by_function(th, table):
    funcs = collections.Counter()
    for off, n in th['pcs'].items():
        funcs[name_of(table, off)] += n
    return funcs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--from', dest='t0', type=float, default=0)
    ap.add_argument('--to', dest='t1', type=float, default=1e9)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--thread', default=None, help='a thread key such as 0x2C/0x2 (priority/mask)')
    a = ap.parse_args()
    pairs = list(zip(a.files[0::2], a.files[1::2]))
    runs = []
    for prof, elf in pairs:
        table = symbols(elf)
        runs.append((prof, load(prof, a.t0, a.t1), table))
    for prof, threads, table in runs:
        print('== %s' % prof)
        for key, th in sorted(threads.items(), key=lambda kv: -sum(kv[1]['cpu']) / max(1, len(kv[1]['cpu']))):
            mean = sum(th['cpu']) / max(1, len(th['cpu']))
            if mean < 2:
                continue
            print('  %-34s CPU %5.1f %% mean over %d windows; %d samples, %.0f %% in the kernel; %s' % (
                key, mean, len(th['cpu']), th['samples'], 100 * th['kernel'] / max(1, th['samples']),
                th['names'].most_common(1)[0][0]))
    if a.thread:
        print()
        cols = []
        for prof, threads, table in runs:
            th = threads.get(a.thread)
            if not th:
                cols.append((prof, []))
                continue
            funcs = by_function(th, table)
            total = sum(funcs.values())
            cols.append((prof, [(n * 100.0 / total, f) for f, n in funcs.most_common(a.top)]))
        for prof, rows in cols:
            print('== %s, thread %s: top functions (share of its sampled pcs)' % (prof, a.thread))
            for share, f in rows:
                print('  %5.1f %%  %s' % (share, f[:150]))


if __name__ == '__main__':
    main()
