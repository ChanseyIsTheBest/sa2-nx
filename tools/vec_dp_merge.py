#!/usr/bin/env python3
"""blur-nx (2026-11-11, CPU batch 2: docs/vector-ideas-2.md VX-15 and VX-14; the user: "do all the exact ones"): two
broadcast dot products merged by vmrghw / vmrglw, and two such pairs merged again, as one pairwise-add tree.

The generated text (vmsum3fp128 / vmsum4fp128, then vmrghw128 / vmrglw128):
    simde_mm_store_ps(R1.f32, simde_mm_dp_ps(A1, B1, IMM));
    simde_mm_store_ps(R2.f32, simde_mm_dp_ps(A2, B2, IMM));
    simde_mm_store_si128((simde__m128i*)D.u32, simde_mm_unpack{hi,lo}_epi32(simde_mm_load_si128((simde__m128i*)R1.u32),
                                                                         simde_mm_load_si128((simde__m128i*)R2.u32)));
D = [d1 d2 d1 d2] (lane 0 first). Rewritten (sdk/include/rex/ppc/vec_batch2.h):
    simde_mm_store_ps(R1.f32, REX_DP_PROD(A1, B1, IMM));     the products only
    simde_mm_store_ps(R2.f32, REX_DP_PROD(A2, B2, IMM));
    simde_mm_store_ps(D.f32, REX_DP_PAIR(simde_mm_load_ps(R1.f32), simde_mm_load_ps(R2.f32)));
and a merge of two such pairs D1 = [a b a b], D2 = [c d c d] (D = [a c b d]) as REX_DP_QUAD(Pa, Pc, Pb, Pd), the two
pair statements becoming comments.

Conditions (each proven on the text):
- the registers whose value changes (R1, R2 now hold products; D1, D2 of a quad are no longer written) are the copy's
  own locals (vN, not ctx.vN or the localized l_vN) that nothing reads after the merge on any path (a control-flow
  walk over the function's labels, gotos, switches and returns: the first statement naming the register on every
  path writes all of it; a construct the walk does not follow counts as a read), unless the merge writes it itself;
- the dot products (or pairs) and the merge are in one straight stretch (no label, branch, block, call between) in
  which no other statement names those registers and none changes the floating-point mode (the adds now run at the
  merge: in the same mode as the dot products, the VMX one);
- IMM's low nibble is 0xF (a broadcast), the same IMM for both, R1 != R2.
Exact: each lane is (p0 + p1) + (p2 + p3) of the same products in the same order (vec_batch2.h, the same form on the
PC with SSE). Only the machine copies' f_ bodies (app/generated/hot/n_*.h, after tools/localize_native.py): the o_ bodies,
the original statements the guard compares a copy with, keep the dot products and zips, so a guarded PC run checks
both the arithmetic and the liveness above. BLUR_DP_MERGE_OFF (environment): nothing.
  python tools/vec_dp_merge.py --gen app/generated [--check]
"""
import argparse
import glob
import os
import re
import sys

DP = re.compile(r'^(?P<ind>\s*)simde_mm_store_ps\((?P<r>v\d+)\.f32, simde_mm_dp_ps\((?P<args>.*), (?P<imm>0x[0-9A-Fa-f]{2})\)\);\s*$')
MERGE = re.compile(r'^(?P<ind>\s*)simde_mm_store_si128\(\(simde__m128i\*\)(?P<d>(?:ctx\.)?v\d+)\.u32, simde_mm_unpack(?P<hl>hi|lo)_epi32\('
                   r'simde_mm_load_si128\(\(simde__m128i\*\)(?P<a>v\d+)\.u32\), simde_mm_load_si128\(\(simde__m128i\*\)(?P<b>v\d+)\.u32\)\)\);\s*$')
PAIR = re.compile(r'^(?P<ind>\s*)simde_mm_store_ps\((?P<d>(?:ctx\.)?v\d+)\.f32, REX_DP_PAIR\(simde_mm_load_ps\((?P<a>v\d+)\.f32\), '
                  r'simde_mm_load_ps\((?P<b>v\d+)\.f32\)\)\);\s*$')
STRAIGHT_BREAK = re.compile(r'\b(?:if|else|goto|return|switch|case|for|while|do|break|continue)\b|[{}]|^\s*[A-Za-z_]\w*:\s*$|'
                            r'\bsub_[0-9A-Fa-f]{8}\b|__imp__|REX_CALL|PPC_CALL|setjmp|longjmp')
MODE_OK = ('ctx.fpscr.enableFlushMode();', 'REX_FLUSH_KNOWN_ON();', 'REX_FLUSH_HOIST_KNOWN_ON();')
EXIT = -1
LABEL = re.compile(r'^\s*(loc_[0-9A-Fa-f]+):\s*$')
GOTO = re.compile(r'^\s*goto (loc_[0-9A-Fa-f]+);\s*$')
IF_GOTO = re.compile(r'^\s*if \(.*\) goto (loc_[0-9A-Fa-f]+);\s*$')
# (a copy's one-line call block, `{ ...; if (...) { ...; } else if (s.Stopped()) return; ... }`, is one statement)
UNSUPPORTED = re.compile(r'^\s*(?:while|for|do|else|break|continue)\b|^\s*\}\s*else\b|\bif \(.*\)\s*\{\s*$')
# The copies' stop check: constant false in the copy that runs; in the guard's logged form an early return, after
# which the guard puts everything back and runs the original statements.
STOP_CHECK = 'if (s.Stopped()) return;'

MACROS_MARKER = '// rexglue ports: dot products merged by vmrghw / vmrglw as one pairwise-add tree (tools/vec_dp_merge.py)'
MACROS = MACROS_MARKER + '''
#ifndef REX_DP_PAIR
#include <rex/ppc/vec_batch2.h>
#define REX_DP_PROD(a, b, imm) rex::ppc::DpProd<imm>((a), (b))
#define REX_DP_PAIR(p1, p2) rex::ppc::DpPair((p1), (p2))
#define REX_DP_QUAD(pw, px, py, pz) rex::ppc::DpQuad((pw), (px), (py), (pz))
#endif  // REX_DP_PAIR
// (end of the macros of tools/vec_dp_merge.py)
'''


def is_comment(line):
    return line.lstrip().startswith('//')


def mode_change(line):
    s = line.strip()
    if s in MODE_OK:
        return False
    return 'fpscr' in s or 'FLUSH' in s or 'FlushMode' in s


def split_args(args):
    depth = 0
    for i, ch in enumerate(args):
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        elif ch == ',' and depth == 0:
            return args[:i].strip(), args[i + 1:].strip()
    return None


def build_cfg(lines, stmt):
    """Successors of every statement line (EXIT: leaving the function), or None for a construct not followed."""
    labels = {}
    for i in stmt:
        m = LABEL.match(lines[i])
        if m:
            labels[m.group(1)] = i
    succ = {}
    if not stmt:
        return succ
    nxt = {stmt[k]: stmt[k + 1] for k in range(len(stmt) - 1)}
    last = stmt[-1]
    skip_until = -1
    for i in stmt:
        if i <= skip_until:
            continue
        l = lines[i]
        s = l.strip()
        if i == last:
            succ[i] = [EXIT]  # the closing brace
            continue
        fall = [nxt[i]]
        if UNSUPPORTED.search(l):
            return None
        if s.startswith('switch'):
            # the block to its closing brace at the same indent: one node, its goto targets and what follows it
            ind = len(l) - len(l.lstrip())
            k = i + 1
            targets = []
            while k < len(lines):
                lk = lines[k]
                if lk.strip() == '}' and len(lk) - len(lk.lstrip()) == ind:
                    break
                for t in re.findall(r'goto (loc_[0-9A-Fa-f]+);', lk):
                    if t not in labels:
                        return None
                    targets.append(labels[t])
                if re.search(r'\breturn\b', lk):
                    targets.append(EXIT)
                k += 1
            if k >= len(lines) - 1:
                return None
            after = [q for q in stmt if q > k]
            targets.append(after[0] if after else EXIT)
            succ[i] = targets
            for q in stmt:
                if i < q <= k:
                    succ[q] = []  # inside the switch: reached only through it
            skip_until = k
            continue
        m = GOTO.match(l)
        if m:
            if m.group(1) not in labels:
                return None
            succ[i] = [labels[m.group(1)]]
            continue
        m = IF_GOTO.match(l)
        if m:
            if m.group(1) not in labels:
                return None
            succ[i] = fall + [labels[m.group(1)]]
            continue
        if re.search(r'\bgoto\b', l):
            return None  # a goto in some other form
        if re.search(r'\breturn\b', l):
            succ[i] = [EXIT] if s == 'return;' else fall + [EXIT]
            continue
        if '__builtin_unreachable' in l or '__builtin_trap' in l:
            succ[i] = [EXIT]
            continue
        succ[i] = fall
    return succ


class Func:
    """One function's text: its statements, a control-flow graph over its lines, and the liveness of a local vN."""

    def __init__(self, lines):
        self.lines = lines
        self.stmt = [i for i, l in enumerate(lines) if l.strip() and not is_comment(l)]
        self.pos = {i: k for k, i in enumerate(self.stmt)}
        self.succ = build_cfg(lines, self.stmt)  # None: a construct this pass does not follow

    @staticmethod
    def mentions(v, line):
        return re.search(r'(?<![\w.])' + v + r'\b', line) is not None

    def full_write(self, v, line):
        """The statement writes all of v and reads nothing of it."""
        m = re.match(r'^\s*simde_mm_store_(?:si128|ps)\((?:\(simde__m128i\*\))?' + v + r'\.(?:u8|u32|f32), (.*)\);\s*$', line)
        if m:
            return not self.mentions(v, m.group(1))
        m = re.match(r'^\s*' + v + r'\s*=\s*(.*);\s*$', line)
        return bool(m) and not self.mentions(v, m.group(1))

    def writes(self, v, line):
        """The statement may write some of v."""
        if not self.mentions(v, line):
            return False
        if self.full_write(v, line):
            return True
        if re.search(r'(?<![\w.])' + v + r'(?:\.\w+)*(?:\[\d+\])?\s*=(?!=)', line) or ('&' + v) in line:
            return True
        # a statement macro naming it (REX_VUPKD3D_COLOR(v, ...), ...): it may write it
        return not re.match(r'^\s*(?:simde_mm_store_\w+\(|[\w.\[\]]+\s*=)', line)

    def live_after(self, j, v):
        """Can a statement after j read v before it is wholly written again (on any path)? True when unknown."""
        if self.succ is None:
            return True
        seen = set()
        work = list(self.succ[j])
        while work:
            q = work.pop()
            if q in seen:
                continue
            seen.add(q)
            if q == EXIT:
                continue  # a local: nothing reads it after the function
            l = self.lines[q]
            if self.mentions(v, l):
                if self.full_write(v, l):
                    continue
                return True
            work.extend(self.succ.get(q, []))
        return False

    def straight(self, i, j, quiet=()):
        """Statements strictly between i < j: one straight stretch, no mode change, none naming a register of quiet."""
        for q in self.stmt[self.pos[i] + 1:self.pos[j]]:
            l = self.lines[q]
            if l.strip() == STOP_CHECK:
                continue
            if STRAIGHT_BREAK.search(l) or mode_change(l):
                return False
            if any(self.mentions(r, l) for r in quiet):
                return False
        return True


def last_def_before(f, v, j, pattern):
    """The statement before j that last writes v, if it matches pattern (else None)."""
    for q in reversed(f.stmt[:f.pos[j]]):
        if f.writes(v, f.lines[q]):
            m = pattern.match(f.lines[q])
            return (q, m) if m else None
    return None


def transform_function(lines, stats):
    f = Func(lines)
    # pairs
    for j in list(f.stmt):
        m = MERGE.match(lines[j])
        if not m:
            continue
        a, b, d = m.group('a'), m.group('b'), m.group('d')
        if a == b:
            continue
        da = last_def_before(f, a, j, DP)
        db = last_def_before(f, b, j, DP)
        if not da or not db:
            continue
        (ia, ma), (ib, mb) = da, db
        if ma.group('imm').upper() != mb.group('imm').upper() or not ma.group('imm').upper().endswith('F'):
            continue
        if not split_args(ma.group('args')) or not split_args(mb.group('args')):
            continue
        # one straight stretch from the first dot product to the merge, nothing naming a product in between
        if not (f.straight(min(ia, ib), j) and f.straight(ia, j, [a]) and f.straight(ib, j, [b])):
            stats['pair_flow'] += 1
            continue
        # the products are seen by nothing after the merge (unless the merge writes that register itself)
        if (a != d and f.live_after(j, a)) or (b != d and f.live_after(j, b)):
            stats['pair_live'] += 1
            continue
        for q, mm in ((ia, ma), (ib, mb)):
            lines[q] = '%ssimde_mm_store_ps(%s.f32, REX_DP_PROD(%s, %s));' % (mm.group('ind'), mm.group('r'), mm.group('args'),
                                                                             mm.group('imm'))
        lines[j] = '%ssimde_mm_store_ps(%s.f32, REX_DP_PAIR(simde_mm_load_ps(%s.f32), simde_mm_load_ps(%s.f32)));' % (
            m.group('ind'), d, a, b)
        stats['pairs'] += 1
    # quads: a merge of two pairs
    f = Func(lines)
    for j in list(f.stmt):
        m = MERGE.match(lines[j])
        if not m:
            continue
        d1, d2, d = m.group('a'), m.group('b'), m.group('d')
        if d1 == d2:
            continue
        p1 = last_def_before(f, d1, j, PAIR)
        p2 = last_def_before(f, d2, j, PAIR)
        if not p1 or not p2:
            continue
        (i1, m1), (i2, m2) = p1, p2
        if m1.group('d') != d1 or m2.group('d') != d2:
            continue
        ra, rb, rc, rd = m1.group('a'), m1.group('b'), m2.group('a'), m2.group('b')
        if len({ra, rb, rc, rd}) != 4:
            continue
        # the products still in their registers at the quad, the pair results read by nothing but it
        if not (f.straight(min(i1, i2), j) and f.straight(i1, j, [ra, rb, d1]) and f.straight(i2, j, [rc, rd, d2])):
            stats['quad_flow'] += 1
            continue
        if (d1 != d and f.live_after(j, d1)) or (d2 != d and f.live_after(j, d2)):
            stats['quad_live'] += 1
            continue
        for q, mm in ((i1, m1), (i2, m2)):
            lines[q] = '%s// (tools/vec_dp_merge.py: the pair into %s folded into the quad below)' % (mm.group('ind'), mm.group('d'))
        lines[j] = ('%ssimde_mm_store_ps(%s.f32, REX_DP_QUAD(simde_mm_load_ps(%s.f32), simde_mm_load_ps(%s.f32), '
                    'simde_mm_load_ps(%s.f32), simde_mm_load_ps(%s.f32)));') % (m.group('ind'), d, ra, rc, rb, rd)
        stats['quads'] += 1
        stats['pairs'] -= 2
        f = Func(lines)  # the pair statements are comments now
    return lines


FUNC_START = re.compile(r'^(?:\[\[gnu::always_inline\]\] )?static (?:inline )?void ([fo])_sub_[0-9A-F]+\(.*\{\s*$')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True, help='generated folder (its hot/ copies and its *_pch.h)')
    ap.add_argument('--check', action='store_true', help='count only, write nothing')
    a = ap.parse_args()
    if os.environ.get('BLUR_DP_MERGE_OFF'):
        print('vec_dp_merge: off (BLUR_DP_MERGE_OFF)')
        return 0
    stats = {'pairs': 0, 'quads': 0, 'pair_live': 0, 'pair_flow': 0, 'quad_live': 0, 'quad_flow': 0}
    changed = 0
    for path in sorted(glob.glob(os.path.join(a.gen, 'hot', 'n_*.h'))):
        text = open(path, encoding='utf-8', newline='').read()
        if 'REX_DP_PAIR(' in text or 'REX_DP_QUAD(' in text or 'simde_mm_dp_ps' not in text:
            continue  # already done, or nothing to do
        if '\r' in text:
            sys.exit('ERROR (post-codegen patch): vec_dp_merge: %s has CR LF line ends' % path)
        lines = text.split('\n')
        out = []
        start = None
        kind = None
        before = dict(stats)
        for l in lines:
            if start is None:
                m = FUNC_START.match(l)
                out.append(l)
                if m:
                    start, kind = len(out) - 1, m.group(1)
            else:
                out.append(l)
                if l == '}':
                    # only the copies that run (f_); the original statements (o_) stay as the guard's reference
                    if kind == 'f':
                        out[start:] = transform_function(out[start:], stats)
                    start = None
        if stats['pairs'] != before['pairs'] or stats['quads'] != before['quads']:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    if not a.check:
        pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
        if len(pchs) != 1:
            sys.exit('ERROR (post-codegen patch): vec_dp_merge: no single *_pch.h in %s' % a.gen)
        text = open(pchs[0], encoding='utf-8').read()
        if MACROS_MARKER not in text:
            anchor = '// rexglue ports: vcfsx / vcfux with a scale as one fixed-point conversion (tools/vec_cvt_fixed.py)'
            if anchor not in text:
                sys.exit('ERROR (post-codegen patch): vec_dp_merge: the vec_cvt_fixed macros are not in %s' % pchs[0])
            text = text.replace(anchor, MACROS + anchor, 1)
            open(pchs[0], 'w', encoding='utf-8', newline='\n').write(text)
    print('vec_dp_merge: %d dot-product pairs and %d quads merged in %d copies (left: %d pairs with a product read after '
          'the merge, %d not straight; %d quads with a pair read after it, %d not straight)' % (
              stats['pairs'], stats['quads'], changed, stats['pair_live'], stats['pair_flow'], stats['quad_live'],
              stats['quad_flow']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
