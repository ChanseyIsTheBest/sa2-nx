#!/usr/bin/env python3
"""Writes the shader cache of Renut's renderer for the Switch build from a PC build's.

The Switch build is a cross build: Renut's XenosRecomp (a host tool) runs on the PC, whose build generates
app/out/pc-me/renut_gpu/shader_cache.cpp (DXIL and SPIR-V). The console reads only the SPIR-V: this copy keeps the
DXIL symbols with empty arrays (the renderer references them; Windows alone reads them), so the NRO does not carry
the DXIL bytes. The compressed SPIR-V (about 4 MB) is written to a binary file next to it and embedded by the
assembler (.incbin): GCC needs minutes and gigabytes for an initializer list of millions of numbers.
Output: app/out/renut_switch/shader_cache_switch.cpp and spirv_cache.zst (cmake/banjo_renut_gpu.cmake,
BANJO_RENUT_SHADER_CACHE_CPP). The cache is game data: never committed or distributed apart from the NRO.

Usage: python tools/renut_switch_shader_cache.py [--pc app/out/pc-me]
       python tools/renut_switch_shader_cache.py --only <folder of shader captures> --out-dir app/out/renut_switch_dist

--only (2026-10-22, the distribution build): the table of those shaders alone (the ones the game makes at runtime,
app/out/pc-me/shader_capture, which no installer finds in the game's files) and no SPIR-V (the Switch build has only
the deko3d back end). banjo-nx-installer writes the rest of the table, with the GLSL, from the player's files into
deko3d_glsl.bin next to the NRO (banjo_renut_deko3d.cpp InstalledShaderTable).
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def numbers(line):
    return [int(v, 0) for v in re.findall(r'0x[0-9A-Fa-f]+|\d+', line)]


def only_table(text, only, out_dir):
    """The table of the shaders captured in `only`, without DXIL or SPIR-V."""
    wanted = {int(f[3:19], 16) for f in os.listdir(only) if re.fullmatch(r'(vs|ps)_[0-9A-Fa-f]{16}\.bin', f)}
    block = lambda name: re.search(name + r'\[\]\s*=\s*\{\n(.*?)\n\};', text, re.S).group(1).split('\n')
    entries = [numbers(line) for line in block('ShaderCacheEntry g_shaderCacheEntries')]
    inputs = [numbers(line) for line in block('ShaderVertexInput g_shaderVertexInputs')]
    literals = [numbers(line) for line in block('ShaderLiteral g_shaderLiterals')]
    if any(len(e) != 29 for e in entries) or any(len(i) != 7 for i in inputs) or any(len(l) != 5 for l in literals):
        sys.exit('renut_switch_shader_cache: the table is not the layout this tool reads (shader_cache.h, layout 7)')
    layout = re.search(r'extern const uint32_t g_shaderCacheLayout = (\d+);', text).group(1)
    out_entries, out_inputs, out_literals = [], [], []
    for e in entries:
        if e[0] not in wanted:
            continue
        vi_off, vi_n, lit_off, lit_n = e[24:28]
        e = e[:1] + [0, 0, 0, 0] + e[5:24] + [len(out_inputs), vi_n, len(out_literals), lit_n] + e[28:]
        out_inputs += inputs[vi_off:vi_off + vi_n]
        out_literals += literals[lit_off:lit_off + lit_n]
        out_entries.append(e)
    if len(out_entries) != len(wanted):
        sys.exit(f'renut_switch_shader_cache: {len(wanted) - len(out_entries)} of --only are not in the PC table')
    hx = lambda v: f'0x{v:X}'
    lines = ['#include "shader_cache.h"',
             "// banjo-nx: the distribution build's table (tools/renut_switch_shader_cache.py --only): the shaders the game",
             "// makes at runtime; the installer's deko3d_glsl.bin next to the NRO has the rest.",
             'ShaderCacheEntry g_shaderCacheEntries[] = {']
    for e in out_entries:
        lines.append(f'\t{{ {hx(e[0])}, 0, 0, 0, 0, {e[5]}, {hx(e[6])}, {hx(e[7])}, '
                     f'{{ {", ".join(map(hx, e[8:16]))} }}, {{ {", ".join(map(hx, e[16:24]))} }}, '
                     f'{e[24]}, {e[25]}, {e[26]}, {e[27]}, {hx(e[28])} }},')
    lines.append('};')
    lines.append('ShaderVertexInput g_shaderVertexInputs[] = {')
    lines += [f'\t{{ {", ".join(map(str, i))} }},' for i in out_inputs + [[0] * 7]]
    lines.append('};')
    lines.append('ShaderLiteral g_shaderLiterals[] = {')
    lines += [f'\t{{ {l[0]}, {{ {", ".join(map(hx, l[1:]))} }} }},' for l in out_literals + [[0] * 5]]
    lines.append('};')
    lines += ['const uint8_t g_compressedDxilCache[] = {0};', 'const size_t g_dxilCacheCompressedSize = 0;',
              'const size_t g_dxilCacheDecompressedSize = 0;', 'const uint8_t g_compressedSpirvCache[] = {0};',
              'const size_t g_spirvCacheCompressedSize = 0;', 'const size_t g_spirvCacheDecompressedSize = 0;',
              f'const size_t g_shaderCacheEntryCount = {len(out_entries)};',
              f'extern const uint32_t g_shaderCacheLayout = {layout};', '']
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, 'shader_cache_switch.cpp')
    open(out + '.tmp', 'w', encoding='utf-8').write('\n'.join(lines))
    os.replace(out + '.tmp', out)
    print(f'renut_switch_shader_cache: {len(out_entries)} of {len(entries)} shaders, {len(out_inputs)} vertex inputs, '
          f'{len(out_literals)} literals, no SPIR-V ({out})')


def main():
    pc = sys.argv[sys.argv.index('--pc') + 1] if '--pc' in sys.argv else os.path.join(ROOT, 'app', 'out', 'pc-me')
    src = os.path.join(pc, 'renut_gpu', 'shader_cache.cpp')
    if not os.path.isfile(src):
        sys.exit(f'renut_switch_shader_cache: no {src}: build the PC tree with BANJO_RENUT_RENDERER=ON first')
    text = open(src, encoding='utf-8', errors='strict').read()
    if '--only' in sys.argv:
        out_dir = sys.argv[sys.argv.index('--out-dir') + 1] if '--out-dir' in sys.argv else \
            os.path.join(ROOT, 'app', 'out', 'renut_switch_dist')
        return only_table(text, sys.argv[sys.argv.index('--only') + 1], out_dir)
    m = re.search(r'(const uint8_t g_compressedDxilCache\[\]\s*=\s*\{)(.*?)(\};)', text, re.S)
    if m:
        text = text[:m.start(2)] + '0' + text[m.end(2):]
        text = re.sub(r'(const size_t g_dxilCacheCompressedSize\s*=\s*)\d+', r'\g<1>0', text)
        text = re.sub(r'(const size_t g_dxilCacheDecompressedSize\s*=\s*)\d+', r'\g<1>0', text)

    out_dir = os.path.join(ROOT, 'app', 'out', 'renut_switch')
    os.makedirs(out_dir, exist_ok=True)
    m = re.search(r'const uint8_t g_compressedSpirvCache\[\]\s*=\s*\{(.*?)\};', text, re.S)
    if not m:
        sys.exit('renut_switch_shader_cache: no g_compressedSpirvCache in the PC shader cache')
    blob = bytes(int(v) for v in m.group(1).replace('\n', '').split(',') if v.strip())
    size = re.search(r'const size_t g_spirvCacheCompressedSize\s*=\s*(\d+)', text)
    if not size or int(size.group(1)) != len(blob):
        sys.exit('renut_switch_shader_cache: the SPIR-V array and its size disagree')
    blob_path = os.path.join(out_dir, 'spirv_cache.zst')
    open(blob_path + '.tmp', 'wb').write(blob)
    os.replace(blob_path + '.tmp', blob_path)
    incbin = blob_path.replace('\\', '/')
    asm = ('// banjo-nx: the compressed SPIR-V, embedded by the assembler (sha-256 ' +
           hashlib.sha256(blob).hexdigest()[:16] + ', ' + str(len(blob)) + ' bytes).\n'
           '__asm__(".section .rodata.banjo_renut_spirv,\\"a\\"\\n"\n'
           '        ".global g_compressedSpirvCache\\n"\n'
           '        ".type g_compressedSpirvCache, %object\\n"\n'
           '        ".balign 16\\n"\n'
           '        "g_compressedSpirvCache:\\n"\n'
           '        ".incbin \\"' + incbin + '\\"\\n"\n'
           '        ".size g_compressedSpirvCache, .-g_compressedSpirvCache\\n"\n'
           '        ".previous\\n");')
    text = text[:m.start()] + asm + text[m.end():]

    out = os.path.join(out_dir, 'shader_cache_switch.cpp')
    open(out + '.tmp', 'w', encoding='utf-8').write(text)
    os.replace(out + '.tmp', out)
    print(f'renut_switch_shader_cache: {os.path.getsize(src) >> 20} MB -> {os.path.getsize(out) >> 10} KB of source '
          f'+ {len(blob) >> 10} KB of SPIR-V ({out})')


if __name__ == '__main__':
    main()
