#!/usr/bin/env python3
"""sa2-nx-ult (2026-11-21): the shader records of SA2's game files for the installer (sa2-nx-ult-installer's
tools/make_release.py --records): the containers in Shader/xbox360/obj (each .wpu / .wvu file is one version 0x11
container, 0x102A11xx, stored as it is) and those inside Shader/xbox360/obj.pak (found by their signature). The
installer's lib/sources.js reads them with the "raw" coding; the shaders compiled into default.xex it finds itself.

  python tools/sa2_shader_records.py <game folder> [out.json]
"""
import hashlib
import json
import os
import struct
import sys


def containers_in(blob):
    """Version 0x11 shader containers in `blob`: [(at, length)] by their header (0x102A11xx, then the sizes)."""
    out = []
    p = 0
    while True:
        p = blob.find(b'\x10\x2a\x11', p)
        if p < 0 or p + 24 > len(blob):
            return out
        # the container's size: the header's two sizes (constants and code) after its fixed 24 bytes, as the base's
        # translator reads it: offset 4 the constant data's end, offset 8 the code's size
        a, b = struct.unpack('>II', blob[p + 4:p + 12])
        length = a + b
        if 24 <= length <= len(blob) - p:
            out.append((p, length))
            p += length
        else:
            p += 1


def main():
    root = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    records = []
    base = os.path.join(root, 'Shader', 'xbox360')
    for dirpath, _, names in os.walk(base):
        for n in sorted(names):
            full = os.path.join(dirpath, n)
            rel = os.path.relpath(full, root).replace('\\', '/')
            blob = open(full, 'rb').read()
            if n.lower().endswith(('.wpu', '.wvu')):
                found = [(0, len(blob))] if blob[:3] == b'\x10\x2a\x11' else []
            else:
                found = containers_in(blob)
            if not found:
                continue
            records.append({'file': rel, 'offset': 0, 'stored': len(blob), 'size': len(blob), 'codec': 'raw',
                            'containers': [{'at': at, 'length': ln,
                                            'sha256': hashlib.sha256(blob[at:at + ln]).hexdigest()} for at, ln in found]})
    total = sum(len(r['containers']) for r in records)
    print(f'{len(records)} files, {total} containers', file=sys.stderr)
    text = json.dumps(records, indent=1)
    if out:
        open(out, 'w', encoding='utf-8').write(text + '\n')
    else:
        print(text)


if __name__ == '__main__':
    main()
