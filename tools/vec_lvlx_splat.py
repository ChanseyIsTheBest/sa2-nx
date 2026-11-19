#!/usr/bin/env python3
"""blur-nx (2026-11-11, docs/vector-store-ideas.md VS-25; the user: "do all the exact ones"): a scalar loaded with
lvlx and splatted, as one 4-byte load.
The XDK compiler loads a float or word for a splat with lvlx128 then vspltw 0:
    simde_mm_store_si128((simde__m128i*)V.u8, REX_SHUFFLE_TABLE(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(E & ~0xF)),
                                                               &VectorMaskL[(E & 0xF) * 16]));
    simde_mm_store_si128((simde__m128i*)D.u32, simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*)V.u32), 0xFF));
On the host that is a 16-byte load, a mask-row load, a table lookup and a dup. Word 0 of lvlx at address E is the four
bytes E..E+3 whenever E & 15 <= 12 (they lie in E's 16-byte block); the splat copies that word, read big-endian, to
every lane: simde_mm_set1_epi32(int(REX_LOAD_U32(E))). For E & 15 > 12 lvlx zero-fills the bytes past the block, so
that case keeps the original two statements (else branch).
Only where the lvlx's register is dead after the splat, proven over the whole function: V is a local (vN, not ctx.vN),
and every statement of the function that reads V is the splat right after an lvlx into V (comment lines skipped). Then
no path can read the lvlx value except its own splat, loops included, and leaving V unwritten on the fast path is
unobservable. The copies (tools/localize_native.py, after this pass) take the same form. Exact by construction; the
app's forms self-check compares both forms for every alignment on the device (banjo_forms_selfcheck.cpp, VecLvlxSplat).
  python tools/vec_lvlx_splat.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import re

LVLX = re.compile(r'^(?P<ind>\s*)simde_mm_store_si128\(\(simde__m128i\*\)(?P<v>v\d+)\.u8, REX_SHUFFLE_TABLE\('
                  r'simde_mm_load_si128\(\(simde__m128i\*\)REX_RAW_ADDR\((?P<e>[\w.]+) & ~0xF\)\), '
                  r'&VectorMaskL\[\((?P=e) & 0xF\) \* 16\]\)\);\s*$')
SPLAT = re.compile(r'^\s*simde_mm_store_si128\(\(simde__m128i\*\)(?P<d>[\w.]+)\.u32, simde_mm_shuffle_epi32\('
                   r'simde_mm_load_si128\(\(simde__m128i\*\)(?P<s>v\d+)\.u32\), 0xFF\)\);\s*$')


def is_comment(line):
    return line.lstrip().startswith('//')


def transform_function(lines):
    """lines: one function (signature to closing brace). Returns (new lines, sites done, pairs left)."""
    stmt = [i for i, l in enumerate(lines) if l.strip() and not is_comment(l)]
    nxt = {stmt[k]: stmt[k + 1] for k in range(len(stmt) - 1)}
    prv = {stmt[k + 1]: stmt[k] for k in range(len(stmt) - 1)}
    pairs = []
    for i in stmt:
        m = LVLX.match(lines[i])
        if m and i in nxt:
            s = SPLAT.match(lines[nxt[i]])
            if s and s.group('s') == m.group('v'):
                pairs.append((i, nxt[i], m, s))
    done = left = 0
    dead_vars = {}
    for i, j, m, s in pairs:
        v = m.group('v')
        if v in dead_vars:
            ok = dead_vars[v]
        else:
            ref = re.compile(r'(?<![\w.])' + v + r'\.')
            ok = True
            for q in stmt:
                l2 = lines[q]
                n = len(ref.findall(l2))
                if not n:
                    continue
                dest = re.match(r'^\s*simde_mm_store_si128\(\(simde__m128i\*\)' + v + r'\.', l2)
                if n - (1 if dest else 0) == 0:
                    continue  # a write of v only
                sp = SPLAT.match(l2)
                lv = LVLX.match(lines[prv[q]]) if q in prv else None
                if not (sp and sp.group('s') == v and lv and lv.group('v') == v):
                    ok = False
                    break
            dead_vars[v] = ok
        if not ok:
            left += 1
            continue
        ind, e, d = m.group('ind'), m.group('e'), s.group('d')
        # One statement (tools/localize_native.py reads the text a statement a line): the lvlx line becomes a comment
        # (its register is dead after the splat) and the splat reads its word 0 itself.
        lines[i] = f'{ind}// (tools/vec_lvlx_splat.py: the lvlx into {v} folded into the splat below)'
        lines[j] = (f'{ind}simde_mm_store_si128((simde__m128i*){d}.u32, (({e} & 0xF) <= 12) ? '
                    f'simde_mm_set1_epi32(int(REX_LOAD_U32({e}))) : simde_mm_shuffle_epi32(REX_SHUFFLE_TABLE('
                    f'simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR({e} & ~0xF)), &VectorMaskL[({e} & 0xF) * 16]), 0xFF));')
        done += 1
    return lines, done, left


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    total = left = changed = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        if 'tools/vec_lvlx_splat.py' in text:
            continue  # already done
        lines = text.split('\n')
        out = []
        start = None
        for k, l in enumerate(lines):
            if start is None:
                if l and not l[0].isspace() and l.rstrip().endswith('{') and not l.startswith('}'):
                    start = len(out)
                out.append(l)
            else:
                out.append(l)
                if l == '}':
                    body, n_done, n_left = transform_function(out[start:])
                    out[start:] = body
                    total += n_done
                    left += n_left
                    start = None
        new = '\n'.join(out)
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    print('vec_lvlx_splat%s: %d lvlx + splat pairs as one 4-byte load (the straddling case kept); %d pairs left (the '
          'lvlx register may be read again); %d files' % (' (check only)' if a.check else '', total, left, changed))


if __name__ == '__main__':
    main()
