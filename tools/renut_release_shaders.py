#!/usr/bin/env python3
"""katamari-nx (2026-11-02, futari-nx's and peggle2-nx's tools/renut_release_shaders.py): the Switch build's Renut shader
files without translated game shaders, for a build that can be distributed (app/out/renut_switch_release/), and the
file the web installer makes in their place, for tests.

    python tools/renut_release_shaders.py [--glsl out/deko3d_shaders/glsl_opt] [--cache app/out/renut_switch/shader_cache_switch.cpp]

Writes into app/out/renut_switch_release/:
  shader_cache_switch.cpp   the shader table (numbers: hashes, fetch masks, vertex input formats) without the SPIR-V
                            (the deko3d back end never reads it; the PC's Vulkan one does)
  sd/deko3d_glsl.bin        the package of the game's shaders, which the web installer makes from the player's files and
                            puts beside the NRO (this one is the Python tools' copy: game data, for tests only, never
                            distributed)
Every one of Beautiful Katamari's 195 shaders is a container stored in its files (111 in default.xex, 84 in the models
of d02-d05.dat), so no GLSL package is built into the NRO: app/cmake/banjo_renut_gpu.cmake
(BANJO_RENUT_RELEASE_SHADERS) builds with these files.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def package(glsl_dir, names, out_dir):
    """tools/deko3d_shader_package.py over the given shaders only."""
    os.makedirs(out_dir, exist_ok=True)
    src = os.path.join(out_dir, 'glsl')
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    for n in names:
        shutil.copyfile(os.path.join(glsl_dir, n), os.path.join(src, n))
    subprocess.run([sys.executable, '-P', os.path.join(ROOT, 'tools', 'deko3d_shader_package.py'), '--glsl', src,
                    '--out', out_dir], check=True)
    shutil.rmtree(src)


def strip_spirv(text):
    """shader_cache_switch.cpp without the SPIR-V: the assembler's .incbin goes, its sizes are 0, and every entry's
    SPIR-V offset and size (the 4th and 5th numbers of g_shaderCacheEntries' lines) are 0."""
    text, n = re.subn(r'// banjo-nx: the compressed SPIR-V.*?\.previous\\n"\);\n',
                      'const uint8_t g_compressedSpirvCache[] = {0};  // no SPIR-V (deko3d reads none)\n',
                      text, flags=re.S)
    if n != 1:
        sys.exit('renut_release_shaders: the SPIR-V block of shader_cache_switch.cpp not found')
    text = re.sub(r'(g_spirvCache(?:Compressed|Decompressed)Size\s*=\s*)\d+', r'\g<1>0', text)

    def entry(m):
        fields = m.group(2).split(', ')
        fields[3] = fields[4] = '0'
        return m.group(1) + ', '.join(fields)
    text, n = re.subn(r'^(\t\{ )(0x[0-9A-F]+, \d+, \d+, \d+, \d+)', entry, text, flags=re.M)
    if not n:
        sys.exit('renut_release_shaders: no entry of g_shaderCacheEntries found')
    if 'incbin' in text:
        sys.exit('renut_release_shaders: an .incbin is left in the shader table')
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--glsl', default=os.path.join(ROOT, 'out', 'deko3d_shaders', 'glsl_opt'))
    ap.add_argument('--cache', default=os.path.join(ROOT, 'app', 'out', 'renut_switch', 'shader_cache_switch.cpp'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'app', 'out', 'renut_switch_release'))
    a = ap.parse_args()
    names = sorted(f for f in os.listdir(a.glsl) if re.fullmatch(r'(vs|ps)_[0-9A-F]{16}\.glsl', f))
    if not names:
        sys.exit(f'renut_release_shaders: no shaders in {a.glsl}')
    os.makedirs(a.out, exist_ok=True)
    package(a.glsl, names, os.path.join(a.out, 'sd'))
    os.remove(os.path.join(a.out, 'sd', 'deko3d_glsl_switch.cpp'))  # the installer's file is never built in
    text = open(a.cache, encoding='utf-8').read()
    with open(os.path.join(a.out, 'shader_cache_switch.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(strip_spirv(text))
    print(f'renut_release_shaders: no shader built in, beside the NRO {len(names)} game shaders -> {a.out}')


if __name__ == '__main__':
    main()
