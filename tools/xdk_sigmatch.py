#!/usr/bin/env python3
"""The XDK's statically linked library functions found by signature in any ReXGlue port (blur-nx, 2026-11-17; #251 of
docs/cpu-codegen-universal-30-1116.md). Every Xbox 360 game links the same XDK libraries (the C runtime, XMCORE, XAPILIB,
D3D9, D3DX9, XAUDIO2, ...) per XDK version: the same machine code at other addresses. A native written for one game's
copy of such a function (Blur's memset, memcpy and the large memcpy, app/src/cpu/hot/blur_hand_natives_crt.h and
blur_hand_natives_audio2.h) serves every game of that XDK version once its address there is known. This finds it.

A signature is the function's instruction words with what relocation changes masked: branch targets (b, bl, bc),
the immediates of addi / addis / ori / oris (address building), and the displacements of loads and stores not
relative to r1 (an address's low half). Registers, opcodes and stack offsets stay. Two functions match when their
instruction counts and signatures are equal. Function boundaries and sizes come from the port's generated code
(DEFINE_REX_FUNC(sub_X) and one `// insn` comment per guest instruction), the words from its image dump
(out/renut_image.bin, REXGLUE_DUMP_IMAGE; base 0x82000000).

  python tools/xdk_sigmatch.py make GEN IMAGE NAME=ADDR [NAME=ADDR ...] --out TABLE.json
  python tools/xdk_sigmatch.py find GEN IMAGE TABLE.json
e.g. python tools/xdk_sigmatch.py make app/generated out/renut_image.bin memset=82953880 memcpy=82954218 ^
       memcpy_large=82953D90 --out tools/xdk_2.0.11164_sigs.json
"""
import argparse
import collections
import glob
import hashlib
import json
import os
import re
import struct
import sys

IMAGE_BASE = 0x82000000
DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$')
INSN = re.compile(r'^\t// [a-z]')


def sizes(gen):
    """{address: instruction count} of every generated function (its `// insn` comment lines)."""
    out = {}
    for path in glob.glob(os.path.join(gen, '*_recomp.*.cpp')):
        cur = None
        n = 0
        for line in open(path, encoding='utf-8', errors='replace'):
            m = DEF.match(line)
            if m:
                cur, n = int(m.group(1), 16), 0
                continue
            if cur is None:
                continue
            if line.startswith('}'):
                out[cur] = n
                cur = None
            elif INSN.match(line):
                n += 1
    return out


def masked(w):
    op = w >> 26
    if op == 18:  # b, bl: the target
        return w & ~0x03FFFFFC
    if op == 16:  # bc: the target
        return w & ~0x0000FFFC
    if op in (14, 15, 24, 25):  # addi, addis, ori, oris: an address's halves
        return w & ~0xFFFF
    ra = (w >> 16) & 31
    if 32 <= op <= 55 or op in (58, 62):  # D / DS-form loads and stores
        return w if ra == 1 else w & ~0xFFFF
    return w


def signature(image, addr, count):
    off = addr - IMAGE_BASE
    if off < 0 or off + 4 * count > len(image):
        return None
    words = struct.unpack_from('>%dI' % count, image, off)
    h = hashlib.sha1()
    for w in words:
        h.update(struct.pack('>I', masked(w)))
    return h.hexdigest()


def cmd_make(a):
    image = open(a.image, 'rb').read()
    sz = sizes(a.gen)
    table = {}
    for item in a.items:
        name, addr = item.split('=')
        addr = int(addr, 16)
        if addr not in sz:
            sys.exit('%s: no generated function at %08X' % (name, addr))
        table[name] = {'address': '%08X' % addr, 'instructions': sz[addr],
                       'signature': signature(image, addr, sz[addr])}
    json.dump(table, open(a.out, 'w'), indent=1)
    print('%d signatures written to %s' % (len(table), a.out))


def cmd_find(a):
    image = open(a.image, 'rb').read()
    sz = sizes(a.gen)
    table = json.load(open(a.table))
    by_size = collections.defaultdict(list)
    for addr, n in sz.items():
        by_size[n].append(addr)
    for name, e in sorted(table.items()):
        hits = [addr for addr in by_size.get(e['instructions'], [])
                if signature(image, addr, e['instructions']) == e['signature']]
        print('%-20s %s' % (name, ', '.join('%08X' % h for h in hits) if hits else 'not found'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('make')
    m.add_argument('gen')
    m.add_argument('image')
    m.add_argument('items', nargs='+')
    m.add_argument('--out', required=True)
    f = sub.add_parser('find')
    f.add_argument('gen')
    f.add_argument('image')
    f.add_argument('table')
    a = ap.parse_args()
    (cmd_make if a.cmd == 'make' else cmd_find)(a)


if __name__ == '__main__':
    main()
