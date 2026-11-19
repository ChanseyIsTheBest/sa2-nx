#!/usr/bin/env python3
"""sa2-nx on the ultimate base: Renut's device layout constants at SA2's Direct3D (2.0.21076).

The base's d3d/d3d.h and d3d/d3d_native.h hold Katamari's layout (XDK 2.0.5632; each value its port moved carries
"katamari-nx: <N&B> -> <Katamari>"). sa2-nx 1.0.6 has the same constants at SA2's values (each with the SA2 function it
was read from and N&B's value in brackets). This lists every `constexpr <integer type> <name> = <value>;` of the two
base files next to 1.0.6's value of the same name (the same file first, then the other), and with --write puts SA2's
value in (the base's line kept, its comment followed by "sa2-nx: SA2's value ..., the base <value>"). Constants 1.0.6
does not have are listed for a look by hand.

  python tools/sa2_renut_layout.py [--old-repo <sa2-nx>] [--old-rev baf523b] [--write]
"""
import argparse
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILES = ["app/src/renut_engine/gpu/d3d/d3d.h", "app/src/renut_engine/gpu/d3d/d3d_native.h",
         "app/src/renut_engine/gpu/d3d/hooks/device.cpp", "app/src/renut_engine/gpu/d3d/hooks/draw.cpp",
         "app/src/renut_engine/gpu/d3d/hooks/resource.cpp", "app/src/renut_engine/gpu/d3d/hooks/shader.cpp"]
CONST = re.compile(r'^(\s*(?:inline\s+)?constexpr\s+(?:u32|u16|u8|uint32_t|uint64_t|uint16_t|uint8_t|int|size_t)\s+)'
                   r'([A-Za-z_]\w*)(\s*=\s*)([^;(]+)(;.*)$')


def consts(text):
    out = {}
    for i, line in enumerate(text.split('\n')):
        m = CONST.match(line)
        if m:
            out.setdefault(m.group(2), (i, m.group(4).strip(), line))
    return out


def value_of(v):
    v = v.strip()
    try:
        return int(v, 0)
    except ValueError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--old-repo', default=os.path.join(os.path.dirname(ROOT), 'sa2-nx'))
    ap.add_argument('--old-rev', default='baf523b')
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()
    old_by_file = {}
    for f in FILES:
        r = subprocess.run(['git', '-C', a.old_repo, 'show', f'{a.old_rev}:{f}'], capture_output=True)
        text = r.stdout.decode('utf-8', 'replace').replace('\r\n', '\n')
        old_by_file[f] = {k: (f, v[1]) for k, v in consts(text).items()}
    for f in FILES:
        path = os.path.join(ROOT, f)
        raw = open(path, 'rb').read()
        crlf = b'\r\n' in raw
        lines = raw.decode('utf-8').replace('\r\n', '\n').split('\n')
        changed = 0
        print(f'== {f}')
        for i, line in enumerate(lines):
            m = CONST.match(line)
            if not m:
                continue
            name, val = m.group(2), m.group(4).strip()
            marked = 'katamari-nx' in line
            old = old_by_file[f].get(name) or next((d[name] for d in old_by_file.values() if name in d), None)
            if old is None:
                if marked:
                    print(f'  [no 1.0.6 value] {name} = {val}  (katamari-nx)')
                continue
            ov = old[1]
            if value_of(ov) is not None and value_of(val) is not None:
                same = value_of(ov) == value_of(val)
            else:
                same = ov == val
            if same:
                continue
            print(f'  {name}: base {val} | 1.0.6 {ov}{"  (katamari-nx)" if marked else "  (NOT marked)"}')
            if a.write and value_of(ov) is not None:
                rest = m.group(5)
                src = os.path.basename(old[0])
                if '//' in rest:
                    rest = rest + f'; sa2-nx: SA2\'s (1.0.6\'s {src}), the base {val}'
                else:
                    rest = rest + f'  // sa2-nx: SA2\'s (1.0.6\'s {src}), the base {val}'
                lines[i] = m.group(1) + name + m.group(3) + ov + rest
                changed += 1
        if a.write and changed:
            text = '\n'.join(lines)
            if crlf:
                text = text.replace('\n', '\r\n')
            open(path, 'wb').write(text.encode('utf-8'))
            print(f'  {changed} written')


if __name__ == '__main__':
    main()
