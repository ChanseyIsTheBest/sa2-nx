#!/usr/bin/env python3
"""katamari-nx (2026-10-30): the lines of a PC or emulator run's log worth reading first: crashes and fatal errors,
unregistered calls, missing shaders, Renut's warnings, the census and verify lines, the frame rate.

  python tools/summarise_run.py <log> [--tail 15]
"""
import argparse
import collections
import re

FIRST = re.compile(r'FATAL|critical|C0000|access violation|unregistered|Unresolved|DIFFERENCE|MISMATCH|'
                   r'not in the shader cache|shader missing|Draw dropped|out of memory|Out of memory', re.I)
COUNT = re.compile(r'\[(warning|error)\] \[(\w+)\]')
KEEP = re.compile(r'census|\[verify\]|fps|frames a second|Render resolution|Swapchain|device .* created|'
                  r'input_script|\[renut\] deko3d|ring thread|GPU per Swap|presented', re.I)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--tail', type=int, default=15)
    a = ap.parse_args()
    lines = open(a.log, encoding='utf-8', errors='replace').read().splitlines()
    counts = collections.Counter()
    first, keep = [], []
    for line in lines:
        m = COUNT.search(line)
        if m:
            counts[f'{m.group(1)} [{m.group(2)}]'] += 1
        if FIRST.search(line) and len(first) < 40:
            first.append(line[:300])
        elif KEEP.search(line) and len(keep) < 60:
            keep.append(line[:300])
    print(f'{len(lines)} lines; warnings / errors by category: {dict(counts.most_common(12))}')
    print('--- first problems ---')
    print('\n'.join(first) or '(none)')
    print('--- status lines ---')
    print('\n'.join(keep) or '(none)')
    print('--- tail ---')
    print('\n'.join(l[:300] for l in lines[-a.tail:]))


if __name__ == '__main__':
    main()
