#!/usr/bin/env python3
"""tw2de_nx (2026-11-01): expands a test .s for miniasm.c: .macro/.endm, C comments, '*' banner lines, the shift mnemonics binutils keeps as
macros (sldi srdi slwi srwi), and two VMX128 opcodes missing from the SDK table (as .long)."""
import re, sys
src = open(sys.argv[1]).read()
src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
macros = {}
out = []
lines = src.split('\n')
i = 0
def reg(s):
    s = s.strip(); return int(s[1:]) if s[0] in 'rv' else int(s, 0)
def expand(line):
    m = re.match(r'(\s*)(\S+)\s*(.*)$', line)
    if not m: return [line]
    ind, mn, rest = m.groups()
    args = [a.strip() for a in rest.split(',')] if rest.strip() else []
    if mn in macros:
        params, body = macros[mn]
        res = []
        for b in body:
            for p, a in zip(params, args):
                b = b.replace('\\' + p, a)
            res += expand(b)
        return res
    if mn in ('sldi', 'srdi', 'slwi', 'srwi', 'sldi.', 'srdi.', 'slwi.', 'srwi.'):
        dot = '.' if mn.endswith('.') else ''
        a, s, n = args[0], args[1], int(args[2], 0)
        base = mn.rstrip('.')
        if base == 'sldi': return ['%s rldicr%s %s, %s, %d, %d' % (ind, dot, a, s, n, 63 - n)]
        if base == 'srdi': return ['%s rldicl%s %s, %s, %d, %d' % (ind, dot, a, s, (64 - n) % 64, n)]
        if base == 'slwi': return ['%s rlwinm%s %s, %s, %d, 0, %d' % (ind, dot, a, s, n, 31 - n)]
        if base == 'srwi': return ['%s rlwinm%s %s, %s, %d, %d, 31' % (ind, dot, a, s, (32 - n) % 32, n)]
    if mn in ('vupkhsb128', 'vupklsb128'):
        vd, vb = reg(args[0]), reg(args[1])
        xop = 896 if mn == 'vupkhsb128' else 960
        w = (6 << 26) | xop | ((vd & 31) << 21) | ((vd >> 5) << 2) | ((vb & 31) << 11) | (vb >> 5)
        return ['%s.long 0x%08X' % (ind, w)]
    if mn == 'dcbz128':
        ra, rb = reg(args[0]), reg(args[1])
        return ['%s.long 0x%08X' % (ind, 0x7C2007EC | (ra << 16) | (rb << 11))]
    return [line]
while i < len(lines):
    l = lines[i]
    s = l.split('#')[0].strip()
    if s.startswith('.macro'):
        parts = s[6:].replace(',', ' ').split()
        name, params = parts[0], parts[1:]
        body = []
        i += 1
        while not lines[i].strip().startswith('.endm'):
            body.append(lines[i]); i += 1
        macros[name] = (params, body)
        i += 1
        continue
    if s.startswith('*') or s.startswith('.'):
        if not s.startswith('.long'):
            out.append('#' + l); i += 1; continue
    if s and not s.endswith(':'):
        out += expand(l.split('#')[0]) + (['#' + l.split('#', 1)[1]] if '#' in l else [])
    else:
        out.append(l)
    i += 1
sys.stdout.write('\n'.join(out) + '\n')
