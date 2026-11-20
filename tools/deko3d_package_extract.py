#!/usr/bin/env python3
"""banjo-nx (2026-11-20): a GLSL package (BDKG v2: deko3d_glsl.bin, the NRO's or the installer's) back into a folder of
vs_<HASH>.glsl / ps_<HASH>.glsl, as tools/deko3d_shaders.py and deko3d_glsl_opt.py name them, so two packages (renut-nx
1.0.2's installed one and this tree's) can be compared shader by shader (tools/deko3d_shader_stats.py --base-glsl).

Layout (app/src/renut_engine/gpu/banjo_renut_deko3d.cpp PackageHeader / PackageEntry): a 32-byte header (magic BDKG,
version, count, dictSize, tableOffset, dataOffset, reserved[2]); the zstd dictionary at 32 (dictSize bytes; 0: raw
frames); the table of 56-byte entries (hash u64, offset, compressedSize, size, stage, boolMask[8]); each entry's frame at
dataOffset + offset. Stage 0 is a vertex shader, 4 a pixel shader. Needs Python 3.14 (compression.zstd).

  python tools/deko3d_package_extract.py <deko3d_glsl.bin> <out folder>
Game data: write the folder under out/ only (never committed).
"""
import os
import struct
import sys

from compression import zstd


def main():
    src, out = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    magic, version, count, dict_size, table, data_off, r0, r1 = struct.unpack_from('<4s7I', data, 0)
    if magic != b'BDKG':
        sys.exit('not a BDKG package: %r' % magic)
    zd = zstd.ZstdDict(data[32:32 + dict_size]) if dict_size else None
    os.makedirs(out, exist_ok=True)
    stages = {0: 'vs', 4: 'ps'}
    done = 0
    for i in range(count):
        h, off, csize, size, stage = struct.unpack_from('<Q4I', data, table + 56 * i)
        frame = data[data_off + off:data_off + off + csize]
        text = zstd.decompress(frame, zstd_dict=zd) if zd else zstd.decompress(frame)
        if len(text) != size:
            sys.exit('entry %d: %d bytes, the table says %d' % (i, len(text), size))
        name = '%s_%016X.glsl' % (stages.get(stage, 'st%d' % stage), h)
        open(os.path.join(out, name), 'wb').write(text)
        done += 1
    print('%s: version %d, %d shaders (dictionary %d bytes) -> %s' % (src, version, done, dict_size, out))


if __name__ == '__main__':
    main()
