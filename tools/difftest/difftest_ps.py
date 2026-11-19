"""Differential test of uam's fragment input interpolation cache (banjo flag 8): pixel shaders with branches and loops
compiled with UAM_BANJO_FLAGS 7 and 15, decompiled by Ryujinx's translator, run in Python (float32 rounding through
struct) on random inputs and constants, outputs compared bit for bit."""
import os, re, struct, subprocess, sys, random, math
sys.path.insert(0, r'D:/Banjo kazooie port/banjo-nx/tools')
import deko3d_glsl_opt as opt

HERE = os.path.dirname(os.path.abspath(__file__))
UAM = r'D:/Banjo kazooie port/banjo-nx/out/deko3d_tools/uam-build/uam.exe'
SHTOOL = os.path.join(HERE, 'shtool', 'out', 'shtool.exe')
# vivapinata-nx (2026-11-12): this repo's uam (the one the shader cache uses) and, while shtool is not built here,
# banjo-nx's build of it (the same program); DIFFTEST_UAM / DIFFTEST_SHTOOL name others.
_VIVA_UAM = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'out', 'deko3d_tools', 'uam-build', 'uam.exe')
UAM = os.environ.get('DIFFTEST_UAM') or (_VIVA_UAM if os.path.isfile(_VIVA_UAM) else UAM)
if not os.path.isfile(SHTOOL):
    SHTOOL = r'D:/Banjo kazooie port/banjo-nx/tools/difftest/shtool/out/shtool.exe'
SHTOOL = os.environ.get('DIFFTEST_SHTOOL') or SHTOOL
HEAD = '''#version 460
layout(binding = 2, std140) uniform S { uvec4 v[84]; } g_UboShared;
layout(binding = 1, std140) uniform P { vec4 v[256]; } g_UboPixel;
layout(location = 0) in vec4 t0;
layout(location = 1) in vec4 t1;
layout(location = 2) in vec4 t2;
layout(location = 0) out vec4 o;
'''


def f32(x):
    if isinstance(x, bool):
        return x
    if not math.isfinite(x):
        return x
    try:
        return struct.unpack('<f', struct.pack('<f', x))[0]
    except OverflowError:
        return math.copysign(math.inf, x)


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def decompile(glsl, flags):
    with open('dp.frag', 'w', encoding='utf-8', newline='\n') as f:
        f.write(glsl)
    env = dict(os.environ, UAM_BANJO_FLAGS=str(flags))
    r = subprocess.run([UAM, '-s', 'frag', '-o', 'dp.dksh', 'dp.frag'], capture_output=True, text=True, env=env)
    if r.returncode:
        raise RuntimeError(r.stdout + r.stderr)
    d = open('dp.dksh', 'rb').read()
    _, _, csz, codesz, poff, _ = struct.unpack_from('<6I', d, 0)
    _, entry, gprs, cb_off, cb_sz, _ = struct.unpack_from('<6I', d, poff)
    open('dp.bin', 'wb').write(d[csz + entry: csz + (cb_off if cb_sz else codesz)] + b'\0' * 256)
    return subprocess.run([SHTOOL, 'dp.bin'], capture_output=True, text=True).stdout


def conv(e):
    while '?' in e:
        done = False
        for q in [mm.start() for mm in re.finditer(r'\?', e)]:
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
    e = re.sub(r'\bfp_c(\d+)\.data\[(\d+)\]\.([xyzw])', lambda m: f'CB[({m.group(1)}, {m.group(2)}, "{m.group(3)}")]', e)
    e = re.sub(r'\bin_attr(\d+)\.([xyzw])', lambda m: f'IN[({m.group(1)}, "{m.group(2)}")]', e)
    e = re.sub(r'\bgl_FragCoord\.([xyzw])', lambda m: f'GLF["{m.group(1)}"]', e)
    e = re.sub(r'\bfma\(', 'FMA(', e)
    e = e.replace('&&', ' and ').replace('||', ' or ')
    e = re.sub(r'!(?!=)', ' not ', e)
    e = re.sub(r'\btrue\b', 'True', e)
    e = re.sub(r'\bfalse\b', 'False', e)
    e = re.sub(r'(?<![\w.])(\d+\.\d+(?:E[-+]?\d+)?)', r'F(\1)', e)
    e = re.sub(r'\b(int|uint)\(', r'\1_(', e)
    e = re.sub(r'\bfloat\(', 'F(', e)
    return e


def transpile(code):
    body = code[code.index('void main()'):]
    out, ind = [], 0
    # vivapinata-nx (2026-11-12): each `do` paired with its `while (cond);` (the line after the block the do opens), so
    # the loop can test cond at the top of every pass after the first: a `continue` then tests it too, as in GLSL
    src = [l.strip() for l in body.split('\n')[1:]]
    src = [l for l in src if l and not l.startswith('//')]
    do_cond = {}
    for k, l in enumerate(src):
        if l == 'do' and k + 1 < len(src) and src[k + 1] == '{':
            depth, j = 0, k + 1
            while j < len(src):
                depth += src[j] == '{'
                depth -= src[j] == '}'
                if depth == 0:
                    break
                j += 1
            mw = re.match(r'^while \((.*)\);$', src[j + 1]) if j + 1 < len(src) else None
            if not mw:
                raise RuntimeError('a do without its while')
            do_cond[k] = mw.group(1)
    for k_line, line in enumerate(src):
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
            out.append(pad + 'if ' + conv(line[3:]) + ':')
            continue
        if line == 'else':
            out.append(pad + 'else:')
            continue
        if line in ('return;', 'discard;'):
            out.append(pad + ('raise Discard()' if line == 'discard;' else 'return'))
            continue
        # vivapinata-nx (2026-11-12): `do { ... break; ... } while (true);`, the form Ryujinx's translator gives a loop
        # left by a break (a tessellated vertex shader's division correction): a Python `while True:`
        if line == 'do':
            cond = do_cond[k_line]
            if cond == 'true':
                out.append(pad + 'while True:')
            else:  # a pass after the first starts with the test (where the body's end or a continue left it)
                out.append(pad + f'dw_{k_line} = False')
                out.append(pad + 'while True:')
                out.append(pad + f'    if dw_{k_line} and not ({conv(cond)}):')
                out.append(pad + '        break')
                out.append(pad + f'    dw_{k_line} = True')
            continue
        if re.match(r'^while \((.*)\);$', line):  # its do has it
            continue
        if line in ('break;', 'continue;'):
            out.append(pad + line[:-1])
            continue
        if re.match(r'^(while|for|do|switch|break|continue)\b', line):
            raise RuntimeError('loop in the decompile: ' + line)
        # renut-ultimate (2026-11-12, port 94): the decompile's local memory (a vertex shader's constants copied for an
        # indexed read: Beautiful Katamari's every vertex shader), a dict whose unwritten words read 0
        ml = re.match(r'^local_memory\[(.*)\] = (.*);$', line)
        if ml:
            out.append(pad + f'local_memory[int({conv(ml.group(1))})] = ' + conv(ml.group(2)))
            continue
        m = re.match(r'^(\w+(?:\.[xyzw])?) = (.*);$', line)
        if not m:
            raise RuntimeError('not understood: ' + line)
        out.append(pad + m.group(1).replace('.', '_') + ' = ' + conv(m.group(2)))
    if 'local_memory' in body:  # renut-ultimate (port 94): see above
        out.insert(0, "    local_memory = type('LocalMemory', (dict,), {'__missing__': lambda self, k: 0})()")
    # an if / else whose body was only comments
    fixed = []
    for k, l in enumerate(out):
        fixed.append(l)
        if l.rstrip().endswith(':'):
            ind_l = len(l) - len(l.lstrip())
            nxt = out[k + 1] if k + 1 < len(out) else ''
            if len(nxt) - len(nxt.lstrip()) <= ind_l or not nxt.strip():
                fixed.append(' ' * (ind_l + 4) + 'pass')
    out = fixed
    names = sorted(set(re.findall(r'^\s*(\w+) = ', '\n'.join(out), re.M)))
    glob = ('    global ' + ', '.join(names) + '\n') if names else ''
    lines = '\n'.join('    ' + l[4:] if l.startswith('    ') else '    ' + l for l in out)
    return 'def main():\n' + glob + lines + '\n    return\n'


class Discard(Exception):
    pass


class RandomCB(dict):
    """Constant buffer words made on first read (the same for both programs of a trial): half small bit fields
    (with the top bit sometimes), half ordinary floats."""
    def __init__(self, rng):
        super().__init__()
        self.rng = rng

    def __missing__(self, key):
        if self.rng.random() < 0.5:
            v = intBitsToFloat(self.rng.randrange(16) | (0x80000000 if self.rng.random() < 0.5 else 0))
        else:
            v = f32(self.rng.uniform(-2, 2))
        self[key] = v
        return v


def F(x):
    return f32(float(x))


def FMA(a, b, c):
    return f32(a * b + c)


def int_(x):
    if isinstance(x, bool):
        return int(x)
    return int(x)


def uint_(x):
    return int(x) & 0xFFFFFFFF


def floatBitsToInt(x):
    v = bits(f32(x))
    return v - (1 << 32) if v & 0x80000000 else v


def intBitsToFloat(i):
    return struct.unpack('<f', struct.pack('<I', int(i) & 0xFFFFFFFF))[0]


def run(py, inputs, cb):
    env = {'GLF': {'x': 10.5, 'y': 20.5, 'z': 0.5, 'w': 1.0}, 'CB': cb, 'IN': inputs, 'F': F, 'FMA': FMA, 'Discard': Discard, 'int_': int_, 'uint_': uint_,
           'floatBitsToInt': floatBitsToInt, 'intBitsToFloat': intBitsToFloat, 'undef': 0x7FC00001, 'floor': lambda x: f32(float(math.floor(x))) if math.isfinite(x) else x, 'trunc': lambda x: f32(float(math.trunc(x))) if math.isfinite(x) else x, 'ceil': lambda x: f32(float(math.ceil(x))) if math.isfinite(x) else x, 'uintBitsToFloat': intBitsToFloat,
           'floatBitsToUint': lambda x: bits(f32(x)), 'abs': abs, 'min': min, 'max': max}
    exec(py, env)
    try:
        env['main']()
    except Discard:
        return 'discard'
    return tuple(bits(f32(env.get(f'out_attr0_{c}', 0.0))) for c in 'xyzw')


TESTS = {
    'branches': '''void main(){ vec4 r = t0 * 2.0;
        if (t1.x > 0.5) { r += t0 * t1; if (t2.y > 0.25) r.x += t0.y; else r.y -= t0.x; }
        else { r -= t0.zwxy; }
        if (t2.x > 0.5) r.w = t0.w + t1.w; o = r; }''',
    'deep': '''void main(){ vec4 r = t2;
        if (t0.x > 0.3) { if (t0.y > 0.3) { if (t0.z > 0.3) r = t1 * t0; else r = t1; } r += t0; }
        else if (t0.w > 0.6) { r = t1 - t0; } o = r * t0.x; }''',
    'reread': '''void main(){ vec4 r = vec4(0.0);
        if (t1.y > 0.0) { r.x = t0.x; } else { r.x = -t0.x; }
        if (t1.z > 0.0) { r.y = t0.x * 3.0; r.z = t0.y; } r.w = t0.x + t0.y; o = r; }''',
    'discard': '''void main(){ if (t2.w < -0.5) discard; vec4 r = t0;
        if (t1.x > 0.0) { if (t0.y < 0.0) discard; r = r * t0.y; } o = r + t0; }''',
    'dupdiamond': '''void main(){ vec4 r = vec4(0.0); bool c = (g_UboShared.v[8].y & 0x80000000u) != 0u;
        float a; if (c) { a = t0.x * t1.y + 0.5; } else { a = t0.z - t1.w; }
        r.x = a * 2.0; if (t2.x > 0.0) r.y = 1.0;
        float b; if (c) { b = t0.x * t1.y + 0.5; } else { b = t0.z - t1.w; }
        r.z = b + t2.z; r.w = a - b; o = r; }''',
    'nested': '''void main(){ vec4 r = vec4(0.0); uint u = g_UboShared.v[8].y;
        float a; if ((u & 1u) != 0u) { float k; if ((u & 2u) != 0u) k = t0.x + 1.0; else k = floor(t0.y * 3.0); a = k * t1.x; } else { a = t1.z; }
        if (a > 0.25) r.y = a; else r.y = -a;
        float b; if ((u & 1u) != 0u) { float k; if ((u & 2u) != 0u) k = t0.x + 1.0; else k = floor(t0.y * 3.0); b = k * t1.x; } else { b = t1.z; }
        r.x = b * t2.x; r.z = a + b; o = r; }''',
    'notequiv': '''void main(){ vec4 r = vec4(0.0); uint u = g_UboShared.v[8].y;
        float a; if ((u & 1u) != 0u) { a = t0.x + t1.x; } else { a = t0.y; }
        float b; if ((u & 2u) != 0u) { b = t0.x + t1.x; } else { b = t0.y; }
        float c; if ((u & 1u) == 0u) { c = t0.x + t1.x; } else { c = t0.y; }
        r.x = a; r.y = b; r.z = c; r.w = a * b - c; o = r; }''',
    'discmid': '''void main(){ float a = t0.x * t1.x + t2.x; if (a < -0.5) discard;
        vec4 r = vec4(a); if (t1.y > 0.0) { r.y = t0.x * t1.x + t2.x + 1.0; } r.z = (t0.x * t1.x + t2.x) * 0.5; o = r; }''',
    'swapside': '''void main(){ vec4 r = vec4(0.0); bool c = t2.y > 0.0;
        float a; if (c) a = t0.x * 2.0; else a = t1.x;
        float b; if (!c) b = t1.x; else b = t0.x * 2.0;
        float d; if (!c) d = t0.x * 2.0; else d = t1.x;
        r.x = a; r.y = b; r.z = d; r.w = a + b + d; o = r; }''',
    'uniformdup': '''void main(){ vec4 r = vec4(0.0); uint u = g_UboShared.v[9].x;
        float a; if ((u & 4u) != 0u) a = uintBitsToFloat(g_UboShared.v[10].z) * t0.x; else a = 0.0;
        if (t1.x > 0.0) { r.x = a + 1.0; }
        float b; if ((u & 4u) != 0u) b = uintBitsToFloat(g_UboShared.v[10].z) * t0.x; else b = 0.0;
        r.y = b; r.z = a * b; o = r; }''',
}


FA, FB = (int(sys.argv[1]), int(sys.argv[2])) if len(sys.argv) > 2 else (7, 15)


def main():
    random.seed(7)
    bad = 0
    for name, body in TESTS.items():
        glsl = HEAD + body + '\n'
        try:
            c7, c15 = decompile(glsl, FA), decompile(glsl, FB)
            p7, p15 = transpile(c7), transpile(c15)
        except RuntimeError as e:
            print(f'{name:9s} SKIPPED: {str(e)[:150]}')
            continue
        ipa7, ipa15 = c7.count(' Ipa'), c15.count(' Ipa')
        mism = 0
        for trial in range(500):
            inputs = {(k, c): f32(random.uniform(-1, 1)) for k in range(4) for c in 'xyzw'}
            seed = random.randrange(1 << 30)
            if run(p7, inputs, RandomCB(random.Random(seed))) != run(p15, inputs, RandomCB(random.Random(seed))):
                mism += 1
        n7, n15 = len(re.findall(r'^\s*\w+(?:\.[xyzw])? = ', c7, re.M)), len(re.findall(r'^\s*\w+(?:\.[xyzw])? = ', c15, re.M))
        print(f'{name:11s} {"OK" if not mism else "MISMATCH " + str(mism)}  (IPA {ipa7} -> {ipa15}, statements {n7} -> {n15})')
        bad += mism > 0
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
