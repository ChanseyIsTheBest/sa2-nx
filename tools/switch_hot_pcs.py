#!/usr/bin/env python3
"""katamari-nx (2026-11-01): the hot instructions inside one function of a console profile (logs/rex/rex_profile.log), by
10-s window, with the disassembly of the hottest windows: what says whether a hand-written native can remove the cost
(docs/optimising-functions.md, "Reading the samples"). Needs the ELF of the SAME build and devkitA64's nm and objdump.

  python tools/switch_hot_pcs.py PROFILE ELF "symbol substring" "thread name" from_report [windows=12] [win_bytes=64]
"""
import bisect
import collections
import re
import subprocess
import sys

BIN = 'C:\\devkitPro\\devkitA64\\bin\\aarch64-none-elf-'


def main():
    log, elf, func, thread_name, from_report = sys.argv[1:6]
    windows = int(sys.argv[6]) if len(sys.argv) > 6 else 12
    win = int(sys.argv[7]) if len(sys.argv) > 7 else 64
    out = subprocess.run([BIN + 'nm.exe', '-n', '-S', '--defined-only', '-C', elf], capture_output=True, text=True,
                         errors='replace').stdout
    starts, sizes, names = [], [], []
    for line in out.splitlines():
        parts = line.split(' ', 3)
        if len(parts) == 4 and parts[2] in 'tTwW':
            starts.append(int(parts[0], 16))
            sizes.append(int(parts[1], 16))
            names.append(parts[3])
    pcs = collections.Counter()
    thread, report, total = None, 0, 0
    for line in open(log, encoding='utf-8', errors='replace'):
        if line.startswith('===='):
            report += 1
            thread = None
            continue
        m = re.match(r'\s*-- thread (\d+ )?"([^"]*)"', line)
        if m:
            thread = m.group(2) if m.group(1) else None
            continue
        if thread is None or not thread.startswith(thread_name) or report < int(from_report):
            continue
        m = re.search(r'pc image\+0x([0-9a-f]+) x(\d+)', line)
        if m:
            pcs[int(m.group(1), 16)] += int(m.group(2))
            total += int(m.group(2))
    idx = [i for i, n in enumerate(names) if func in n]
    if not idx:
        print('no symbol'); return
    i = max(idx, key=lambda k: sum(n for pc, n in pcs.items() if starts[k] <= pc < starts[k] + sizes[k]))
    s0, size = starts[i], sizes[i]
    inside = {pc: n for pc, n in pcs.items() if s0 <= pc < s0 + size}
    tot_in = sum(inside.values())
    print('%s at 0x%x size 0x%x: %d of %d thread samples (%.1f%%)' % (names[i], s0, size, tot_in, total,
                                                                         100.0 * tot_in / max(total, 1)))
    w = collections.Counter()
    for pc, n in inside.items():
        w[(pc - s0) // win * win] += n
    top = w.most_common(windows)
    for off, n in top:
        print('  +0x%05x  %4d  %5.1f%% of function' % (off, n, 100.0 * n / tot_in))
    for off, n in sorted(top):
        a = s0 + off
        dis = subprocess.run([BIN + 'objdump.exe', '-d', '--no-show-raw-insn', '--start-address=0x%x' % a,
                              '--stop-address=0x%x' % (a + win), elf], capture_output=True, text=True,
                             errors='replace').stdout
        print('---- +0x%05x (%d samples)' % (off, n))
        for line in dis.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\s+(.*)', line)
            if m:
                pc = int(m.group(1), 16)
                print('  %4s  +0x%05x  %s' % (inside.get(pc, '') or '', pc - s0, m.group(2)[:90]))


if __name__ == '__main__':
    main()
