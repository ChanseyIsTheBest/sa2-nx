#!/usr/bin/env python3
"""katamari-nx (2026-10-30): rewrites Renut's hard-coded Direct3D device offsets and guest data addresses from N&B's
title update 3 (XDK 2.0.7978, the layout of banjo-nx's Renut) to Beautiful Katamari's (XDK 2.0.5632).
docs/katamari-notes.md, "The device layout", has the evidence for every zone and address.

The sites are out/renut_map/offset_sites.txt (tools/renut_offset_sites.py: literals 10944..24191 / 0x2AC0..0x5E7F in
Renut's code); below 10944 the two layouts are the same (checked over 288 paired functions). Each literal on a site's
line that is a device offset is mapped by kat_offset(); masks, sizes and timings on the same lines are left alone. The
data addresses are rewritten wherever they appear in code. Lines already rewritten carry `katamari-nx` and are skipped.

  python -P tools/renut_device_offsets_katamari.py [--dry-run]
"""
import argparse
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPU = os.path.join(ROOT, 'app', 'src', 'renut_engine', 'gpu')
NUM = re.compile(r'(?<![0-9A-Za-z_.])(0x[0-9A-Fa-f]+|\d+)((?:u|U)?)(?![0-9A-Za-z_.])')

# N&B title update 3 -> Beautiful Katamari, the zones of the device (docs/katamari-notes.md, "The device layout").
ZONES = [
    (0, 10943, 0),          # ring, state calls, fetch constants, register packets, fences, flags: the same
    (10944, 11827, -12),    # 11012 -> 11000, VertexDeclaration 11824 -> 11812
    (11828, 13470, -16),    # 11832 -> 11816 ... VerticesRingEnd 13444 -> 13428, 13456 -> 13440
    (13471, 16800, -24),    # CommandBuffer 13516 -> 13492 ... SwapCount 16560 -> 16536, SideBuffer 16728 -> 16704
    (21500, 21539, -24),    # 21532 -> 21508 (Swap)
    (21540, 21599, -28),    # display mode 21544-21556 -> 21516-21528, the -1 words 21560/4, perf frequency 21588/92
    (21600, 21699, -32),    # 21624-21632 -> 21592-21600 (Swap)
    (23300, 23969, -1708),  # 23348 -> 21640, 23360 -> 21652, 23380 -> 21672 (the 41 -1 words), gamma 23952/6
    (23970, 24063, -1716),  # 23976 -> 22260, BehaviorFlags 23980 -> 22264
]
EXPLICIT = {0x5E00: 0x5700}  # kDeviceAllocSize: Katamari allocates and clears 0x5700 (sub_824D0598 / sub_824D0240)
NOT_OFFSETS = {0x3FFF, 0x4000, 0x3C00, 0x2B00}  # masks, sizes, Renut's own struct split (d3d.h pad_2AA4)
SKIP_FILES = {'banjo_renut_tuning.cpp', 'render/pipeline.cpp', 'render/surfaces.cpp'}
SKIP_LINES = {('backend/queued.cpp', 52), ('d3d/hooks/resource.cpp', 101), ('d3d/hooks/resource.cpp', 339)}

# N&B title update 3 guest data -> Beautiful Katamari (docs/katamari-notes.md, "Guest data").
DATA = {
    0x82DA3478: 0x82AD93F8,  # the XDK build qword (0x0002000016004000 here)
    0x820007F8: 0x82000750,  # VdGlobalDevice's import record (both resolve to the same kernel variable)
    0x82161B30: 0x820FC1F8,  # D3DCAPS9, GetDeviceCaps sub_824E6410 copies 304 bytes
    0x82001744: 0x820FAEB4,  # the full viewport constant (EndTiling, the viewport setter)
    0x82DA3778: 0x8315DFE0,  # XPhysicalAlloc's no-large-pages flag (XPhysicalAlloc 0x824C1C30)
    0x82005200: 0x820FA9D0,  # the format table (seven D3D functions)
    0x82E108A0: 0x8315E830,  # the HiZ owner (CreateSurface, D3DResource_Destroy)
    0x82D8A948: 0x82AD8E48,  # the render state table (tools/renut_state_tables.py)
    0x82D8AE28: 0x82AD9308,  # the sampler state table
}


def kat_offset(v):
    if v in EXPLICIT:
        return EXPLICIT[v]
    for lo, hi, d in ZONES:
        if lo <= v <= hi:
            return v + d
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    sites = []
    for line in open(os.path.join(ROOT, 'out', 'renut_map', 'offset_sites.txt'), encoding='utf-8'):
        rel, num = line.split(':', 2)[:2]
        sites.append((rel, int(num)))
    by_file = {}
    for rel, num in sites:
        if rel in SKIP_FILES or (rel, num) in SKIP_LINES:
            continue
        by_file.setdefault(rel, set()).add(num)
    report, unknown = [], []
    for rel in sorted(set(by_file) | {r for r in ('d3d/hooks/device.cpp', 'd3d/hooks/resource.cpp', 'd3d/hooks/draw.cpp',
                                                  'd3d/d3d_native_state.cpp')}):
        path = os.path.join(GPU, rel)
        raw = open(path, encoding='utf-8', newline='').read()
        nl = '\r\n' if '\r\n' in raw else '\n'
        lines = raw.split(nl)
        changed = False
        for i, line in enumerate(lines):
            if 'katamari-nx' in line:
                continue
            cut = line.find('//')
            head, tail = (line, '') if cut < 0 else (line[:cut], line[cut:])
            new_head = head
            what = []
            # data addresses anywhere in code
            for old, new in DATA.items():
                for s in ('0x%08X' % old, '0x%08x' % old):
                    if s in new_head:
                        new_head = new_head.replace(s, '0x%08X' % new)
                        what.append(f'{s}->0x{new:08X}')
            if (i + 1) in by_file.get(rel, ()):
                def sub(m):
                    s, suffix = m.group(1), m.group(2)
                    hexa = s.lower().startswith('0x')
                    v = int(s, 16) if hexa else int(s)
                    in_range = (0x2AC0 <= v < 0x5E80) if hexa else (10944 <= v < 24192)
                    if not in_range or v in NOT_OFFSETS:
                        return m.group(0)
                    k = kat_offset(v)
                    if k is None:
                        unknown.append(f'{rel}:{i + 1}: {s} has no zone: {line.strip()[:120]}')
                        return m.group(0)
                    if k == v:
                        return m.group(0)
                    what.append(f'{s}->{("0x%X" % k) if hexa else k}')
                    return (('0x%X' % k) if hexa else str(k)) + suffix
                new_head = NUM.sub(sub, new_head)
            if new_head != head:
                lines[i] = new_head + (tail + '  ' if tail else '  // ') + 'katamari-nx: ' + ', '.join(what)
                if tail:
                    lines[i] = new_head + tail + ' (katamari-nx: ' + ', '.join(what) + ')'
                changed = True
                report.append(f'{rel}:{i + 1}: {", ".join(what)}')
        if changed and not a.dry_run:
            open(path, 'w', encoding='utf-8', newline='').write(nl.join(lines))
    out = os.path.join(ROOT, 'out', 'renut_map', 'offsets_report.txt')
    open(out, 'w', encoding='utf-8').write('\n'.join(report + ['', '# no zone:'] + unknown) + '\n')
    print(f'{len(report)} lines rewritten, {len(unknown)} literals without a zone; {out}')
    for u in unknown:
        print('  ' + u)


if __name__ == '__main__':
    main()
