#!/usr/bin/env python3
"""banjo-nx (package 27, codegen round): a jump table's default as unreachable where the guest's own bounds check proves
it, so GCC drops the compare and branch it puts in front of the table.

The generator writes a `bctr` through a jump table as `switch (rX.u32) { case 0: goto ...; ... default:
__builtin_trap(); }`. The guest has already checked the index:

    cr6.compare<uint32_t>(r11.u32, 32, xer);     // cmplwi cr6,r11,32
    if (cr6.gt) goto loc_8275F42C;              // bgt cr6,...
    ... straight-line code that writes neither r11 nor cr6 ...
    switch (r11.u32) {                          // cases 0..32

so the default cannot be reached, and GCC's own range test in front of the table repeats the guest's. A site is
rewritten only when, walking back from the switch inside its basic block (no label between: nothing jumps in), the
last write of the index register is not after the guest's unsigned compare of it with a constant K, the branch right
after that compare leaves on `gt` (index <= K) or `ge` with K (index < K), nothing writes that cr field or the
register in between, and the cases cover every index the check lets through. The default becomes
REX_SWITCH_UNREACHABLE(): __builtin_unreachable() on the Switch, the trap as before on the PC (so a wrong proof would
show there). Idempotent; --restore puts every site back.

usage: switch_unreachable.py --gen <app/generated> [--restore]
"""
import glob
import os
import re
import sys

TRAP = '__builtin_trap(); // Switch case out of range'
DONE = 'REX_SWITCH_UNREACHABLE(); // Switch case out of range (banjo-nx: the guest\'s bound check above)'
SWITCH_RE = re.compile(r'^\tswitch \((?:ctx\.)?(r\d+)\.u32\) \{$')
LABEL_RE = re.compile(r'^loc_[0-9A-F]+:')
CASE_RE = re.compile(r'^\tcase (\d+):$')
PCH_MACRO = '''
// banjo-nx (package 27): tools/switch_unreachable.py. A jump table's default the guest's own bounds check rules out:
// unreachable on the Switch (GCC drops its range test in front of the table), the trap on the PC.
#ifndef REX_SWITCH_UNREACHABLE
#if defined(__aarch64__) && !defined(REX_SWITCH_UNREACHABLE_OFF)
#define REX_SWITCH_UNREACHABLE() __builtin_unreachable()
#else
#define REX_SWITCH_UNREACHABLE() __builtin_trap()
#endif
#endif
'''


def writes(line, reg):
    return re.search(r'(?:^|[^.\w])(?:ctx\.)?' + reg + r'\.(?:u64|s64|u32|s32|u16|u8|f64)\s*=(?!=)', line) is not None


def prove(lines, at, reg):
    """The highest index the guest's check lets reach the switch at lines[at], or None."""
    i = at - 1
    while i >= 0:
        l = lines[i]
        if LABEL_RE.match(l) or l.startswith('}') or 'REX_FUNC' in l or 'DEFINE_REX_FUNC' in l:
            return None
        s = l.strip()
        m = re.fullmatch(r'if \((cr\d)\.(gt|lt|eq)\) goto loc_[0-9A-F]+;', s)
        if m:
            cr, cond = m.groups()
            # The compare right before it (comments between).
            j = i - 1
            while j >= 0 and lines[j].strip().startswith('//'):
                j -= 1
            cm = re.fullmatch(r'(cr\d)\.compare<uint32_t>\((?:ctx\.)?(r\d+)\.u32, (\d+), xer\);', lines[j].strip())
            if not cm or cm.group(1) != cr:
                # Some other branch in the block: it leaves the block, which the index reaches the switch without.
                i -= 1
                continue
            if cm.group(2) != reg:
                i -= 1
                continue
            k = int(cm.group(3))
            if cond == 'gt':
                return k
            return None
        if writes(l, reg):
            return None
        i -= 1
    return None


def process(path, restore):
    text = open(path, encoding='utf-8').read()
    if restore:
        if DONE not in text:
            return 0, 0
        open(path, 'w', encoding='utf-8', newline='\n').write(text.replace(DONE, TRAP))
        return 0, 0
    lines = text.split('\n')
    sites = proven = 0
    for at, l in enumerate(lines):
        m = SWITCH_RE.match(l)
        if not m:
            continue
        reg = m.group(1)
        # The switch's cases and its default line.
        cases, default = set(), None
        j = at + 1
        while j < len(lines) and lines[j] != '\t}':
            cm = CASE_RE.match(lines[j])
            if cm:
                cases.add(int(cm.group(1)))
            if lines[j].strip() == 'default:' and j + 1 < len(lines):
                default = j + 1
            j += 1
        if default is None or lines[default].strip() not in (TRAP, DONE):
            continue
        sites += 1
        k = prove(lines, at, reg)
        if k is not None and cases >= set(range(k + 1)):
            lines[default] = lines[default].replace(TRAP, DONE)
            proven += 1
        else:
            lines[default] = lines[default].replace(DONE, TRAP)
    new = '\n'.join(lines)
    if new != text:
        open(path, 'w', encoding='utf-8', newline='\n').write(new)
    return sites, proven


def main():
    args = sys.argv[1:]
    gen = args[args.index('--gen') + 1]
    restore = '--restore' in args
    pch = os.path.join(gen, 'renut_pch.h')
    if not restore and os.path.isfile(pch):
        p = open(pch, encoding='utf-8').read()
        if 'REX_SWITCH_UNREACHABLE' not in p:
            open(pch, 'w', encoding='utf-8', newline='\n').write(p + PCH_MACRO)
    sites = proven = 0
    for path in sorted(glob.glob(os.path.join(gen, '*.cpp'))):
        s, p = process(path, restore)
        sites += s
        proven += p
    if not restore:
        print(f'switch_unreachable: {proven} of {sites} jump-table defaults proven unreachable by the guest\'s check')


if __name__ == '__main__':
    main()
