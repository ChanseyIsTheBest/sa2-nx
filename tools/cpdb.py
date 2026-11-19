#!/usr/bin/env python3
"""katamari-nx (2026-11-01): Beautiful Katamari's CPDB decompression, read from the game's own code.

A compressed file of d02.dat-d05.dat (flag 0x40 in its record of the executable's file tables, see
docs/katamari-notes.md, "rc8") starts with a 16-byte header: "CPDB", the coding (byte 4; bytes 5-7 are 0), 0, the
uncompressed size (big endian). The loader (sub_824A77D0) picks the decoder by the coding byte: 0 stored (the bytes
after the header), 1 plain LZSS, 2 Huffman LZSS. The rest is a bit stream: 64-bit big-endian words, each read from
its least significant bit (sub_824B3340):

  plain LZSS (sub_824B35D8): repeated until the output is complete
      1 bit flag; 0: an 8-bit literal; 1: a 4-bit length (+3) and a 12-bit distance, copied from distance bytes back
  Huffman LZSS (sub_824B3500, sub_824B3688, sub_824B3458): blocks of
      16-bit entry count; a Huffman tree written depth first (1 = node: the subtree taken on a 1 bit first, then the
      one taken on a 0 bit; 0 = leaf with a 9-bit symbol); then entries: a symbol (walking the tree one bit at a time),
      and after a symbol of 256 or more a 12-bit distance (an entry of its own). Symbol < 256: a literal; otherwise a
      copy of symbol - 253 bytes from distance bytes back.

  python tools/cpdb.py <archive> <offset> [--out file] [--compare <file> <offset>]
"""
import argparse
import struct
import sys


class Bits:
    """sub_824B3340: 64-bit big-endian words, each consumed from its least significant bit; a field is the next n
    bits, its first bit the field's bit 0 (a field crossing two words takes the rest of the old word as its low bits)."""

    def __init__(self, data, pos=0):
        self.d = data
        self.p = pos
        self.buf = 0
        self.avail = 0

    def get(self, n):
        while self.avail < n:
            word = int.from_bytes(self.d[self.p:self.p + 8].ljust(8, b'\0'), 'big')
            self.p += 8
            self.buf |= word << self.avail
            self.avail += 64
        v = self.buf & ((1 << n) - 1)
        self.buf >>= n
        self.avail -= n
        return v


def copy(out, dist, length):
    """The game's copy loop: byte by byte from dist bytes back, so a copy may overlap itself."""
    src = len(out) - dist
    if dist == 0 or src < 0:
        raise ValueError(f'distance {dist} at {len(out)}')
    if dist >= length:
        out += out[src:src + length]
    else:
        for k in range(length):
            out.append(out[src + k])


def lzss(bits, size):
    """Bits.get inlined (the bit order is the same: the stream is read in order, so reading a word ahead of time does
    not change what follows)."""
    d = bytes(bits.d) + bytes(8)
    p, buf, avail = bits.p, bits.buf, bits.avail
    out = bytearray()
    n = 0
    while n < size:
        if avail < 17:  # the longest token: flag, length, distance
            buf |= int.from_bytes(d[p:p + 8], 'big') << avail
            p += 8
            avail += 64
        if buf & 1 == 0:
            out.append((buf >> 1) & 0xFF)
            buf >>= 9
            avail -= 9
            n += 1
        else:
            length = ((buf >> 1) & 0xF) + 3
            copy(out, (buf >> 5) & 0xFFF, length)
            buf >>= 17
            avail -= 17
            n += length
    bits.p, bits.buf, bits.avail = p, buf, avail
    return bytes(out[:size])


def read_tree(bits):
    """Nested tuples: (subtree on 1, subtree on 0) for a node, an int for a leaf."""
    if bits.get(1):
        one = read_tree(bits)
        zero = read_tree(bits)
        return (one, zero)
    return bits.get(9)


def huffman(bits, size):
    out = bytearray()
    while len(out) < size:
        count = bits.get(16)
        tree = read_tree(bits)
        entries = []
        while len(entries) < count:
            node = tree
            while isinstance(node, tuple):
                node = node[0] if bits.get(1) else node[1]
            entries.append(node)
            if node >= 256:
                entries.append(bits.get(12))
        i = 0
        while i < len(entries):
            sym = entries[i]
            i += 1
            if sym < 256:
                out.append(sym)
            else:
                copy(out, entries[i], sym - 253)
                i += 1
    return bytes(out[:size])


CODINGS = {0: 'stored', 1: 'lzss', 2: 'huffman'}


def decompress(block, coding=None):
    """(coding, data); coding overrides the header's (testing). A stream that does not fit its header's coding is
    tried with the other one: d02.dat's Test[4] says 1 and is Huffman (an older packer; the game never loads the Test
    files). Stored files can be shorter than their record's size, which is then the buffer's (the fonts)."""
    magic, size = struct.unpack('>4s8xI', block[:16])
    if magic != b'CPDB':
        raise ValueError('not a CPDB block')
    tries = [coding] if coding else [CODINGS.get(block[4])]
    if tries[0] == 'stored':
        return 'stored', block[16:16 + size]
    if tries[0] not in ('lzss', 'huffman'):
        raise ValueError(f'unknown coding {block[4]}')
    if not coding:
        tries.append('huffman' if tries[0] == 'lzss' else 'lzss')
    errors = []
    for c in tries:
        try:
            return c, (huffman if c == 'huffman' else lzss)(Bits(block, 16), size)
        except (IndexError, RecursionError, ValueError) as e:
            errors.append(f'{c}: {e}')
    raise ValueError('stream broken (' + '; '.join(errors) + ')')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('archive')
    ap.add_argument('offset', type=lambda v: int(v, 0))
    ap.add_argument('--stored', type=lambda v: int(v, 0), default=0, help='stored size (default: read 16 MB)')
    ap.add_argument('--coding', choices=['stored', 'huffman', 'lzss'], help="override the header's coding")
    ap.add_argument('--out')
    ap.add_argument('--compare', nargs=2)
    ap.add_argument('--limit', type=int, default=0, help='decompress only this many bytes (testing)')
    a = ap.parse_args()
    with open(a.archive, 'rb') as f:
        f.seek(a.offset)
        block = f.read(a.stored or (16 << 20))
    if a.limit:
        size = struct.unpack('>I', block[12:16])[0]
        block = block[:12] + struct.pack('>I', min(size, a.limit)) + block[16:]
    coding, data = decompress(block, a.coding)
    print(f'{coding}: {len(data)} bytes')
    if a.compare:
        with open(a.compare[0], 'rb') as f:
            f.seek(int(a.compare[1], 0))
            plain = f.read(len(data))
        diff = next((i for i in range(len(data)) if data[i] != plain[i]), None)
        print('identical' if diff is None else f'first difference at {diff:#x}')
    if a.out:
        open(a.out, 'wb').write(data)


if __name__ == '__main__':
    sys.setrecursionlimit(10000)
    main()
