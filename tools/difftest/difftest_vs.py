"""Differential test of a GLSL pass on a whole game vertex shader (vivapinata-nx, 2026-11-12, the shader round): as
difftest_game.py for pixel shaders. The shader before and after, with the same specialization defines, compiled by uam
as a vertex stage (UAM_BANJO_FLAGS from DIFFTEST_FLAGS, 1471 being uam's default and the shader cache's), decompiled by
Ryujinx's translator (shtool), turned into Python and run on random inputs, constant buffers and vertex indices;
texture fetches (textureLod's level as part of the binding's key) a fixed function of the binding and the coordinates'
bits, the same in both. Every output the programs write (gl_Position, each out_attrN) is compared bit for bit.

    python difftest_vs.py <before.glsl> <after.glsl> [trials] [N=value ...] [--cbmask C:I-J:comps:MASK]
                          [--inputs LO:HI] [--vid LO:HI]

Not handled (the program raises and the trial is reported): memory stores, loops in the decompile, geometry outputs."""
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import types
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
_argv, sys.argv = sys.argv, sys.argv[:1]
import difftest_game as g  # noqa: E402  (it changes to a work folder of its own)
sys.argv = _argv
d = g.d

VID_RANGE = [0, 4096]


def decompile_vs(glsl, flags):
    with open('dv.vert', 'w', encoding='utf-8', newline='\n') as f:
        f.write(glsl)
    env = dict(os.environ, UAM_BANJO_FLAGS=str(flags))
    r = subprocess.run([d.UAM, '-s', 'vert', '-o', 'dv.dksh', 'dv.vert'], capture_output=True, text=True, env=env)
    if r.returncode:
        raise RuntimeError(r.stdout + r.stderr)
    b = open('dv.dksh', 'rb').read()
    _, _, csz, codesz, poff, _ = struct.unpack_from('<6I', b, 0)
    _, entry, gprs, cb_off, cb_sz, _ = struct.unpack_from('<6I', b, poff)
    open('dv.bin', 'wb').write(b[csz + entry: csz + (cb_off if cb_sz else codesz)] + b'\0' * 256)
    return subprocess.run([d.SHTOOL, 'dv.bin'], capture_output=True, text=True).stdout


def prepare_vs(path, defs):
    text = open(os.path.join(g.ORIG, path), encoding='utf-8').read()
    nl = text.index('\n') + 1
    text = text[:nl] + ''.join(f'#define SPIRV_CROSS_CONSTANT_ID_{k} {v}u\n' for k, v in defs) + text[nl:]
    code = decompile_vs(text, int(os.environ.get('DIFFTEST_FLAGS', '1471'), 0))
    # the vertex stage's names as the pixel harness knows them: its constant buffers and textures, the vertex index
    code = re.sub(r'\bvp_c(\d+)\b', r'fp_c\1', code)
    code = re.sub(r'\bvp_t_', 'fp_t_', code)
    code = re.sub(r'\bgl_VertexID\b', 'VID', code)
    code = re.sub(r'\bgl_InstanceID\b', 'IID', code)
    code = re.sub(r'\btextureLod\(', 'texture(', code)  # the level, a constant here, keys the binding as a bias does
    # a constant buffer read at a computed index (bones): CBX(buffer, index, component), from the trial's buffer
    code = re.sub(r'\bfp_c(\d+)\.data\[([^\[\]]+)\]\[([^\[\]]+)\]',
                  lambda m: f'CBX({m.group(1)}, {m.group(2)}, {m.group(3)})', code)  # and a computed component
    code = re.sub(r'\bfp_c(\d+)\.data\[([^\[\]]*[^\[\]\d][^\[\]]*)\]\.([xyzw])',
                  lambda m: f'CBX({m.group(1)}, {m.group(2)}, "{m.group(3)}")', code)
    # a storage buffer's word (vertex data fetched from memory): MEMR(buffer, index), a 32-bit word that is a function
    # of the trial and the word's place (the same in both programs)
    # renut-ultimate (2026-11-12, port 94): a store into a storage buffer (the translator's memory export path, in every
    # Beautiful Katamari vertex shader): MEMW(buffer, index, value), logged in order and compared as an output
    code = re.sub(r'^(\s*)vp_s(\d+)\.data\[([^\[\]]+)\] = (.+);$', r'\1memw_ = MEMW(\2, \3, \4);', code, flags=re.M)
    code = re.sub(r'\bvp_s(\d+)\.data\[([^\[\]]+)\]', lambda m: f'MEMR({m.group(1)}, {m.group(2)})', code)
    # the clip distances as outputs of their own (out_attr90 + N, compared with the others)
    code = re.sub(r'\bgl_ClipDistance\[(\d+)\]', lambda m: f'out_attr{90 + int(m.group(1))}.x', code)
    types = dict((n, t) for t, n in re.findall(r'^\s*(int|uint|float|bool) (temp_\d+);', code, re.M))

    def bfe(m):
        arg = m.group(1).strip()
        signed = types.get(arg) == 'int' or arg.startswith('int(') or arg.startswith('floatBitsToInt(')
        return f'BFE({arg}, {m.group(2)}, {m.group(3)}, {int(signed)})'
    code = re.sub(r'\bbitfieldExtract\(((?:[^(),]|\([^()]*\))+),\s*([^,()]+),\s*([^,()]+)\)', bfe, code)
    py = d.transpile(code)
    # a temp the program can read before it writes it (an undefined value in the shader, e.g. a format's unused
    # component): 0 in both programs, so the run does not stop there (an output that depends on it would differ anyway)
    decl = re.findall(r'^\s*(?:precise )?(float|int|uint|bool|[iub]?vec[234]) (temp_\d+);', code, re.M)
    init = ''.join(f'    {n} = {"(0.0, 0.0, 0.0, 0.0)[:" + t[-1] + "]" if "vec" in t else "0"}\n' for t, n in decl)
    py = py.replace('def main():\n', 'def main():\n', 1)
    gl = re.search(r'^    global .*\n', py, re.M)
    if gl and init:
        names = [n for _, n in decl]
        line = gl.group(0).rstrip('\n')
        missing = [n for n in names if not re.search(r'\b' + n + r'\b', line)]
        line = line + (', ' + ', '.join(missing) if missing else '') + '\n'
        py = py[:gl.start()] + line + init + py[gl.end():]
    wrap = {'int': 'WI', 'uint': 'WU'}
    py = re.sub(r'^(\s+)(\w+) = (.+)$', lambda m: f'{m.group(1)}{m.group(2)} = {wrap.get(types.get(m.group(2)), "R")}'
                f'({m.group(3)})' if not m.group(2) == 'global' else m.group(0), py, flags=re.M)
    return code, py


def main():
    import math
    args = sys.argv[1:]
    for flag, fn in (('--cbmask', g.parse_cbmask), ('--cbset', g.parse_cbset),
                     ('--inputs', lambda s: g.IN_RANGE.__setitem__(slice(None), [float(x) for x in s.split(':')])),
                     ('--vid', lambda s: VID_RANGE.__setitem__(slice(None), [int(x) for x in s.split(':')]))):
        while flag in args:
            i = args.index(flag)
            fn(args[i + 1])
            del args[i:i + 2]
    a, b = args[0], args[1]
    trials = int(args[2]) if len(args) > 2 and args[2].isdigit() else 300
    defs = [tuple(x.split('=')) for x in args[2:] if '=' in x]
    ca, pa = prepare_vs(a, defs)
    cb_, pb = prepare_vs(b, defs)
    print('statements', len(re.findall(r'^\s*\w+(?:\.[xyzw])? = ', ca, re.M)), '->',
          len(re.findall(r'^\s*\w+(?:\.[xyzw])? = ', cb_, re.M)))
    extra = {'sin': lambda x: g.f32(math.sin(x)) if math.isfinite(x) else math.nan,
             'cos': lambda x: g.f32(math.cos(x)) if math.isfinite(x) else math.nan,
             # two halves of a word (vertex data in half floats), each widened exactly
             'unpackHalf2x16': lambda u: types.SimpleNamespace(
                 x=struct.unpack('<e', struct.pack('<H', int(u) & 0xFFFF))[0],
                 y=struct.unpack('<e', struct.pack('<H', (int(u) >> 16) & 0xFFFF))[0]),
             # Ryujinx's helpers for IMUL.HI (the high 32 bits of a 32 x 32 multiply)
             'Helper_MultiplyHighU32': lambda a, b: ((int(a) & 0xFFFFFFFF) * (int(b) & 0xFFFFFFFF)) >> 32,
             'Helper_MultiplyHighS32': lambda a, b: g.WI((g.WI(int(a)) * g.WI(int(b))) >> 32)}
    random.seed(13)
    mism = 0
    stores = fetches = 0  # renut-ultimate (port 94)
    # vivapinata-nx (2026-11-12, its commit 57a8a63f, after RR6 found Beautiful Katamari's export shaders' outputs all
    # NaN or 0): an output whose value varied across the trials (two or more distinct finite values) is live
    seen = {}

    def texlog(out):
        def tex(name, coords, swz):
            out.append((name, tuple(d.bits(g.f32(float(c))) for c in coords)))
            return g.TEX(name, coords, swz)
        return tex
    for trial in range(trials):
        inputs = {(k, c): g.f32(random.uniform(g.IN_RANGE[0], g.IN_RANGE[1])) for k in range(32) for c in 'xyzw'}
        seed = random.randrange(1 << 30)
        vid = random.randint(VID_RANGE[0], VID_RANGE[1])
        cba, cbb = g.KeyedCB(seed), g.KeyedCB(seed)
        memr = lambda b, i, s=seed: random.Random(zlib.crc32(repr((s, 'mem', int(b), int(i))).encode())).getrandbits(32)
        wa, wb = [], []  # renut-ultimate (port 94): the stores, (buffer, index, value) in order
        # renut-ultimate (2026-11-12, port 94): every texture fetch, (binding, its coordinates' bits), compared as a
        # multiset (the GPU compiler issues fetches in its own order) as an output too: with random inputs most of a vertex shader's outputs can come out NaN or 0 in both programs
        # (Beautiful Katamari's), and a fetch's address is where a fetch pass would go wrong
        ta, tb = [], []
        ra = g.run(pa, inputs, cba, dict(extra, VID=vid, IID=0, TEX=texlog(ta),
                                         MEMW=lambda n, i, v: wa.append((int(n), int(i), int(v) & 0xFFFFFFFF)) or 0,
                                         MEMR=memr, CBX=lambda n, i, c: cba[(int(n), int(i) & 0xFFFF, c if isinstance(c, str) else 'xyzw'[int(c) & 3])]), True)
        rb = g.run(pb, inputs, cbb, dict(extra, VID=vid, IID=0, TEX=texlog(tb),
                                         MEMW=lambda n, i, v: wb.append((int(n), int(i), int(v) & 0xFFFFFFFF)) or 0,
                                         MEMR=memr, CBX=lambda n, i, c: cbb[(int(n), int(i) & 0xFFFF, c if isinstance(c, str) else 'xyzw'[int(c) & 3])]), True)
        stores += len(wa)
        fetches += len(ta)
        if isinstance(ra, tuple):
            for k, v in ra:
                if isinstance(v, int) and (v & 0x7F800000) != 0x7F800000:  # a finite float's bits
                    seen.setdefault(k, set()).add(v)
        if ra != rb or wa != wb or sorted(ta) != sorted(tb):
            mism += 1
            if mism <= 3:
                print('  trial', trial, 'vid', vid, [x for x in ra if x not in rb][:4] if isinstance(ra, tuple) else ra,
                      [x for x in rb if x not in ra][:4] if isinstance(rb, tuple) else rb)
                if sorted(ta) != sorted(tb):  # renut-ultimate (port 94): a fetch only one program made
                    print('  fetches', len(ta), '/', len(tb), [x for x in ta if x not in tb][:2],
                          [x for x in tb if x not in ta][:2])
    outs = len(ra) if isinstance(ra, tuple) else 0
    live = sum(1 for vals in seen.values() if len(vals) >= 2)
    verdict = 'OK' if not mism else 'MISMATCH ' + str(mism)
    if not mism and not live and not fetches and not stores:
        verdict = 'NOTHING LIVE'  # nothing compared varied: the run says nothing
    print(f'{os.path.basename(a)}: {verdict} of {trials} ({outs} outputs, {live} live, {fetches} fetches, '
          f'{stores} stores)')
    return 1 if mism else 0


if __name__ == '__main__':
    sys.exit(main())
