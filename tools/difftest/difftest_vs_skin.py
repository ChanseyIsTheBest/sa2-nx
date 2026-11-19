"""Differential test of the skinonce pass (2026-10-20, tools/deko3d_glsl_opt.py, constant 913): a fur vertex shader as
it was (A), and after the pass with 913 = 1 in the write mode (W: skins, stores the rows) and the read mode (R: loads
them), each compiled by uam (flags 1471), decompiled by Ryujinx's translator (shtool) and run here: A and W on the same
random bone memory, R on W's stores. Every output (gl_Position and the varyings) must be equal bit for bit in all three.

    python difftest_vs_skin.py <before.glsl> <after.glsl> [trials] [N=value ...]

Integers are 32-bit with the decompile's declared types (int / uint temps), floats rounded to float32 at every
assignment; bone words and constant-buffer words are a function of the trial's seed and their index (the same in every
program). The read and write modes are picked by replacing the shared word's test in the text (the branch is uniform)."""
import math
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import deko3d_glsl_opt as opt  # noqa: E402

UAM = r'D:/Banjo kazooie port/banjo-nx/out/deko3d_tools/uam-build/uam.exe'
SHTOOL = os.path.join(HERE, 'shtool', 'out', 'shtool.exe')
WORK = tempfile.mkdtemp(prefix='difftest_vs_')
M32 = 0xFFFFFFFF


def f32(x):
    if isinstance(x, bool) or not isinstance(x, float) or not math.isfinite(x):
        return x
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def U32(x):
    return int(x) & M32


def S32(x):
    x = int(x) & M32
    return x - (1 << 32) if x & 0x80000000 else x


def decompile(glsl):
    src = os.path.join(WORK, 'v.vert')
    out = os.path.join(WORK, 'v.dksh')
    with open(src, 'w', encoding='utf-8', newline='\n') as f:
        f.write(glsl)
    env = dict(os.environ, UAM_BANJO_FLAGS='1471')
    r = subprocess.run([UAM, '-s', 'vert', '-o', out, src], capture_output=True, text=True, env=env)
    if r.returncode:
        raise RuntimeError(r.stdout + r.stderr)
    d = open(out, 'rb').read()
    _, _, csz, codesz, poff, _ = struct.unpack_from('<6I', d, 0)
    _, entry, gprs, cb_off, cb_sz, _ = struct.unpack_from('<6I', d, poff)
    binp = os.path.join(WORK, 'v.bin')
    open(binp, 'wb').write(d[csz + entry: csz + (cb_off if cb_sz else codesz)] + b'\0' * 256)
    return subprocess.run([SHTOOL, binp], capture_output=True, text=True).stdout


# --- the decompile's operations ---------------------------------------------------------------------------------------

def INT(x):
    if isinstance(x, bool):
        return int(x)
    if isinstance(x, float):  # F2I: NaN 0, saturating, toward zero
        if math.isnan(x):
            return 0
        if x >= 2147483647:
            return 2147483647
        if x <= -2147483648:
            return -2147483648
        return int(x)
    return S32(x)


def UINT(x):
    if isinstance(x, bool):
        return int(x)
    if isinstance(x, float):
        if math.isnan(x) or x <= 0:
            return 0
        return min(int(x), M32)
    return U32(x)


def BFE(v, off, n, signed):
    off, n = int(off) & 0xFF, int(n) & 0xFF
    u = U32(v)
    if n == 0:
        return 0
    if off >= 32:
        val = 0 if not signed or not (u & 0x80000000) else M32
    else:
        n = min(n, 32 - off)
        val = (u >> off) & ((1 << n) - 1)
        if signed and val & (1 << (n - 1)):
            val -= 1 << n
    return S32(val) if signed else U32(val)


def fnan(x):
    return isinstance(x, float) and math.isnan(x)


def MIN(a, b):
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


def RSQ(x):
    if fnan(x) or x < 0:
        return math.nan
    if x == 0:
        return math.copysign(math.inf, x)
    return f32(1.0 / math.sqrt(x)) if math.isfinite(x) else 0.0


def SQRT(x):
    if fnan(x) or x < 0:
        return math.nan
    return f32(math.sqrt(x)) if math.isfinite(x) else x


def DIV(a, b):
    try:
        return f32(a / b)
    except ZeroDivisionError:
        if fnan(a) or a == 0:
            return math.nan
        return math.copysign(math.inf, a) * math.copysign(1.0, b)


def FMA(a, b, c):
    try:
        return f32(a * b + c)
    except (OverflowError, ValueError):
        return math.nan


def i2f(i):
    return struct.unpack('<f', struct.pack('<I', U32(i)))[0]


def f2i(x):
    return S32(bits(f32(x)))


def f2u(x):
    return bits(f32(x))


def FLOOR(x):
    return f32(float(math.floor(x))) if isinstance(x, float) and math.isfinite(x) else x


# --- transpiling ------------------------------------------------------------------------------------------------------

def _ternaries(e):
    while '?' in e:
        done = False
        for q in [m.start() for m in re.finditer(r'\?', e)]:
            t = opt._ternary_at(e, q)
            if not t:
                continue
            a, colon, b = t
            c, x, y = e[a:q], e[q + 1:colon], e[colon + 1:b]
            if '?' in c or '?' in x or '?' in y:
                continue
            e = e[:a] + f'(({x.strip()}) if ({c.strip()}) else ({y.strip()}))' + e[b:]
            done = True
            break
        if not done:
            raise RuntimeError('ternary: ' + e)
    return e


def conv(e, types):
    e = re.sub(r'\bvp_c(\d+)\.data\[(\d+)\]\.([xyzw])', lambda m: f'CB[({m.group(1)}, {m.group(2)}, "{m.group(3)}")]', e)
    e = re.sub(r'\bvp_s(\d+)\.data\[int\((\w+)\)\]', r'MEM(\1, \2)', e)
    e = re.sub(r'\bin_attr(\d+)\.([xyzw])', lambda m: f'IN[({m.group(1)}, "{m.group(2)}")]', e)
    e = re.sub(r'\bgl_VertexID\b', 'VID', e)

    def bfe(m):
        arg = m.group(1).strip()
        signed = types.get(arg) == 'int' or arg.startswith('int(')
        return f'BFE({arg}, {m.group(2)}, {m.group(3)}, {signed})'
    e = re.sub(r'\bbitfieldExtract\(((?:[^(),]|\([^()]*\))+),\s*([^,()]+),\s*([^,()]+)\)', bfe, e)
    e = _ternaries(e)
    e = re.sub(r'(?<![\w.])(0x[0-9A-Fa-f]+|\d+)u\b', r'\1', e)
    e = e.replace('&&', ' and ').replace('||', ' or ')
    e = re.sub(r'!(?!=)', ' not ', e)
    e = re.sub(r'\btrue\b', 'True', e)
    e = re.sub(r'\bfalse\b', 'False', e)
    e = re.sub(r'(?<![\w.])(\d+\.\d+(?:E[-+]?\d+)?)', r'F(\1)', e)
    e = re.sub(r'\bint\(', 'INT(', e)
    e = re.sub(r'\buint\(', 'UINT(', e)
    e = re.sub(r'\bfloat\(', 'F(', e)
    e = re.sub(r'\bfma\(', 'FMA(', e)
    e = re.sub(r'\bmin\(', 'MIN(', e)
    e = re.sub(r'\bmax\(', 'MAX(', e)
    e = re.sub(r'\binversesqrt\(', 'RSQ(', e)
    e = re.sub(r'\bsqrt\(', 'SQRT(', e)
    e = re.sub(r'\bfloor\(', 'FLOOR(', e)
    e = re.sub(r'\bintBitsToFloat\(', 'i2f(', e)
    e = re.sub(r'\buintBitsToFloat\(', 'i2f(', e)
    e = re.sub(r'\bfloatBitsToInt\(', 'f2i(', e)
    e = re.sub(r'\bfloatBitsToUint\(', 'f2u(', e)
    e = re.sub(r'\bisnan\(', 'fnan(', e)
    e = e.strip()
    if e.count(' / ') == 1:
        x, y = e.split(' / ')
        e = f'DIV({x}, {y})'
    return e


def transpile(code):
    types = dict((m.group(2), m.group(1)) for m in re.finditer(r'^\s*(?:precise )?(float|int|uint|bool) (temp_\d+);', code,
                                                               re.M))
    body = code[code.index('void main()'):]
    out, ind = [], 0
    for line in body.split('\n')[1:]:
        line = line.strip()
        if not line or line.startswith('//'):
            continue
        if re.match(r'^(precise )?(float|int|uint|bool|vec[234]) temp_\d+;$', line):
            continue
        if line == '{':
            ind += 1
            continue
        if line == '}':
            ind -= 1
            if ind < 1:
                break
            continue
        pad = '    ' * ind
        if line.startswith('if ('):
            out.append(pad + 'if ' + conv(line[3:], types) + ':')
            continue
        if line == 'else':
            out.append(pad + 'else:')
            continue
        if line == 'return;':
            out.append(pad + 'return')
            continue
        if re.match(r'^(while|for|do|switch|break|continue|discard)\b', line):
            raise RuntimeError('not supported in the decompile: ' + line)
        m = re.match(r'^vp_s(\d+)\.data\[int\((\w+)\)\] = (.*);$', line)
        if m:
            out.append(pad + f'STORE({m.group(1)}, {m.group(2)}, {conv(m.group(3), types)})')
            continue
        m = re.match(r'^(\w+(?:\.[xyzw])?) = (.*);$', line)
        if not m:
            raise RuntimeError('not understood: ' + line)
        target = m.group(1)
        rhs = conv(m.group(2), types)
        kind = types.get(target, 'float')
        wrap = {'float': 'R', 'int': 'S32', 'uint': 'U32', 'bool': 'bool'}[kind]
        out.append(pad + target.replace('.', '_') + f' = {wrap}({rhs})')
    fixed = []
    for k, l in enumerate(out):
        fixed.append(l)
        if l.rstrip().endswith(':'):
            ind_l = len(l) - len(l.lstrip())
            nxt = out[k + 1] if k + 1 < len(out) else ''
            if len(nxt) - len(nxt.lstrip()) <= ind_l or not nxt.strip():
                fixed.append(' ' * (ind_l + 4) + 'pass')
    names = sorted(set(re.findall(r'^\s*(\w+) = ', '\n'.join(fixed), re.M)))
    glob = ('    global ' + ', '.join(names) + '\n') if names else ''
    lines = '\n'.join('    ' + l[4:] if l.startswith('    ') else '    ' + l for l in fixed)
    return 'def main():\n' + glob + lines + '\n    return\n'


def R(x):
    if isinstance(x, float):
        return f32(x)
    if isinstance(x, int) and not isinstance(x, bool):
        return i2f(x)  # an integer into a float temp: the bits
    return x


class Keyed(dict):
    """Words as a function of the trial's seed and the key: half small bit fields, half floats (as bits)."""
    def __init__(self, seed, floats=True):
        super().__init__()
        self.seed = seed
        self.floats = floats

    def __missing__(self, key):
        rng = random.Random(zlib.crc32(repr((self.seed, key)).encode()))
        r = rng.random()
        if r < 0.4:
            v = rng.randrange(16) | (0x80000000 if rng.random() < 0.5 else 0)
        elif r < 0.9:
            v = bits(f32(rng.uniform(-2, 2)))
        else:
            v = rng.randrange(1 << 32)
        v = i2f(v) if self.floats else v
        self[key] = v
        return v


def run(py, inputs, cb, mem, vid, buffers):
    stores = {}

    def MEM(buf, index):
        role = buffers.get(int(buf), 'win')
        return mem[(role, U32(index))]

    def STORE(buf, index, value):
        stores[U32(index)] = U32(value) if not isinstance(value, float) else bits(f32(value))

    env = {'CB': cb, 'IN': inputs, 'VID': vid, 'MEM': MEM, 'STORE': STORE, 'F': lambda x: f32(float(x)),
           'FMA': FMA, 'INT': INT, 'UINT': UINT, 'BFE': BFE, 'MIN': MIN, 'MAX': MAX, 'RSQ': RSQ, 'SQRT': SQRT,
           'DIV': DIV, 'FLOOR': FLOOR, 'i2f': i2f, 'f2i': f2i, 'f2u': f2u, 'fnan': fnan, 'abs': abs, 'R': R,
           'S32': S32, 'U32': U32, 'bool': bool, 'undef': 0}
    exec(py, env)
    env['main']()
    outs = {}
    for k, v in env.items():
        if re.match(r'^(out_attr\d+|gl_Position)_[xyzw]$', k):
            outs[k] = bits(f32(v)) if isinstance(v, float) else U32(v)
    return outs, stores


class StrictSkin(dict):
    """The read program's memory: the write program's stores for the skin buffer (a load of another word is an error),
    the bone and vertex words for the others."""
    def __init__(self, stored, mem):
        super().__init__(stored)
        self.mem = mem

    def __missing__(self, key):
        if key[0] == 'skin':
            raise KeyError(f'the read program loads skin word {key[1]} the write program did not store')
        return self.mem[key]


def buffers_of(code, writes):
    """vp_sN -> 'skin' for the buffer the program stores to (W) or the only one it has when it reads rows (R)."""
    names = sorted(set(int(n) for n in re.findall(r'\bvp_s(\d+)\.data', code)))
    stored = set(int(n) for n in re.findall(r'^\s*vp_s(\d+)\.data\[[^\]]*\] = ', code, re.M))
    roles = {}
    for n in names:
        roles[n] = 'skin' if (n in stored or writes == 'read') else 'win'
    return roles


def prepare(text, defs, mode=None):
    if mode is not None:
        text = text.replace('g_UboShared.v[84u].z == 2u', 'true' if mode == 2 else 'false')
        text = text.replace('g_UboShared.v[84u].z == 1u', 'true' if mode == 1 else 'false')
    nl = text.index('\n') + 1
    text = text[:nl] + ''.join(f'#define SPIRV_CROSS_CONSTANT_ID_{k} {v}u\n' for k, v in defs) + text[nl:]
    code = decompile(text)
    return code, transpile(code)


def main():
    a, b = sys.argv[1], sys.argv[2]
    trials = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 200
    defs = [tuple(x.split('=')) for x in sys.argv[3:] if '=' in x]
    before = open(a, encoding='utf-8').read()
    after = open(b, encoding='utf-8').read()
    ca, pa = prepare(before, defs)
    cw, pw = prepare(after, defs + [('913', '1')], 1)
    cr, pr = prepare(after, defs + [('913', '1')], 2)
    ba, bw = buffers_of(ca, None), buffers_of(cw, 'write')
    # The read program's skin buffer: the one whose loads all hit words the write program stored (a program that
    # also fetches its vertex data from memory has the address window as well; the translator numbers buffers by use).
    rnames = sorted(set(int(n) for n in re.findall(r'\bvp_s(\d+)\.data', cr)))
    br = None
    if not any(v == 'skin' for v in bw.values()):
        print('MISMATCH: no skin buffer found in the write program')
        return 1
    for cand in rnames:
        roles = {n: ('skin' if n == cand else 'win') for n in rnames}
        rs = random.Random(5)
        inputs = {(k, c): f32(rs.uniform(-4, 40)) for k in range(32) for c in 'xyzw'}
        cb, mem = Keyed(77), Keyed(78, floats=False)
        _, stores = run(pw, inputs, cb, mem, 123, bw)
        try:
            run(pr, inputs, Keyed(77), StrictSkin({('skin', k): v for k, v in stores.items()}, mem), 123, roles)
            br = roles
            break
        except KeyError:
            continue
    print('buffers: before', ba, 'write', bw, 'read', br)
    if br is None:
        print('MISMATCH: no buffer of the read program reads what the write program stored')
        return 1
    random.seed(13)
    mism = 0
    for trial in range(trials):
        inputs = {(k, c): f32(random.uniform(-4, 40) if c in 'xyzw' else 0.0) for k in range(32) for c in 'xyzw'}
        seed = random.randrange(1 << 30)
        vid = random.randrange(0, 4000)
        cb = Keyed(seed)
        mem = Keyed(seed + 1, floats=False)
        oa, _ = run(pa, inputs, cb, mem, vid, ba)
        ow, stores = run(pw, inputs, cb, mem, vid, bw)
        rmem = {('skin', k): v for k, v in stores.items()}

        sm = StrictSkin(rmem, mem)
        orr, _ = run(pr, inputs, Keyed(seed), sm, vid, br)
        if len(stores) != 12:
            print(f'  trial {trial}: the write program stored {len(stores)} words, not 12')
            mism += 1
            continue
        bad = [k for k in oa if not (oa[k] == ow.get(k) == orr.get(k))]
        if bad or set(oa) != set(ow) or set(oa) != set(orr):
            mism += 1
            if mism <= 3:
                print('  trial', trial, 'differs in', bad[:6], [(oa.get(k), ow.get(k), orr.get(k)) for k in bad[:3]])
    print(f'{os.path.basename(b)}: {"OK" if not mism else "MISMATCH " + str(mism)} of {trials} '
          f'(outputs {len(oa)}, rows stored {len(stores)} words)')
    return 1 if mism else 0


if __name__ == '__main__':
    sys.exit(main())
