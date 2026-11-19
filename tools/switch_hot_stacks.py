#!/usr/bin/env python3
"""banjo-nx: the call chains a thread was in during a Switch run's hitch frames (logs/rex/rex_profile.log).

The 10 s pc histograms (tools/switch_hot_symbols.py) say which function a sample is in, not who called it. The
"during the hitches" sections carry whole stacks ("stack image+A <- image+B <- ..."): this names their frames from
the ELF's symbol table and adds them up, by the first --depth frames or by every function that appears anywhere in
the stack (--inclusive: the share of the thread's hitch samples that had that function on the stack).

  python tools/switch_hot_stacks.py rex_profile.log --elf builds/<date>/renut.elf [--thread "GPU ring native"]
      [--from-report 5] [--depth 3] [--top 40] [--inclusive] [--under NAME]

--under NAME keeps only the stacks that have a function containing NAME, and prints what was above it (the chain from
the leaf down to NAME).
"""
import argparse
import bisect
import collections
import re
import subprocess

NM = 'C:\\devkitPro\\devkitA64\\bin\\aarch64-none-elf-nm.exe'


def short(name):
    name = name.replace('masseffect::native::(anonymous namespace)::', '')
    name = name.replace('me::native::(anonymous namespace)::NativeGraphicsSystem::', 'System::')
    name = name.replace('masseffect::native::', '')
    return re.sub(r'\(.*', '', name)[:46]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--elf', required=True)
    ap.add_argument('--thread', default='GPU ring native')
    ap.add_argument('--from-report', type=int, default=1)
    ap.add_argument('--depth', type=int, default=3)
    ap.add_argument('--top', type=int, default=40)
    ap.add_argument('--inclusive', action='store_true')
    ap.add_argument('--under', default='')
    args = ap.parse_args()
    out = subprocess.run([NM, '-n', '--defined-only', '-C', args.elf], capture_output=True, text=True,
                         errors='replace').stdout
    starts, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) == 3 and parts[1] in 'tTwW':
            starts.append(int(parts[0], 16))
            names.append(parts[2])

    def sym(offset):
        i = bisect.bisect_right(starts, offset) - 1
        return short(names[i]) if i >= 0 else '?'

    chains = collections.Counter()
    thread, report, samples, total = None, 0, 0, 0.0
    for line in open(args.log, encoding='utf-8', errors='replace'):
        if line.startswith('===='):
            report += 1
            thread = None
            continue
        m = re.match(r'\s*-- thread (\d+ )?"([^"]*)": (\d+) samples during', line)
        if m:
            thread = None if m.group(1) else m.group(2)
            samples = int(m.group(3))
            continue
        if re.match(r'\s*-- thread', line):
            thread = None
            continue
        if thread != args.thread or report < args.from_report:
            continue
        m = re.match(r'\s*([0-9.]+)%\s+stack (.*)', line)
        if not m:
            continue
        weight = float(m.group(1)) * samples / 100.0
        frames = [sym(int(x, 16)) for x in re.findall(r'image\+0x([0-9a-f]+)', m.group(2))]
        total += weight
        if args.under:
            hit = [i for i, f in enumerate(frames) if args.under in f]
            if not hit:
                continue
            chains[' <- '.join(frames[:hit[0] + 1][:args.depth + 1])] += weight
        elif args.inclusive:
            for f in set(frames):
                chains[f] += weight
        else:
            chains[' <- '.join(frames[:args.depth])] += weight
    print('%.0f stack samples of thread "%s" (reports %d on)' % (total, args.thread, args.from_report))
    for chain, weight in chains.most_common(args.top):
        print('  %5.1f%%  %s' % (100.0 * weight / max(1.0, total), chain))


if __name__ == '__main__':
    main()
