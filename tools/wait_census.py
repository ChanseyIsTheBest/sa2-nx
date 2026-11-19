#!/usr/bin/env python3
"""What the game's threads wait on, from a console profile's sampled stacks (blur-nx, 2026-11-17; any ReXGlue port on
the Switch with the profiler's stack sampling, switch_profile_stacks_ms).

The profiler (rex_profile.log) lists, per window and thread, the sampled stacks with their share, those of a waiting
thread marked `wait`. This sums, over windows FROM..TO (a race's racing stretch: the menus wait differently), each
thread's waiting share by where it waits: the first frame that is not a lock or wait primitive (pthread, mutex,
condition, yield, sleep, svc...), and the primitive itself. It found Blur's frame-job workers waiting ~11 % of their
time on Renut's record mutex (NewOp), which a per-thread view of the busy time never shows.

  python tools/wait_census.py ELF PROFILE [PROFILE ...] [--from 9 --to 20] [--threads "Game Master|JQ Worker 0"]
        [--top 8]
Threads: a regular expression over the thread names (default: every thread with waits).
"""
import argparse
import bisect
import collections
import re
import subprocess

DKP = 'C:/devkitPro/devkitA64/bin/aarch64-none-elf-'
WIN = re.compile(r'^==== ')
THREAD = re.compile(r'^-- thread \d+ "([^"]*)"')
STACK = re.compile(r'^\s+([0-9.]+)%\s+(stack|wait) (image\+0x[0-9a-f]+(?: <- image\+0x[0-9a-f]+)*)')
PRIMITIVE = re.compile(r'pthread|mutex|Mutex|Wait|wait|svc|Condition|condvar|cond|futex|Arbitrate|Sleep|sleep|yield|Yield'
                       r'|lock')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf')
    ap.add_argument('profiles', nargs='+')
    ap.add_argument('--from', dest='frm', type=int, default=9)
    ap.add_argument('--to', type=int, default=20)
    ap.add_argument('--threads', default='.')
    ap.add_argument('--top', type=int, default=8)
    a = ap.parse_args()
    starts, syms = [], []
    out = subprocess.run([DKP + 'nm', '-n', '--defined-only', a.elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'TtWw':
            starts.append(int(p[0], 16))
            syms.append(p[2])
    dem = subprocess.run([DKP + 'c++filt'], input='\n'.join(syms), capture_output=True, text=True).stdout.split('\n')

    def sym(addr):
        i = bisect.bisect_right(starts, addr) - 1
        s = dem[i].replace('(anonymous namespace)', 'anon') if i >= 0 else '?'
        s = re.sub(r'\(.*', '', s)
        for _ in range(3):
            s = re.sub(r'<[^<>]*>', '', s)
        return s[:56]

    want = re.compile(a.threads)
    waits = collections.Counter()
    seen = collections.Counter()
    for prof in a.profiles:
        win, thread = -1, None
        for line in open(prof, encoding='utf-8', errors='replace'):
            if WIN.match(line):
                win += 1
                continue
            if not a.frm <= win <= a.to:
                continue
            m = THREAD.match(line)
            if m:
                thread = m.group(1)
                seen[thread] += 1
                continue
            m = STACK.match(line)
            if not m or m.group(2) != 'wait' or not thread or not want.search(thread):
                continue
            frames = [sym(int(x.split('+0x')[1], 16)) for x in m.group(3).split(' <- ')]
            k = 0
            while k < len(frames) - 1 and PRIMITIVE.search(frames[k]):
                k += 1
            waits[(thread, frames[0], frames[k])] += float(m.group(1)) / 100.0
    rows = collections.defaultdict(list)
    for (t, prim, site), w in waits.items():
        rows[t].append((100.0 * w / seen[t], prim, site))
    for t in sorted(rows, key=lambda t: -sum(r[0] for r in rows[t])):
        print('== %s: %.1f %% of its sampled stacks waiting' % (t, sum(r[0] for r in rows[t])))
        for share, prim, site in sorted(rows[t], reverse=True)[:a.top]:
            print('   %5.1f %%  %-30s at %s' % (share, prim, site))


if __name__ == '__main__':
    main()
