#!/usr/bin/env python3
"""banjo-nx (2026-10-23): an installed GLSL package (banjo-nx-installer's deko3d_glsl.bin, version 2, each shader's GLSL
in a zstd frame of raw blocks: 170 MB) made again with each shader compressed against a dictionary trained on them all
(about 13 MB), so that the NRO keeps the whole package in memory and never reads a shader from the card while the game
runs (the console's stutters of 2026-10-23: the render thread waited on fseek / fread of this file for each new shader,
and on the pipeline prebuild's reads of it).

    python tools/deko3d_glsl_compress.py <deko3d_glsl.bin in> <deko3d_glsl.bin out> [--level 19]

Version 2 with a dictionary (banjo_renut_deko3d.cpp OpenInstalled):
  header (32 bytes): "BDKG", 2, count, dictionary size, table offset, data offset, info offset, info size
  dictionary: at offset 32 (zstd, trained by zstd --train)
  table, info: as the installer writes them (the table's offsets and compressed sizes for the new frames; the info
               section copied byte for byte)
  data: one zstd frame per shader (dictionary compressed), each 8-byte aligned
Game data: the build tree and the SD card only.
"""
import argparse
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZSTD = r'C:\devkitPro\msys2\usr\bin\zstd.exe'


def raw_frame_text(data, at, size):
    """The bytes of a zstd frame of raw blocks (single segment, 4-byte content size), as package.js writes it."""
    assert struct.unpack_from('<I', data, at)[0] == 0xFD2FB528, 'not a zstd frame'
    assert data[at + 4] == (2 << 6) | (1 << 5), 'not the installer\'s frame header'
    pos = at + 9
    out = bytearray()
    while True:
        b = data[pos] | data[pos + 1] << 8 | data[pos + 2] << 16
        pos += 3
        last, kind, bsize = b & 1, (b >> 1) & 3, b >> 3
        assert kind == 0, 'not a raw block'
        out += data[pos:pos + bsize]
        pos += bsize
        if last:
            break
    assert len(out) == size, 'size'
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--level', default='19')
    a = ap.parse_args()
    data = open(a.src, 'rb').read()
    magic, version, count, dict_size, table_off, data_off, info_off, info_size = struct.unpack_from('<4sIIIIIII', data, 0)
    if magic != b'BDKG' or version != 2 or dict_size != 0:
        sys.exit('deko3d_glsl_compress: not an installer package (version 2, raw frames)')
    entries = [list(struct.unpack_from('<QIIII8I', data, table_off + 56 * i)) for i in range(count)]
    work = os.path.join(ROOT, 'out', 'deko3d_shaders', 'glsl_compress_work')
    # The tool's own scratch folder in the build tree, written over at each run (nothing is deleted).
    os.makedirs(os.path.join(work, 'raw'), exist_ok=True)
    os.makedirs(os.path.join(work, 'z'), exist_ok=True)
    names = []
    raw_total = 0
    for i, e in enumerate(entries):
        text = raw_frame_text(data, data_off + e[1], e[3])
        name = f'{i:05d}.glsl'
        open(os.path.join(work, 'raw', name), 'wb').write(text)
        names.append(name)
        raw_total += len(text)
    dict_path = os.path.join(work, 'dict')
    maxdict = max(4096, min(131072, raw_total // 100))
    subprocess.run([ZSTD, '--train', '-q', f'--maxdict={maxdict}', '-o', dict_path, '-r', os.path.join(work, 'raw')],
                   check=True)
    raw_dir = os.path.join(work, 'raw')
    zdir = os.path.join(work, 'z')
    # In batches: one zstd process per few hundred files (the command line's length).
    for k in range(0, len(names), 400):
        subprocess.run([ZSTD, f'-{a.level}', '-q', '-f', '-D', dict_path, '--output-dir-flat', zdir] + names[k:k + 400],
                       check=True, cwd=raw_dir)  # names relative to raw_dir: Windows' command line is 32 K
    dictionary = open(dict_path, 'rb').read()
    frames = [open(os.path.join(zdir, n + '.zst'), 'rb').read() for n in names]
    info = data[info_off:info_off + info_size]
    new_table = 32 + ((len(dictionary) + 7) & ~7)
    new_info = new_table + 56 * count
    new_data = (new_info + len(info) + 7) & ~7
    out = bytearray(new_data)
    at = 0
    blob = bytearray()
    for e, f in zip(entries, frames):
        e[1] = at
        e[2] = len(f)
        blob += f
        blob += b'\0' * ((8 - len(blob) % 8) % 8)
        at = len(blob)
    struct.pack_into('<4sIIIIIII', out, 0, b'BDKG', 2, count, len(dictionary), new_table, new_data, new_info, len(info))
    out[32:32 + len(dictionary)] = dictionary
    for i, e in enumerate(entries):
        struct.pack_into('<QIIII8I', out, new_table + 56 * i, *e)
    out[new_info:new_info + len(info)] = info
    out += blob
    open(a.dst, 'wb').write(out)
    print(f'deko3d_glsl_compress: {count} shaders, {raw_total >> 20} MB of GLSL -> {len(out) >> 20} MB '
          f'(dictionary {len(dictionary) >> 10} KB) -> {a.dst}')


if __name__ == '__main__':
    main()
