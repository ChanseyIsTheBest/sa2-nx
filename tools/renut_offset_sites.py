#!/usr/bin/env python3
"""katamari-nx (2026-10-30): lists the literals in Renut's sources that may be N&B (title update 3) device offsets above
the part of the device XDK 2.0.5632 lays out the same (>= 10944 bytes: docs/katamari-notes.md, "The device layout"),
for tools/renut_device_offsets_katamari.py. Decimal 10944-24191 and hex 0x2AC0-0x5E7F, in code (not comments).

  python tools/renut_offset_sites.py [--out out/renut_map/offset_sites.txt]
"""
import argparse
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
NUM = re.compile(r'(?<![0-9A-Za-z_.])(0x[0-9A-Fa-f]+|\d+)(?:u|U)?(?![0-9A-Za-z_.])')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'renut_map', 'offset_sites.txt'))
    a = ap.parse_args()
    out = []
    for dirpath, _, files in os.walk(GPU):
        for name in sorted(files):
            if not name.endswith(('.cpp', '.h', '.inc')):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, GPU).replace('\\', '/')
            for i, line in enumerate(open(path, encoding='utf-8', errors='replace'), 1):
                code = line.split('//', 1)[0]
                if not code.strip() or code.lstrip().startswith(('*', '/*', '#include')):
                    continue
                vals = []
                for m in NUM.finditer(code):
                    s = m.group(1)
                    v = int(s, 16) if s.lower().startswith('0x') else int(s)
                    if s.lower().startswith('0x') and 0x2AC0 <= v < 0x5E80:
                        vals.append(s)
                    elif not s.lower().startswith('0x') and 10944 <= v < 24192:
                        vals.append(s)
                if vals:
                    out.append(f'{rel}:{i}: [{", ".join(vals)}]  {line.strip()[:160]}')
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    open(a.out, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print(f'{len(out)} lines -> {a.out}')


if __name__ == '__main__':
    main()
