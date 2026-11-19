#!/usr/bin/env python3
"""banjo-nx (deko3d renderer, docs/deko3d-renderer.md section 5): the game shaders' uam GLSL, packaged for the NRO.

    python tools/deko3d_shader_package.py [--glsl out/deko3d_shaders/glsl_opt] [--out app/out/renut_switch]

Input: tools/deko3d_shaders.py's GLSL after tools/deko3d_glsl_opt.py (glsl_opt, 2026-10-18: one <vs|ps>_<HASH>.glsl per
shader, HASH the shader container's hash as Renut's shader cache keys it). Output: deko3d_glsl.bin, embedded in the NRO by deko3d_glsl_switch.cpp (.incbin, as
tools/renut_switch_shader_cache.py does with the SPIR-V). Each shader is compressed alone with a dictionary trained on
all of them, so the console unpacks only the ones it compiles variants of (all of them unpacked would be 158 MB).

deko3d_glsl.bin, little endian:
  header (32 bytes): "BDKG", version 1, count, dictionary size, table offset, data offset, 2 x 0
  dictionary (zstd, trained by zstd --train)
  table, sorted by hash, 56 bytes an entry: u64 hash, u32 data offset, u32 compressed size, u32 size, u32 stage
    (0 vertex, 4 fragment: DkStage), u32 x 8 the boolean specialization constants (ids 1000..1255) the shader has
  data: one zstd frame per shader (dictionary compressed)
The package is game data: the build tree and the NRO only.

--only <folder of shader captures>: only those shaders (the distribution build, 2026-10-22: the NRO carries the
shaders the game makes at runtime, app/out/pc-me/shader_capture, which no installer finds in the game's files;
banjo-nx-installer makes the rest from the player's files, read by the NRO from deko3d_glsl.bin beside it).
"""
import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZSTD = r'C:\devkitPro\msys2\usr\bin\zstd.exe'


def fwd(p):
    return p.replace('\\', '/')


def raw_frame(data):
    """One zstd frame of raw blocks (RFC 8878): single segment, 4-byte content size, no checksum, no dictionary id
    (banjo-nx-installer's lib/package.js zstdRawFrame, from bejeweled3-nx-installer)."""
    out = bytearray(struct.pack('<IBI', 0xFD2FB528, (2 << 6) | (1 << 5), len(data)))
    blocks = max(1, -(-len(data) // (128 << 10)))
    for b in range(blocks):
        chunk = data[b * (128 << 10):(b + 1) * (128 << 10)]
        header = (1 if b == blocks - 1 else 0) | (len(chunk) << 3)
        out += struct.pack('<I', header)[:3] + chunk
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--glsl', default=os.path.join(ROOT, 'out', 'deko3d_shaders', 'glsl_opt'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'app', 'out', 'renut_switch'))
    ap.add_argument('--level', default='19')
    ap.add_argument('--only', help='a folder of shader captures (<vs|ps>_<hash>.bin): package only those')
    ap.add_argument('--raw', action='store_true',
                    help='no dictionary, each GLSL in a zstd frame of raw blocks (the distribution build: the '
                         "installer's page reads them out of the NRO without a zstd decoder)")
    a = ap.parse_args()
    names = sorted(f for f in os.listdir(a.glsl) if re.fullmatch(r'(vs|ps)_[0-9A-F]{16}\.glsl', f))
    if a.only:
        wanted = {f[:-4].upper().replace('VS_', 'vs_').replace('PS_', 'ps_') + '.glsl' for f in os.listdir(a.only)
                  if re.fullmatch(r'(vs|ps)_[0-9A-Fa-f]{16}\.bin', f)}
        missing = wanted - set(names)
        if missing:
            sys.exit(f'deko3d_shader_package: {len(missing)} of --only have no GLSL in {a.glsl}, e.g. {sorted(missing)[0]}')
        names = sorted(wanted)
    if not names:
        sys.exit(f'deko3d_shader_package: no shaders in {a.glsl}')
    os.makedirs(a.out, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=a.out) as tmp:
        if a.only:
            # The dictionary is trained on the shaders packaged, so they alone go to a folder of their own.
            chosen = os.path.join(tmp, 'glsl')
            os.makedirs(chosen)
            for n in names:
                shutil.copyfile(os.path.join(a.glsl, n), os.path.join(chosen, n))
            a.glsl = chosen
        files = [fwd(os.path.relpath(os.path.join(a.glsl, n))) for n in names]
        zdir = fwd(os.path.relpath(os.path.join(tmp, 'z')))
        if a.raw:
            dictionary = b''
        else:
            dict_path = fwd(os.path.relpath(os.path.join(tmp, 'dict')))
            # zstd wants 10x (better 100x) the dictionary's size to train on: a small --only package gets a smaller one.
            raw = sum(os.path.getsize(os.path.join(a.glsl, n)) for n in names)
            maxdict = max(4096, min(131072, raw // 100))
            subprocess.run([ZSTD, '--train', '-q', f'--maxdict={maxdict}', '-o', dict_path, '-r',
                            fwd(os.path.relpath(a.glsl))], check=True)
            os.makedirs(zdir, exist_ok=True)
            for i in range(0, len(files), 300):
                subprocess.run([ZSTD, f'-{a.level}', '-q', '-f', '-D', dict_path, '--output-dir-flat', zdir] +
                               files[i:i + 300], check=True)
            dictionary = open(dict_path, 'rb').read()
        entries = []
        data = bytearray()
        for n in names:
            text = open(os.path.join(a.glsl, n), 'rb').read()
            comp = raw_frame(text) if a.raw else open(os.path.join(zdir, n + '.zst'), 'rb').read()
            h = int(n[3:19], 16)
            stage = 4 if n.startswith('ps_') else 0
            mask = [0] * 8
            for m in re.finditer(rb'#define SPIRV_CROSS_CONSTANT_ID_(\d+) ', text):
                i = int(m.group(1))
                if 1000 <= i < 1256:
                    mask[(i - 1000) // 32] |= 1 << ((i - 1000) % 32)
            entries.append((h, len(data), len(comp), len(text), stage, mask))
            data += comp
            data += b'\0' * (-len(data) % 8)
    entries.sort(key=lambda e: e[0])
    header_size = 32
    dict_off = header_size
    table_off = dict_off + len(dictionary)
    table_off += -table_off % 8
    data_off = table_off + 56 * len(entries)
    out = bytearray(struct.pack('<4sIIIIIII', b'BDKG', 1, len(entries), len(dictionary), table_off, data_off, 0, 0))
    out += dictionary
    out += b'\0' * (table_off - len(out))
    for h, off, csize, size, stage, mask in entries:
        out += struct.pack('<QIIII8I', h, off, csize, size, stage, *mask)
    out += data
    bin_path = os.path.join(a.out, 'deko3d_glsl.bin')
    open(bin_path + '.tmp', 'wb').write(out)
    os.replace(bin_path + '.tmp', bin_path)
    incbin = fwd(os.path.abspath(bin_path))
    digest = hashlib.sha256(out).hexdigest()[:16]
    cpp = ('// banjo-nx (deko3d renderer): the game shaders\' uam GLSL (tools/deko3d_shader_package.py), embedded by the\n'
           f'// assembler. sha-256 {digest}, {len(out)} bytes, {len(entries)} shaders. Game data: build tree and NRO only.\n'
           '#include <cstddef>\n#include <cstdint>\n'
           '__asm__(".section .rodata.banjo_deko3d_glsl,\\"a\\"\\n"\n'
           '        ".global g_deko3dGlslPackage\\n"\n'
           '        ".type g_deko3dGlslPackage, %object\\n"\n'
           '        ".balign 16\\n"\n'
           '        "g_deko3dGlslPackage:\\n"\n'
           '        ".incbin \\"' + incbin + '\\"\\n"\n'
           '        ".size g_deko3dGlslPackage, .-g_deko3dGlslPackage\\n"\n'
           '        ".previous\\n");\n'
           f'extern const size_t g_deko3dGlslPackageSize = {len(out)};\n')
    cpp_path = os.path.join(a.out, 'deko3d_glsl_switch.cpp')
    open(cpp_path + '.tmp', 'w', encoding='utf-8').write(cpp)
    os.replace(cpp_path + '.tmp', cpp_path)
    raw = sum(e[3] for e in entries)
    print(f'deko3d_shader_package: {len(entries)} shaders, {raw >> 20} MB of GLSL -> {len(out) >> 10} KB '
          f'(dictionary {len(dictionary) >> 10} KB) at {bin_path}')


if __name__ == '__main__':
    main()
