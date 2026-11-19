#!/usr/bin/env python3
"""katamari-nx (2026-11-01): Beautiful Katamari's file system, read from the executable, and the shader keys of every
model in it.

The executable holds 31 category descriptors at 0x82AD5078 (20 bytes: name, record table, alternate record table,
record count, run-time state). A record is 16 bytes: archive name pointer (d02.dat ... d05.dat), flags in the top
3 bits and the uncompressed size in the low 29, offset in the archive, stored size. Flag 0x40 = compressed (CPDB
block, tools/cpdb.py; its header names the coding). The loader reads the alternate table when there is one
(sub_824A6A20's callers), so both are listed.

Each material of a model names its shader by a key (MT0_00000000_OP9_..._VFn1c0t0_00000000), and the model carries
the compiled shader containers (Alchemy igVertexShaderInfo / igPixelShaderInfo, the 0x102A11xx containers that
D3DDevice_Create*Shader receives). This lists the keys of every file and, with --shaders, writes every container as
<folder>/{vs,ps}_<xxh3_64>.bin, the name Renut looks shaders up by (tools/renut_shader_dump.py).

  python tools/katamari_files.py --image out/renut_image.bin --game <game folder> [--keys out/shader_keys.txt]
      [--shaders out/katamari_file_shaders] [--known out/renut_shader_dump] [--category Map ...] [--dump <folder>]

Game data: the dumps, the containers and the key list stay on this PC (out/, never committed or distributed).
"""
import argparse
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cpdb  # noqa: E402

BASE = 0x82000000
DESCRIPTORS = 0x82AD5078
KEY = re.compile(rb'MT\d_[0-9A-F]{8}_OP\d_GR\d_NM\d_RL\d_TA\d_LT\d_SK\d_VC\d_VF[a-z0-9]+_[0-9A-F]{8}')


def cstr(image, address):
    o = address - BASE
    e = image.find(b'\0', o)
    return image[o:e].decode('latin1')


def categories(image):
    out, k = [], 0
    while True:
        name_p, table, table2, count, _ = struct.unpack_from('>5I', image, DESCRIPTORS - BASE + 20 * k)
        if not (BASE <= name_p < BASE + len(image)) or not count or count > 100000:
            break
        name = cstr(image, name_p)
        if not name.isidentifier():
            break
        out.append((name, table, table2, count))
        k += 1
    return out


def records(image, table, count):
    for i in range(count):
        arch_p, word, offset, stored = struct.unpack_from('>4I', image, table - BASE + 16 * i)
        yield i, cstr(image, arch_p), word >> 29, word & 0x1FFFFFFF, offset, stored


def read_file(game, archive, flags, size, offset, stored, handles):
    path = os.path.join(game, archive.replace('\\', os.sep))
    if path not in handles:
        handles[path] = open(path, 'rb')
    f = handles[path]
    f.seek(offset)
    data = f.read(stored)
    if not flags & 0b010:
        return data[:size]
    if data[:4] != b'CPDB':
        raise ValueError('flag says compressed, no CPDB header')
    coding, out = cpdb.decompress(data)
    if len(out) != size and coding != 'stored':
        raise ValueError(f'{len(out)} bytes, record says {size}')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--image', required=True)
    ap.add_argument('--game', required=True)
    ap.add_argument('--keys')
    ap.add_argument('--category', nargs='*')
    ap.add_argument('--dump')
    ap.add_argument('--shaders', help='write the shader containers here')
    ap.add_argument('--known', help='a folder of containers to count against (out/renut_shader_dump)')
    a = ap.parse_args()
    if a.shaders:
        import xxhash
        from renut_shader_dump import find_shader_headers
        os.makedirs(a.shaders, exist_ok=True)
    known = {n[:19] for n in os.listdir(a.known)} if a.known else set()
    shaders = {}
    image = open(a.image, 'rb').read()
    handles = {}
    seen = set()  # (archive, offset): the alternate tables mostly name the same files
    keys = collections.defaultdict(set)
    totals = collections.Counter()
    for name, table, table2, count in categories(image):
        if a.category and name not in a.category:
            continue
        for which, t in (('', table), ('alt', table2)):
            if not t:
                continue
            files = bad = 0
            for i, archive, flags, size, offset, stored in records(image, t, count):
                if not archive.endswith('.dat'):
                    continue  # movies, sounds and the like named by their own path
                if (archive, offset) in seen and not a.dump:
                    files += 1
                    continue
                seen.add((archive, offset))
                try:
                    data = read_file(a.game, archive, flags, size, offset, stored, handles)
                except Exception as e:  # noqa: BLE001
                    bad += 1
                    if bad <= 3:
                        print(f'  {name}{which}[{i}] {archive} flags {flags:03b}: {e}')
                    continue
                files += 1
                totals['bytes'] += len(data)
                for k in set(KEY.findall(data)):
                    keys[k.decode()].add(f'{name}{which}')
                for h in find_shader_headers(data, aligned=False) if a.shaders else ():
                    virt, phys = struct.unpack_from('>II', data, h + 4)
                    blob = data[h:h + virt + phys]
                    kind = 'vs' if struct.unpack_from('>I', data, h)[0] & 1 else 'ps'
                    n = f'{kind}_{xxhash.xxh3_64_hexdigest(blob)}'
                    if n not in shaders:
                        shaders[n] = f'{name}{which}[{i}]'
                        open(os.path.join(a.shaders, n + '.bin'), 'wb').write(blob)
                if a.dump:
                    os.makedirs(os.path.join(a.dump, name + which), exist_ok=True)
                    open(os.path.join(a.dump, name + which, f'{i:05d}.bin'), 'wb').write(data)
            print(f'{name}{which}: {count} records, {files} read, {bad} failed; keys so far {len(keys)}, '
                  f'containers so far {len(shaders)}', flush=True)
    print(f'{len(keys)} distinct shader keys in {totals["bytes"] / 1e6:.0f} MB')
    if a.shaders:
        vs = sum(n.startswith('vs') for n in shaders)
        print(f'{len(shaders)} distinct containers ({vs} vertex, {len(shaders) - vs} pixel)'
              + (f'; {len(known & set(shaders))} of the {len(known)} known, {len(set(shaders) - known)} new'
                 if a.known else ''))
        with open(os.path.join(a.shaders, 'sources.txt'), 'w', encoding='utf-8', newline='\n') as f:
            for n in sorted(shaders):
                f.write(f'{n} {shaders[n]}{" known" if n in known else ""}\n')
    if a.keys:
        with open(a.keys, 'w', encoding='utf-8', newline='\n') as f:
            for k in sorted(keys):
                f.write(f'{k} {",".join(sorted(keys[k]))}\n')


if __name__ == '__main__':
    sys.setrecursionlimit(10000)
    main()
