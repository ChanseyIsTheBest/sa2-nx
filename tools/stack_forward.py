#!/usr/bin/env python3
"""sasrt-nx (2026-11-12, docs/cpu-moonshots-1112.md idea 2): a vector load of a stack slot that four 4-byte stores of
the same straight-line block just wrote takes the stored values instead of reading them back.

The 360 has no moves between its general, float and vector registers, so the game's compiler builds a vector from
scalars through the stack: stw / stfs x4 to r1+A .. r1+A+12, then lvx / lvx128 of r1+A. The A57 cannot forward four
narrow stores into one wide load: the load waits until the stores reach the cache. Here the stores stay (guest memory
is unchanged), each one's value is also kept in a local (rex_fwK = value, inside the store's own argument: the store
macros evaluate it once), and the load becomes REX_STACK_FWD4: when its address is exactly r1+A (the block cannot have
moved r1, and nothing else can have written those 16 bytes: no call, label, jump or store through another base in
between), the vector is set from the four values (lane 3 = the word at A, as the byte-swapped load gives it); any
other address takes the original load. Exact either way.

Runs in tools/post_codegen.py after vec_lane_index.py and before the copies (localize_native.py), so the copies take
the same form. --restore puts the original text back.

  python tools/stack_forward.py --gen app/generated [--restore] [--check]

renut-ultimate (2026-11-12, port 121): as sasrt-nx sent it (batch_cc2); tools/post_codegen.py runs it last before the
copies, after fma_single.py, and takes it back out first with the other rewrites.
"""
import argparse
import glob
import os
import re
import sys

MARKER = '// rexglue ports: vector loads of stack slots just stored, forwarded (tools/stack_forward.py)'
MACROS = MARKER + '''
#ifndef REX_STACK_FWD4
#if defined(REX_STACK_FWD_OFF)
#define REX_STACK_FWD4(vd, ea, at, w0, w1, w2, w3) \\
  simde_mm_store_si128((simde__m128i*)(vd).u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))))
#elif defined(REX_STACK_FWD_VERIFY)
// The self-check (the hooked guard build): the real load, compared with the forwarded words when the address matches.
extern "C" volatile unsigned long long g_rex_stack_fwd_checked;
extern "C" void RexStackFwdMismatch(unsigned address) noexcept;
#define REX_STACK_FWD4(vd, ea, at, w0, w1, w2, w3)                                                                  \\
  do {                                                                                                              \\
    const simde__m128i rex_ld_ = REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)));               \\
    if ((ea) == (u32)(at)) {                                                                                        \\
      const simde__m128i rex_fw_ = simde_mm_set_epi32((int)(u32)(w0), (int)(u32)(w1), (int)(u32)(w2), (int)(u32)(w3)); \\
      ++g_rex_stack_fwd_checked;                                                                                    \\
      if (simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(rex_ld_, rex_fw_)) != 0xFFFF) [[unlikely]]                     \\
        RexStackFwdMismatch((u32)(ea));                                                                             \\
    }                                                                                                               \\
    simde_mm_store_si128((simde__m128i*)(vd).u8, rex_ld_);                                                          \\
  } while (0)
#else
#define REX_STACK_FWD4(vd, ea, at, w0, w1, w2, w3)                                                                   \\
  ((ea) == (u32)(at) ? simde_mm_store_si128((simde__m128i*)(vd).u8,                                                  \\
                                            simde_mm_set_epi32((int)(u32)(w0), (int)(u32)(w1), (int)(u32)(w2),       \\
                                                               (int)(u32)(w3)))                                      \\
                     : simde_mm_store_si128((simde__m128i*)(vd).u8,                                                  \\
                                            REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea)))))
#endif
#endif
'''
DECL_MARK = '  // stack_forward captures'
HEAD = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
COMMENT = re.compile(r'^\t// (\S+)[ \t]*(.*)$')
LABEL = re.compile(r'^loc_[0-9A-F]{8}:')
STORE32 = re.compile(r'^(\t)REX_STORE_D_U32\(((?:ctx\.r1|r1l)\.u32), (-?\d+), (.+)\);$')
VLOAD = re.compile(r'^\tsimde_mm_store_si128\(\(simde__m128i\*\)((?:ctx\.)?v\d+)\.u8, REX_BSWAP128\(simde_mm_load_si128'
                   r'\(\(simde__m128i\*\)REX_RAW_ADDR\(ea\)\)\)\);$')
EA_LINE = re.compile(r'^\tea = \(.+\) & ~0xF;$')
FWD = re.compile(r'^\tREX_STACK_FWD4\(((?:ctx\.)?v\d+), ea, .*\);$')
CAPTURE = re.compile(r'\(rex_fw\d+ = \((.*?)\)\)\);$')
STORE_MNEMONICS = re.compile(r'^(st|dcbz|dcbz128)')


def rewrite_function(lines, stats):
    """lines: the body of one function (without its head and closing brace). Returns the new body or None."""
    out = list(lines)
    n_caps = 0
    slots = {}      # stack offset -> (line index of its C++ store, the r1 expression)
    base = {'r1': 0}
    foreign = False
    first_slot_line = None
    changed = False
    i = 0
    while i < len(out):
        line = out[i]
        if LABEL.match(line) or 'goto ' in line or 'return;' in line or '(ctx, base)' in line or 'switch (' in line:
            slots, base, foreign = {}, {'r1': 0}, False
            i += 1
            continue
        m = COMMENT.match(line)
        if not m:
            i += 1
            continue
        mn = m.group(1).rstrip('.')
        ops = [o.strip() for o in m.group(2).split(',')] if m.group(2).strip() else []
        # the C++ lines of this instruction: up to the next comment
        j = i + 1
        while j < len(out) and not COMMENT.match(out[j]) and not LABEL.match(out[j]):
            j += 1
        body = range(i + 1, j)
        writes_r1 = (ops and ops[0] == 'r1' and not mn.startswith('st')) or (
            mn in ('stwu', 'stdu', 'stwux', 'stdux') and len(ops) >= 2 and ops[1].endswith('(r1)'))
        if writes_r1:
            slots, base, foreign = {}, {'r1': 0}, False
            i = j
            continue
        if mn == 'addi' and len(ops) == 3 and ops[1] in base:
            base[ops[0]] = base[ops[1]] + int(ops[2])
            i = j
            continue
        if mn == 'mr' and len(ops) == 2 and ops[1] in base:
            base[ops[0]] = base[ops[1]]
            i = j
            continue
        if mn in ('stw', 'stfs') and len(ops) == 2:
            d = re.match(r'^(-?\d+)\(r1\)$', ops[1])
            st = None
            for k in body:
                st = STORE32.match(out[k]) or st
                if STORE32.match(out[k]):
                    sk = k
            if d and st:
                slots[int(d.group(1))] = (sk, st.group(2))
                i = j
                continue
            foreign = True   # a store this pass does not follow
            i = j
            continue
        if STORE_MNEMONICS.match(mn):
            foreign = True
            i = j
            continue
        if mn in ('lvx', 'lvx128') and len(ops) == 3 and ops[1] in ('r0', '0') and ops[2] in base and not foreign:
            a = base[ops[2]]
            need = [a, a + 4, a + 8, a + 12]
            ea_k = next((k for k in body if EA_LINE.match(out[k])), None)
            ld_k = next((k for k in body if VLOAD.match(out[k])), None)
            if all(x in slots for x in need) and ea_k is not None and ld_k is not None and a % 4 == 0:
                exprs = {slots[x][1] for x in need}
                if len(exprs) == 1:
                    names = []
                    for x in need:
                        k, _ = slots[x]
                        cap = CAPTURE.search(out[k])
                        if cap:
                            name = re.search(r'\((rex_fw\d+) = ', out[k]).group(1)
                        else:
                            name = 'rex_fw%d' % n_caps
                            n_caps += 1
                            sm = STORE32.match(out[k])
                            out[k] = '%sREX_STORE_D_U32(%s, %s, (%s = (%s)));' % (sm.group(1), sm.group(2), sm.group(3),
                                                                                  name, sm.group(4))
                        names.append(name)
                    vd = VLOAD.match(out[ld_k]).group(1)
                    r1e = exprs.pop()
                    out[ld_k] = '\tREX_STACK_FWD4(%s, ea, (u32)(%s) + (%d), %s);' % (vd, r1e, a, ', '.join(names))
                    stats['sites'] += 1
                    changed = True
        # any other write of a tracked register forgets it (the first operand of a non-store)
        if ops and re.match(r'^r\d+$', ops[0]) and ops[0] in base and ops[0] != 'r1':
            base.pop(ops[0], None)
        i = j
    if not changed:
        return None
    stats['functions'] += 1
    stats['captures'] += n_caps
    return ['\tu32 %s;%s' % (', '.join('rex_fw%d' % k for k in range(n_caps)), DECL_MARK)] + out


def restore_text(text):
    if MARKER not in text and 'REX_STACK_FWD4(' not in text and 'rex_fw' not in text:
        return text, 0
    lines = text.split('\n')
    out = []
    n = 0
    for line in lines:
        if line.endswith(DECL_MARK):
            continue
        m = FWD.match(line)
        if m:
            line = ('\tsimde_mm_store_si128((simde__m128i*)%s.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)'
                    'REX_RAW_ADDR(ea))));' % m.group(1))
            n += 1
        elif 'rex_fw' in line:
            line = re.sub(r'\(rex_fw\d+ = \((.*)\)\)\);$', r'\1);', line)
        out.append(line)
    return '\n'.join(out), n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--restore', action='store_true')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp')))
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): stack_forward: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    if a.restore:
        total = 0
        for path in files:
            text = open(path, encoding='utf-8').read()
            new, n = restore_text(text)
            if new != text:
                total += n
                if not a.check:
                    open(path, 'w', encoding='utf-8', newline='\n').write(new)
        print('stack_forward: restored %d forwarded loads' % total)
        return
    stats = {'sites': 0, 'functions': 0, 'captures': 0}
    for path in files:
        lines = open(path, encoding='utf-8').read().split('\n')
        out = []
        i = 0
        changed = False
        while i < len(lines):
            out.append(lines[i])
            if HEAD.match(lines[i]):
                j = i + 1
                while j < len(lines) and lines[j] != '}':
                    j += 1
                body = lines[i + 1:j]
                if any(l.endswith(DECL_MARK) for l in body):
                    sys.exit('stack_forward: already applied in %s (run --restore first)' % path)
                new = rewrite_function(body, stats)
                out.extend(new if new is not None else body)
                changed = changed or new is not None
                i = j
                continue
            i += 1
        if changed and not a.check:
            open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    pch = pchs[0]
    text = open(pch, encoding='utf-8').read()
    if MARKER not in text and not a.check:
        open(pch, 'w', encoding='utf-8', newline='\n').write(text.rstrip('\n') + '\n\n' + MACROS)
    print('stack_forward: %d vector loads of stack slots forwarded from their four stores, in %d functions '
          '(%d captured values)' % (stats['sites'], stats['functions'], stats['captures']))


if __name__ == '__main__':
    main()
