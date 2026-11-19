"""Compile one GLSL stage with uam and print its Maxwell opcodes in order (with counts).
Usage: python vs_ops.py <glsl> <stage vert|frag> [define=value ...] [--seq]"""
import collections
import os
import struct
import subprocess
import sys
import tempfile

ROOT = 'D:/Banjo kazooie port/banjo-nx'
sys.path.insert(0, ROOT + '/tools')
import deko3d_shader_stats as st  # noqa: E402

st.TABLE = st.opcode_table()
path, stage = sys.argv[1], sys.argv[2]
defs = [a for a in sys.argv[3:] if '=' in a]
seq = '--seq' in sys.argv
text = open(path, 'rb').read()
nl = text.index(b'\n') + 1
extra = b''.join(b'#define SPIRV_CROSS_CONSTANT_ID_%s %su\n' % tuple(d.encode().split(b'=')) for d in defs)
text = text[:nl] + extra + text[nl:]
with tempfile.TemporaryDirectory() as tmp:
    src = os.path.join(tmp, 'v.glsl')
    out = os.path.join(tmp, 'v.dksh')
    open(src, 'wb').write(text)
    r = subprocess.run([st.UAM, '-s', stage, '-o', out, src], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit(1)
    dksh = open(out, 'rb').read()
s = st.dksh_stats(dksh)
print('instr', s['instr'], 'gprs', s['gprs'], 'classes', s['classes'])
print(sorted(s['ops'].items(), key=lambda kv: -kv[1]))
if seq:
    magic, header_sz, control_sz, code_sz, programs_off, num_programs = struct.unpack_from('<6I', dksh, 0)
    ptype, entry, gprs, cb_off, cb_sz, scratch = struct.unpack_from('<6I', dksh, programs_off)
    start = control_sz + entry + 0x50
    end = control_sz + (cb_off if cb_sz else code_sz)
    code = dksh[start:end]
    names = []
    for g in range(0, len(code) - 31, 32):
        for i in range(1, 4):
            lo, hi = struct.unpack_from('<II', code, g + i * 8)
            names.append('-' if (lo == 0 and hi == 0) else st.classify(hi))
    line = []
    for n in names:
        line.append(n)
        if len(line) == 16:
            print(' '.join(line))
            line = []
    print(' '.join(line))
