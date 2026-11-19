#!/usr/bin/env python3
"""rr6-nx (2026-11-10): game files as LZ4 blocks (*.lz4b, sdk/include/rex/filesystem/lz4_blocks.h has the format).

The installer pages (renut_nx_ultimate_installer, the files its game.json "lz4" names; rr6-nx-installer, the three RRM
archives) pack game files in the browser; this is the same format on the PC, for tests, and a checker for files the page
made.

  python tools/lz4b_pack.py pack IN OUT.lz4b [--level 9] [--block 262144]
  python tools/lz4b_pack.py check PACKED.lz4b ORIGINAL      (every block decompressed and compared)
  python tools/lz4b_pack.py info PACKED.lz4b

Needs the lz4 module (pip's lz4: LZ4's block functions). Game data: never commit or share what it writes.
"""
import argparse
import hashlib
import os
import struct
import sys
import time

import lz4.block

MAGIC = b'RXLZ4BLK'
END = b'RXLZ4END'
HEADER = struct.Struct('<8sIIQII')   # magic, version, block size, size, block count, 0
FOOTER = struct.Struct('<Q8s')       # index offset, end magic


def pack(src, dst, level=9, block=262144):
    size = os.path.getsize(src)
    count = (size + block - 1) // block
    sizes = []
    started = time.perf_counter()
    with open(src, 'rb') as f, open(dst, 'wb') as out:
        out.write(HEADER.pack(MAGIC, 1, block, size, count, 0))
        for _ in range(count):
            raw = f.read(block)
            packed = lz4.block.compress(raw, mode='high_compression', compression=level, store_size=False)
            data = packed if len(packed) < len(raw) else raw  # stored as it is when not smaller
            out.write(data)
            sizes.append(len(data))
        index_offset = out.tell()
        out.write(struct.pack(f'<{count}I', *sizes))
        out.write(FOOTER.pack(index_offset, END))
    packed_size = os.path.getsize(dst)
    print(f'{os.path.basename(src)}: {size / 1048576:.1f} MB -> {packed_size / 1048576:.1f} MB '
          f'({packed_size / max(size, 1) * 100:.1f} %), {count} blocks, {time.perf_counter() - started:.1f} s')


def read_layout(path):
    with open(path, 'rb') as f:
        magic, version, block, size, count, _ = HEADER.unpack(f.read(HEADER.size))
        if magic != MAGIC or version != 1:
            sys.exit(f'{path}: not an .lz4b file')
        f.seek(-FOOTER.size, os.SEEK_END)
        index_offset, end = FOOTER.unpack(f.read(FOOTER.size))
        if end != END:
            sys.exit(f'{path}: no footer (cut short?)')
        f.seek(index_offset)
        sizes = struct.unpack(f'<{count}I', f.read(4 * count))
    if HEADER.size + sum(sizes) != index_offset:
        sys.exit(f'{path}: the blocks do not end where the index starts')
    return block, size, count, sizes


def check(path, original):
    block, size, count, sizes = read_layout(path)
    if os.path.getsize(original) != size:
        sys.exit(f'{path}: holds {size} bytes, {original} has {os.path.getsize(original)}')
    started = time.perf_counter()
    h_packed, h_orig = hashlib.sha256(), hashlib.sha256()
    with open(path, 'rb') as f, open(original, 'rb') as o:
        f.seek(HEADER.size)
        for i, stored in enumerate(sizes):
            length = block if i + 1 < count else size - i * block
            data = f.read(stored)
            raw = data if stored == length else lz4.block.decompress(data, uncompressed_size=length)
            want = o.read(length)
            if raw != want:
                sys.exit(f'{path}: block {i} differs from {original}')
            h_packed.update(raw)
            h_orig.update(want)
    print(f'{os.path.basename(path)}: all {count} blocks equal {os.path.basename(original)} '
          f'(SHA-256 {h_orig.hexdigest()[:16]}), {time.perf_counter() - started:.1f} s')


def info(path):
    block, size, count, sizes = read_layout(path)
    stored_raw = sum(1 for i, s in enumerate(sizes) if s == (block if i + 1 < count else size - i * block))
    packed = os.path.getsize(path)
    print(f'{path}: {size} bytes in {count} blocks of {block}, {packed} packed ({packed / size * 100:.1f} %), '
          f'{stored_raw} blocks stored as they are')


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('pack')
    p.add_argument('src')
    p.add_argument('dst')
    p.add_argument('--level', type=int, default=9)
    p.add_argument('--block', type=int, default=262144)
    c = sub.add_parser('check')
    c.add_argument('packed')
    c.add_argument('original')
    i = sub.add_parser('info')
    i.add_argument('packed')
    a = ap.parse_args()
    if a.cmd == 'pack':
        pack(a.src, a.dst, a.level, a.block)
    elif a.cmd == 'check':
        check(a.packed, a.original)
    else:
        info(a.packed)


if __name__ == '__main__':
    main()
