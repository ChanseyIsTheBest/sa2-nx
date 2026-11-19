#!/usr/bin/env python3
"""blur-nx (2026-11-11, CPU batch 2: docs/vector-store-ideas.md VS-14; the user: "do all the exact ones"): one card
check per group of card stores to the same base register in the machine copies.

store_cards.py makes every store outside the stack mark its page: BANJO_HOT_STORE_D_Ux_C(r, d, y) stores and then
REX_CARD((u32)(r) + (u32)(d)). With the card runs (banjo_hot_prelude.h) each REX_CARD compares the page with the
run's and marks the run's page when it moves on. A copy's stores come in groups on one base with constant offsets
(a structure filled field by field), and often two or three such groups interleaved (two output records written
in turns), so the run moves between their pages at every store.

Here, in each straight stretch of a copy's f_ statements (no label, branch, block, call, flush or other card
statement in between), the D-form card stores to one base r with offsets d_min..d_max (d_max - d_min < 4096) are
written without their card, and two card statements follow the stretch's last store of that group, at the end of the
group: REX_CARD(r + d_min) and REX_CARD(r + d_max) (one when d_min == d_max). The group ends at the stretch's end or
before a statement that may change r.

Exact for the cards: every store's page lies between the pages of r + d_min and r + d_max, which are at most one page
apart, so the same pages are marked, each after its stores (as the runs already do: they mark a page when the run
moves on, at the earliest after the store). The stores themselves and their order are unchanged. In the guard's
logged form a copy that stops early leaves the cards of the group's stores so far unmarked; the guard then puts the
stores back and runs the original, which marks its own.

Only the f_ bodies (the copies that run); the o_ bodies (the original statements for the guard) are left as they are.
Idempotent: a converted store no longer has its card, and the inserted card statements end stretches.

Run by post_codegen.py after store_cards.py.
"""
import argparse
import glob
import os
import re
import sys

STORE = re.compile(r'^(\s*)BANJO_HOT_STORE_D_U(8|16|32|64)_C\(([A-Za-z_]\w*\.(?:u8|u16|u32|u64|s8|s16|s32|s64)), (-?\d+), (.*)\);\s*$')
FUNC_START = re.compile(r'^(?:\[\[gnu::always_inline\]\] )?static (?:inline )?void ([fo])_sub_[0-9A-F]+\(.*\{\s*$')
FUNC_END = re.compile(r'^\}\s*$')
# Anything that ends a straight stretch.
BREAK = re.compile(
    r'\b(?:if|else|goto|return|switch|case|default|for|while|do|break|continue)\b|[{}]|^\s*[A-Za-z_]\w*:\s*$|'
    r'\bsub_[0-9A-Fa-f]{8}\b|Native\(|REX_CARD|REX_GUEST_CAS|REX_CALL|PPC_CALL|setjmp|longjmp|REX_DCBZ|s\.Open\(|'
    r'::banjo::hand|__builtin_trap|__builtin_unreachable|TRAP|SYSCALL|REX_KERNEL|REX_BRANCH|REX_LOOP_')
NEUTRAL = ('if (s.Stopped()) return;',)
ASSIGN = re.compile(r'^\s*([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)?(?:\[\d+\])?)\s*=(?!=)\s*(.*);\s*$')


def balanced(expr):
    depth = 0
    for ch in expr:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


def word_in(root, text):
    return re.search(r'(?<![\w.])' + re.escape(root) + r'\b', text) is not None


def writes_root(root, line):
    """True when the statement may change the register `root` (anything but a plain read of it)."""
    if not word_in(root, line):
        return False
    m = ASSIGN.match(line)
    if not m:
        return True  # a statement macro or anything else naming it: it may write it
    lhs, rhs = m.group(1), m.group(2)
    if word_in(root, lhs):
        return True
    if ('&' + root) in rhs.replace(' ', '') or ';' in rhs or re.search(r'(?<![=!<>])=(?!=)', rhs) or '++' in rhs or '--' in rhs:
        return True
    return False


def process_body(lines, stats):
    """lines: the statements of one f_ body (without its first and last line). Returns the new lines."""
    out = []
    groups = {}  # base -> {'root', 'stores': [index in out], 'dmin', 'dmax', 'indent'}

    def close(base):
        g = groups.pop(base)
        if len(g['stores']) < 2 or g['dmax'] - g['dmin'] >= 4096:
            return
        for i in g['stores']:
            out[i] = out[i].replace('_C(', '(', 1)
        cards = ['%sREX_CARD((u32)(%s) + (u32)(%d));\n' % (g['indent'], base, g['dmin'])]
        if g['dmax'] != g['dmin']:
            cards.append('%sREX_CARD((u32)(%s) + (u32)(%d));\n' % (g['indent'], base, g['dmax']))
        # after the group's last store (and the folded stop check that follows it)
        at = g['stores'][-1] + 1
        if at < len(out) and out[at].strip() in NEUTRAL:
            at += 1
        # later groups' store indices shift by the inserted lines
        for other in groups.values():
            other['stores'] = [i + len(cards) if i >= at else i for i in other['stores']]
        out[at:at] = cards
        stats['groups'] += 1
        stats['stores'] += len(g['stores'])
        stats['cards'] += len(cards)

    def close_all():
        # in the order of their last store, so the inserted lines keep the stretch's order
        for base in sorted(groups, key=lambda b: groups[b]['stores'][-1]):
            close(base)

    for line in lines:
        s = line.strip()
        if not s or s.startswith('//') or s in NEUTRAL or re.match(r'^s\.Hit\(\d+\);$', s):
            out.append(line)
            continue
        m = STORE.match(line)
        if m and balanced(m.group(5)) and not BREAK.search(m.group(5)):
            base, d = m.group(3), int(m.group(4))
            root = base.split('.')[0]
            value = m.group(5)
            # a value that may write a register of an open group ends that group first
            for b in list(groups):
                if groups[b]['root'] != root and writes_root(groups[b]['root'], 'x = ' + value + ';'):
                    close(b)
            g = groups.get(base)
            if g is None:
                g = groups[base] = {'root': root, 'stores': [], 'dmin': d, 'dmax': d, 'indent': m.group(1)}
            out.append(line)
            g['stores'].append(len(out) - 1)
            g['dmin'] = min(g['dmin'], d)
            g['dmax'] = max(g['dmax'], d)
            continue
        if BREAK.search(line):
            close_all()
            out.append(line)
            continue
        for b in list(groups):
            if writes_root(groups[b]['root'], line):
                close(b)
        out.append(line)
    close_all()
    return out


def process_file(path, stats):
    with open(path, encoding='utf-8', newline='') as f:
        text = f.read()
    lines = text.splitlines(keepends=True)
    out = []
    i = 0
    changed = False
    while i < len(lines):
        m = FUNC_START.match(lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        j = i + 1
        while j < len(lines) and not FUNC_END.match(lines[j]):
            j += 1
        out.append(lines[i])
        body = lines[i + 1:j]
        if m.group(1) == 'f':
            before = stats['groups']
            new = process_body(body, stats)
            if stats['groups'] != before:
                changed = True
            out.extend(new)
        else:
            out.extend(body)
        i = j
    if changed:
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(''.join(out))
        stats['files'] += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--gen', required=True, help='the generated folder (its hot/ copies are rewritten)')
    a = ap.parse_args()
    if os.environ.get('BLUR_CARD_GROUPS_OFF'):
        print('card_groups: off (BLUR_CARD_GROUPS_OFF)')
        return 0
    stats = {'files': 0, 'groups': 0, 'stores': 0, 'cards': 0}
    for p in sorted(glob.glob(os.path.join(a.gen, 'hot', 'n_*.h'))):
        process_file(p, stats)
    print('card_groups: %d groups of card stores (%d stores, %d card statements instead) in %d copies' %
          (stats['groups'], stats['stores'], stats['cards'], stats['files']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
