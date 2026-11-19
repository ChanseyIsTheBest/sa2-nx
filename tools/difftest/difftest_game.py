"""Differential test of a GLSL pass on a whole game shader (2026-10-20): the shader before and after, with the same
specialization defines, compiled by uam, decompiled by Ryujinx's translator (shtool), turned into Python and run on
random inputs and constant buffers; texture fetches are a fixed function of their binding and coordinates (the same in
both programs), the special functions Python's. Compares the outputs (and discard) bit for bit.

    python difftest_game.py <before.glsl> <after.glsl> [trials] [N=value ...]"""
import math
import os
import random
import re
import struct
import types
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ORIG = os.getcwd()
# difftest_ps compiles through dp.frag / dp.dksh / dp.bin in the working directory: one folder of its own per process,
# so runs in parallel do not read each other's files.
import tempfile  # noqa: E402
import atexit  # noqa: E402
import shutil  # noqa: E402
_WORK = tempfile.mkdtemp(prefix='difftest_')
os.chdir(_WORK)


def _remove_work():
    """renut-ultimate (2026-11-12, port 113): the run's folder removed when it ends (4,380 of them had piled up in
    %TEMP% since 2026-10-20). A run killed from outside (a timeout) still leaves its own."""
    os.chdir(ORIG)
    shutil.rmtree(_WORK, ignore_errors=True)


atexit.register(_remove_work)
_argv, sys.argv = sys.argv, sys.argv[:1]
import difftest_ps as d  # noqa: E402
sys.argv = _argv

f32 = d.f32


def fnan(x):
    return isinstance(x, float) and math.isnan(x)


def MIN(a, b):  # Maxwell FMNMX: a NaN operand gives the other
    if fnan(a):
        return b
    if fnan(b):
        return a
    return a if a < b else b


def MAX(a, b):
    if fnan(a):
        return b
    if fnan(b):
        return a
    return a if a > b else b


def CLAMP(x, lo, hi):
    return MIN(MAX(x, lo), hi)


def DIV(a, b):
    try:
        return f32(a / b)
    except ZeroDivisionError:
        if fnan(a) or a == 0:
            return math.nan
        return math.copysign(math.inf, a) * math.copysign(1.0, b)


def SQRT(x):
    if fnan(x) or x < 0:
        return math.nan
    return f32(math.sqrt(x)) if math.isfinite(x) else x


def RSQ(x):
    if fnan(x) or x < 0:
        return math.nan
    if x == 0:
        return math.copysign(math.inf, x)
    return f32(1.0 / math.sqrt(x)) if math.isfinite(x) else 0.0


def LOG2(x):
    if fnan(x) or x < 0:
        return math.nan
    if x == 0:
        return -math.inf
    return f32(math.log2(x)) if math.isfinite(x) else x


def EXP2(x):
    if fnan(x):
        return x
    try:
        return f32(2.0 ** x)
    except OverflowError:
        return math.inf


def ROUNDEVEN(x):
    return f32(float(round(x))) if math.isfinite(x) else x


def INT(x):  # F2I: NaN 0, saturating
    if isinstance(x, bool):
        return int(x)
    if isinstance(x, int):
        return x
    if fnan(x):
        return 0
    if x >= 2147483647:
        return 2147483647
    if x <= -2147483648:
        return -2147483648
    return int(x)


def UINT(x):
    if isinstance(x, float):
        if fnan(x) or x <= 0:
            return 0
        if x >= 4294967295.0:  # vivapinata-nx (2026-11-12): +inf too (F2U saturates)
            return 0xFFFFFFFF
        return int(x)
    return int(x) & 0xFFFFFFFF


TEX_ALIAS = {}  # vivapinata-nx (2026-11-12): --texalias B=A, binding B (a suffix of its name, e.g. 1A5) reads binding A's texels
TEX_COPY = {}   # vivapinata-nx (2026-11-12): --texcopy B:y=x, binding B's component y is its component x


def TEX(name, coords, swz):
    """A texel as a fixed function of the binding and the coordinates (their bits)."""
    base = name.split('@')[0]
    for b, a in TEX_ALIAS.items():
        if base.endswith(b):
            name = name.replace(base, base[:-len(b)] + a)
            base = name.split('@')[0]
            break
    key = name.encode() + b''.join(struct.pack('<I', d.bits(f32(float(c)))) for c in coords)
    h = zlib.crc32(key)
    vals = []
    for k in range(4):
        h = zlib.crc32(struct.pack('<I', h) + bytes([k]))
        vals.append(f32((h & 0xFFFF) / 65535.0))
    for b, (dst, src) in TEX_COPY.items():
        if base.endswith(b):
            vals['xyzw'.index(dst)] = vals['xyzw'.index(src)]
    out = tuple(vals['xyzw'.index(c) if c in 'xyzw' else 'rgba'.index(c)] for c in swz)
    return out[0] if len(out) == 1 else out


def TSIZE(name, swz):
    h = zlib.crc32(name.encode())
    vals = [16 << (h % 5), 16 << ((h >> 3) % 5), 1 + (h >> 6) % 4, 1]
    out = tuple(vals['xyzw'.index(c) if c in 'xyzw' else 'rgba'.index(c)] for c in swz)
    return out[0] if len(out) == 1 else out


def conv(e):
    # texture(NAME, vecN(a, b, ...)).swz and textureSize(NAME, 0).swz before the generic conversion
    # vivapinata-nx (2026-11-12): texture(NAME, vecN(...), bias): the bias (a constant) part of the binding's key
    def tex(m):
        name = m.group(1) + (f'@{m.group(4).strip()}' if m.group(4) else '')
        return f'TEX("{name}", ({m.group(3)},), "{m.group(5) or "xyzw"}")'
    e = re.sub(r'\btexture\((\w+), vec([234])\(((?:[^()]|\([^()]*\)|\((?:[^()]|\([^()]*\))*\))*)\)'
               r'(?:,\s*([0-9.eE+-]+))?\)(?:\.([xyzwrgba]+))?', tex, e)
    e = re.sub(r'\btextureSize\((\w+), 0\)\.([xyzwrgba]+)', lambda m: f'TSIZE("{m.group(1)}", "{m.group(2)}")', e)
    e = re.sub(r'\b(temp_\d+)\.([xyzw])\b', lambda m: f'{m.group(1)}[{"xyzw".index(m.group(2))}]', e)
    e = re.sub(r'(?<![\w.])(0x[0-9A-Fa-f]+|\d+)u\b', r'\1', e)
    e = _base_conv(e)
    e = re.sub(r'\bclamp\(', 'CLAMP(', e)
    e = re.sub(r'\bmin\(', 'MIN(', e)
    e = re.sub(r'\bmax\(', 'MAX(', e)
    e = re.sub(r'\binversesqrt\(', 'RSQ(', e)
    e = re.sub(r'\bsqrt\(', 'SQRT(', e)
    e = re.sub(r'\blog2\(', 'LOG2(', e)
    e = re.sub(r'\bexp2\(', 'EXP2(', e)
    e = re.sub(r'\broundEven\(', 'ROUNDEVEN(', e)
    e = re.sub(r'\bint_\(', 'INT(', e)
    e = re.sub(r'\buint_\(', 'UINT(', e)
    e = re.sub(r'^(F\(1\.0\)|[\w.\[\]]+) / ([\w.\[\]]+)$', r'DIV(\1, \2)', e.strip())
    return e


_base_conv = d.conv
d.conv = conv  # transpile() calls the module's conv


FRONT = [True]


class KeyedCB(dict):
    """Constant buffer words as a function of the trial's seed and the word's key (difftest_ps.RandomCB draws them in
    the order the program reads them, so two programs reading in another order saw other constants)."""
    def __init__(self, seed):
        super().__init__()
        self.seed = seed

    def __missing__(self, key):
        rng = random.Random(zlib.crc32(repr((self.seed, key)).encode()))
        if rng.random() < 0.5:
            v = d.intBitsToFloat(rng.randrange(16) | (0x80000000 if rng.random() < 0.5 else 0))
        else:
            v = f32(rng.uniform(-2, 2))
        # vivapinata-nx (2026-11-12): a word the renderer always writes with some bits clear (--cbmask C:I:comps:MASK,
        # e.g. a deko3d sampler word's bits 0-12): half of the draws random words, all ANDed with the mask
        if key in CB_SETS:  # 2026-11-12: --cbset C:I:comp:VALUE, a word the renderer gives as a constant too
            v = d.intBitsToFloat(CB_SETS[key])
            self[key] = v
            return v
        mask = CB_MASKS.get(key)
        if mask is not None:
            raw = d.bits(v) if rng.random() < 0.5 else rng.getrandbits(32)
            v = d.intBitsToFloat(raw & mask)
        self[key] = v
        return v


CB_MASKS = {}
CB_SETS = {}


def parse_cbset(arg):
    c, i, comp, value = arg.split(':')
    CB_SETS[(int(c), int(i), comp)] = int(value, 0)
IN_RANGE = [-2.0, 2.0]


def parse_cbmask(arg):
    """--cbmask C:I[-J]:comps:MASK: constant buffer C (the decompile's fp_c number), words I to J, those components."""
    c, idx, comps, mask = arg.split(':')
    lo, _, hi = idx.partition('-')
    for i in range(int(lo), int(hi or lo) + 1):
        for comp in comps:
            CB_MASKS[(int(c), i, comp)] = int(mask, 0) & CB_MASKS.get((int(c), i, comp), 0xFFFFFFFF)


def run(py, inputs, cb, extra=None, all_outputs=False):
    """vivapinata-nx (2026-11-12): extra, more names for the program (difftest_vs.py's VID); all_outputs, every
    out_attrN and gl_Position component the program wrote (a vertex shader's), else out_attr0."""
    env = {'GLF': {'x': 10.5, 'y': 20.5, 'z': 0.5, 'w': 1.0}, 'CB': cb, 'IN': inputs, 'F': d.F, 'FMA': d.FMA,
           'Discard': d.Discard, 'INT': INT, 'UINT': UINT, 'floatBitsToInt': d.floatBitsToInt,
           'intBitsToFloat': d.intBitsToFloat, 'undef': 0,
           'floor': lambda x: f32(float(math.floor(x))) if math.isfinite(x) else x,
           'trunc': lambda x: f32(float(math.trunc(x))) if math.isfinite(x) else x,
           'ceil': lambda x: f32(float(math.ceil(x))) if math.isfinite(x) else x,
           'uintBitsToFloat': d.intBitsToFloat, 'floatBitsToUint': lambda x: d.bits(f32(x)), 'abs': abs,
           'MIN': MIN, 'MAX': MAX, 'CLAMP': CLAMP, 'DIV': DIV, 'SQRT': SQRT, 'RSQ': RSQ, 'LOG2': LOG2, 'EXP2': EXP2,
           'ROUNDEVEN': ROUNDEVEN, 'TEX': TEX, 'TSIZE': TSIZE, 'R': R, 'gl_FrontFacing': FRONT[0],
           'dFdx': lambda x: 0.0, 'dFdy': lambda x: 0.0, 'dFdxFine': lambda x: 0.0, 'dFdyFine': lambda x: 0.0,
           'support_buffer': types.SimpleNamespace(render_scale=[1.0] * 73),
           # 2026-10-20 (SA2's report): helpers other ports' decompiles call
           'isnan': lambda x: isinstance(x, float) and math.isnan(x),
           'TextureSizeUnscale': lambda size, index: size, 'BFE': BFE, 'WI': WI, 'WU': WU,
           # vivapinata-nx (2026-11-12): Ryujinx's helper for SHFL (a quad's derivative): one pixel whose quad-mates
           # hold the same values (as dFdx = 0 above), so the neighbour's value is its own
           'ShuffleXor': lambda v, mask, c, valid=None: v}
    lmem = {}
    env['CBX'] = lambda n, i, c: cb[(int(n), int(i) & 0xFFFF, c if isinstance(c, str) else 'xyzw'[int(c) & 3])]
    env['LMEMSET'] = lambda i, v: lmem.__setitem__(int(i) & 0xFFFFFFFF, v)
    env['LMEMGET'] = lambda i: lmem.get(int(i) & 0xFFFFFFFF, 0)
    if extra:
        env.update(extra)
    exec(py, env)
    try:
        env['main']()
    except d.Discard:
        return 'discard'
    except ZeroDivisionError:
        # 2026-10-20 (SA2's report): a division the transpiler did not wrap in DIV (an operand that is not a plain
        # name); reported as its own outcome, so a mismatch shows when only one program gets there.
        return 'zerodiv'
    if all_outputs:
        return tuple((k, d.bits(f32(v)) if isinstance(v, float) else v) for k, v in sorted(env.items())
                     if re.match(r'^(out_attr\d+|gl_Position)_[xyzw]$', k))
    return tuple(d.bits(f32(env.get(f'out_attr0_{c}', 0.0))) for c in 'xyzw')


def prepare(path, defs):
    text = open(os.path.join(ORIG, path), encoding='utf-8').read()
    nl = text.index('\n') + 1
    text = text[:nl] + ''.join(f'#define SPIRV_CROSS_CONSTANT_ID_{k} {v}u\n' for k, v in defs) + text[nl:]
    # vivapinata-nx (2026-11-12): DIFFTEST_FLAGS, the uam flags (1471 is uam's default, what the shader cache uses)
    code = d.decompile(text, int(os.environ.get('DIFFTEST_FLAGS', '447'), 0))
    # vivapinata-nx (2026-11-12): BFE (uam's flags 1471 make them), signed when its operand is an int (as
    # difftest_vs_skin.py): BFE(value, offset, bits, signed)
    types = dict((n, t) for t, n in re.findall(r'^\s*(int|uint|float|bool) (temp_\d+);', code, re.M))

    def bfe(m):
        arg = m.group(1).strip()
        signed = types.get(arg) == 'int' or arg.startswith('int(') or arg.startswith('floatBitsToInt(')
        return f'BFE({arg}, {m.group(2)}, {m.group(3)}, {int(signed)})'
    code = re.sub(r'\bbitfieldExtract\(((?:[^(),]|\([^()]*\))+),\s*([^,()]+),\s*([^,()]+)\)', bfe, code)
    # vivapinata-nx (2026-11-12): local memory (an array the shader indexes at run time): a word store becomes
    # LMEMSET(index, value), a load LMEMGET(index), on a dictionary of its own each run
    code = re.sub(r'^(\s*)local_memory\[([^\]]+)\] = (.*);$', r'\1lmem_store = LMEMSET(\2, \3);', code, flags=re.M)
    code = re.sub(r'local_memory\[([^\]]+)\]', r'LMEMGET(\1)', code)
    # a constant buffer read at a computed index (and component): CBX(buffer, index, component) on the trial's words
    code = re.sub(r'\bfp_c(\d+)\.data\[([^\[\]]+)\]\[([^\[\]]+)\]', r'CBX(\1, \2, \3)', code)
    code = re.sub(r'\bfp_c(\d+)\.data\[([^\[\]]*[^\[\]\d][^\[\]]*)\]\.([xyzw])', r'CBX(\1, \2, "\3")', code)
    py = d.transpile(code)
    # vivapinata-nx (2026-11-12): a temp the program can read before it writes it (an undefined value in the shader, or a
    # helper's `out` argument): 0 in both programs
    decl = re.findall(r'^\s*(?:precise )?(float|int|uint|bool|[iub]?vec[234]) (temp_\d+);', code, re.M)
    init = ''.join(f'    {n} = {"(0.0, 0.0, 0.0, 0.0)[:" + t[-1] + "]" if "vec" in t else "0"}\n' for t, n in decl)
    gl = re.search(r'^    global .*\n', py, re.M)
    if gl and init:
        line = gl.group(0).rstrip('\n')
        missing = [n for _, n in decl if not re.search(r'\b' + n + r'\b', line)]
        py = py[:gl.start()] + line + (', ' + ', '.join(missing) if missing else '') + '\n' + init + py[gl.end():]
    # every instruction's result is a float32 on the GPU: round each assignment (ints, bools and tuples pass through)
    # vivapinata-nx (2026-11-12): and an int or uint temp wraps at 32 bits as the GPU's (Python's ints do not: a
    # decompiled `x << 6` kept every bit, so `(x << 6) >> 26`, the exponent adjust's extract, came out wrong whenever
    # the word had bits above 25: a false mismatch between a program that reads the word and one that does not)
    wrap = {'int': 'WI', 'uint': 'WU'}
    py = re.sub(r'^(\s+)(\w+) = (.+)$', lambda m: f'{m.group(1)}{m.group(2)} = {wrap.get(types.get(m.group(2)), "R")}'
                f'({m.group(3)})' if not m.group(2) == 'global' else m.group(0), py, flags=re.M)
    return code, py


def WI(x):
    if isinstance(x, bool) or not isinstance(x, int):
        return R(x)
    x &= 0xFFFFFFFF
    return x - (1 << 32) if x >> 31 else x


def WU(x):
    if isinstance(x, bool) or not isinstance(x, int):
        return R(x)
    return x & 0xFFFFFFFF


def BFE(value, offset, bits, signed):
    v = (int(value) & 0xFFFFFFFF) >> int(offset)
    field = v & ((1 << int(bits)) - 1) if int(bits) < 32 else v
    if signed and int(bits) and field >> (int(bits) - 1) & 1:
        field -= 1 << int(bits)
    return field


def R(x):
    if isinstance(x, float):
        return f32(x)
    if isinstance(x, tuple):
        return tuple(f32(v) if isinstance(v, float) else v for v in x)
    return x


def main():
    args = sys.argv[1:]
    # vivapinata-nx (2026-11-12): --inputs LO:HI, the range of the random inputs (-2:2 by default); a block that runs only
    # for coordinates in [0, 1] is reached in every trial with 0:1
    while '--inputs' in args:
        i = args.index('--inputs')
        IN_RANGE[:] = [float(x) for x in args[i + 1].split(':')]
        del args[i:i + 2]
    while '--cbmask' in args:
        i = args.index('--cbmask')
        parse_cbmask(args[i + 1])
        del args[i:i + 2]
    # vivapinata-nx (2026-11-12): --cbset C:I:comp:VALUE (parse_cbset was there, the option was not read here: pinned
    # constant words did nothing; difftest_vs.py reads it itself)
    while '--cbset' in args:
        i = args.index('--cbset')
        parse_cbset(args[i + 1])
        del args[i:i + 2]
    while '--texalias' in args:  # vivapinata-nx (2026-11-12): --texalias B=A (binding name suffixes)
        i = args.index('--texalias')
        b, a = args[i + 1].split('=')
        TEX_ALIAS[b] = a
        del args[i:i + 2]
    while '--texcopy' in args:  # vivapinata-nx (2026-11-12): --texcopy B:y=x
        i = args.index('--texcopy')
        b, comp = args[i + 1].split(':')
        dst, src = comp.split('=')
        TEX_COPY[b] = (dst, src)
        del args[i:i + 2]
    sys.argv[1:] = args
    a, b = sys.argv[1], sys.argv[2]
    trials = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 300
    defs = [tuple(x.split('=')) for x in sys.argv[3:] if '=' in x]
    ca, pa = prepare(a, defs)
    cb_, pb = prepare(b, defs)
    print('statements', len(re.findall(r'^\s*\w+(?:\.[xyzw])? = ', ca, re.M)), '->',
          len(re.findall(r'^\s*\w+(?:\.[xyzw])? = ', cb_, re.M)))
    random.seed(11)
    mism = disc = 0
    for trial in range(trials):
        inputs = {(k, c): f32(random.uniform(IN_RANGE[0], IN_RANGE[1])) for k in range(32) for c in 'xyzw'}
        seed = random.randrange(1 << 30)
        ra = run(pa, inputs, KeyedCB(seed))
        rb = run(pb, inputs, KeyedCB(seed))
        disc += ra == 'discard'
        if ra != rb:
            mism += 1
            if mism <= 3:
                print('  trial', trial, ra, rb)
    print(f'{os.path.basename(a)}: {"OK" if not mism else "MISMATCH " + str(mism)} of {trials} ({disc} discarded)')
    return 1 if mism else 0


if __name__ == '__main__':
    sys.exit(main())
