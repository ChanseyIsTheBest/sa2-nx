#!/usr/bin/env python3
"""banjo-nx (2026-10-18, docs/deko3d-shader-optimisations.md): GLSL passes for uam, after tools/deko3d_glsl.py.

    python tools/deko3d_glsl_opt.py --in out/deko3d_shaders/glsl --out out/deko3d_shaders/glsl_opt [--passes a,b,...]
    python tools/deko3d_glsl_opt.py --list

uam is Mesa's GLSL compiler and nouveau's codegen (GLSL IR -> TGSI -> nv50 IR), not NIR: what NAK and the D3D
compilers fold for themselves it often leaves in. Each pass rewrites a form of SPIRV-Cross's output (of Renut's
XenosRecomp SPIR-V) into one that uam compiles to less code, with the same results. tools/deko3d_shader_stats.py
measures each (--passes there too). The passes and why are in PASSES below and in the document.

Only the game shaders (vs_/ps_<hash>.glsl) are rewritten; the text is what the console's uam compiles, so the
DKSH cache (tools/deko3d_variant_cache.py --glsl <out>) and the package (tools/deko3d_shader_package.py --glsl <out>)
take the output directory. Game data: the build tree only.
"""
import argparse
import os
import re
import types
import sys

# ---------------------------------------------------------------------------------------------------------------------
# helpers


def matching(text, open_index, open_ch, close_ch):
    depth = 0
    for i in range(open_index, len(text)):
        c = text[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
    return -1


def main_span(text):
    """(start, end) of main()'s body, or None."""
    m = re.search(r'\nvoid main\(\)\n\{', text)
    if not m:
        return None
    open_i = m.end() - 1
    close_i = matching(text, open_i, '{', '}')
    if close_i < 0:
        return None
    return open_i + 1, close_i


def insert_before_main(text, code):
    at = text.find('\nvoid main()')
    if at < 0:
        return text
    return text[:at + 1] + code + text[at + 1:]


def insert_globals(text, code):
    """After the #version line and the banjo-dk / extension / define lines that follow it."""
    lines = text.split('\n')
    k = 1
    while k < len(lines) and (lines[k].startswith('//') or lines[k].startswith('#extension') or
                              lines[k].startswith('#define dkh')):
        k += 1
    lines[k:k] = code.rstrip('\n').split('\n')
    return '\n'.join(lines)


def map_main(text, fn):
    span = main_span(text)
    if not span:
        return text
    a, b = span
    return text[:a] + fn(text[a:b]) + text[b:]


# ---------------------------------------------------------------------------------------------------------------------
# pass: sel — `c ? a : b` -> dk_sel(c, a, b)

SEL_TYPES = [('float', 'mix(b, a, c)'), ('vec2', 'mix(b, a, bvec2(c))'), ('vec3', 'mix(b, a, bvec3(c))'),
             ('vec4', 'mix(b, a, bvec4(c))'), ('int', 'mix(b, a, c)'), ('ivec2', 'mix(b, a, bvec2(c))'),
             ('ivec3', 'mix(b, a, bvec3(c))'), ('ivec4', 'mix(b, a, bvec4(c))'), ('uint', 'mix(b, a, c)'),
             ('uvec2', 'mix(b, a, bvec2(c))'), ('uvec3', 'mix(b, a, bvec3(c))'), ('uvec4', 'mix(b, a, bvec4(c))'),
             ('bool', 'mix(b, a, c)'), ('bvec2', 'mix(b, a, bvec2(c))'), ('bvec3', 'mix(b, a, bvec3(c))'),
             ('bvec4', 'mix(b, a, bvec4(c))')]
SEL_HELPERS = ''.join(f'{t} dk_sel(bool c, {t} a, {t} b) {{ return {body}; }}\n' for t, body in SEL_TYPES)

def _is_assignment(s, i):
    """The '=' at i is an assignment (=, +=, <<=, ...), not part of ==, !=, <= or >=."""
    if i + 1 < len(s) and s[i + 1] == '=':
        return False  # the first of '=='
    prev = s[i - 1] if i > 0 else ''
    if prev in ('=', '!'):
        return False  # '==' or '!='
    if prev in ('<', '>'):
        return i > 1 and s[i - 2] == prev  # '<<=' / '>>=' assign, '<=' / '>=' compare
    return True  # '=' or a compound assignment


def _ternary_at(s, q):
    """For the '?' at q: (start, colon, end) of the conditional expression, or None."""
    # condition: back to the enclosing bracket, a comma, an assignment, a ternary operator, or the statement start
    depth = 0
    i = q - 1
    while i >= 0:
        c = s[i]
        if c in ')]':
            depth += 1
        elif c in '([':
            if depth == 0:
                break
            depth -= 1
        elif depth == 0:
            if c in ',;{}?:':
                break
            if c == '=' and _is_assignment(s, i):
                break
        i -= 1
    start = i + 1
    if re.match(r'\s*return\b', s[start:q]):
        start = start + s[start:q].index('return') + len('return')
    # then-part: to the ':' that pairs with this '?'
    depth = 0
    pending = 0
    j = q + 1
    colon = -1
    while j < len(s):
        c = s[j]
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
            if depth < 0:
                return None
        elif depth == 0:
            if c == '?':
                pending += 1
            elif c == ':':
                if pending == 0:
                    colon = j
                    break
                pending -= 1
            elif c in ',;':
                return None
        j += 1
    if colon < 0:
        return None
    # else-part: to the enclosing bracket's close, a comma, ';', or a ':' of an enclosing conditional
    depth = 0
    pending = 0
    k = colon + 1
    while k < len(s):
        c = s[k]
        if c in '([':
            depth += 1
        elif c in ')]':
            if depth == 0:
                break
            depth -= 1
        elif depth == 0:
            if c in ',;':
                break
            if c == '?':
                pending += 1
            elif c == ':':
                if pending == 0:
                    break
                pending -= 1
        k += 1
    return start, colon, k


def pass_sel(text):
    """Every conditional expression a select: uam turns a ?: whose operands are not plain expressions (another ?:, a
    vector) into an if/else (SSY, branches, SYNC and moves), and a mix() with a bool selector into SEL/ICMP. The two
    operands of SPIRV-Cross's ?: are values SPIR-V's OpSelect computed both of anyway."""
    count = 0

    def body(s):
        nonlocal count
        lines = s.split('\n')
        for n, line in enumerate(lines):
            if '?' not in line or line.lstrip().startswith('#') or line.lstrip().startswith('//'):
                continue
            guard = 0
            while '?' in line and guard < 400:
                guard += 1
                done = False
                # the innermost first: a '?' whose three parts hold no other '?'
                for q in [m.start() for m in re.finditer(r'\?', line)]:
                    t = _ternary_at(line, q)
                    if t is None:
                        continue
                    start, colon, end = t
                    cond, a, b = line[start:q], line[q + 1:colon], line[colon + 1:end]
                    if '?' in cond or '?' in a or '?' in b:
                        continue
                    lead = cond[:len(cond) - len(cond.lstrip())]
                    trail = b[len(b.rstrip()):]
                    line = (line[:start] + lead + f'dk_sel({cond.strip()}, {a.strip()}, {b.strip()})' + trail +
                            line[end:])
                    count += 1
                    done = True
                    break
                if not done:
                    break
            lines[n] = line
        return '\n'.join(lines)

    out = map_main(text, body)
    if count:
        out = insert_before_main(out, SEL_HELPERS)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# pass: alpha — the alpha test function as specialization constant 903

ALPHA_WORD = 'g_UboShared.v[34u].w'
ALPHA_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_903\n#define SPIRV_CROSS_CONSTANT_ID_903 0u\n#endif\n'
              'const uint g_banjoAlphaFunc = SPIRV_CROSS_CONSTANT_ID_903;\n'
              '#define DK_ALPHA_FUNC ((g_banjoAlphaFunc != 0u) ? (g_banjoAlphaFunc - 1u) : g_UboShared.v[34u].w)\n')


def pass_alpha(text):
    """The alpha test's compare function (SharedConstants.alpha_test_func) as specialization constant 903, its value
    + 1 (0: read at run time): the pipeline key has it (render/pipeline.cpp), so a draw without the alpha test (7,
    always) has no compare chain and no discard, and a pixel shader without discard and depth output keeps the GPU's
    early depth test (the SPH's kills_pixels bit is off). The early-alpha block (translator: the quad's alpha test
    before the texture fetches) folds away with it."""
    if ALPHA_WORD not in text:
        return text
    span = main_span(text)
    if not span:
        return text
    a, b = span
    body = text[a:b].replace(ALPHA_WORD, 'DK_ALPHA_FUNC')
    text = text[:a] + body + text[b:]
    return insert_before_main(text, ALPHA_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: scale1 — every texture and target at the console's resolution

SCALE_CHAIN_RE = re.compile(
    r'\((\w+) == 1u\) \? 0\.666666686534881591796875 : \(\(\1 == 2u\) \? 1\.5 : \(\(\1 == 3u\) \? 2\.0 : '
    r'\(\(\1 == 4u\) \? 3\.0 : \(\(\1 == 5u\) \? 4\.0 : \(\(\1 == 6u\) \? 6\.0 : '
    r'\(\(\1 == 7u\) \? 1\.33333337306976318359375 : 1\.0\)\)\)\)\)\)')
SCALE_CODE_RE = re.compile(r'^(\s*uint (_\d+) = )\(([^;]*?) >> 26u\) & 7u;$', re.M)
ROUND_RE = re.compile(r'roundEven\((vec[234])\((uvec[234])\(textureSize\(([\w]+, \d+)\)\)\) / vec[234]\(1\.0\)\)')
ROUNDTRIP_RE = re.compile(r'(uvec[234])\((vec[234])\(\1\(textureSize\(([\w]+, \d+)\)\)\)\)')
SCALE_WORD = 'uintBitsToFloat(g_UboShared.v[83u].z)'


def pass_scale1(text):
    """The deko3d renderer draws every texture at the console's scale (render/surfaces.cpp and graphics_options.cpp
    force 1x on deko3d): a texture word's scale code (bits 26-28) is 0. The per-texture scale divisor (a seven-way
    select on the code, before every sample with a texel offset and every texel-coordinate fetch) and the roundEven of
    size / 1 with its float and integer conversions go."""
    text = SCALE_CODE_RE.sub(lambda m: f'{m.group(1)}0u;', text)
    text = SCALE_CHAIN_RE.sub('1.0', text)
    text = ROUND_RE.sub(lambda m: f'{m.group(1)}({m.group(2)}(textureSize({m.group(3)})))', text)
    text = ROUNDTRIP_RE.sub(lambda m: f'{m.group(1)}(textureSize({m.group(3)}))', text)
    return text


# banjo-nx (2026-10-21, the internal resolutions): render_scale.h's codes 4 and 6 are the 3D image's scale in handheld and
# docked mode (3x and 6x were PC-only: the 4K resolution, ambient occlusion at twice the scene's), any height / 720 since
# package 36z2: the shaders read the heights from SharedConstants pad_1352[1] (v[84].w: handheld | docked << 16, set by
# render/draw.cpp for every draw). The shaders' factor chain (shader_common.h renutTextureScale, as SPIRV-Cross writes
# it) is rewritten to match; scale1 and fragscale are left out of a package for scaled targets.
SCALE_FACTOR_4 = '(float(g_UboShared.v[84u].w & 65535u) / 720.0)'
SCALE_FACTOR_6 = '(float(g_UboShared.v[84u].w >> 16u) / 720.0)'


def pass_scalecodes(text):
    """Codes 4 and 6 of every scale chain: the handheld and docked heights / 720 (render_scale.h)."""
    def chain(m):
        s = m.group(0)
        s = s.replace(f'(({m.group(1)} == 4u) ? 3.0 :', f'(({m.group(1)} == 4u) ? {SCALE_FACTOR_4} :')
        s = s.replace(f'(({m.group(1)} == 6u) ? 6.0 :', f'(({m.group(1)} == 6u) ? {SCALE_FACTOR_6} :')
        return s
    return SCALE_CHAIN_RE.sub(chain, text)


def pass_fragscale(text):
    """SharedConstants.resolution_scale is 1 on deko3d (1280x720 targets only): the gl_FragCoord / resolution_scale
    of every shader that reads the pixel position (the Xenos VPOS input: the depth of field, the screen-space
    shadows and effects) is gl_FragCoord, without its reciprocal and multiplies."""
    span = main_span(text)
    if span:
        a, b = span
        text = text[:a] + text[a:b].replace(SCALE_WORD, '1.0') + text[b:]
    return text


# ---------------------------------------------------------------------------------------------------------------------
# pass: alphamask — the generic alpha test as a bit test

ALPHA_HEAD = '!(dk_sel((DK_ALPHA_FUNC == 0u), false, (dk_sel((DK_ALPHA_FUNC == 1u), ('
ALPHA_CMP_RE = re.compile(r'^\(dk_sel\(\(DK_ALPHA_FUNC == 0u\), false, \(dk_sel\(\(DK_ALPHA_FUNC == 1u\), \((\S+) < (\S+)\), '
                          r'\(dk_sel\(\(DK_ALPHA_FUNC == 2u\), \(\1 == \2\), \(dk_sel\(\(DK_ALPHA_FUNC == 3u\), \(\1 <= \2\), '
                          r'\(dk_sel\(\(DK_ALPHA_FUNC == 4u\), \(\1 > \2\), \(dk_sel\(\(DK_ALPHA_FUNC == 5u\), \(\1 != \2\), '
                          r'\(dk_sel\(\(DK_ALPHA_FUNC == 6u\), \(\1 >= \2\), true\)\)[)]*$')
ALPHA_HELPER = ('bool dk_alpha_pass(uint f, float a, float r)\n{\n'
                '    // D3D compare functions are a mask: 1 less, 2 equal, 4 greater (0 never, 7 always); with a NaN only\n'
                '    // not-equal (5) and always (7) pass.\n'
                '    uint m = mix(mix(mix(5u, 2u, a == r), 4u, a > r), 1u, a < r);\n'
                '    return mix((f & m) != 0u, (f & 5u) == 5u, m == 5u);\n}\n')


def pass_alphamask(text):
    """The alpha test of a pipeline whose function is read at run time (constant 903 at 0: the generic pipeline a draw
    uses while its own is built, or banjo_deko3d_alpha_spec off): the seven-way compare chain (seven compares and
    selects) as the compare function's bit mask (D3D's functions are less | equal | greater bits). Runs after sel."""
    out = []
    pos = 0
    n = 0
    while True:
        at = text.find(ALPHA_HEAD, pos)
        if at < 0:
            out.append(text[pos:])
            break
        close = matching(text, at + 1, '(', ')')
        inner = text[at + 1:close + 1]  # the whole parenthesised chain after the '!'
        m = ALPHA_CMP_RE.match(inner)
        if close < 0 or not m:
            out.append(text[pos:at + 1])
            pos = at + 1
            continue
        out.append(text[pos:at])
        # Only where the function is read at run time: with constant 903 the chain folds to one compare (or to
        # nothing, and no discard, for 7), which the bit test would hide from uam (package 33 kept the discard).
        out.append(f'!((g_banjoAlphaFunc != 0u) ? {inner} : '
                   f'dk_alpha_pass(g_UboShared.v[34u].w, {m.group(1)}, {m.group(2)}))')
        pos = close + 1
        n += 1
    if not n:
        return text
    return insert_before_main(''.join(out), ALPHA_HELPER)


# ---------------------------------------------------------------------------------------------------------------------
# pass: pred — a condition known inside its own if

def pass_pred(text):
    """Inside `if (X) { ... }` (X a bool the block does not assign), an inner `if (X)` is `if (true)` and `if (!X)`
    `if (false)`, X anywhere else in the block is true (a bvec4(X) of a mix, a ?:), and the same in the else block
    the other way round. In the translator's predicated blocks the same predicate is tested again and again (the inner
    else blocks, their copies and the phis are dead), which Mesa's GLSL optimiser does not see."""
    span = main_span(text)
    if not span:
        return text
    a, b = span
    lines = text[a:b].split('\n')
    bools = set(re.findall(r'^\s*bool (_\d+)\b', text[a:b], re.M))
    if not bools:
        return text
    # the line of each block's closing brace, and the lines assigning each bool
    close_of = {}
    opened = []
    for n, line in enumerate(lines):
        st = line.strip()
        if st == '{':
            opened.append(n)
        elif (st == '}' or st.startswith('} while')) and opened:
            close_of[opened.pop()] = n
    assign_at = {}
    for n, line in enumerate(lines):
        for m in re.finditer(r'(?<![\w.])(_\d+)\s*=[^=]', line):
            if m.group(1) in bools:
                assign_at.setdefault(m.group(1), []).append(n)

    def stable(var, open_line):
        close = close_of.get(open_line, len(lines))
        return not any(open_line < k < close for k in assign_at.get(var, ()))

    stack = []      # (indent of the block's braces, var or None, value inside)
    pending = None  # (indent, var, value) of an if or else whose '{' comes next
    last_if = {}    # indent -> (var, value) of the if just closed (for its else)
    changed = 0
    for n, line in enumerate(lines):
        stripped = line.strip()
        indent = len(line) - len(line.lstrip())
        known = {var: val for _, var, val in stack if var is not None}
        m = re.match(r'^(\s*)(?:else )?if \((!?)(_\d+)\)$', line)
        if m and m.group(3) in known:
            val = known[m.group(3)] != (m.group(2) == '!')
            lines[n] = f'{m.group(1)}{"else " if stripped.startswith("else") else ""}if ({"true" if val else "false"})'
            changed += 1
        if m and m.group(3) in bools:
            pending = (indent, m.group(3), m.group(2) != '!')
            continue
        if stripped == 'else' and indent in last_if:
            var, val = last_if.pop(indent)
            pending = (indent, var, not val)
            continue
        if stripped == '{':
            if pending and pending[0] == indent and stable(pending[1], n):
                stack.append(pending)
            else:
                stack.append((indent, None, None))
            pending = None
            continue
        pending = None
        if stripped == '}' or stripped.startswith('} while'):
            if stack:
                ind, var, val = stack.pop()
                if var is not None and stripped == '}':
                    last_if[ind] = (var, val)
            continue
        last_if = {k: v for k, v in last_if.items() if k < indent}
        if known:
            def use(mm):
                v = mm.group(1)
                if v in known:
                    return 'true' if known[v] else 'false'
                return mm.group(0)
            new = re.sub(r'(?<![\w.])(_\d+)(?![\w\[])(?!\s*=[^=])', use, line)
            if new != line:
                lines[n] = new
                changed += 1
    if not changed:
        return text
    return text[:a] + '\n'.join(lines) + text[b:]


# ---------------------------------------------------------------------------------------------------------------------

# ---------------------------------------------------------------------------------------------------------------------
# pass: tess — the vertex shader's tessellation kind as specialization constant 904

TESS_WORD = 'g_UboShared.v[65u].x'
TESS_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_904\n#define SPIRV_CROSS_CONSTANT_ID_904 0u\n#endif\n'
             'const uint g_banjoTessKind = SPIRV_CROSS_CONSTANT_ID_904;\n'
             '#define DK_TESS_KIND ((g_banjoTessKind != 0u) ? (g_banjoTessKind - 1u) : g_UboShared.v[65u].x)\n')


def pass_tess(text):
    """SharedConstants.tessellation[0] (the patch kind: 0 for a draw that is not tessellated) as specialization
    constant 904, its value + 1 (0: read at run time): render/pipeline.cpp gives 1 to every pipeline whose key is not a
    tessellated draw's (per_instance_streams 0), so the vertex shader's patch-grid code (the domain point from the
    vertex index: square roots, divisions, the four kinds' branches) goes, and its registers with it. No new
    pipelines: the key decides it."""
    if TESS_WORD not in text:
        return text
    span = main_span(text)
    if not span:
        return text
    a, b = span
    text = text[:a] + text[a:b].replace(TESS_WORD, 'DK_TESS_KIND') + text[b:]
    return insert_before_main(text, TESS_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: vsout — the vertex shader's outputs the pixel shader reads, as specialization constant 905

VSOUT_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_905\n#define SPIRV_CROSS_CONSTANT_ID_905 4294967295u\n#endif\n'
              'const uint g_banjoVsOutputs = SPIRV_CROSS_CONSTANT_ID_905;\n')
VSOUT_RE = re.compile(r'^layout\(location = (\d+)\) out (\w+) (\w+);$', re.M)


def pass_vsout(text):
    """Each write of a vertex shader output (layout location N) under `(g_banjoVsOutputs & (1u << N)) != 0u`,
    specialization constant 905: plume_deko3d gives a pipeline's vertex shader the locations its pixel shader reads (none
    for a depth-only pipeline: the shadow maps), so the outputs nothing reads, the arithmetic and the vertex fetches
    behind them go. An output the shader reads back is left alone."""
    outs = [(int(m.group(1)), m.group(3)) for m in VSOUT_RE.finditer(text) if int(m.group(1)) < 32]
    if not outs:
        return text
    span = main_span(text)
    if not span:
        return text
    a, b = span
    body = text[a:b]
    wrapped = 0
    for loc, name in outs:
        uses = list(re.finditer(r'(?<![\w.])' + re.escape(name) + r'\b', body))
        writes = re.findall(r'^\s*' + re.escape(name) + r'(?:\.[xyzw]+)?\s*=[^=][^;]*;$', body, re.M)
        if not writes or len(writes) != len(uses):
            continue  # read back somewhere, or written in a form not handled: kept
        body = re.sub(r'^(\s*)(' + re.escape(name) + r'(?:\.[xyzw]+)?\s*=[^=][^;]*;)$',
                      lambda m: f'{m.group(1)}if ((g_banjoVsOutputs & {1 << loc}u) != 0u) {{ {m.group(2)} }}',
                      body, flags=re.M)
        wrapped += 1
    if not wrapped:
        return text
    text = text[:a] + body + text[b:]
    return insert_before_main(text, VSOUT_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# passes: vszero / pszero — an input its vertex shader writes as zero (constant 906)

ZERO_RE = re.compile(r'^(?:(?:vec[234])\()?0\.0\)?$')
# 2026-11-01: an interpolation qualifier may come first ("centroid in", XenosRecomp)
PSIN_RE = re.compile(r'^layout\(location = (\d+)\) (?:(?:centroid|sample|noperspective|flat) )?in (\w+) (\w+);$', re.M)
PSZERO_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_906\n#define SPIRV_CROSS_CONSTANT_ID_906 0u\n#endif\n'
               'const uint g_banjoPsZeroIn = SPIRV_CROSS_CONSTANT_ID_906;\n')


def pass_vszero(text):
    """Records in a `// banjo-vszero=0x...` line the outputs the vertex shader writes as zero (one write of 0.0 or
    vecN(0.0), nothing else touching it): plume_deko3d gives the pipeline's pixel shader those of its inputs as constant
    906 (pszero). Renut's vertex shaders write every output; the point coordinate (location 23) is zero except under
    the point-sprite geometry shader, and 2124 pixel shaders read it."""
    span = main_span(text)
    if not span:
        return text
    body = text[span[0]:span[1]]
    mask = 0
    for m in VSOUT_RE.finditer(text):
        loc, name = int(m.group(1)), m.group(3)
        if loc >= 32:
            continue
        uses = re.findall(r'(?<![\w.])' + re.escape(name) + r'\b', body)
        writes = re.findall(r'^\s*' + re.escape(name) + r' = ([^;]*);$', body, re.M)
        if len(uses) == 1 and len(writes) == 1 and ZERO_RE.match(writes[0].strip()):
            mask |= 1 << loc
    if not mask:
        return text
    lines = text.split('\n')
    lines.insert(1, f'// banjo-vszero=0x{mask:x}')
    return '\n'.join(lines)


def pass_pszero(text):
    """Each input read through `DK_IN_<name>`: `vecN(0.0)` when bit <location> of constant 906 is set (its vertex
    shader writes it as zero, vszero), else the input. The interpolation and what depends on it go."""
    ins = [(int(m.group(1)), m.group(2), m.group(3)) for m in PSIN_RE.finditer(text) if int(m.group(1)) < 32]
    if not ins:
        return text
    span = main_span(text)
    if not span:
        return text
    a, b = span
    body = text[a:b]
    macros = ''
    for loc, typ, name in ins:
        if not re.search(r'(?<![\w.])' + re.escape(name) + r'\b', body):
            continue
        body = re.sub(r'(?<![\w.])' + re.escape(name) + r'\b', f'DK_IN_{name}', body)
        macros += (f'#define DK_IN_{name} (((g_banjoPsZeroIn & {1 << loc}u) != 0u) ? {typ}(0.0) : {name})\n')
    if not macros:
        return text
    text = text[:a] + body + text[b:]
    return insert_before_main(text, PSZERO_DECL + macros)


# ---------------------------------------------------------------------------------------------------------------------
# pass: bonerow — a relative constant read past the registers reads the zero register

BONEROW_RE = re.compile(r'mix\(vec4\(0\.0\), (g_Ubo(?:Vertex|Pixel))\.v\[min\((_\d+), 255u\)\], bvec4\(\2 < 256u\)\)')


def pass_bonerow(text):
    """Renut's relative constant reads (the bone rows of skinning: c[a0 + n]) are `index < 256 ? c[min(index, 255)] :
    0`, a select per component and a compare. The float constant uploads have a zero register 256 after the 256
    registers (render/upload_ring.h, kFloatConstantsUboBytes), so `c[min(index, 256)]` is the same value with no
    select: an index past the registers reads zeros."""
    out, n = BONEROW_RE.subn(lambda m: f'{m.group(1)}.v[min({m.group(2)}, 256u)]', text)
    if not n:
        return text
    for block in set(m.group(1) for m in BONEROW_RE.finditer(text)):
        out = re.sub(r'(uniform type_ConstantBuffer_RenutUboFloat\n\{\n\s*vec4 v\[)256(\];\n\} ' + block + ';)',
                     r'\g<1>257\2', out)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# pass: predmul — a predicated scale by one value

PREDMUL_RE = re.compile(r'mix\((_\d+(?:\.[xyzw]+)?), \1 \* (_\d+\.([xyzw])\3{1,3}|vec[234]\([0-9.e+-]+\)), '
                        r'bvec([234])\((_\d+)\)\)')


def pass_predmul(text):
    """The translator's predicated vector multiply by one scalar (a broadcast swizzle or a constant):
    `mix(a, a * k, bvec3(p))` (three multiplies and three selects) is `a * mix(vec3(1.0), k, bvec3(p))`: the selects
    are of the same two values, one select. Exact: a * 1.0 is a (uam's multiplies keep denormals; only a NaN's payload
    may change)."""
    def rep(m):
        n = m.group(4)
        return f'({m.group(1)} * mix(vec{n}(1.0), {m.group(2)}, bvec{n}({m.group(5)})))'
    return map_main(text, lambda body: PREDMUL_RE.sub(rep, body))


# ---------------------------------------------------------------------------------------------------------------------
# pass: sext — a signed bitfield as one extract

SEXT_RE = re.compile(r'int\(\(\((_\d+) >> (\d+)u\) & (\d+)u\) << (\d+)u\) >> (\d+)')


def pass_sext(text):
    """The vertex fetch's signed fields (10:10:10:2 normals, signed shorts): `int(((x >> s) & m) << k) >> k`, a shift,
    a mask, a shift and an arithmetic shift, is `bitfieldExtract(int(x), s, n)` (one BFE; GLSL's extract of an int
    sign-extends from bit n - 1, as the shift pair does). Only when m is n low bits and k = 32 - n."""
    def rep(m):
        x, sh, mask, k1, k2 = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))
        n = mask.bit_length()
        if mask != (1 << n) - 1 or k1 != k2 or k1 != 32 - n or sh + n > 32 or n == 0:
            return m.group(0)
        return f'bitfieldExtract(int({x}), {sh}, {n})'
    return map_main(text, lambda body: SEXT_RE.sub(rep, body))


# ---------------------------------------------------------------------------------------------------------------------
# pass: bool01 — a select of the two constants of a type is a conversion

BOOL01 = {('1.0', '0.0'): 'float({c})', ('0.0', '1.0'): 'float(!({c}))', ('1u', '0u'): 'uint({c})',
          ('0u', '1u'): 'uint(!({c}))', ('1', '0'): 'int({c})', ('0', '1'): 'int(!({c}))',
          ('true', 'false'): '({c})', ('false', 'true'): '(!({c}))'}


def pass_bool01(text):
    """`c ? 1.0 : 0.0` is float(c) (one FSET; the select is a compare and two moves), and the same for 0.0 / 1.0, the
    integers and the bools: the translator's predicates turned into numbers (setp results, the Xenos kill and pred
    instructions) are written so."""
    count = 0

    def body(s):
        nonlocal count
        lines = s.split('\n')
        for n, line in enumerate(lines):
            if '?' not in line:
                continue
            guard = 0
            while guard < 200:
                guard += 1
                done = False
                for q in [m.start() for m in re.finditer(r'\?', line)]:
                    t = _ternary_at(line, q)
                    if t is None:
                        continue
                    start, colon, end = t
                    cond, x, y = line[start:q], line[q + 1:colon].strip(), line[colon + 1:end].strip()
                    form = BOOL01.get((x, y))
                    if form is None or '?' in cond:
                        continue
                    lead = cond[:len(cond) - len(cond.lstrip())]
                    trail = line[colon + 1:end][len(line[colon + 1:end].rstrip()):]
                    line = line[:start] + lead + form.format(c=cond.strip()) + trail + line[end:]
                    count += 1
                    done = True
                    break
                if not done:
                    break
            lines[n] = line
        return '\n'.join(lines)

    return map_main(text, body)


# ---------------------------------------------------------------------------------------------------------------------
# pass: earlyhoist — the main body's first texture region ahead of Renut's early alpha test

EARLY_IF_RE = re.compile(r'\n([ \t]*)if \(DK_ALPHA_FUNC != 7u\)\n[ \t]*\{')
ASSIGNED_RE = re.compile(r'\b(_\d+)(?:\.[xyzw]+)?\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')


def _next_statement(body, at):
    """(start, end) of the statement at 'at' (after blanks): a simple one up to its ';', or a `do { ... } while(false);`;
    None for anything else (if, for, a block, the end of the enclosing block)."""
    n = len(body)
    i = at
    while i < n and body[i] in ' \t\n':
        i += 1
    if i >= n or body[i] == '}':
        return None
    if re.match(r'do\s*\{', body[i:i + 8]):
        open_i = body.index('{', i)
        close_i = matching(body, open_i, '{', '}')
        if close_i < 0:
            return None
        m = re.match(r'\s*while\s*\(\s*false\s*\)\s*;', body[close_i + 1:])
        if not m:
            return None
        return i, close_i + 1 + m.end()
    if re.match(r'(if|for|while|switch|return|discard|break|continue|else)\b', body[i:]) or body[i] == '{':
        return None
    depth = 0
    for j in range(i, n):
        c = body[j]
        if c in '({[':
            depth += 1
        elif c in ')}]':
            depth -= 1
            if depth < 0:
                return None
        elif c == ';' and depth == 0:
            return i, j + 1
        elif c == '{':
            return None
    return None


def pass_earlyhoist(text):
    """Renut's early alpha test (a quad whose pixels all fail is discarded before the main body) computes the alpha as
    the main body does, and the main body's first region then fetches the same textures through the same uniform
    branches (the fur shader: a texture array or a 2D texture by a constant-buffer bit). uam's value numbering
    replaces what an equivalent dominating computation already did, so the main body's region goes first: its
    statements up to its first `do { } while(false)` (pure: no discard, no output, nothing the early test assigns), when
    that region fetches a texture the early test also fetches. uam then finds the early test's alpha in the region's
    results and drops the early copy; were the two not equivalent, the region would only run earlier (pure code).
    Specialization constant 907 (1 by default; banjo_deko3d_early_hoist off gives 0) picks the place: the region is
    written at both, its top-level declarations ahead of the first."""
    changed = []

    def body(b):
        m = EARLY_IF_RE.search(b)
        if not m:
            return b
        open_i = b.index('{', m.end() - 1)
        close_i = matching(b, open_i, '{', '}')
        if close_i < 0:
            return b
        early = b[m.start() + 1:close_i + 1]
        stmts = []
        at = close_i + 1
        while True:
            st = _next_statement(b, at)
            if not st:
                return b
            stmts.append(st)
            at = st[1]
            if b[st[0]:st[0] + 2] == 'do':
                break
            if len(stmts) > 12:
                return b
        start, end = stmts[0][0], stmts[-1][1]
        region = b[start:end]
        if re.search(r'\b(discard|return)\b|\bout_var_|gl_FragDepth|\bdFd[xy]|\bfwidth', region):
            return b
        # nothing the early test assigns (declared before it) may be read by the region
        declared_in_early = set(re.findall(r'\b(?:float|int|uint|bool|[biu]?vec[234]|mat[234])\s+(_\d+)\b', early))
        assigned = {v for v in ASSIGNED_RE.findall(early)} - declared_in_early
        if assigned & set(re.findall(r'\b_\d+\b', region)):
            return b
        # worth it only when the region fetches what the early test fetches
        tex_region = set(re.findall(r'\btexture\w*\((dk_tex_b\d+)', region))
        tex_early = set(re.findall(r'\btexture\w*\((dk_tex_b\d+)', early))
        if not (tex_region & tex_early):
            return b
        indent = m.group(1)
        # the region's top-level declarations go ahead of both places; in each it assigns
        decls, inner = [], []
        for a0, a1 in stmts:
            st = b[a0:a1]
            d = re.match(r'((?:float|int|uint|bool|[biu]?vec[234]|mat[234])\s+)(_\d+)\s*(=|;)', st)
            if not d and re.match(r'(const\s+)?(float|int|uint|bool|[biu]?vec[234]|mat[234]|[biu]?vec\d|\w+\s+_\d+\s*\[)\b',
                                  st):
                return b  # a declaration this does not split (const, array): left where it is
            if d:
                decls.append(f'{d.group(1)}{d.group(2)};')
                st = '' if d.group(3) == ';' else st[len(d.group(1)):]
            if st:
                inner.append(st)
        inner_text = ('\n' + indent + '    ').join(x.replace('\n', '\n    ') for x in inner)

        def place(cond):
            return f'{indent}if (g_banjoEarlyHoist {cond} 0u)\n{indent}{{\n{indent}    {inner_text}\n{indent}}}'

        changed.append(True)
        head = ''.join(f'{indent}{d}\n' for d in decls)
        return (b[:m.start() + 1] + head + place('!=') + '\n' + b[m.start() + 1:close_i + 1] + '\n' + place('==') +
                b[end:])
    out = map_main(text, body)
    if changed:
        out = insert_globals(out, '#ifndef SPIRV_CROSS_CONSTANT_ID_907\n#define SPIRV_CROSS_CONSTANT_ID_907 1u\n#endif\n'
                                  'const uint g_banjoEarlyHoist = SPIRV_CROSS_CONSTANT_ID_907;\n')
    return out


# ---------------------------------------------------------------------------------------------------------------------
# pass: ifmerge — consecutive ifs on the same unchanged condition variable as one

IF_VAR_RE = re.compile(r'\n([ \t]*)if \((!?)(_\d+)\)\n\1\{')
DECL_ONLY_RE = re.compile(r'\n([ \t]*)(?:float|int|uint|bool|[biu]?vec[234]|mat[234]) _\d+;')


def _if_end(b, m):
    """For an if matched by IF_VAR_RE: (then body, else body or None, end index)."""
    open_i = b.index('{', m.end() - 1)
    close_i = matching(b, open_i, '{', '}')
    if close_i < 0:
        return None
    then_body = b[open_i + 1:close_i]
    indent = m.group(1)
    em = re.match(r'\n' + re.escape(indent) + r'else\n' + re.escape(indent) + r'\{', b[close_i + 1:])
    if not em:
        return then_body, None, close_i + 1
    eo = close_i + 1 + em.end() - 1
    ec = matching(b, eo, '{', '}')
    if ec < 0:
        return None
    return then_body, b[eo + 1:ec], ec + 1


def pass_ifmerge(text):
    """SPIRV-Cross writes Renut's per-pixel branches as one if per value: the fur shader's shadow cascades test `_678`
    in four ifs in a row and `_914` in three (each its own branches and, the condition being per pixel, its own
    reconvergence: SSY, BRA, SYNC). Two ifs at the same level on the same condition variable (or its negation, the
    same both times), with only declarations between them and nothing in the first assigning the variable, are one if:
    the first's then and the second's then, the first's else and the second's else, in that order (the same statements
    run in the same order on each path). The declarations go ahead of the first."""
    def body(b):
        pos = 0
        while True:
            m = IF_VAR_RE.search(b, pos)
            if not m:
                return b
            first = _if_end(b, m)
            if not first:
                pos = m.end()
                continue
            then1, else1, end1 = first
            indent, neg, var = m.group(1), m.group(2), m.group(3)
            # declarations only between
            at = end1
            decls = []
            while True:
                dm = DECL_ONLY_RE.match(b, at)
                if not dm or dm.group(1) != indent:
                    break
                decls.append(b[at + 1:dm.end()].strip())
                at = dm.end()
            m2 = IF_VAR_RE.match(b, at)
            assigned = re.compile(r'\b' + re.escape(var) + r'\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
            if (not m2 or m2.group(1) != indent or m2.group(2) != neg or m2.group(3) != var or
                    assigned.search(then1) or (else1 is not None and assigned.search(else1))):
                pos = m.end()
                continue
            second = _if_end(b, m2)
            if not second:
                pos = m.end()
                continue
            then2, else2, end2 = second
            merged = (''.join(f'\n{indent}{d}' for d in decls) +
                      f'\n{indent}if ({neg}{var})\n{indent}{{{then1.rstrip()}{then2.rstrip()}\n{indent}}}')
            if else1 is not None or else2 is not None:
                merged += f'\n{indent}else\n{indent}{{{(else1 or "").rstrip()}{(else2 or "").rstrip()}\n{indent}}}'
            b = b[:m.start()] + merged + b[end2:]
            pos = m.start()  # a third may follow
    return map_main(text, body)


# ---------------------------------------------------------------------------------------------------------------------
# pass: dowhile — `do { ... break; } while(false)` early exits as if / else

DO_RE = re.compile(r'\n([ \t]*)do\n\1\{')
LOOP_START_RE = re.compile(r'(do|for|while|switch)\b')


def _statements(body):
    """The top-level statements of a block's text: (start, end) spans; None when it has one this does not parse."""
    out = []
    i, n = 0, len(body)
    while True:
        while i < n and body[i] in ' \t\n':
            i += 1
        if i >= n:
            return out
        if body[i] == '{':
            j = matching(body, i, '{', '}')
            if j < 0:
                return None
            out.append((i, j + 1))
            i = j + 1
            continue
        m = re.match(r'(if|for|while|switch)\s*\(', body[i:])
        if m:
            p0 = i + m.end() - 1
            p1 = matching(body, p0, '(', ')')
            if p1 < 0:
                return None
            k = p1 + 1
            while k < n and body[k] in ' \t\n':
                k += 1
            if k >= n or body[k] != '{':
                return None
            e = matching(body, k, '{', '}')
            if e < 0:
                return None
            end = e + 1
            if m.group(1) == 'if':
                em = re.match(r'\s*else\b', body[end:])
                while em:
                    k = end + em.end()
                    while k < n and body[k] in ' \t\n':
                        k += 1
                    if body.startswith('if', k):  # else if: one more if statement
                        q0 = body.index('(', k)
                        q1 = matching(body, q0, '(', ')')
                        k = q1 + 1
                        while k < n and body[k] in ' \t\n':
                            k += 1
                    if k >= n or body[k] != '{':
                        return None
                    e = matching(body, k, '{', '}')
                    if e < 0:
                        return None
                    end = e + 1
                    em = re.match(r'\s*else\b', body[end:])
            out.append((i, end))
            i = end
            continue
        if body.startswith('do', i) and re.match(r'do\s*\{', body[i:]):
            k = body.index('{', i)
            e = matching(body, k, '{', '}')
            if e < 0:
                return None
            m2 = re.match(r'\s*while\s*\([^;]*\)\s*;', body[e + 1:])
            if not m2:
                return None
            out.append((i, e + 1 + m2.end()))
            i = e + 1 + m2.end()
            continue
        depth = 0
        j = i
        while j < n:
            c = body[j]
            if c in '([':
                depth += 1
            elif c in ')]':
                depth -= 1
            elif c == '{':
                return None
            elif c == ';' and depth == 0:
                break
            j += 1
        if j >= n:
            return None
        out.append((i, j + 1))
        i = j + 1


def _breaks_outside_loops(text):
    """break / continue statements of 'text' that are not inside a nested loop or switch (they would leave ours)."""
    found = 0
    i = 0
    while True:
        m = re.search(r'\b(break|continue|do|for|while|switch)\b', text[i:])
        if not m:
            return found
        k = i + m.start()
        w = m.group(1)
        if w in ('break', 'continue'):
            found += 1
            i = k + len(w)
            continue
        # a nested loop or switch: skip its body (and a do's `while (...);`)
        b = text.find('{', k)
        if b < 0:
            return found + 1
        e = matching(text, b, '{', '}')
        if e < 0:
            return found + 1
        i = e + 1
        if w == 'do':
            wm = re.match(r'\s*while\s*\([^;]*\)\s*;', text[i:])
            if wm:
                i += wm.end()


def pass_dowhile(text):
    """SPIRV-Cross writes the early exits of Renut's shaders (the texture-array choice, the early alpha test, the main
    body's three fog / colour paths) as `do { ...; if (c) { ...; break; } ...; break; } while(false);`: uam compiles it
    as a loop (PBK, BRK, a loop for the value numbering of 2.26). When its breaks are only the last statement of ifs at
    its top level (no else) and its last statement, it is if / else: `if (c) { A } else { rest }`, in a block of its
    own. The same statements run on each path in the same order."""
    def convert(body):
        stmts = _statements(body)
        if stmts is None:
            return None
        pieces = []
        for idx, (a, b) in enumerate(stmts):
            st = body[a:b]
            ls = body.rfind('\n', 0, a) + 1
            ind = body[ls:a] if not body[ls:a].strip() else ''
            last = idx == len(stmts) - 1
            if st.strip() == 'break;':
                if not last:
                    return None
                pieces.append(('end', None))
                continue
            if st.startswith('if'):
                p0 = st.index('(')
                p1 = matching(st, p0, '(', ')')
                k = st.index('{', p1)
                e = matching(st, k, '{', '}')
                inner = st[k + 1:e]
                rest = st[e + 1:].strip()
                istmts = _statements(inner)
                if istmts and inner[istmts[-1][0]:istmts[-1][1]].strip() == 'break;' and not rest:
                    head = inner[:istmts[-1][0]]
                    if _breaks_outside_loops(head):
                        return None
                    pieces.append(('exit', (ind, st[:p1 + 1], head)))
                    continue
            if _breaks_outside_loops(st):
                return None
            pieces.append(('stmt', ind + st))
        if any(k == 'end' for k, _ in pieces[:-1]):
            return None
        # nest from the last exit outwards
        out = ''
        for kind, val in reversed(pieces):
            if kind == 'end':
                continue
            if kind == 'stmt':
                out = val + '\n' + out
            else:
                ind, cond, head = val
                out = f'{ind}{cond}\n{ind}{{{head.rstrip()}\n{ind}}}\n{ind}else\n{ind}{{\n{out}{ind}}}\n'
        return out

    def body(b):
        pos = 0
        while True:
            m = DO_RE.search(b, pos)
            if not m:
                return b
            open_i = b.index('{', m.end() - 1)
            close_i = matching(b, open_i, '{', '}')
            if close_i < 0:
                return b
            wm = re.match(r'\s*while\s*\(\s*false\s*\)\s*;', b[close_i + 1:])
            if not wm:
                pos = m.end()
                continue
            inner = b[open_i + 1:close_i]
            if 'continue' in inner and _breaks_outside_loops(inner) and re.search(r'\bcontinue\b', inner):
                pos = m.end()
                continue
            new = convert(inner)
            if new is None:
                pos = m.end()
                continue
            ind = m.group(1)
            b = b[:m.start()] + f'\n{ind}{{\n' + new + f'{ind}}}' + b[close_i + 1 + wm.end():]  # its own block
            pos = m.start()
    return map_main(text, body)


# ---------------------------------------------------------------------------------------------------------------------
# pass: selsame — a select inside a select on the same condition

def _split_args(text):
    """Top-level comma-separated arguments of 'text' (the inside of a call's parentheses)."""
    out, depth, start = [], 0, 0
    for i, c in enumerate(text):
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        elif c == ',' and depth == 0:
            out.append(text[start:i])
            start = i + 1
    out.append(text[start:])
    return out


def _unparen(e):
    e = e.strip()
    while e.startswith('(') and matching(e, 0, '(', ')') == len(e) - 1:
        e = e[1:-1].strip()
    return e


def pass_selsame(text):
    """Renut's lights select each term on the light's flag twice: `dk_sel(c, (dk_sel(c, exp2(a), b)), d)`. With the same
    condition text, the inner select's other value can never be chosen: `dk_sel(c, x, d)` (and `dk_sel(c, a, dk_sel(c, x,
    y))` is `dk_sel(c, a, y)`). The conditions are variables or pure expressions (no calls with effects in GLSL)."""
    def body(b):
        changed = True
        while changed:
            changed = False
            out, pos = [], 0
            for m in re.finditer(r'\bdk_sel\(', b):
                if m.start() < pos:
                    continue
                o = m.end() - 1
                c = matching(b, o, '(', ')')
                if c < 0:
                    continue
                args = _split_args(b[o + 1:c])
                if len(args) != 3:
                    continue
                cond = args[0].strip()
                new = list(args)
                for k, keep in ((1, 1), (2, 2)):
                    inner = _unparen(args[k])
                    if inner.startswith('dk_sel(') and matching(inner, 6, '(', ')') == len(inner) - 1:
                        ia = _split_args(inner[7:-1])
                        if len(ia) == 3 and ia[0].strip() == cond:
                            new[k] = ' (' + ia[keep].strip() + ')'
                if new != args:
                    out.append(b[pos:m.start()])
                    out.append('dk_sel(' + ','.join(new) + ')')
                    pos = c + 1
                    changed = True
            if changed:
                out.append(b[pos:])
                b = ''.join(out)
        return b
    return map_main(text, body)


# ---------------------------------------------------------------------------------------------------------------------
# pass: txq — a texture's level-0 size from the texture size buffer instead of a texture query

TXQ_RE = re.compile(r'textureSize\((dk_tex_b(\d+)), 0\)')
TXQ_TYPES = {'sampler2D': ('ivec2', 'xy'), 'samplerCube': ('ivec2', 'xy'), 'sampler2DArray': ('ivec3', 'xyz'),
             'sampler3D': ('ivec3', 'xyz')}
TXQ_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_908\n#define SPIRV_CROSS_CONSTANT_ID_908 1u\n#endif\n'
            'const uint g_banjoTxq = SPIRV_CROSS_CONSTANT_ID_908;\n'
            'layout(binding = 3, std140) uniform type_DkTexSize\n{\n    uvec4 v[64];\n} g_UboTexSize;\n'
            'bool g_dkTxqBad = false;\n'
            'ivec2 dk_txq_check(ivec2 c, ivec2 q) { g_dkTxqBad = g_dkTxqBad || any(notEqual(c, q)); return q; }\n'
            'ivec3 dk_txq_check(ivec3 c, ivec3 q) { g_dkTxqBad = g_dkTxqBad || any(notEqual(c, q)); return q; }\n')


def pass_txq(text):
    """A texture query of level 0 (`textureSize(t, 0)`: Renut's half-texel offsets, the stacked textures' layer count)
    is a texture instruction per pixel, as costly as a fetch in the texture unit: A5E2 (the second heaviest pixel shader
    of the console's profile) runs 4 fetches and 4 queries a pixel over the whole screen. The size of each queried
    binding comes from the texture size buffer instead (UBO binding 3, entry k for binding k), which plume deko3d fills
    from the view it wrote into the descriptor (Deko3DDescriptorSet::imageSizes) and updates when it changes; the
    banjo-dk line gets txq=<binding mask> and UBO bit 3. Specialization constant 908: 1 (default) the buffer, 0 the
    query as before (banjo_deko3d_txq_constants off), 2 both, the query's value used and the pixel magenta where the two
    differ (banjo_deko3d_txq_verify: the GPU checks plume's sizes). Pixel shaders only."""
    if 'textureSize(' not in text or 'g_banjoTxq' in text:
        return text
    types = {m.group(2): m.group(1) for m in re.finditer(r'uniform (\w+) dk_tex_b(\d+);', text)}
    mask = 0

    def repl(m):
        nonlocal mask
        b = m.group(2)
        t = TXQ_TYPES.get(types.get(b, ''))
        if not t or int(b) >= 32:
            return m.group(0)
        mask |= 1 << int(b)
        vec, comps = t
        c = f'{vec}(g_UboTexSize.v[{b}u].{comps})'
        q = m.group(0)
        return f'((g_banjoTxq == 1u) ? {c} : ((g_banjoTxq == 2u) ? dk_txq_check({c}, {q}) : {q}))'

    span = main_span(text)
    if not span:
        return text
    a, b = span
    body = TXQ_RE.sub(repl, text[a:b])
    if not mask:
        return text
    # The verify variant: magenta where a size differed (before main's closing brace).
    if 'out_var_SV_Target0' in text:
        body = body.rstrip() + '\n    if (g_banjoTxq == 2u && g_dkTxqBad)\n    {\n' \
               '        out_var_SV_Target0 = vec4(1.0, 0.0, 1.0, 1.0);\n    }\n'
    text = text[:a] + body + text[b:]
    text = insert_before_main(text, TXQ_DECL)

    def header(m):
        line = m.group(0)
        ubo = re.search(r'ubo=0x([0-9a-fA-F]+)', line)
        if ubo:
            line = line.replace(ubo.group(0), 'ubo=0x%x' % (int(ubo.group(1), 16) | 8))
        return line + ' txq=0x%x' % mask
    return re.sub(r'^// banjo-dk [^\n]*', header, text, count=1, flags=re.M)


# ---------------------------------------------------------------------------------------------------------------------
# pass: earlyz — depth and stencil tested before the shader where the draw writes neither

EARLYZ_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_909\n#define SPIRV_CROSS_CONSTANT_ID_909 0u\n#define DK_EARLY_Z 0\n'
               '#else\n#define DK_EARLY_Z 1\n#endif\n#if DK_EARLY_Z\nlayout(early_fragment_tests) in;\n#endif\n')


def pass_earlyz(text):
    """A pixel shader that discards (the alpha test, Renut's early alpha test of a quad) has its SPH's kills_pixels
    bit, and the GPU may then test depth after it: every covered pixel runs the shader, also those the depth test
    rejects. Where the draw writes neither depth nor stencil (blended draws: the fur shells, particles), testing first
    gives the same pixels: the discard has nothing to undo, and nothing counts samples (the renderer has no occlusion
    queries). Specialization constant 909 = 1 (render/pipeline.cpp, banjo_deko3d_early_z: depth writes and stencil off
    in the key) declares `layout(early_fragment_tests)`; its default 0 leaves the shader as it was. The constant is
    tested by the preprocessor: plume writes `#define SPIRV_CROSS_CONSTANT_ID_909 1u` ahead of the text."""
    if 'discard' not in text or 'gl_FragDepth' in text or 'SPIRV_CROSS_CONSTANT_ID_909' in text:
        return text
    return insert_globals(text, EARLYZ_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: plainfetch — a fetch's sign, gamma and exponent code dropped where the draw's fetch words have none
# (sasrt-nx, 2026-11-08, the GPU round after the move to the ultimate base: sasrt_gpu_plain_fetches, constant 926)

PLAINFETCH_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_926\n#define SPIRV_CROSS_CONSTANT_ID_926 0u\n#endif\n'
                   'const uint g_sasrtPlainFetch = SPIRV_CROSS_CONSTANT_ID_926;\n')
_PF_COND = r'(?:!\(\((_\d+) \| (_\d+)\) != 0u\)|(_\d+))'
_PF_BLOCK = re.compile(r'bool (_\d+);\s*if \(' + _PF_COND + r'\)\s*\{\s*\1 = (_\d+) != 0;\s*\}\s*else\s*\{\s*\1 = true;\s*\}')
_PF_NOT = r'bool {v} = !\(\((_\d+) \| (_\d+)\) != 0u\);'
_PF_SIGNS = r'uint {v} = \(g_UboShared\.v\[(\d+)u\]\.([xyzw]) >> 16u\) & 15u;'
_PF_GAMMA = r'uint {v} = \(g_UboShared\.v\[(\d+)u\]\.([xyzw]) >> 13u\) & 7u;'
_PF_EXP = r'int {v} = int\(g_UboShared\.v\[(\d+)u\]\.([xyzw]) << 6u\) >> 26;'


def _pf_byte(m):
    return int(m.group(1)) * 16 + 'xyzw'.index(m.group(2)) * 4


def pass_plainfetch(text):
    """Renut's texture fetches apply Xenos' per-fetch-constant signs and gamma (SharedConstants.sampler_indices bits
    13-19: v[24 + r / 4]) and the exponent adjust (texture_indices bits 20-25: v[r / 4] for a 2D fetch) to the texel in
    a block that runs only when the fetch constant has any; for every other fetch the block is skipped and the texel is
    used as it is. The block is a uniform branch, but its temporaries count in the shader's registers (the race's
    G-buffer shader 33 GPRs with them, 28 without; the final pass 42 and 31). Constant 926, bit r: the draw's words for
    fetch constant r have none of the three (render/draw.cpp SasrtPlainFetchMask), so the block's condition is false at
    compile time and uam drops it. Its default 0 leaves every block as it was. Same pixels: the condition was false for
    that draw anyway."""
    if 'SPIRV_CROSS_CONSTANT_ID_926' in text or '>> 16u) & 15u;' not in text:
        return text
    is_vertex = re.search(r'\bgl_Position\b', text) is not None  # vivapinata-nx (2026-11-12): vertex shaders too
    edits = []
    for m in _PF_BLOCK.finditer(text):
        b, x, y, p, e = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        if p:
            mn = re.search(_PF_NOT.format(v=re.escape(p)), text)
            if not mn:
                continue
            x, y = mn.group(1), mn.group(2)
        ms = re.search(_PF_SIGNS.format(v=re.escape(x)), text)
        mg = re.search(_PF_GAMMA.format(v=re.escape(y)), text)
        me = re.search(_PF_EXP.format(v=re.escape(e)), text)
        if not (ms and mg and me):
            continue
        sb, gb, eb = _pf_byte(ms), _pf_byte(mg), _pf_byte(me)
        r = (sb - 384) // 4
        # the words of one fetch constant r: sampler_indices[r] (bytes 384-511) for both, texture_indices[d][r] (d the
        # fetch's dimension, bytes 0-383) for the exponent
        if sb != gb or not 384 <= sb < 512 or eb >= 384 or eb % 128 != 4 * r:
            continue
        # vivapinata-nx (2026-11-12, A/B 150): a samplerk read (its line after SK_MARK) skips its block only with 975
        gate = f'((g_sasrtPlainFetch >> {r}u) & 1u) == 0u'
        # 2026-11-12: and every fetch of a vertex shader (926 says the vertex shader's fetches too only since the shader
        # round, render/draw.cpp SasrtPlainFetchMask's vs_entry)
        if text[:ms.start()].rstrip().endswith(SK_MARK) or is_vertex:
            gate = f'(!g_vivaRound || {gate})'
        edits.append((m.end(), f'\n    {b} = {b} && {gate};'))
    if not edits:
        return text
    for at, code in reversed(edits):
        text = text[:at] + code + text[at:]
    text = insert_globals(text, PLAINFETCH_DECL)
    return with_viva_round(text) if '(!g_vivaRound || ' in text else text


# ---------------------------------------------------------------------------------------------------------------------
# pass: fetchconv — a fetch's sign, gamma and exponent words as constants of the draw (sasrt-nx, 2026-11-09, the fifth GPU
# round: sasrt_gpu_fetch_conversion_spec, constants 950-957)

_FC_SIGNS = re.compile(r'^(\s*uint (_\d+) = )(\(g_UboShared\.v\[(2[4-5])u\]\.([xyzw]) >> 16u\) & 15u);$', re.M)
_FC_GAMMA = re.compile(r'^(\s*uint (_\d+) = )(\(g_UboShared\.v\[(2[4-5])u\]\.([xyzw]) >> 13u\) & 7u);$', re.M)
_FC_EXP = re.compile(r'^(\s*int (_\d+) = )(int\(g_UboShared\.v\[([0-1])u\]\.([xyzw]) << 6u\) >> 26);$', re.M)
# sasrt-nx (2026-11-09, sasrt_gpu_fetch_exp_3d_spec): the 3D (v[8 + r / 4]) and cube (v[16 + r / 4]) words' exponent
_FC_EXP3 = re.compile(r'^(\s*int (_\d+) = )(int\(g_UboShared\.v\[(8|9|16|17)u\]\.([xyzw]) << 6u\) >> 26);$', re.M)


def _fc_decl(r):
    return (f'#ifndef SPIRV_CROSS_CONSTANT_ID_{950 + r}\n#define SPIRV_CROSS_CONSTANT_ID_{950 + r} 0u\n#endif\n'
            f'const uint g_sasrtConv{r} = SPIRV_CROSS_CONSTANT_ID_{950 + r};\n'
            f'const bool g_sasrtConvKnown{r} = (g_sasrtConv{r} >> 15u) != 0u;\n')


def pass_fetchconv(text):
    """Renut's fetch code reads fetch constant r's signs (sampler_indices[r] bits 16-19: v[24 + r / 4]), gamma (bits 13-15)
    and, for a 2D fetch, the exponent adjust (texture_indices[0][r] bits 20-25: v[r / 4]), and applies each to each
    channel through selects among the conversions: every conversion is computed for every channel where any applies.
    Constant 950 + r (fetch constant r < 8; render/draw.cpp SasrtSpecWordsOf [12]-[15]): bit 15 set, the draw's three
    words in bits 0-3, 4-6 and 7-12; each read of them becomes the constant, so the selects fold to the conversions this
    draw's texture has on the channels that have them, and the block's condition to false where none (as pass plainfetch,
    which it runs after). Default 0: each read as it was. Same values: the same pixels."""
    if 'SPIRV_CROSS_CONSTANT_ID_950' in text or '>> 16u) & 15u;' not in text:
        return text
    used = set()

    def slot(m, base):
        return (int(m.group(4)) - base) * 4 + 'xyzw'.index(m.group(5))

    def signs(m):
        r = slot(m, 24)
        if r >= 8:
            return m.group(0)
        used.add(r)
        return f'{m.group(1)}g_sasrtConvKnown{r} ? (g_sasrtConv{r} & 15u) : ({m.group(3)});'

    def gamma(m):
        r = slot(m, 24)
        if r >= 8:
            return m.group(0)
        used.add(r)
        return f'{m.group(1)}g_sasrtConvKnown{r} ? ((g_sasrtConv{r} >> 4u) & 7u) : ({m.group(3)});'

    def exp(m):
        r = slot(m, 0)
        if r >= 8:
            return m.group(0)
        used.add(r)
        return f'{m.group(1)}g_sasrtConvKnown{r} ? (int(g_sasrtConv{r} << 19u) >> 26) : ({m.group(3)});'

    used3 = set()

    def exp3(m):
        v = int(m.group(4))
        cube = v >= 16
        r = (v - (16 if cube else 8)) * 4 + 'xyzw'.index(m.group(5))
        if r >= 8:
            return m.group(0)
        cid = (966 if cube else 958) + r
        used3.add(cid)
        return f'{m.group(1)}((g_sasrtExp{cid} >> 7u) != 0u) ? (int(g_sasrtExp{cid} << 26u) >> 26) : ({m.group(3)});'

    text = _FC_SIGNS.sub(signs, text)
    text = _FC_GAMMA.sub(gamma, text)
    text = _FC_EXP.sub(exp, text)
    text = _FC_EXP3.sub(exp3, text)
    if not used and not used3:
        return text
    # vivapinata-nx (2026-11-12, A/B 150): a samplerk read (its line after SK_MARK) takes the known word only with 975
    text, marked = re.subn(r'(' + re.escape(SK_MARK) + r'\n[ \t]*uint _\d+ = )g_sasrtConvKnown(\d+) \?',
                           r'\1(g_vivaRound && g_sasrtConvKnown\2) ?', text)
    decl = ''.join(_fc_decl(r) for r in sorted(used))
    decl += ''.join(f'#ifndef SPIRV_CROSS_CONSTANT_ID_{c}\n#define SPIRV_CROSS_CONSTANT_ID_{c} 0u\n#endif\n'
                    f'const uint g_sasrtExp{c} = SPIRV_CROSS_CONSTANT_ID_{c};\n' for c in sorted(used3))
    text = insert_globals(text, decl)
    return with_viva_round(text) if marked else text


# ---------------------------------------------------------------------------------------------------------------------
# pass: onex — a draw at 1x (targets and every sampled texture): the texture scale codes and the pixel position's scale
# fold (sasrt-nx, 2026-11-08, the GPU round: sasrt_gpu_one_x, constant 926 bit 31)

ONEX_CODE_RE = re.compile(r'^(\s*uint (_\d+) = )(\(g_UboShared\.v\[\d+u\]\.[xyzw] >> 26u\) & 7u);$', re.M)


def pass_onex(text):
    """banjo-nx's scale1 and fragscale (left out of the default package since the internal resolutions) as a draw's
    choice: constant 926's bit 31 (render/draw.cpp SasrtPlainFetchMask: the draw's targets are 1x and the scale code of
    every texture word its pixel shader samples is 0; a pixel shader never samples fetch constant 31, so the bit is free)
    makes each texture word's scale code 0 (the seven-way select of the scale factor after it then folds to 1.0) and
    the pixel position's resolution scale (SharedConstants.resolution_scale) 1.0, which they are for that draw: the
    selects, the reciprocal and the multiplies go. Its default 0 leaves them as they were."""
    if 'SPIRV_CROSS_CONSTANT_ID_926' in text and 'g_sasrtOneX' in text:
        return text
    has_code = ONEX_CODE_RE.search(text) is not None
    span = main_span(text)
    has_scale = bool(span) and SCALE_WORD in text[span[0]:span[1]]
    if not has_code and not has_scale:
        return text
    text = ONEX_CODE_RE.sub(lambda m: f'{m.group(1)}g_sasrtOneX ? 0u : ({m.group(3)});', text)
    if has_scale:
        a, b = main_span(text)
        text = text[:a] + text[a:b].replace(SCALE_WORD, f'(g_sasrtOneX ? 1.0 : {SCALE_WORD})') + text[b:]
    onex = 'const bool g_sasrtOneX = (g_sasrtPlainFetch >> 31u) != 0u;\n'
    # vivapinata-nx (2026-11-12): in a vertex shader only with constant 975 too (bit 31 says the vertex shader's words
    # are at scale code 0 only since the shader round, render/draw.cpp SasrtPlainFetchMask's vs_entry; A/B 150)
    vertex = re.search(r'\bgl_Position\b', text) is not None
    if vertex:
        onex = 'const bool g_sasrtOneX = ((g_sasrtPlainFetch >> 31u) != 0u) && g_vivaRound;\n'
    anchor = 'const uint g_sasrtPlainFetch = SPIRV_CROSS_CONSTANT_ID_926;\n'
    if anchor in text:  # the plainfetch pass's declaration: after it
        text = text.replace(anchor, anchor + onex, 1)
    else:
        text = insert_globals(text, PLAINFETCH_DECL + onex)
    return with_viva_round(text) if vertex else text


# ---------------------------------------------------------------------------------------------------------------------
# pass: onexsize — a fetch's texel step at 1x without its float round trip (vivapinata-nx, 2026-11-12, the shader round)
# A fetch with a texel offset divides the offset by the texture's size at the draw's scale, written
# `vec2(uvec2(roundEven(vec2(uvec2(SIZE)) / vec2(CHAIN))))`, CHAIN the seven-way select of the scale factor on the texture
# word's scale code V. Where pass onex made V `g_sasrtOneX ? 0u : (...)`, the draws with constant 926's bit 31 have V = 0,
# so CHAIN is 1.0 (every arm compares V with a code of 1 or more; checked by evaluating it), x / 1.0 is x, x = float(u)
# is integral so roundEven(x) is x, and float(uint(float(u))) is float(u) for every u (float(u) is integral and at most
# 2^32, where the conversion back saturates to 2^32 - 1, whose float is 2^32 again). So the whole is vec2(uvec2(SIZE))
# in those draws: `(g_sasrtOneX ? vec2(uvec2(SIZE)) : <as it was>)`. Two conversions and a round a component (305 pixel
# shaders, 1,029 places). Without bit 31 the expression is as it was.

def _paren_end(t, i):
    """The index after the parenthesis that closes the one at t[i] == '(' (-1 if none)."""
    depth = 0
    for k in range(i, len(t)):
        c = t[k]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return k + 1
    return -1


def _top_args(t):
    """t's top-level comma-separated parts."""
    parts, depth, start = [], 0, 0
    for k, c in enumerate(t):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        elif c == ',' and depth == 0:
            parts.append(t[start:k].strip())
            start = k + 1
    parts.append(t[start:].strip())
    return parts


def _strip_parens(t):
    t = t.strip()
    while t.startswith('(') and _paren_end(t, 0) == len(t):
        t = t[1:-1].strip()
    return t


def _scale_chain_at_zero(t, var):
    """The value of a dk_sel chain on `var` when var is 0, as its text, or None if it is not such a chain."""
    t = _strip_parens(t)
    while t.startswith('dk_sel('):
        if _paren_end(t, 6) != len(t):
            return None
        args = _top_args(t[7:-1])
        if len(args) != 3:
            return None
        m = re.fullmatch(r'\((_\d+) == (\d+)u\)', args[0])
        if not m or m.group(1) != var or int(m.group(2)) == 0:
            return None
        t = _strip_parens(args[2])  # var == 0: the condition is false
    return t


_OS_HEAD = 'vec2(uvec2(roundEven(vec2(uvec2('


def pass_onexsize(text):
    if 'g_sasrtOneX' not in text or _OS_HEAD not in text:
        return text
    out, i, done = [], 0, 0
    while True:
        j = text.find(_OS_HEAD, i)
        if j < 0:
            out.append(text[i:])
            break
        end = _paren_end(text, j + 4)  # the outer vec2(...)
        inner = j + len('vec2(uvec2(roundEven(')
        size_end = _paren_end(text, inner + 4)  # vec2(uvec2(SIZE))
        ok = end > 0 and size_end > 0
        if ok:
            size = text[inner:size_end]
            rest = text[size_end:end]
            m = re.match(r' / vec2\(', rest)
            ok = bool(m) and rest.endswith(')))')
        if ok:
            div_open = size_end + len(' / vec2')
            div_end = _paren_end(text, div_open)
            ok = div_end == end - 3  # `... / vec2(CHAIN)` then the roundEven's, uvec2's and vec2's closing
        if ok:
            chain = text[div_open + 1:div_end - 1]
            mv = re.search(r'\((_\d+) == \d+u\)', chain)
            ok = bool(mv) and _scale_chain_at_zero(chain, mv.group(1)) == '1.0' and re.search(
                r'\buint ' + re.escape(mv.group(1)) + r' = g_sasrtOneX \? 0u : ', text) is not None
        if not ok:
            out.append(text[i:j + len(_OS_HEAD)])
            i = j + len(_OS_HEAD)
            continue
        out.append(text[i:j] + f'((g_sasrtOneX && g_vivaRound) ? {size} : {text[j:end]})')  # 975: A/B 150
        i = end
        done += 1
    return with_viva_round(''.join(out)) if done else text


# ---------------------------------------------------------------------------------------------------------------------
# pass: noearly — Renut's early alpha test made optional, constant 976 (vivapinata-nx, 2026-11-12, the shader round;
# A/B 151, render/draw.cpp vivapinata_shader_noearly)
# The translator's early alpha test (tools/renut_early_alpha.py) computes the alpha ahead of the main body and discards a
# quad whose four pixels all fail the alpha test by a margin; the main body's own test then discards each failing pixel.
# It pays where whole quads fail often and the main body after it is long. After the shader round the grass shell's main
# body is short: the test is 19 of its 80 instructions, run by every fragment, and saves about 40 for a quad it kills.
# Constant 976 = 0 skips the block: every quad it would have killed is killed by the main body's test (each of its
# pixels fails that test: the block fails a pixel only when it fails by the margin), and the block writes nothing the
# rest reads (checked here: no assignment inside it to a name declared outside it), so the same pixels. Default 1: as
# before. Which shaders gain is the console's answer (the A/B's items).
NOEARLY_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_976\n#define SPIRV_CROSS_CONSTANT_ID_976 1u\n#endif\n'
                'const bool g_vivaEarly = SPIRV_CROSS_CONSTANT_ID_976 != 0u;\n')


def _noearly_keep():
    """tools/noearly_keep.txt: the pixel shaders that keep the early test (the console's A/B 91, now 151, found them
    slower without)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'noearly_keep.txt')
    if not os.path.isfile(path):
        return set()
    return {l.split('#')[0].strip().upper() for l in open(path, encoding='utf-8') if l.split('#')[0].strip()}


NOEARLY_KEEP = _noearly_keep()


def pass_noearly(text, name=None):
    if 'SPIRV_CROSS_CONSTANT_ID_976' in text:
        return text
    if name and name.upper().replace('PS_', '').replace('.GLSL', '') in NOEARLY_KEEP:
        return text
    found = list(EARLY_IF_RE.finditer(text))
    if len(found) != 1:
        return text
    m = found[0]
    open_i = text.index('{', m.end() - 1)
    close_i = matching(text, open_i, '{', '}')
    if close_i < 0:
        return text
    block = text[open_i:close_i + 1]
    declared = set(re.findall(r'\b(?:float|int|uint|bool|[iub]?vec[234]|mat[234])\s+(_\d+)\b', block))
    for name in re.findall(r'(?<![\w.])(_\d+)(?:\.[xyzw]+)?\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)', block):
        if name not in declared:
            return text  # the block assigns something outside it: skipping it would change what follows
    cond_at = text.index('if (DK_ALPHA_FUNC != 7u)', m.start())
    text = text[:cond_at] + 'if (DK_ALPHA_FUNC != 7u && g_vivaEarly)' + text[cond_at + len('if (DK_ALPHA_FUNC != 7u)'):]
    return insert_globals(text, NOEARLY_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: earlykill — the final alpha test moved up to right after what it needs (vivapinata-nx, 2026-11-12, the shader
# round; constant 977, A/B 152, render/draw.cpp vivapinata_shader_earlykill)
# Renut's pixel shaders test alpha as their last statement, after all their colour arithmetic. A pixel the test discards
# still runs that arithmetic, and a warp whose pixels all fail runs it for nothing. With 977 = 1 the top-level
# statements of main() before the test are split in two: those the test needs (its operands, transitively), those that
# fetch a texture, take a derivative, discard or write an output (they must run for every pixel the quad still has), and
# those a staying statement reads, overwrites or writes after them, stay before it; the others (pure arithmetic only
# the colour uses) move after it in their order. Moving pure statements past a discard changes nothing for the pixels
# that survive it (the same operations on the same values, no derivative after the discard) and the test sees the
# same values, so the same pixels. 977 = 0 (default): the test where it was, the statements in the new order (the same
# values). The grass shell: its alpha kill at instruction 48 of 61 instead of 60; 98381523: 46 of 71 instead of 70.
EARLYKILL_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_977\n#define SPIRV_CROSS_CONSTANT_ID_977 0u\n#endif\n'
                  'const bool g_vivaKill = SPIRV_CROSS_CONSTANT_ID_977 != 0u;\n')
_EK_TEST = re.compile(r'\n    float (_\d+) = uintBitsToFloat\(g_UboShared\.v\[34u\]\.z\);\n'
                      r'    if \((!\(\(g_banjoAlphaFunc != 0u\) \? .*? : dk_alpha_pass\(g_UboShared\.v\[34u\]\.w, '
                      r'(_\d+(?:\.[xyzw])?), \1\)\))\)\n    \{\n        discard;\n    \}\n')
_EK_NAME = r'(?<![\w.])(_\d+)\b'
_EK_WRITE = re.compile(r'(?<![\w.])(_\d+)(?:\.[xyzw]+|\[[^\]]*\])?\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)')
_EK_DECL = re.compile(r'\b(?:float|int|uint|bool|[iub]?vec[234]|mat[234])\s+(_\d+)\b')
_EK_INCR = re.compile(r'(?:\+\+|--)\s*(_\d+)|(_\d+)\s*(?:\+\+|--)')
_EK_BARRIER = re.compile(r'\b(texture\w*|texelFetch\w*|textureSize|dFd\w+|fwidth\w*|interpolateAt\w+|image\w+|atomic\w+|'
                         r'barrier|discard|return|out_var_\w+|gl_FragDepth|gl_SampleMask)\b')


def _ek_units(body):
    """main()'s top-level statements, each a list of lines; a compound statement (an if / else chain, a do-while, a
    for, a block) with the attribute and comment lines before it is one."""
    lines = body.split('\n')
    units, cur, depth, i = [], [], 0, 0
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        cur.append(line)
        depth += line.count('{') - line.count('}')
        i += 1
        if depth > 0:
            continue
        if not s or s.startswith('//') or s.startswith('SPIRV_CROSS_') or s.startswith('#'):
            if s.startswith('#'):
                units.append(cur)
                cur = []
            continue
        if s.endswith('{') or re.match(r'^(if|for|while|do|switch)\b', s) or s == 'else':
            if s.endswith(';'):
                units.append(cur)
                cur = []
            continue
        if s == '}' or s.startswith('} '):
            j = i
            while j < len(lines) and not lines[j].strip():
                j += 1
            if j < len(lines) and re.match(r'^else\b', lines[j].strip()):
                continue
            units.append(cur)
            cur = []
            continue
        if s.endswith(';'):
            units.append(cur)
            cur = []
    if cur:
        units.append(cur)
    return units


def pass_earlykill(text):
    if 'SPIRV_CROSS_CONSTANT_ID_977' in text:
        return text
    span = main_span(text)
    if not span:
        return text
    a, b = span
    body = text[a:b]
    found = list(_EK_TEST.finditer(body))
    if len(found) != 1:
        return text
    m = found[0]
    before, test, after = body[:m.start() + 1], body[m.start() + 1:m.end()], body[m.end():]
    units = _ek_units(before.rstrip('\n'))
    if sum(len(u) for u in units) != len(before.rstrip('\n').split('\n')):
        return text  # the split lost or doubled a line: leave the shader as it is
    info = []
    for u in units:
        ut = '\n'.join(u)
        w = set(_EK_WRITE.findall(ut)) | set(_EK_DECL.findall(ut)) | {x or y for x, y in _EK_INCR.findall(ut)}
        info.append((w, set(re.findall(_EK_NAME, ut)), bool(_EK_BARRIER.search(ut)) or ut.lstrip().startswith('#')))
    n = len(units)
    need = set(re.findall(_EK_NAME, m.group(2)))
    stay = [inf[2] for inf in info]
    changed = True
    while changed:
        changed = False
        for k in range(n - 1, -1, -1):
            if stay[k]:
                continue
            w, r, _ = info[k]
            if w & need:
                stay[k] = changed = True
                continue
            for j in range(k + 1, n):
                if stay[j]:
                    wj, rj, _ = info[j]
                    if (w & rj) or (wj & r) or (wj & w):
                        stay[k] = changed = True
                        break
    moved = [k for k in range(n) if not stay[k]]
    if not moved:
        return text
    test_lines = test.rstrip('\n').split('\n')
    gated = ['        ' + l[4:] if l.startswith('    ') else l for l in test_lines[1:]]
    out = [l for k in range(n) if stay[k] for l in units[k]]
    out += [test_lines[0], '    if (g_vivaKill)', '    {'] + gated + ['    }']
    out += [l for k in moved for l in units[k]]
    out += ['    if (!g_vivaKill)', '    {'] + gated + ['    }']
    text = text[:a] + '\n'.join(out) + '\n' + after + text[b:]
    return insert_globals(text, EARLYKILL_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: facesign — the Xenos face register's sign test without its reciprocal (sasrt-nx, 2026-11-08, the GPU round:
# sasrt_gpu_face_sign, constant 928, 1 by default)

FACESIGN_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_928\n#define SPIRV_CROSS_CONSTANT_ID_928 1u\n#endif\n'
                 'const uint g_sasrtFaceSign = SPIRV_CROSS_CONSTANT_ID_928;\n')
_FS_TAIL = re.compile(r'\.x, (_\d+), (_\d+)\) >= 0\.0')
FLT_MAX_WORD = 'uintBitsToFloat(g_UboShared.v[83u].w)'


def pass_facesign(text):
    """Renut's shaders give the Xenos face register's sign as `clamp(1.0 / Y, -FLT_MAX, FLT_MAX) >= 0.0`, Y the pixel
    position (gl_FragCoord / the resolution scale, less half a pixel and the tile origin) times +1 or -1 by
    gl_FrontFacing: a reciprocal (a quarter-rate MUFU on Maxwell), two clamps and a compare a pixel, for one sign. For
    any Y that is not NaN the answer is Y's sign bit clear (1 / +0 is +inf, 1 / -0 is -inf, and the clamp keeps the
    sign); Y is never NaN (the position, the scale and the origin are finite, the scale above 0). Constant 928 (1 by
    default; 0 when sasrt_gpu_face_sign is off, render/pipeline.cpp) picks the sign bit; 0 the expression as it was."""
    if 'gl_FrontFacing' not in text or 'clamp(1.0 / (' not in text or 'SPIRV_CROSS_CONSTANT_ID_928' in text:
        return text
    hi_vars = set(re.findall(r'float (_\d+) = ' + re.escape(FLT_MAX_WORD) + ';', text))
    lo_vars = {lo: hi for lo, hi in re.findall(r'float (_\d+) = -(_\d+);', text) if hi in hi_vars}
    out, i, changed = [], 0, False
    key = 'clamp(1.0 / ('
    while True:
        j = text.find(key, i)
        if j < 0:
            break
        open_idx = j + len('clamp(1.0 / ')
        close = matching(text, open_idx, '(', ')')
        m = _FS_TAIL.match(text, close + 1) if close > 0 else None
        expr = text[open_idx:close + 1] if close > 0 else ''
        if (m and 'gl_FrontFacing' in expr and 'gl_FragCoord' in expr and lo_vars.get(m.group(1)) == m.group(2)):
            lo, hi = m.group(1), m.group(2)
            out.append(text[i:j])
            out.append(f'((g_sasrtFaceSign != 0u) ? ((floatBitsToUint({expr}.x) >> 31u) == 0u) : '
                       f'(clamp(1.0 / {expr}.x, {lo}, {hi}) >= 0.0))')
            i = m.end()
            changed = True
        else:
            out.append(text[i:j + 1])
            i = j + 1
    if not changed:
        return text
    out.append(text[i:])
    return insert_globals(''.join(out), FACESIGN_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: gpr32 — the listed shaders get constant 910: compiled with 32 GPRs at most when it is 1

GPR32_LIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gpr32_shaders.txt')
GPR32_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_910\n#define SPIRV_CROSS_CONSTANT_ID_910 0u\n#endif\n'
              'const uint g_banjoGpr32 = SPIRV_CROSS_CONSTANT_ID_910;\n')
_GPR32 = None


def pass_gpr32(text, name=None):
    """On Maxwell a warp's registers come in blocks of 8 per thread: 33-40 GPRs leave about 48 of the SM's 64 warp
    slots, 32 all of them, to hide texture latency with. The pixel shaders tools/gpr32_shaders.txt lists (33-40 GPRs:
    the fur family, the top of the console's GPU profile) declare constant 910; with 910 = 1 (banjo_deko3d_gpr32,
    off by default: whether the spills cost less than the occupancy gives is for the console to measure) uam
    compiles them with a 32-GPR register file (compiler_iface.cpp CompileBest: the define's text), spilling the rest
    (the fur shader: 15 loads, 5 stores). The constant changes no code by itself."""
    global _GPR32
    if _GPR32 is None:
        _GPR32 = set()
        if os.path.isfile(GPR32_LIST):
            for line in open(GPR32_LIST, encoding='utf-8'):
                line = line.strip()
                if line and not line.startswith('#'):
                    _GPR32.add(line.upper())
    if name is None or name.upper() not in _GPR32 or 'SPIRV_CROSS_CONSTANT_ID_910' in text:
        return text
    return insert_before_main(text, GPR32_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: bonesel — a predicated bone's data select under its own accumulate select (2026-10-20)

BONESEL_NAMED_RE = re.compile(r'mix\((_\d+), \((_\d+) \* ([\w.]+)\) \+ \1, (_\d+)\)')


def pass_bonesel(text):
    """Skinning with predicated bones (pred-select): bone k's rows are `D = mix(P, L, c)` (the fetch L when the bone's
    weight is not zero, else the register's old value P) and its sum `mix(A, (D * w) + A, c)`. Under the same c the
    sum keeps A whenever D would be P, so the sum reads L itself: `mix(A, (L * w) + A, c)` (the same multiply-add of
    the same values when c holds). D stays defined for any other reader; with none left (the next bone's data select
    goes the same way) the compiler drops it: a select per component less (36 a vertex for 3 predicated bones). Both
    forms: D a variable (`vec4 D = mix(P, L, c);`, then `vec4 D_bl = L;` ahead of it) and the data select written
    inside the sum."""
    span = main_span(text)
    if not span:
        return text
    a, b = span
    body = text[a:b]
    count = 0
    # Named: D defined by a select on the sum's own condition.
    defs = {}
    for m in re.finditer(r'^([ \t]*)vec4 (_\d+) = mix\((.*)\);$', body, re.M):
        args = _split_args(m.group(3))
        if len(args) == 3 and re.fullmatch(r'_\d+', args[2].strip()) and re.fullmatch(r'_\d+', args[0].strip()):
            defs[m.group(2)] = (m, args[1].strip(), args[2].strip())
    renamed = {}

    def named(mm):
        nonlocal count
        acc, d, w, c = mm.groups()
        info = defs.get(d)
        if not info or info[2] != c:
            return mm.group(0)
        renamed[d] = info
        count += 1
        return f'mix({acc}, ({d}_bl * {w}) + {acc}, {c})'
    body = BONESEL_NAMED_RE.sub(named, body)
    for d, (m, load, c) in renamed.items():
        line = re.compile(r'^([ \t]*)vec4 ' + d + r' = mix\(', re.M).search(body)
        if not line:
            return text
        body = body[:line.start()] + f'{line.group(1)}vec4 {d}_bl = {load};\n' + body[line.start():]
        # the data select itself reads D_bl (one load, as before)
        body = body.replace(f'vec4 {d} = mix({m.group(3)});', f'vec4 {d} = mix({_split_args(m.group(3))[0].strip()}, '
                            f'{d}_bl, {c});', 1)
    # Inline: mix(A, (mix(P, L, c) * w) + A, c).
    out, pos = [], 0
    for mm in re.finditer(r'mix\((_\d+), \(mix\(', body):
        if mm.start() < pos:
            continue
        acc = mm.group(1)
        inner_open = mm.end() - 1
        inner_close = matching(body, inner_open, '(', ')')
        if inner_close < 0:
            continue
        iargs = _split_args(body[inner_open + 1:inner_close])
        if len(iargs) != 3:
            continue
        c = iargs[2].strip()
        tail = re.match(r' \* ([\w.]+)\) \+ ' + re.escape(acc) + r', ' + re.escape(c) + r'\)', body[inner_close + 1:])
        if not re.fullmatch(r'_\d+', c) or not tail:
            continue
        out.append(body[pos:mm.start()])
        out.append(f'mix({acc}, ({iargs[1].strip()} * {tail.group(1)}) + {acc}, {c})')
        pos = inner_close + 1 + tail.end()
        count += 1
    if pos:
        out.append(body[pos:])
        body = ''.join(out)
    if not count:
        return text
    return text[:a] + body + text[b:]


# ---------------------------------------------------------------------------------------------------------------------
# pass: deadcomp — a constructor's component overwritten before anything reads it (2026-10-20)

_DC_CTOR = re.compile(r'^([ \t]*)vec([234]) (_\d+) = vec\2\((.*)\);$')


def pass_deadcomp(text):
    """`vec4 x = vec4(a, b, c, d);` then, in the same block before any read of x, `x.w = e;`: the constructor's d is
    never read, so it is 0.0 (a value the next light's vector carries from the light before goes, which guardsel then
    sees). Only plain single-component writes at the same level whose right side does not mention x count; any other
    occurrence of x, a deeper or shallower line, ends the search."""
    span = main_span(text)
    if not span:
        return text
    a, b = span
    lines = text[a:b].split('\n')
    count = 0
    for i, ln in enumerate(lines):
        m = _DC_CTOR.match(ln)
        if not m:
            continue
        ind, n, x = m.group(1), int(m.group(2)), m.group(3)
        args = _split_args(m.group(4))
        if len(args) != n:
            continue
        name_re = re.compile(r'(?<![\w.])' + re.escape(x) + r'\b')
        dead = set()
        for j in range(i + 1, len(lines)):
            lj = lines[j]
            if not lj.strip():
                continue
            if not lj.startswith(ind) or lj[len(ind):len(ind) + 1] in (' ', '\t') or lj.strip() in ('{', '}'):
                break
            w = re.match(r'^[ \t]*' + re.escape(x) + r'\.([xyzw]) = (.*);$', lj)
            if w and not name_re.search(w.group(2)):
                c = 'xyzw'.index(w.group(1))
                if c < n:
                    dead.add(c)
                continue
            if name_re.search(lj):
                break
        dead = {c for c in dead if args[c].strip() != '0.0'}
        if not dead:
            continue
        for c in dead:
            args[c] = ' 0.0' if c else '0.0'
        lines[i] = f'{ind}vec{n} {x} = vec{n}({",".join(args)});'
        count += len(dead)
    if not count:
        return text
    return text[:a] + '\n'.join(lines) + text[b:]


# ---------------------------------------------------------------------------------------------------------------------
# pass: guardsel — a select whose other arm nobody reads (2026-10-20)

_GS_VAR = re.compile(r'(?<![\w.])(_\d+)\b')
_GS_TYPES = r'(?:[biu]?vec[234]|float|int|uint|bool)'
_GS_ASSIGN = re.compile(r'^([ \t]*)(?:' + _GS_TYPES + r' )?(_\d+)((?:\.[xyzw]+)?) = (.*);$')
_GS_DECL = re.compile(r'^\s*' + _GS_TYPES + r' _\d+;$')


def _gs_selects(expr, booleans):
    """(condition, true arm span, false arm span, call span) of each select in expr: dk_sel(c, a, b), and mix(b, a, s)
    where s is bvecN(x) or a bool / bvec variable (a mix by a float is a blend, never taken)."""
    out = []
    for m in re.finditer(r'(?<![\w.])(dk_sel|mix)\(', expr):
        op = m.end() - 1
        cl = matching(expr, op, '(', ')')
        if cl < 0:
            continue
        args = _split_args(expr[op + 1:cl])
        if len(args) != 3:
            continue
        spans, pos = [], op + 1
        for x in args:
            spans.append((pos, pos + len(x)))
            pos += len(x) + 1
        if m.group(1) == 'dk_sel':
            out.append((args[0], spans[1], spans[2], (m.start(), cl + 1)))
            continue
        sel = args[2].strip()
        mm = re.fullmatch(r'bvec[234]\((_\d+|true|false)\)', sel)
        if mm and (mm.group(1) in booleans or mm.group(1) in ('true', 'false')):
            out.append((mm.group(1), spans[1], spans[0], (m.start(), cl + 1)))
        elif sel in booleans:
            out.append((sel, spans[1], spans[0], (m.start(), cl + 1)))
    return out


def _gs_analyse(text):
    """pass_guardsel's analysis (also pass_lightbranch's): None, or the main body's lines and what is known
    about them (see pass_guardsel)."""
    span = main_span(text)
    if not span:
        return None
    a, b = span
    body = text[a:b]
    if re.search(r'\b(for|while|do)\b', body):
        return None
    lines = body.split('\n')
    booleans = set(re.findall(r'\b(?:bool|bvec[234]) (_\d+)\b', text))
    assigns = {}
    for i, ln in enumerate(lines):
        m = _GS_ASSIGN.match(ln)
        if m:
            assigns.setdefault(m.group(2), []).append(i)
    alias = {}
    for v, idx in assigns.items():
        if len(idx) == 1:
            m = (re.match(r'^\s*bvec[234] ' + v + r' = bvec[234]\((_\d+|true|false)\);$', lines[idx[0]]) or
                 re.match(r'^\s*bool ' + v + r' = (_\d+)(?: && \1)?;$', lines[idx[0]]))
            if m:
                alias[v] = m.group(1)

    def norm(c):
        c = _unparen(c)
        while c in alias:
            c = alias[c]
        return c

    def block_close(k):  # the line of the } closing the { at line k
        d = 0
        for e in range(k, len(lines)):
            s2 = lines[e].strip()
            if s2 == '{':
                d += 1
            elif s2 == '}':
                d -= 1
                if d == 0:
                    return e
        return -1

    def block_of(i):  # (open, close) of the innermost block holding line i
        depth = 0
        for k in range(i - 1, -1, -1):
            s2 = lines[k].strip()
            if s2 == '}':
                depth += 1
            elif s2 == '{':
                if depth == 0:
                    return k, block_close(k)
                depth -= 1
        return None

    def exclusive(i, j):  # line i in an if's then-block, line j in its else-block
        blk = block_of(i)
        if not blk or blk[0] < 1 or not lines[blk[0] - 1].strip().startswith('if ('):
            return False
        k = blk[1] + 1
        if k + 1 >= len(lines) or lines[k].strip() != 'else' or lines[k + 1].strip() != '{':
            return False
        return k + 1 < j < block_close(k + 1)

    single_cache = {}

    def single(c):
        if re.fullmatch(r'_\d+', c) is None:
            return False
        if c not in single_cache:
            idx = assigns.get(c, [])
            single_cache[c] = len(idx) == 1 or (len(idx) == 2 and exclusive(idx[0], idx[1]))
        return single_cache[c]

    # a float that is a condition's 0 / 1 (the bool01 pass's float(c) and float(!c))
    as_float = {}
    for v, idx in assigns.items():
        if len(idx) == 1:
            m = re.match(r'^\s*float ' + v + r' = float\((!?)\(?(_\d+)\)?\);$', lines[idx[0]])
            if m:
                as_float[v] = (m.group(2), not m.group(1))

    def if_guard(cond_line):  # the guard of an `if (c)` / `if (!c)` / `if (!(c))` / `if (f == 0.0)` line's block
        m = re.fullmatch(r'\s*if \((!?)\(?(_\d+)\)?\)', cond_line)
        if m:
            c = norm(m.group(2))
            return (c, not m.group(1)) if single(c) else None
        m = re.fullmatch(r'\s*if \((_\d+) == ([01])\.0\)', cond_line)
        if m and m.group(1) in as_float:
            c, pol = as_float[m.group(1)]
            c = norm(c)
            return (c, pol == (m.group(2) == '1')) if single(c) else None
        return None

    # the if / else blocks around each line (guards of the block conditions)
    block_guards = []
    stack, last_closed = [], None
    for i, ln in enumerate(lines):
        s = ln.strip()
        if s == '{':
            prev = lines[i - 1].strip() if i else ''
            if prev.startswith('if ('):
                stack.append(if_guard(lines[i - 1]))
            elif prev == 'else' and last_closed is not None:
                stack.append((last_closed[0], not last_closed[1]))
            else:
                stack.append(None)
            last_closed = None
        elif s == '}':
            last_closed = stack.pop() if stack else None
        elif s != 'else':
            last_closed = None
        block_guards.append(frozenset(g for g in stack if g is not None))

    # implications between conditions: (x, true) => (a, true) for x = a && b; (x, p) => (a, !p) for x = !a; a
    # condition assigned once in each arm of an if / else implies what both arms imply (an arm assigning false, or
    # true for the false side, implies everything: it never gives that value)
    imp = {}

    def add_imp(k, g):
        imp.setdefault(k, set()).add(g)
    every = object()
    two_arm = {}
    for v, idx in assigns.items():
        if not single(v):
            continue
        if len(idx) == 1:
            m = re.match(r'^\s*bool ' + v + r' = (.+);$', lines[idx[0]])
            if not m:
                continue
            e = _unparen(m.group(1))
            parts = [_unparen(x) for x in e.split(' && ')] if ' && ' in e and ' || ' not in e else None
            if parts and all(re.fullmatch(r'_\d+', x) for x in parts):
                for x in parts:
                    x = norm(x)
                    if single(x):
                        add_imp((v, True), (x, True))
                continue
            parts = [_unparen(x) for x in e.split(' || ')] if ' || ' in e and ' && ' not in e else None
            if parts and all(re.fullmatch(r'_\d+', x) for x in parts):
                for x in parts:
                    x = norm(x)
                    if single(x):
                        add_imp((v, False), (x, False))
                continue
            m2 = re.fullmatch(r'!\(?(_\d+)\)?', e)
            if m2 and single(norm(m2.group(1))):
                x = norm(m2.group(1))
                add_imp((v, True), (x, False))
                add_imp((v, False), (x, True))
        else:
            arms = []
            for i in idx:
                m = re.match(r'^\s*' + v + r' = (.+);$', lines[i])
                arms.append((_unparen(m.group(1)) if m else None, i))
            if all(e is not None for e, _ in arms):
                two_arm[v] = arms

    def closure_now(gs):
        out, todo = set(gs), list(gs)
        while todo:
            g = todo.pop()
            for h in imp.get(g, ()):
                if h not in out:
                    out.add(h)
                    todo.append(h)
        return out

    # the two-arm rule needs what the arms imply: to a fixpoint
    grown = True
    while grown:
        grown = False
        for v, arms in two_arm.items():
            for pol in (True, False):
                sets = []
                for e, i in arms:
                    if e == ('false' if pol else 'true'):
                        sets.append(every)
                        continue
                    s = set()
                    if re.fullmatch(r'_\d+', e) and single(norm(e)):
                        s |= closure_now({(norm(e), pol)})
                    blk = block_of(i)
                    if blk and blk[0] >= 1:
                        s |= closure_now(block_guards[blk[0]] - block_guards[blk[0] - 1])
                    sets.append(s)
                real = [s for s in sets if s is not every]
                common = set.intersection(*real) if real else set()
                for g in common:
                    if g not in imp.get((v, pol), ()):
                        add_imp((v, pol), g)
                        grown = True

    closure_cache = {}

    def closure(gs):
        if gs in closure_cache:
            return closure_cache[gs]
        out, todo = set(gs), list(gs)
        while todo:
            g = todo.pop()
            for h in imp.get(g, ()):
                if h not in out:
                    out.add(h)
                    todo.append(h)
        out = frozenset(out)
        closure_cache[gs] = out
        return out

    uses = {}  # variable -> [(guards at the read, the variable the read's statement assigns or None)]
    parsed = []
    for i, ln in enumerate(lines):
        if _GS_DECL.match(ln):
            parsed.append(None)
            continue
        m = _GS_ASSIGN.match(ln)
        if m:
            target, start = m.group(2), len(ln) - len(m.group(4)) - 1
        else:
            target, start = None, 0
        expr = ln[start:-1] if m else ln
        sels = _gs_selects(expr, booleans)
        parsed.append((target, start) if m else None)
        for vm in _GS_VAR.finditer(expr):
            p = vm.start()
            g = set(block_guards[i])
            dead = False
            for cond, ts, fs, _ in sels:
                c = norm(cond)
                if c in ('true', 'false'):  # the arm a literal never takes reads nothing
                    dead = dead or (fs if c == 'true' else ts)[0] <= p < (fs if c == 'true' else ts)[1]
                    continue
                if not single(c):
                    continue
                if ts[0] <= p < ts[1]:
                    g.add((c, True))
                elif fs[0] <= p < fs[1]:
                    g.add((c, False))
            if not dead:
                uses.setdefault(vm.group(1), []).append((frozenset(g), target))
    top = None
    guard = {v: top for v in assigns}
    changed = True
    while changed:
        changed = False
        for v in assigns:
            acc = top
            for g, target in uses.get(v, []):
                if target is not None and target in assigns:
                    tg = guard[target]
                    s = top if tg is top else closure(g | tg)
                else:
                    s = closure(g)
                if s is top or any((c, not p) in s for c, p in s):  # never read, or read under a contradiction
                    continue
                acc = s if acc is top else (acc & s)
            if acc != guard[v]:
                guard[v] = acc
                changed = True
    return types.SimpleNamespace(a=a, b=b, lines=lines, parsed=parsed, guard=guard, uses=uses, imp=imp,
                                 closure=closure, norm=norm, single=single, booleans=booleans,
                                 assigns=assigns, block_guards=block_guards)


def pass_guardsel(text):
    """A value read only where a condition c holds (inside the chosen arm of selects on c, directly or through the
    values it feeds) never shows what a select on c inside its own definition gives when c does not hold: that select
    is its chosen arm. Renut's lights select every term on the light's flag (`dk_sel(f, clamp(...), old)`) and then
    add it only under the same flag (`mix(acc, (t * k) + acc, bvec3(f))`): the selects go (20 in the fur shader).
    A greatest fixpoint over every variable's readers; the conditions are variables with one value a run: assigned
    once, or once in each arm of one if / else. Not in a body with loops (a condition could change between the reads).
    Exact: a value changes only in runs where nothing reads it."""
    an = _gs_analyse(text)
    if an is None:
        return text
    a, b, lines, parsed, guard, norm, booleans = an.a, an.b, an.lines, an.parsed, an.guard, an.norm, an.booleans
    count = 0
    for i, info in enumerate(parsed):
        if not info:
            continue
        target, start = info
        g = guard.get(target)
        if not g:
            continue
        expr = lines[i][start:-1]
        while True:
            for cond, ts, fs, (cs, ce) in _gs_selects(expr, booleans):
                c = norm(cond)
                if (c, True) in g:
                    keep = expr[ts[0]:ts[1]]
                elif (c, False) in g:
                    keep = expr[fs[0]:fs[1]]
                else:
                    continue
                expr = expr[:cs] + '(' + keep.strip() + ')' + expr[ce:]
                count += 1
                break
            else:
                break
        lines[i] = lines[i][:start] + expr + ';'
    if not count:
        return text
    return text[:a] + '\n'.join(lines) + text[b:]


# ---------------------------------------------------------------------------------------------------------------------
# pass: lightbranch — a light's work skipped where the light does not reach (2026-10-20, constant 911, A/B)

LIGHTBRANCH_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_911\n#define SPIRV_CROSS_CONSTANT_ID_911 0u\n#endif\n'
                    'const uint g_banjoLightBranch = SPIRV_CROSS_CONSTANT_ID_911;\n')
_LB_LIST_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'lightbranch_shaders.txt')
_LB_FORBIDDEN = re.compile(r'\b(texture\w*|texelFetch\w*|dFd\w+|fwidth\w*|discard|out_var_\w+|gl_FragDepth|return|'
                           r'imageStore|atomic\w*)\b')


def _lb_list():
    try:
        return {ln.split('#')[0].strip().upper() for ln in open(_LB_LIST_PATH, encoding='utf-8')
                if ln.split('#')[0].strip()}
    except OSError:
        return set()


_LIGHTBRANCH = _lb_list()


def pass_lightbranch(text, name=None):
    """After guardsel and deadcomp a light's work is read only where its attenuation test c holds, except the sums it
    adds to (`acc2 = mix(acc, x + acc, bvecN(f))` with f => c, which keep acc where c does not hold). The statements
    after `bool c = ...;` up to the last one whose results are read later go into `if (g_banjoLightBranch == 0u || c)`
    with an else that gives each sum its previous value (the declarations of what is read later move ahead of it).
    Specialization constant 911 (0 by default: the if folds away, the code as before; banjo_deko3d_light_branch gives
    1): warps whose pixels are all out of a light's reach skip it. Exact: with c true the same statements run; with c
    false nothing reads their results but the sums, which get the value their select gives. Not over texture fetches,
    derivatives, discards or outputs. Only the shaders of tools/lightbranch_shaders.txt (an A/B for the console)."""
    if name is None or name.upper() not in _LIGHTBRANCH or 'SPIRV_CROSS_CONSTANT_ID_911' in text:
        return text
    an = _gs_analyse(text)
    if an is None:
        return text
    lines, guard, closure, norm, single = an.lines, an.guard, an.closure, an.norm, an.single
    ind_of = [len(ln) - len(ln.lstrip(' \t')) for ln in lines]
    # every read of each variable: (line, assigning target or None)
    reads = {}
    for i, ln in enumerate(lines):
        m = _GS_ASSIGN.match(ln)
        expr = ln[len(ln) - len(m.group(4)) - 1:-1] if m else ln
        if _GS_DECL.match(ln):
            continue
        for vm in _GS_VAR.finditer(expr):
            reads.setdefault(vm.group(1), []).append(i)
    edits = []  # (start, end, cond, hoisted [(type, var)], else [(var, value)])
    taken_until = -1
    for i, ln in enumerate(lines):
        m = re.match(r'^([ \t]*)bool (_\d+) = (.+);$', ln)
        if not m or i <= taken_until or not single(m.group(2)):
            continue
        ind, c = m.group(1), m.group(2)
        # the block's statements after the condition: whole lines at this level (nested blocks taken whole)
        ends = []
        j = i + 1
        while j < len(lines):
            s = lines[j].strip()
            if not s:
                j += 1
                continue
            if ind_of[j] < len(ind) or s == '}' and ind_of[j] == len(ind):
                break
            if ind_of[j] > len(ind):
                break  # not expected right after a statement at this level
            k = j
            if s.startswith('if (') or s == 'else':
                # take the if / else blocks whole
                k = j + 1
                while k < len(lines):
                    if lines[k].strip() == '{' and ind_of[k] == len(ind):
                        d = 0
                        for e in range(k, len(lines)):
                            if lines[e].strip() == '{':
                                d += 1
                            elif lines[e].strip() == '}':
                                d -= 1
                                if d == 0:
                                    k = e
                                    break
                        if k + 1 < len(lines) and lines[k + 1].strip() == 'else':
                            k += 2
                            continue
                        break
                    k += 1
            ends.append(k)
            j = k + 1
        best = None
        for e in ends:
            region = range(i + 1, e + 1)
            if any(_LB_FORBIDDEN.search(lines[r]) for r in region):
                break
            ok, hoist, elses = True, [], []
            assigned = {}
            for r in region:
                mm = _GS_ASSIGN.match(lines[r])
                dm = re.match(r'^\s*(' + _GS_TYPES + r') (_\d+);$', lines[r])
                if mm:
                    assigned.setdefault(mm.group(2), r)
                elif dm:
                    assigned.setdefault(dm.group(2), r)
            for v, first in assigned.items():
                later = [r for r in reads.get(v, []) if r > e]
                if not later:
                    continue
                g = guard.get(v)
                if g is not None and (c, True) in closure(g):
                    tm = re.match(r'^\s*(' + _GS_TYPES + r') ' + v + r'\b', lines[first])
                    hoist.append((tm.group(1) if tm else None, v))
                    continue
                # a sum: `T v = mix(P, X, bvecN(f));` assigned once in the region, at its level, with f => c
                defs = [r for r in region if (_GS_ASSIGN.match(lines[r]) or [None, None, None])[2] == v]
                sm = re.match(r'^[ \t]*(vec[234]|float) ' + v + r' = mix\((.*)\);$', lines[defs[0]]) if len(
                    defs) == 1 and ind_of[defs[0]] == len(ind) else None
                if sm:
                    args = _split_args(sm.group(2))
                    sel = args[2].strip() if len(args) == 3 else ''
                    fm = re.fullmatch(r'bvec[234]\((_\d+)\)', sel)
                    f = norm(fm.group(1) if fm else sel)
                    prev = args[0].strip()
                    # a region-local copy of an outside value (`vec3 p = acc.xyz;`, assigned once): its expression
                    pm = re.fullmatch(r'(_\d+)', prev)
                    if pm and pm.group(1) in assigned:
                        pdefs = [r for r in region if (_GS_ASSIGN.match(lines[r]) or [None, None, None])[2] ==
                                 pm.group(1)]
                        pd = _GS_ASSIGN.match(lines[pdefs[0]]) if len(pdefs) == 1 else None
                        if pd and not pd.group(3):
                            prev = '(' + pd.group(4).strip() + ')'
                    prev_vars = _GS_VAR.findall(prev)
                    if (re.fullmatch(r'_\d+', f) and single(f) and (c, True) in closure(frozenset([(f, True)])) and
                            all(pv not in assigned for pv in prev_vars)):
                        hoist.append((sm.group(1), v))
                        elses.append((v, prev))
                        continue
                ok = False
                break
            if not ok:
                continue
            if any(t is None for t, _ in hoist):
                continue
            n_stmt = sum(1 for r in region if _GS_ASSIGN.match(lines[r]))
            if n_stmt >= 8:
                best = (i + 1, e, c, hoist, elses)
        if best:
            edits.append((best[0], best[1], best[2], best[3], best[4], ind))
            taken_until = best[1]
    if not edits:
        return text
    out = list(lines)
    for start, end, c, hoist, elses, ind in reversed(edits):
        names = {v for _, v in hoist}
        body = []
        for r in range(start, end + 1):
            ln = out[r]
            mm = re.match(r'^([ \t]*)(' + _GS_TYPES + r') (_\d+)( = .*;|;)$', ln)
            if mm and mm.group(3) in names:
                if mm.group(4) == ';':
                    continue  # declared ahead
                ln = f'{mm.group(1)}{mm.group(3)}{mm.group(4)}'
            body.append('    ' + ln if ln.strip() else ln)
        block = [f'{ind}{t} {v};' for t, v in hoist]
        block.append(f'{ind}if (g_banjoLightBranch == 0u || {c})')
        block.append(f'{ind}{{')
        block.extend(body)
        block.append(f'{ind}}}')
        if elses:
            block.append(f'{ind}else')
            block.append(f'{ind}{{')
            block.extend(f'{ind}    {v} = {p};' for v, p in elses)
            block.append(f'{ind}}}')
        out[start:end + 1] = block
    text = text[:an.a] + '\n'.join(out) + text[an.b:]
    return insert_before_main(text, LIGHTBRANCH_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: nullps — a diagnostic: the listed pixel shaders discard at once (2026-10-20, constant 912)

NULLPS_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_912\n#define SPIRV_CROSS_CONSTANT_ID_912 0u\n#endif\n'
               'const uint g_banjoNullPs = SPIRV_CROSS_CONSTANT_ID_912;\n')
_NULLPS_LIST_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nullps_shaders.txt')


def _nullps_list():
    try:
        return {ln.split('#')[0].strip().upper() for ln in open(_NULLPS_LIST_PATH, encoding='utf-8')
                if ln.split('#')[0].strip()}
    except OSError:
        return set()


_NULLPS = _nullps_list()


def pass_nullps(text, name=None):
    """Diagnostic (banjo_renut_diag_null_ps, constant 912 = 1): the listed pixel shaders (tools/nullps_shaders.txt, the
    fur family, which discard already) discard as their first statement, so the console's per-shader GPU profile shows
    what their draws cost without the pixel work (vertex shading, rasterisation, the pixel launches): the fur's vertex
    share. 0 (default): the branch folds away, the shader as before. Not a picture to keep (the fur vanishes)."""
    if name is None or name.upper() not in _NULLPS or 'SPIRV_CROSS_CONSTANT_ID_912' in text or 'discard' not in text:
        return text
    m = re.search(r'\nvoid main\(\)\n\{\n', text)
    if not m:
        return text
    text = text[:m.end()] + '    if (g_banjoNullPs != 0u)\n    {\n        discard;\n    }\n' + text[m.end():]
    return insert_before_main(text, NULLPS_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: aw4 — a bone row's four address-window words as one 16-byte load (2026-10-20)

AW4_DECL = ('layout(binding = 15, std430) readonly buffer type_ByteAddressBuffer4\n{\n    uvec4 _m0[];\n} '
            'g_DkAddressWindow4;\n')
AW4_INDEX_RE = re.compile(r'\(g_UboShared\.v\[35u\]\.x \+ (?:min\(\((_\d+) \+ \(uint\((_\d+)(?: \+ (\d+))?\) \* 4u\)\), '
                          r'(_\d+)\)|\((_\d+) \+ \(uint\((_\d+)(?: \+ (\d+))?\) \* 4u\)\))\) >> 2u')


def pass_aw4(text):
    """The vertex fetch from memory (g_DkAddressWindow, shader_common.h vfetchMemory) reads a bone row as four words
    W, W + 1, W + 2, W + 3 (four 32-bit loads and the index adds). Where W is provably a multiple of 4 the row is one
    uvec4 load of the same binding (a second view, g_DkAddressWindow4) at W >> 2. W = (window offset + slot base +
    4 * (I + k)) >> 2, and each part is a multiple of 16 bytes: the window offset (SharedConstants.vertex_fetch_data's
    low byte) and the slot base (vertex_fetch_slots, minus the endian bits) because render/draw.cpp's fetch copies
    start at 16-byte multiples of 256-byte aligned allocations; 4 * (I + k) because I is `int(floor(x)) * K` with K a
    multiple of 4 and k one too; the clamp min(..., C) because C is vertex_fetch_bytes - 16 (a multiple of 16). Only
    rows whose four loads and index definitions are all in one block at the same level (the loads run there anyway)."""
    span = main_span(text)
    if not span or 'g_DkAddressWindow._m0[' not in text or 'g_DkAddressWindow4' in text:
        return text
    a, b = span
    body = text[a:b]
    lines = body.split('\n')
    defs = {}  # name -> (line index, indentation, expression)
    for i, ln in enumerate(lines):
        m = re.match(r'^([ \t]*)(?:uint|int) (_\d+) = (.+);$', ln)
        if m:
            defs[m.group(2)] = (i, m.group(1), m.group(3))

    def mult16_int(name):  # I with 4 * I a multiple of 16
        d = defs.get(name)
        if not d:
            return False
        m = re.fullmatch(r'int\(floor\([^;]*\)\) \* (\d+)', d[2])
        return bool(m) and int(m.group(1)) % 4 == 0

    def slot_base(name):
        d = defs.get(name)
        return bool(d) and re.fullmatch(r'g_UboShared\.v\[\d+u\]\.[xyzw] & 4294967292u', d[2]) is not None

    def clamp_bound(name):
        d = defs.get(name)
        return bool(d) and d[2] == 'max(g_UboShared.v[60u].w, 16u) - 16u'

    load_lines = {}
    for i, ln in enumerate(lines):
        for v in re.findall(r'g_DkAddressWindow\._m0\[(_\d+)\]', ln):
            load_lines.setdefault(v, []).append(i)
    inserts = {}  # line index -> lines to insert after it
    replace = {}  # word index variable -> 'Q.x'
    rows = 0
    for w, (wi, ind, expr) in defs.items():
        m = AW4_INDEX_RE.fullmatch(expr)
        if not m or w not in load_lines:
            continue
        if m.group(1):
            base, i_var, k, bound = m.group(1), m.group(2), m.group(3), m.group(4)
            if not clamp_bound(bound):
                continue
        else:
            base, i_var, k = m.group(5), m.group(6), m.group(7)
        if not slot_base(base) or not mult16_int(i_var) or (k is not None and int(k) % 4):
            continue
        sibs = {}
        for s, (si, sind, sexpr) in defs.items():
            mm = re.fullmatch(re.escape(w) + r' \+ ([123])u', sexpr)
            if mm and sind == ind and s in load_lines:
                sibs[int(mm.group(1))] = (s, si)
        if len(sibs) != 3:
            continue
        group = [(w, wi)] + [sibs[n] for n in (1, 2, 3)]
        last = max(i for _, i in group)
        first_load = min(min(load_lines[v]) for v, _ in group)
        if first_load <= last:
            continue
        # every load at the definitions' level of the same block: no line between leaves the block or nests deeper
        ok = True
        end = max(max(load_lines[v]) for v, _ in group)
        for j in range(wi, end + 1):
            lj = lines[j]
            if not lj.strip():
                continue
            lind = len(lj) - len(lj.lstrip(' \t'))
            if lind < len(ind) or (lind > len(ind) and any(j in load_lines[v] for v, _ in group)):
                ok = False
                break
        if not ok:
            continue
        q = f'{w}_q'
        # the row index straight from the byte offset (uam keeps `(x >> 2u) >> 2u` as two shifts)
        inserts.setdefault(last, []).append(f'{ind}uvec4 {q} = g_DkAddressWindow4._m0[{expr[:-len(" >> 2u")]} >> 4u];')
        for n, (v, _) in enumerate(group):
            replace[v] = f'{q}.{"xyzw"[n]}'
        rows += 1
    if not rows:
        return text
    out = []
    for i, ln in enumerate(lines):
        if any(v in replace for v in re.findall(r'g_DkAddressWindow\._m0\[(_\d+)\]', ln)):
            ln = re.sub(r'g_DkAddressWindow\._m0\[(_\d+)\]',
                        lambda mm: replace.get(mm.group(1), mm.group(0)), ln)
        out.append(ln)
        out.extend(inserts.get(i, []))
    text = text[:a] + '\n'.join(out) + text[b:]
    decl = re.search(r'\} g_DkAddressWindow;\n', text)
    if not decl:
        return text
    return text[:decl.end()] + '\n' + AW4_DECL + text[decl.end():]


# ---------------------------------------------------------------------------------------------------------------------
# pass: skinonce — a fur mesh skinned once a frame, its shells read the skinned rows (2026-10-20, constant 913, A/B)

# Without the constant (the key off) the shader is its old text, main and all (the same DKSH bytes): DK_SKIN_ONCE 0.
SKIN_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_913\n#define SPIRV_CROSS_CONSTANT_ID_913 0u\n#define DK_SKIN_ONCE 0\n'
             '#else\n#define DK_SKIN_ONCE 1\n#endif\n'
             '#if DK_SKIN_ONCE\nconst uint g_banjoSkinOnce = SPIRV_CROSS_CONSTANT_ID_913;\n'
             'layout(binding = 13, std430) buffer type_DkSkinCache\n{\n    vec4 _m0[];\n} g_DkSkinCache;\n#endif\n')
_SKIN_LIST_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'skinonce_shaders.txt')
# The last bone's accumulation of one skinned matrix row: vec4 _R = mix(_P, (<row> * <weights>.wwww) + _P, _C);
SKIN_ROW_RE = re.compile(r'^    vec4 (_\d+) = mix\((_\d+), \((uintBitsToFloat\(.*\)) \* (in_var_\w+)\.wwww\) \+ \2, (_\d+)\);$',
                         re.M)
_IDENT_RE = re.compile(r'\b(_\d+\w*)\b')  # _152, and the aw4 pass's _152_q


def _skin_list():
    try:
        return {ln.split('#')[0].strip().upper() for ln in open(_SKIN_LIST_PATH, encoding='utf-8')
                if ln.split('#')[0].strip()}
    except OSError:
        return set()


_SKIN = _skin_list()


def pass_skinonce(text, name=None):
    """The fur's vertex shaders (tools/skinonce_shaders.txt) skin the same mesh once a shell: 12 or more draws of each
    part a frame, each loading 12 bone rows and blending them (about 300 of the ~470 instructions). With constant 913
    = 1 (banjo_deko3d_skin_once; 0, the default, folds the new code away) the three skinned matrix rows come from a
    storage buffer (binding 13, three vec4 a vertex at gl_VertexID * 3) where the draw's SharedConstants word v[84].z
    (pad_1352[0]) is 2, and are computed and also written there where it is 1 (the part's first draw of the frame,
    render/draw.cpp). Everything the rows need and nothing else needs (their backward slice in main) moves into the
    branch that computes them; the code after them is the same text either way, reading the same values: the rows are
    the select results the original computes, stored and loaded as they are (bit for bit)."""
    if name is None or name.upper() not in _SKIN or 'SPIRV_CROSS_CONSTANT_ID_913' in text:
        return text
    span = main_span(text)
    if not span:
        return text
    a, b = span
    body = text[a:b]
    rows = list(SKIN_ROW_RE.finditer(body))
    if len(rows) < 3:
        return text
    rows = rows[-3:]
    # three consecutive lines with one condition
    if rows[0].group(5) != rows[1].group(5) or rows[1].group(5) != rows[2].group(5):
        return text
    if body[rows[0].end():rows[1].start()].strip() or body[rows[1].end():rows[2].start()].strip():
        return text
    row_vars = [m.group(1) for m in rows]
    stmts = _statements(body)
    if stmts is None:
        return text
    first_row = next((k for k, (s, e) in enumerate(stmts) if s == rows[0].start() + 4), None)
    if first_row is None:
        return text
    before = stmts[:first_row]
    after = stmts[first_row + 3:]

    def defs_uses(s):
        t = body[s[0]:s[1]]
        m = re.match(r'(?:\w+ )?(_\d+\w*)(?:\.[xyzw]+)?\s*=[^=]', t)
        uses = set(_IDENT_RE.findall(t))
        if m and not t.startswith(('if', 'for', 'while', 'do', '{')):
            d = {m.group(1)}
            # a whole-variable definition does not read its old value
            if re.match(r'\w+ _\d+\w*\s*=', t) or re.match(r'_\d+\w*\s*=', t):
                uses_rhs = set(_IDENT_RE.findall(t[t.index('=') + 1:]))
                return d, uses_rhs
            return d, uses
        m2 = re.match(r'\w+ (_\d+\w*);$', t)
        if m2:
            return {m2.group(1)}, set()
        # a compound statement: every variable it names, both ways
        return uses, uses

    info = [defs_uses(s) for s in before]
    row_text = [body[s[0]:s[1]] for s in stmts[first_row:first_row + 3]]
    need = set()
    for t in row_text:
        need |= set(_IDENT_RE.findall(t[t.index('=') + 1:]))
    in_slice = [False] * len(before)
    for k in range(len(before) - 1, -1, -1):
        d, u = info[k]
        if d & need:
            in_slice[k] = True
            need |= u
    # used outside the slice (by the statements after the rows, or the rest before them): stays where it is
    outside = set()
    for s in after:
        outside |= set(_IDENT_RE.findall(body[s[0]:s[1]]))
    for k in range(len(before)):
        if not in_slice[k]:
            outside |= info[k][1]
    movable = list(in_slice)
    changed = True
    while changed:
        changed = False
        for k in range(len(before)):
            if movable[k] and (info[k][0] & outside):
                movable[k] = False
                outside |= info[k][1]
                changed = True
    # the rows are read after (that is the point), never written again
    if not any(movable) or any(re.match(rf'{v}(?:\.[xyzw]+)?\s*=[^=]', body[s[0]:s[1]]) for v in row_vars
                               for s in after):
        return text
    kept = [body[s[0]:s[1]] for k, s in enumerate(before) if not movable[k]]
    moved = [body[s[0]:s[1]] for k, s in enumerate(before) if movable[k]]
    # The renderer reuses rows between draws with the same shared constants, vertex data and fetch data
    # (render/draw.cpp FurSkinLocked): the skinning may read nothing else (not the float constants, where the shell
    # offset is, nor textures or the instance; gl_VertexID is the same vertex for vertex in the draws that share rows).
    slice_text = '\n'.join(body[s[0]:s[1]] for k, s in enumerate(before) if in_slice[k]) + '\n'.join(row_text)
    if re.search(r'g_UboVertex|texture|texelFetch|gl_InstanceID', slice_text):
        return text
    # Of the shared constants only the vertex-fetch part, which the renderer's key compares (FurSkinLocked:
    # booleans v[32-33], fetch data and slots v[35-60], tessellation and bound fetches v[65-81], flt_max v[83].w).
    for v, c in re.findall(r'g_UboShared\.v\[(\d+)u?\]\.([xyzw])', slice_text):
        v = int(v)
        if not (32 <= v <= 33 or 35 <= v <= 60 or 65 <= v <= 81 or (v == 83 and c == 'w')):
            return text
    if re.search(r'g_UboShared\.v\[[^\]]*[^\du\]][^\]]*\]', slice_text):  # an indexed read: unknown words
        return text
    ind = '        '
    out = []
    out += ['    ' + t for t in kept]
    out.append(f'    vec4 {row_vars[0]};')
    out.append(f'    vec4 {row_vars[1]};')
    out.append(f'    vec4 {row_vars[2]};')
    out.append('    if (g_banjoSkinOnce != 0u && g_UboShared.v[84u].z == 2u)')
    out.append('    {')
    out.append(f'{ind}uint dk_skin = uint(gl_VertexID) * 3u;')
    for i, v in enumerate(row_vars):
        out.append(f'{ind}{v} = g_DkSkinCache._m0[dk_skin + {i}u];')
    out.append('    }')
    out.append('    else')
    out.append('    {')
    for t in moved:
        out += [ind + ln.lstrip(' ') if j == 0 else '    ' + ln for j, ln in enumerate(t.split('\n'))]
    for t in row_text:
        out.append(ind + t[len('vec4 '):])
    out.append(f'{ind}if (g_banjoSkinOnce != 0u && g_UboShared.v[84u].z == 1u)')
    out.append(f'{ind}{{')
    out.append(f'{ind}    uint dk_skin = uint(gl_VertexID) * 3u;')
    for i, v in enumerate(row_vars):
        out.append(f'{ind}    g_DkSkinCache._m0[dk_skin + {i}u] = {v};')
    out.append(f'{ind}}}')
    out.append('    }')
    out += ['    ' + body[s[0]:s[1]] for s in after]
    new_body = '\n#if DK_SKIN_ONCE\n' + '\n'.join(out) + '\n#else' + body.rstrip(' ') + '#endif\n'
    text = text[:a] + new_body + text[b:]
    # the storage binding: the stages' usage mask (plume binds the slots it names)
    text = re.sub(r'(// banjo-dk ubo=0x[0-9a-f]+ ssbo=0x)([0-9a-f]+)',
                  lambda m: m.group(1) + format(int(m.group(2), 16) | (1 << 13), 'x'), text, count=1)
    return insert_before_main(text, SKIN_DECL)


FETCH_WORDS = ['g_UboShared.v[%du].w' % (66 + k) for k in range(4)]


def pass_fetchspec(text):
    """banjo-nx (banjo_gpu_fetch_spec, 2026-10-31; vivapinata-nx 2026-11-03): the type words of the first four bound fetches
    (SharedConstants.bound_fetch[k][3], rows 66-69: the format in bits 0-5, signed in bit 8, integer in bit 9) as
    specialization constants 920-923, each the word + 1 (0: read at run time). render/pipeline.cpp gives them to a
    tessellated draw's pipeline from its key (render/draw.cpp FetchSpecWords, the draw's own words): the generic fetch's
    switch over fifteen formats becomes one format's decode."""
    if FETCH_WORDS[0] not in text or 'g_DkAddressWindow' not in text:
        return text
    span = main_span(text)
    if not span:
        return text
    a, b = span
    body = text[a:b]
    decl = ''
    for k, word in enumerate(FETCH_WORDS):
        if word not in body:
            continue
        body = body.replace(word, 'DK_FETCH_TYPE%d' % k)
        c = 920 + k
        decl += (f'#ifndef SPIRV_CROSS_CONSTANT_ID_{c}\n#define SPIRV_CROSS_CONSTANT_ID_{c} 0u\n#endif\n'
                 f'const uint g_banjoFetchType{k} = SPIRV_CROSS_CONSTANT_ID_{c};\n'
                 f'#define DK_FETCH_TYPE{k} ((g_banjoFetchType{k} != 0u) ? (g_banjoFetchType{k} - 1u) : {word})\n')
    return insert_before_main(text[:a] + body + text[b:], decl)


# ---------------------------------------------------------------------------------------------------------------------
# pass: samplerk — a fetch through the sampler after its fetch constant's (Xenos' sampler index + k) reads its signs and
# gamma from the sampler word itself (vivapinata-nx, 2026-11-12, the shader round)
# The translator writes a tfetch that names sampler index + k (the game's grass shell, PS 0F8483A3, samples fetch
# constant 2 through `s2_SamplerDescriptorIndex + 1`) as `uint _a = g_UboShared.v[24u].z + 1u;` and takes the signs and
# gamma from _a: `(_a >> 16u) & 15u`, `(_a >> 13u) & 7u`. Neither plainfetch nor fetchconv knows that form, so those
# fetches kept their sign, gamma and exponent block in every variant (the grass shell: 226 instructions and 25 GPRs with
# it, 80 and 12 without). On deko3d a sampler word is (slot << 20) | gamma << 16 | biased << 13 (render/draw.cpp
# SamplerWord and BindingWords): its bits 0-12 are always 0, so adding k < 2^13 never carries into bit 13 and the two
# fields of word + k are the word's own. Each such field read becomes the word's, the form both passes after it know.
# Same values for every word the renderer writes; deko3d only (these passes are deko3d's).

_SK_DEF = re.compile(r'^\s*uint (_\d+) = (g_UboShared\.v\[(2[4-9]|3[01])u\]\.[xyzw]) \+ (\d+)u;$', re.M)


def pass_samplerk(text):
    if not _SK_DEF.search(text):
        return text
    for m in list(_SK_DEF.finditer(text)):
        name, word, k = m.group(1), m.group(2), int(m.group(4))
        if not 0 < k < (1 << 13):
            continue
        for shift, mask in (('16u', '15u'), ('13u', '7u')):
            # 2026-11-12 (A/B 150): each rewritten line after a marker line (SK_MARK), by which plainfetch and fetchconv
            # make their folding of it depend on constant 975 (VIVAROUND_DECL)
            text = re.sub(r'^([ \t]*)(uint _\d+ = )\(' + re.escape(name) + r' >> ' + shift + r'\) & ' + mask + ';$',
                          lambda mm: f'{mm.group(1)}{SK_MARK}\n{mm.group(1)}{mm.group(2)}({word} >> {shift}) & {mask};',
                          text, flags=re.M)
    return text


# vivapinata-nx (2026-11-12, A/B 150, render/draw.cpp vivapinata_shader_round): constant 975, 1 by default: the shader
# round's passes act; 0 (the A/B's off half): their code does what it did before them, so one run measures them.
SK_MARK = '// dk-samplerk'
VIVAROUND_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_975\n#define SPIRV_CROSS_CONSTANT_ID_975 1u\n#endif\n'
                  'const bool g_vivaRound = SPIRV_CROSS_CONSTANT_ID_975 != 0u;\n')


def with_viva_round(text):
    return text if 'SPIRV_CROSS_CONSTANT_ID_975' in text else insert_globals(text, VIVAROUND_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: plainfetch — a fetch's sign, gamma and exponent code dropped where the draw's fetch words have none
# (sasrt-nx, 2026-11-08, the GPU round after its move to the ultimate base: sasrt_gpu_plain_fetches, constant 926;
# renut-ultimate port 44, measured by rr6-nx the same day: with the ultimate renderer its pixel shaders were about 120
# instructions heavier per fetch and the race drew 3.2 ms a frame more on the GPU. The mask: render/draw.cpp
# PlainFetchMask)

PLAINFETCH_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_926\n#define SPIRV_CROSS_CONSTANT_ID_926 0u\n#endif\n'
                   'const uint g_sasrtPlainFetch = SPIRV_CROSS_CONSTANT_ID_926;\n')
_PF_COND = r'(?:!\(\((_\d+) \| (_\d+)\) != 0u\)|(_\d+))'
_PF_BLOCK = re.compile(r'bool (_\d+);\s*if \(' + _PF_COND + r'\)\s*\{\s*\1 = (_\d+) != 0;\s*\}\s*else\s*\{\s*\1 = true;\s*\}')
_PF_NOT = r'bool {v} = !\(\((_\d+) \| (_\d+)\) != 0u\);'
_PF_SIGNS = r'uint {v} = \(g_UboShared\.v\[(\d+)u\]\.([xyzw]) >> 16u\) & 15u;'
_PF_GAMMA = r'uint {v} = \(g_UboShared\.v\[(\d+)u\]\.([xyzw]) >> 13u\) & 7u;'
_PF_EXP = r'int {v} = int\(g_UboShared\.v\[(\d+)u\]\.([xyzw]) << 6u\) >> 26;'


def _pf_byte(m):
    return int(m.group(1)) * 16 + 'xyzw'.index(m.group(2)) * 4


def pass_plainfetch(text):
    """Renut's texture fetches apply Xenos' per-fetch-constant signs and gamma (SharedConstants.sampler_indices bits
    13-19: v[24 + r / 4]) and the exponent adjust (texture_indices bits 20-25: v[r / 4] for a 2D fetch) to the texel in
    a block that runs only when the fetch constant has any; for every other fetch the block is skipped and the texel is
    used as it is. The block is a uniform branch, but its temporaries count in the shader's registers (the race's
    G-buffer shader 33 GPRs with them, 28 without; the final pass 42 and 31). Constant 926, bit r: the draw's words for
    fetch constant r have none of the three (render/draw.cpp PlainFetchMask), so the block's condition is false at
    compile time and uam drops it. Its default 0 leaves every block as it was. Same pixels: the condition was false for
    that draw anyway."""
    if 'SPIRV_CROSS_CONSTANT_ID_926' in text or '>> 16u) & 15u;' not in text:
        return text
    is_vertex = re.search(r'\bgl_Position\b', text) is not None  # vivapinata-nx (2026-11-12): vertex shaders too
    edits = []
    for m in _PF_BLOCK.finditer(text):
        b, x, y, p, e = m.group(1), m.group(2), m.group(3), m.group(4), m.group(5)
        if p:
            mn = re.search(_PF_NOT.format(v=re.escape(p)), text)
            if not mn:
                continue
            x, y = mn.group(1), mn.group(2)
        ms = re.search(_PF_SIGNS.format(v=re.escape(x)), text)
        mg = re.search(_PF_GAMMA.format(v=re.escape(y)), text)
        me = re.search(_PF_EXP.format(v=re.escape(e)), text)
        if not (ms and mg and me):
            continue
        sb, gb, eb = _pf_byte(ms), _pf_byte(mg), _pf_byte(me)
        r = (sb - 384) // 4
        # the words of one fetch constant r: sampler_indices[r] (bytes 384-511) for both, texture_indices[d][r] (d the
        # fetch's dimension, bytes 0-383) for the exponent
        if sb != gb or not 384 <= sb < 512 or eb >= 384 or eb % 128 != 4 * r:
            continue
        # vivapinata-nx (2026-11-12, A/B 150): a samplerk read (its line after SK_MARK) skips its block only with 975
        gate = f'((g_sasrtPlainFetch >> {r}u) & 1u) == 0u'
        # 2026-11-12: and every fetch of a vertex shader (926 says the vertex shader's fetches too only since the shader
        # round, render/draw.cpp SasrtPlainFetchMask's vs_entry)
        if text[:ms.start()].rstrip().endswith(SK_MARK) or is_vertex:
            gate = f'(!g_vivaRound || {gate})'
        edits.append((m.end(), f'\n    {b} = {b} && {gate};'))
    if not edits:
        return text
    for at, code in reversed(edits):
        text = text[:at] + code + text[at:]
    text = insert_globals(text, PLAINFETCH_DECL)
    return with_viva_round(text) if '(!g_vivaRound || ' in text else text


# ---------------------------------------------------------------------------------------------------------------------
# pass: vskill — a vertex kill as the GPU does it: only a primitive whose vertices are all killed goes (constant 914)
# vivapinata-nx (2026-11-03, 6eecb25a), ported into renut-ultimate.

VSKILL_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_914\n#define SPIRV_CROSS_CONSTANT_ID_914 0u\n#endif\n'
               'const uint g_banjoVsKillAnd = SPIRV_CROSS_CONSTANT_ID_914;\n')
VSKILL_NAN_RE = re.compile(r'^(\s*)gl_Position = (mix\((\w+), vec4\(uintBitsToFloat\(0x7fc00000u /\* nan \*/\)\), '
                           r'bvec4\((\w+)\)\));$', re.M)


# ---------------------------------------------------------------------------------------------------------------------
# pass: expwin - sasrt-nx (2026-11-08, sasrt_gpu_export_window_ubo; the fourth GPU round, the 33 ms goal): Renut's
# memexport writes each exported value into the first of the draw's four export windows that holds its address, a loop
# over `uvec4 _N[4] = uvec4[](g_UboShared.v[61u], ... v[64u])` indexed by the loop counter. A local array indexed at run
# time lives in local memory: SASRT's skinning export shader (three exports a vertex) kept 192 bytes a thread there, and
# its draws cost ~180 ns a vertex on the console (8.5 ms a race frame, measured by drawing every export twice). With
# constant 944 = 1 each use `_N[i]` reads the constant buffer itself, `g_UboShared.v[61u + min(i, 3u)]`: the same four
# words (i < 4 wherever the loop reads them; the min only bounds an index the loop never makes), no local memory.
# 944 = 0 (default) expands every use back to `_N[i]` and keeps the array: the code is the same as without this pass.
EXPWIN_RE = re.compile(r'^([ \t]*)uvec4 (_\d+)\[4\] = uvec4\[\]\(g_UboShared\.v\[(\d+)u\], g_UboShared\.v\[(\d+)u\], '
                       r'g_UboShared\.v\[(\d+)u\], g_UboShared\.v\[(\d+)u\]\);$', re.M)
EXPWIN_DECL = '#ifndef SPIRV_CROSS_CONSTANT_ID_944\n#define SPIRV_CROSS_CONSTANT_ID_944 0u\n#endif\n'


def pass_expwin(text):
    if 'SPIRV_CROSS_CONSTANT_ID_944' in text or 'uvec4[](g_UboShared.v[' not in text:
        return text
    done = 0
    for m in list(EXPWIN_RE.finditer(text)):
        indent, name = m.group(1), m.group(2)
        regs = [int(m.group(k)) for k in range(3, 7)]
        if regs != list(range(regs[0], regs[0] + 4)):
            continue
        uses = re.findall(r'(?<![\w.])' + re.escape(name) + r'\b', text)
        indexed = re.findall(r'(?<![\w.])' + re.escape(name) + r'\[_\d+\]', text)
        if len(uses) != len(indexed) + 1:  # the declaration, then only `_N[_M]` reads
            continue
        decl = m.group(0)
        if text.count(decl) != 1:
            continue
        text = text.replace(decl, f'#if SPIRV_CROSS_CONSTANT_ID_944 != 0u\n'
                                  f'#define DKW{name}(i) g_UboShared.v[{regs[0]}u + min((i), 3u)]\n'
                                  f'#else\n{decl}\n#define DKW{name}(i) {name}[i]\n#endif')
        text = re.sub(r'(?<![\w.])' + re.escape(name) + r'\[(_\d+)\]', f'DKW{name}(\\1)', text)
        done += 1
    return insert_globals(text, EXPWIN_DECL) if done else text


# ---------------------------------------------------------------------------------------------------------------------
# pass: expparts - sasrt-nx (2026-11-09, sasrt_gpu_exp_export_parts; the fourth GPU round): an EXPERIMENT that changes
# pixels, to find what makes a memexport vertex shader cost ~150 ns a vertex on the console. Constant 945 (0 by default:
# the code exactly as before): bit 0 its export stores skipped (the values still computed), bit 1 its bone-row loads from
# the address window replaced by a constant-buffer word, bit 2 its vertex inputs replaced by zero, bit 3 no export
# store at all (the export work then compiles away), bit 4 the position (2, 2, 2, 1): outside the view volume.
EXPPARTS_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_945\n#define SPIRV_CROSS_CONSTANT_ID_945 0u\n#endif\n'
                 'const uint g_sasrtExpParts = SPIRV_CROSS_CONSTANT_ID_945;\n')
EXPPARTS_STORE_RE = re.compile(r'^(\s*)(g_DkMemExport\._m0\[.+\]) = (_\d+);$', re.M)
EXPPARTS_IN_RE = re.compile(r'^layout\(location = \d+\) in (\w+) (in_var_\w+);$', re.M)


def pass_expparts(text):
    if 'SPIRV_CROSS_CONSTANT_ID_945' in text or 'g_DkMemExport._m0[' not in text:
        return text
    text = EXPPARTS_STORE_RE.sub(lambda m: f'{m.group(1)}if ((g_sasrtExpParts & 8u) == 0u && ((g_sasrtExpParts & 1u) == 0u || '
                                           f'{m.group(3)} == 2654435769u)) {{ {m.group(2)} = {m.group(3)}; }}', text)
    key = 'g_DkAddressWindow4._m0['
    out, at = [], 0
    while True:
        k = text.find(key, at)
        if k < 0:
            out.append(text[at:])
            break
        depth, j = 1, k + len(key)
        while depth:
            depth += {'[': 1, ']': -1}.get(text[j], 0)
            j += 1
        out.append(text[at:k])
        out.append(f'(((g_sasrtExpParts & 2u) != 0u) ? g_UboShared.v[1u] : {text[k:j]})')
        at = j
    text = ''.join(out)
    text = re.sub(r'^(\s*)gl_Position = (_\d+);$', lambda m: f'{m.group(1)}gl_Position = ((g_sasrtExpParts & 16u) != 0u) ? '
                  f'vec4(2.0, 2.0, 2.0, 1.0) : {m.group(2)};', text, flags=re.M)
    ins = EXPPARTS_IN_RE.findall(text)
    span = main_span(text)
    if ins and span:
        a, b = span
        body = text[a:b]
        for ty, name in ins:
            body = re.sub(r'(?<![\w.])' + name + r'\b', 'dkx_' + name, body)
        decl = ''.join(f'\n    {ty} dkx_{name} = ((g_sasrtExpParts & 4u) != 0u) ? {ty}(0.0) : {name};' for ty, name in ins)
        body = decl + body  # main_span's body starts after main's brace
        text = text[:a] + body + text[b:]
    return insert_globals(text, EXPPARTS_DECL)


def pass_vskill(text):
    """vivapinata-nx (2026-11-03): the XDK's vertex kill (oPts.z) drops a primitive only when all its vertices are killed
    (PA_CL_CLIP_CNTL.vtx_kill_or off; the SDK's translators do the same). XenosRecomp's SPIR-V has only the NaN position,
    which drops any primitive with one killed vertex: Viva Pinata's ground layers kill vertices where a layer is absent, so
    each layer lost a row of cells at its border and ended in hard steps, unlike D3D12's cull distance. deko3d has neither
    cull distances nor a clip distance enable, so with constant 914 = 1 the vertex shader keeps its position and passes
    its kill flag at location 30 (renut-ultimate: 26 in vivapinata-nx, where the point size is now: the 25 interpolators
    are 0-24, then the point coordinate and size), and the pixel shader discards as its first statement where the flag interpolates to 1:
    only inside a primitive with all three killed. render/pipeline.cpp gives both stages 914 when the vertex shader
    declares it and the pipeline has a pixel shader; otherwise (0, depth-only draws) the NaN as before. The location's
    line has a leading space: it is not one of the varyings that constants 905 and 906 cover. This pass comes last:
    the earlier ones' discard tests must not see this one's."""
    span = main_span(text)
    if not span:
        return text
    if 'gl_Position' in text:
        if not VSKILL_NAN_RE.search(text):
            return text

        def write(m):
            i = m.group(1)
            return (f'{i}if (g_banjoVsKillAnd != 0u)\n{i}{{\n{i}    gl_Position = {m.group(3)};\n'
                    f'{i}    dk_kill_v = {m.group(4)} ? 1.0 : 0.0;\n{i}}}\n{i}else\n{i}{{\n'
                    f'{i}    gl_Position = {m.group(2)};\n{i}}}')
        text = VSKILL_NAN_RE.sub(write, text)
        return insert_before_main(text, VSKILL_DECL + ' layout(location = 30) out float dk_kill_v;\n')
    m = re.search(r'\nvoid main\(\)\n\{\n', text)
    if not m:
        return text
    text = (text[:m.end()] + '    if (g_banjoVsKillAnd != 0u)\n    {\n        if (dk_kill_v >= 0.9999)\n        {\n'
            '            discard;\n        }\n    }\n' + text[m.end():])
    return insert_before_main(text, VSKILL_DECL + ' layout(location = 30) in float dk_kill_v;\n')


# ---------------------------------------------------------------------------------------------------------------------
# pass: vsepi — the vertex shaders' epilogue tests as constants of the draw (sasrt-nx, 2026-11-08, the third GPU round:
# sasrt_gpu_vs_screen_space_spec, sasrt_gpu_vs_depth_remap_spec, constant 932)

VSEPI_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_932\n#define SPIRV_CROSS_CONSTANT_ID_932 0u\n#endif\n'
              'const uint g_sasrtVsEpi = SPIRV_CROSS_CONSTANT_ID_932;\n')
VSEPI_SCREEN = 'if (g_UboShared.v[60u].z != 0u)'
VSEPI_REMAP_VAR = re.compile(r'float (_\d+) = uintBitsToFloat\(g_UboShared\.v\[94u\]\.x\);')


def pass_vsepi(text):
    """Every vertex shader ends with Renut's screen-space branch (SharedConstants.screen_space, v[60].z: positions in
    pixels, mapped to clip space by the shader with the scale and offset it then reads) and the depth remap branch
    (depth_remap[0], v[94].x, never set on deko3d). Constant 932 (render/draw.cpp SasrtSpecWordsOf): bit 0 set, bit 1 is
    the screen-space test's answer; bit 2 set, bit 3 the remap test's. Its default 0 leaves both tests as they were.
    The draw's own answer: same pixels."""
    if 'SPIRV_CROSS_CONSTANT_ID_932' in text or text.count(VSEPI_SCREEN) != 1:
        return text
    text = text.replace(VSEPI_SCREEN, 'if (((g_sasrtVsEpi & 1u) != 0u) ? ((g_sasrtVsEpi & 2u) != 0u) : '
                                      '(g_UboShared.v[60u].z != 0u))')
    m = VSEPI_REMAP_VAR.search(text)
    if m:
        test = f'if ({m.group(1)} != 0.0)'
        if text.count(test) == 1:
            text = text.replace(test, f'if (((g_sasrtVsEpi & 4u) != 0u) ? ((g_sasrtVsEpi & 8u) != 0u) : '
                                      f'({m.group(1)} != 0.0))')
    return insert_globals(text, VSEPI_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: stack3d — a 3D fetch's stacked / layer filter / Z wrap tests as constants of the draw (sasrt-nx, 2026-11-08, the
# third GPU round: sasrt_gpu_stacked_3d_spec, sasrt_gpu_stacked_filter_spec, constants 934-936)

STACK3D_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_934\n#define SPIRV_CROSS_CONSTANT_ID_934 0u\n#endif\n'
                'const uint g_sasrtStack3d = SPIRV_CROSS_CONSTANT_ID_934;\n'
                '#ifndef SPIRV_CROSS_CONSTANT_ID_935\n#define SPIRV_CROSS_CONSTANT_ID_935 0u\n#endif\n'
                'const uint g_sasrtStackLinear = SPIRV_CROSS_CONSTANT_ID_935;\n'
                '#ifndef SPIRV_CROSS_CONSTANT_ID_936\n#define SPIRV_CROSS_CONSTANT_ID_936 0u\n#endif\n'
                'const uint g_sasrtStackWrap = SPIRV_CROSS_CONSTANT_ID_936;\n')
STACK3D_TEST = re.compile(r'\(g_UboShared\.v\[(\d+)u\]\.([xyzw]) & (2147483648u|1073741824u|536870912u)\) != 0u')
STACK3D_CONST = {'2147483648u': 'g_sasrtStack3d', '1073741824u': 'g_sasrtStackLinear', '536870912u': 'g_sasrtStackWrap'}


def pass_stack3d(text):
    """Renut's 3D fetch (tfetchStacked) tests the draw's 3D texture word (SharedConstants.texture_indices[1][r]: bytes
    128-255, v[8]-v[15]) at run time: bit 31 a stacked texture (a 2D array: two fetches and a blend between layers, with
    its layer count queried) or a volume texture, bit 30 the filter between layers, bit 29 the Z wrap. Constants 934,
    935, 936 (render/draw.cpp SasrtSpecWordsOf): bit 31 set, bit r is that bit of fetch constant r's word for the draw;
    the test becomes that constant and uam drops the path not taken. Their default 0 leaves the tests as they were.
    The draw's own answer: same pixels."""
    if 'SPIRV_CROSS_CONSTANT_ID_934' in text:
        return text

    def sub(m):
        byte = int(m.group(1)) * 16 + 'xyzw'.index(m.group(2)) * 4
        if not 128 <= byte < 256:
            return m.group(0)
        r = (byte - 128) // 4
        c = STACK3D_CONST[m.group(3)]
        return f'((({c} >> 31u) != 0u) ? ((({c} >> {r}u) & 1u) != 0u) : ({m.group(0)}))'

    new = STACK3D_TEST.sub(sub, text)
    if new == text:
        return text
    return insert_globals(new, STACK3D_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: fetchendian — a vertex fetch by address's byte order as constants of the draw (sasrt-nx, 2026-11-08, the third
# GPU round: sasrt_gpu_fetch_endian_spec, constants 937-939). Renut's translator output: any port's.

FETCHENDIAN_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_937\n#define SPIRV_CROSS_CONSTANT_ID_937 0u\n#endif\n'
                    'const uint g_sasrtEndianSet = SPIRV_CROSS_CONSTANT_ID_937;\n'
                    '#ifndef SPIRV_CROSS_CONSTANT_ID_938\n#define SPIRV_CROSS_CONSTANT_ID_938 0u\n#endif\n'
                    'const uint g_sasrtEndianLo = SPIRV_CROSS_CONSTANT_ID_938;\n'
                    '#ifndef SPIRV_CROSS_CONSTANT_ID_939\n#define SPIRV_CROSS_CONSTANT_ID_939 0u\n#endif\n'
                    'const uint g_sasrtEndianHi = SPIRV_CROSS_CONSTANT_ID_939;\n')
FETCHENDIAN_RE = re.compile(r'\b(uint _\d+ = )(g_UboShared\.v\[(\d+)u\]\.([xyzw]) & 3u);')


def pass_fetchendian(text):
    """Renut's vfetchMemory reads a fetch slot's word (SharedConstants.vertex_fetch_slots[64 + i], bytes 832-959: its copy
    offset, and its byte order in bits 0-1) and swaps each fetched word all four ways, picking one by the order at run
    time: the race's skinning exports fetch 64 words a vertex so. Constant 937 (render/draw.cpp SasrtSpecWordsOf): bit i
    set: slot 64 + i's order is two bits of 938 (i < 16) or 939; the order becomes that constant and uam keeps one swap a
    word. Its default 0 leaves the order read at run time. The draw's own order: same values."""
    if 'SPIRV_CROSS_CONSTANT_ID_937' in text or 'g_DkAddressWindow' not in text:
        return text

    def sub(m):
        byte = int(m.group(3)) * 16 + 'xyzw'.index(m.group(4)) * 4
        if not 832 <= byte < 960:
            return m.group(0)
        i = (byte - 832) // 4
        word = 'g_sasrtEndianLo' if i < 16 else 'g_sasrtEndianHi'
        return (f'{m.group(1)}(((g_sasrtEndianSet >> {i}u) & 1u) != 0u) ? (({word} >> {2 * (i % 16)}u) & 3u) : '
                f'({m.group(2)});')

    new = FETCHENDIAN_RE.sub(sub, text)
    if new == text:
        return text
    return insert_globals(new, FETCHENDIAN_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: expfmt — a memexport's address register's z word (its format) as constants of the draw (sasrt-nx, 2026-11-08, the
# third GPU round: sasrt_gpu_export_format_spec, constants 940-943). Renut's translator output: any port's.

EXPFMT_DECL = ''.join(f'#ifndef SPIRV_CROSS_CONSTANT_ID_{c}\n#define SPIRV_CROSS_CONSTANT_ID_{c} 0u\n#endif\n'
                      f'const uint {n} = SPIRV_CROSS_CONSTANT_ID_{c};\n'
                      for c, n in ((940, 'g_sasrtExpSet'), (941, 'g_sasrtExpZ0'), (942, 'g_sasrtExpZ1'),
                                   (943, 'g_sasrtExpZ2')))
EXPFMT_RE = re.compile(r'vec4 (_\d+) = \(_\d+(?:\.[xyzw]+)? \* vec4\(0\.0, 1\.0, 0\.0, 0\.0\)\) \+ '
                       r'g_UboVertex\.v\[(\d+)u\]\.xyzw;')


def pass_expfmt(text):
    """Renut's memexport: the export address register eA is a float constant c<N> plus the index (`vec4 _A = (i * vec4(0,
    1, 0, 0)) + c<N>`), and every eM write packs its value by the format in eA.z (`floatBitsToUint(_A.z)`: bits 8-13, the
    number flags 16-18) through a switch over the formats and a loop over the components with arrays indexed at run time
    (local memory). Constant 940 (render/draw.cpp SasrtSpecWordsOf): bit N set: c<N>'s z word for the draw (as its
    constant buffer has it; zero or a normal float) is 941, 942 or 943 by N's order among the set bits, and that word
    replaces the read of eA.z: the same bits, since eA.z = i * 0 + c<N>.z is c<N>.z exactly for a finite index i (an
    export happens only for an index in its window), and uam folds the pack to the draw's format. Its default 0 leaves
    eA.z read at run time."""
    if 'SPIRV_CROSS_CONSTANT_ID_940' in text or 'vec4(0.0, 1.0, 0.0, 0.0)) + g_UboVertex.v[' not in text:
        return text
    regs = {m.group(1): int(m.group(2)) for m in EXPFMT_RE.finditer(text) if int(m.group(2)) < 31}
    if not regs:
        return text

    def sub(m):
        n = regs.get(m.group(1))
        if n is None:
            return m.group(0)
        rank = f'bitCount(g_sasrtExpSet & {(1 << n) - 1}u)'
        pick = f'(({rank} == 0) ? g_sasrtExpZ0 : (({rank} == 1) ? g_sasrtExpZ1 : g_sasrtExpZ2))'
        return f'((((g_sasrtExpSet >> {n}u) & 1u) != 0u) ? {pick} : {m.group(0)})'

    new = re.sub(r'floatBitsToUint\((_\d+)\.z\)', sub, text)
    if new == text:
        return text
    return insert_globals(new, EXPFMT_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: nofxaa — sasrt-nx (2026-11-08, sasrt_smaa_replaces_fxaa, the user's request): the game's FXAA pass (pixel shader
# 6C215354C35798B8, 1152x544, between the composite and the final 1280x720 pass) as a copy of its centre sample under
# constant 930 (0 by default: FXAA as before); banjo-nx's SMAA then runs on its target (banjo_renut_smaa_after_ps).

NOFXAA_PS = {'6C215354C35798B8'}
NOFXAA_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_930\n#define SPIRV_CROSS_CONSTANT_ID_930 0u\n#endif\n'
               'const uint g_sasrtNoFxaa = SPIRV_CROSS_CONSTANT_ID_930;\n')
NOFXAA_OUT = '    vec4 _1234 = mix(_1230, _1231, bvec4(!_509));'


def pass_nofxaa(text, name=None):
    """FXAA 360's result is its centre sample (_277: the fetch with its sign / exponent code) where it finds no edge,
    the blend of its edge samples where it does (_1234); with 930 = 1 the result is the centre sample everywhere, and
    uam drops the edge search. Only for the shader it was read from (the variable names are that shader's)."""
    if not name or name.upper() not in NOFXAA_PS or text.count(NOFXAA_OUT) != 1 or 'vec4 _277;' not in text:
        return text
    text = text.replace(NOFXAA_OUT, '    vec4 _1234 = (g_sasrtNoFxaa != 0u) ? _277 : mix(_1230, _1231, bvec4(!_509));')
    return insert_before_main(text, NOFXAA_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: clipspec — sasrt-nx (2026-11-08, sasrt_gpu_clip_spec; the second GPU round): the vertex shader's user clip
# distance under constant 929 (1 by default: written, as before)

CLIPSPEC_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_929\n#define SPIRV_CROSS_CONSTANT_ID_929 1u\n#endif\n'
                 'const uint g_sasrtUserClip = SPIRV_CROSS_CONSTANT_ID_929;\n')
CLIPSPEC_RE = re.compile(r'^(\s*)(gl_ClipDistance\[0u?\]\s*=[^;]*;)$', re.M)


def pass_clipspec(text):
    """Every vertex shader writes gl_ClipDistance[0] = dot(position, user clip plane 0) (rr6-nx / vivapinata-nx: the
    plane, or zeros: a distance of 0 clips nothing). Under `g_sasrtUserClip != 0u`, constant 929: render/pipeline.cpp
    gives 0 to the pipelines of draws without a plane (PipelineKey::reserved bit 3 clear), and plume_deko3d switches the
    hardware clip slot off for those (SET_USER_CLIP_ENABLE 0): no dot product and no clip test a vertex. A distance never
    read, or read back, is left alone."""
    if len(re.findall(r'\bgl_ClipDistance\b', text)) != 2 or len(CLIPSPEC_RE.findall(text)) != 1:
        return text  # the declaration and one write, or a form not handled
    text = CLIPSPEC_RE.sub(lambda m: f'{m.group(1)}if (g_sasrtUserClip != 0u) {{ {m.group(2)} }}', text)
    return insert_before_main(text, CLIPSPEC_DECL)


# ---------------------------------------------------------------------------------------------------------------------
# pass: invariant — `invariant gl_Position;` in every vertex shader (dishonored-nx, 2026-10-30, its dishonored_vs_invariant;
# renut-ultimate port 100)
# Unreal Engine 3 draws a depth pass, then colour passes with the depth test GEQUAL and no depth bias, each with a vertex
# shader of its own. uam compiled each one's position arithmetic its own way (fused or not), so at a silhouette one
# shader covered a pixel the other did not, and the pixel kept the clear and the later additive passes: bright,
# object-coloured edges in Dishonored. The Xenos gives the same position to shaders that compute it the same way;
# `invariant` makes uam do so too. dishonored-nx inserted the line when a shader was loaded; here the package holds it.
def pass_invariant(text):
    if 'invariant gl_Position' in text or not re.search(r'\bgl_Position\b', text):
        return text
    return insert_globals(text, 'invariant gl_Position;')


# ---------------------------------------------------------------------------------------------------------------------
# pass: clamp7e3 — a 7e3 target's output clamped to the 7e3 range (dishonored-nx, 2026-10-31, dishonored_renut_clamp_7e3;
# renut-ultimate port 106, constant 990)
# The Xenos stores a 2_10_10_10_FLOAT colour (7e3: 3-bit exponent, 7-bit mantissa, no sign) clamped to 0..31.875 and its
# 2-bit alpha to 0..1; Renut's host target is 16-bit float, which keeps a negative value or one far above the range (in
# Dishonored: negative light passes made black walls, over-range highlights a washed-out frame). Constant 990 is a mask of
# colour targets (render/draw.cpp sets bit i for a 7e3 target i with exponent bias 0): each such output is clamped, NaN
# to 0, at every exit from main (after everything else writes it). 0 (default): nothing changes.
CLAMP7E3_DECL = ('#ifndef SPIRV_CROSS_CONSTANT_ID_990\n#define SPIRV_CROSS_CONSTANT_ID_990 0u\n#endif\n'
                 'const uint g_ult7e3 = SPIRV_CROSS_CONSTANT_ID_990;\n')
CLAMP7E3_FN = ('vec4 dk_clamp7e3(vec4 c)\n{\n    c = mix(c, vec4(0.0), isnan(c));\n'
               '    return vec4(clamp(c.xyz, vec3(0.0), vec3(31.875)), clamp(c.w, 0.0, 1.0));\n}\n\n')


def pass_clamp7e3(text):
    if 'SPIRV_CROSS_CONSTANT_ID_990' in text:
        return text
    outs = re.findall(r'layout\(location = (\d)\) out vec4 (out_var_SV_Target\d+)\s*;', text)
    outs = [(int(i), name) for i, name in outs if int(i) < 4]
    span = main_span(text)
    if not outs or not span:
        return text
    a, b = span
    body = text[a:b]
    code = ''.join(f'if (((g_ult7e3 >> {i}u) & 1u) != 0u) {{ {name} = dk_clamp7e3({name}); }} ' for i, name in outs)
    body = re.sub(r'(?<![\w.])return;', lambda m: '{ ' + code + 'return; }', body)
    body = body.rstrip() + '\n    ' + code.rstrip() + '\n'
    text = text[:a] + body + text[b:]
    return insert_globals(insert_before_main(text, CLAMP7E3_FN), CLAMP7E3_DECL)


PASSES = [
    ('scalecodes', pass_scalecodes),
    ('scale1', pass_scale1),
    ('fragscale', pass_fragscale),
    ('alpha', pass_alpha),
    ('tess', pass_tess),
    ('fetchspec', pass_fetchspec),
    ('bonerow', pass_bonerow),
    ('vszero', pass_vszero),
    ('vsout', pass_vsout),
    ('pszero', pass_pszero),
    ('pred', pass_pred),
    ('predmul', pass_predmul),
    ('sext', pass_sext),
    ('bool01', pass_bool01),
    ('sel', pass_sel),
    ('alphamask', pass_alphamask),
    ('earlyhoist', pass_earlyhoist),
    ('ifmerge', pass_ifmerge),
    ('dowhile', pass_dowhile),
    ('selsame', pass_selsame),
    ('deadcomp', pass_deadcomp),
    ('guardsel', pass_guardsel),
    ('lightbranch', pass_lightbranch),
    ('nullps', pass_nullps),
    ('txq', pass_txq),
    ('earlyz', pass_earlyz),
    ('gpr32', pass_gpr32),
    ('bonesel', pass_bonesel),
    ('aw4', pass_aw4),
    ('skinonce', pass_skinonce),
    ('samplerk', pass_samplerk),  # vivapinata-nx (2026-11-12): the shader round, before plainfetch and fetchconv
    ('plainfetch', pass_plainfetch),  # sasrt-nx (2026-11-08): the GPU round, constant 926
    ('fetchconv', pass_fetchconv),  # sasrt-nx (2026-11-09): the fifth GPU round, constants 950-957
    ('onex', pass_onex),  # sasrt-nx (2026-11-08): the GPU round, constant 926 bit 31
    ('onexsize', pass_onexsize),  # vivapinata-nx (2026-11-12): the shader round, after onex
    ('noearly', pass_noearly),  # vivapinata-nx (2026-11-12): the shader round, constant 976 (A/B 151)
    ('earlykill', pass_earlykill),  # vivapinata-nx (2026-11-12): the shader round, constant 977 (A/B 152)
    ('facesign', pass_facesign),  # sasrt-nx (2026-11-08): the GPU round, constant 928
    ('clipspec', pass_clipspec),  # sasrt-nx (2026-11-08): the second GPU round, constant 929
    ('nofxaa', pass_nofxaa),  # sasrt-nx (2026-11-08): sasrt_smaa_replaces_fxaa, constant 930
    ('vsepi', pass_vsepi),  # sasrt-nx (2026-11-08): the third GPU round, constant 932
    ('stack3d', pass_stack3d),  # sasrt-nx (2026-11-08): the third GPU round, constants 934-936
    ('fetchendian', pass_fetchendian),  # sasrt-nx (2026-11-08): the third GPU round, constants 937-939
    ('expfmt', pass_expfmt),  # sasrt-nx (2026-11-08): the third GPU round, constants 940-943
    ('vskill', pass_vskill),  # vivapinata-nx (2026-11-03): last
    ('expwin', pass_expwin),  # sasrt-nx (2026-11-08): the fourth GPU round, constant 944
    ('expparts', pass_expparts),  # sasrt-nx (2026-11-09): an EXPERIMENT, constant 945 (0: as before)
    ('clamp7e3', pass_clamp7e3),  # dishonored-nx (2026-10-31; port 106): constant 990, the last pixel pass
    ('invariant', pass_invariant),  # dishonored-nx (2026-10-30; port 100): vertex shaders, no constant
]
# banjo-nx (2026-10-21): scaled targets (648p / 900p): scale1 and fragscale (1x only) are not in the default.
DEFAULT = ','.join(name for name, _ in PASSES if name not in ('scale1', 'fragscale'))


# The passes for one stage only.
# vivapinata-nx (2026-11-12): samplerk, plainfetch, fetchconv, onex and onexsize act on both stages (vertex shaders'
# fetches through constant 975, render/draw.cpp SasrtPlainFetchMask's vs_entry).
STAGE_OF = {'earlykill': 'frag', 'noearly': 'frag', 'facesign': 'frag', 'clipspec': 'vert', 'nofxaa': 'frag',
            'vsepi': 'vert', 'expwin': 'vert', 'expparts': 'vert', 'stack3d': 'frag', 'fetchendian': 'vert', 'expfmt': 'vert', 'skinonce': 'vert', 'bonesel': 'vert', 'aw4': 'vert', 'lightbranch': 'frag', 'nullps': 'frag', 'gpr32': 'frag', 'earlyz': 'frag', 'txq': 'frag', 'alpha': 'frag', 'alphamask': 'frag', 'earlyhoist': 'frag', 'tess': 'vert', 'vsout': 'vert', 'vszero': 'vert', 'pszero': 'frag'}
STAGE_OF['invariant'] = 'vert'  # dishonored-nx (port 100)
STAGE_OF['clamp7e3'] = 'frag'  # dishonored-nx (port 106)


def optimise(text, passes=DEFAULT, stage=None, name=None):
    """stage: 'vert' or 'frag' (from the text when not given: a vertex shader writes gl_Position). name: the shader's
    hash (the gpr32 pass's list)."""
    names = [p for p in passes.split(',') if p] if isinstance(passes, str) else list(passes)
    if stage is None:
        stage = 'vert' if re.search(r'\bgl_Position\b', text) else 'frag'
    for pname, fn in PASSES:
        if pname in names and STAGE_OF.get(pname, stage) == stage:
            text = fn(text, name) if fn in (pass_gpr32, pass_lightbranch, pass_nullps, pass_skinonce, pass_nofxaa,
                                            pass_noearly) else fn(text)
    return text


UAM = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'out', 'deko3d_tools', 'uam-build',
                   'uam.exe')


def check_one(item):
    """uam compiles the shader's default variant (and with the alpha test's function 7 and 8): '' or the error."""
    import subprocess
    import tempfile
    name, text = item
    stage = 'frag' if name.startswith('ps_') else 'vert'
    variants = [text]
    if 'SPIRV_CROSS_CONSTANT_ID_903' in text:
        nl = text.find('\n') + 1
        variants += [text[:nl] + f'#define SPIRV_CROSS_CONSTANT_ID_903 {f}u\n' + text[nl:] for f in (7, 8)]
    with tempfile.TemporaryDirectory() as tmp:
        for k, v in enumerate(variants):
            src = os.path.join(tmp, f'v{k}.glsl')
            out = os.path.join(tmp, f'v{k}.dksh')
            with open(src, 'w', encoding='utf-8', newline='\n') as f:
                f.write(v)
            r = subprocess.run([UAM, '-s', stage, '-o', out, src], capture_output=True, text=True)
            if r.returncode != 0 or not os.path.isfile(out):
                return name, (r.stdout + r.stderr).strip()[:300]
    return name, ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='src')
    ap.add_argument('--out')
    ap.add_argument('--passes', default=DEFAULT)
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--check', action='store_true',
                    help='compile each output with uam; a shader that fails keeps its text as it came in')
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    a = ap.parse_args()
    if a.list:
        for name, fn in PASSES:
            print(f'{name}: {fn.__doc__.split(".")[0].strip()}')
        return 0
    os.makedirs(a.out, exist_ok=True)
    sources = {}
    texts = {}
    for name in sorted(os.listdir(a.src)):
        if not re.fullmatch(r'(vs|ps)_[0-9A-F]{16}\.glsl', name):
            continue
        sources[name] = open(os.path.join(a.src, name), encoding='utf-8').read()
        texts[name] = optimise(sources[name], a.passes, 'frag' if name.startswith('ps_') else 'vert', name[3:19])
    failed = []
    if a.check:
        import concurrent.futures
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            for name, err in ex.map(check_one, texts.items()):
                if err:
                    failed.append(name)
                    if len(failed) <= 10:
                        print(f'  {name}: uam failed, kept as it was: {err}')
                    texts[name] = sources[name]
    for name, out in texts.items():
        with open(os.path.join(a.out, name), 'w', encoding='utf-8', newline='\n') as f:
            f.write(out)
    print(f'deko3d_glsl_opt: {len(texts)} shaders, passes {a.passes} -> {a.out}' +
          (f' ({len(failed)} kept as they were: uam failed)' if a.check else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
