#!/usr/bin/env python3
"""banjo-nx: where inside a function a Switch run spends its samples (logs/rex/rex_profile.log), by source line.

  python tools/switch_hot_lines.py rex_profile.log --elf builds/<date>/renut.elf --function "DrawsVulkanImpl::Draw("
      [--thread "GPU ring native"] [--from-report 5] [--top 25] [--bucket 10]

Takes the 10 s pc histograms of the thread, keeps the pcs inside the functions whose demangled name contains
--function, and asks addr2line for each one's source line (the innermost inlined frame). Lines are grouped in buckets
of --bucket lines. Needs the ELF of the SAME build and devkitA64's nm and addr2line.
"""
import argparse
import bisect
import collections
import re
import subprocess

BIN = 'C:\\devkitPro\\devkitA64\\bin\\aarch64-none-elf-'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--elf', required=True)
    ap.add_argument('--function', required=True)
    ap.add_argument('--thread', default='GPU ring native')
    ap.add_argument('--from-report', type=int, default=1)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--bucket', type=int, default=10)
    args = ap.parse_args()
    out = subprocess.run([BIN + 'nm.exe', '-n', '--defined-only', '-C', args.elf], capture_output=True, text=True,
                         errors='replace').stdout
    starts, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'tTwW':
            starts.append(int(parts[0], 16))
            names.append(parts[2])
    pcs = collections.Counter()
    thread, report, total = None, 0, 0
    for line in open(args.log, encoding='utf-8', errors='replace'):
        if line.startswith('===='):
            report += 1
            thread = None
            continue
        m = re.match(r'\s*-- thread (\d+ )?"([^"]*)"', line)
        if m:
            thread = m.group(2) if m.group(1) else None
            continue
        if thread != args.thread or report < args.from_report:
            continue
        m = re.search(r'pc image\+0x([0-9a-f]+) x(\d+)', line)
        if m:
            pcs[int(m.group(1), 16)] += int(m.group(2))
            total += int(m.group(2))
    chosen = [(pc, n) for pc, n in pcs.items() if args.function in names[bisect.bisect_right(starts, pc) - 1]]
    inside = sum(n for _, n in chosen)
    print('%d of the thread\'s %d samples are inside "%s"' % (inside, total, args.function))
    if not chosen:
        return
    where = subprocess.run([BIN + 'addr2line.exe', '-e', args.elf] + [hex(pc) for pc, _ in chosen],
                           capture_output=True, text=True, errors='replace').stdout.splitlines()
    lines = collections.Counter()
    for (pc, n), text in zip(chosen, where):
        text = text.replace('\\', '/').split('/')[-1]
        m = re.match(r'(.*):(\d+)', text)
        lines[(m.group(1), int(m.group(2)) // args.bucket * args.bucket) if m else (text, 0)] += n
    for (name, line), n in lines.most_common(args.top):
        print('  %5.1f%%  %4d  %s:%d' % (100.0 * n / total, n, name, line))


if __name__ == '__main__':
    main()
