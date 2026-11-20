#!/usr/bin/env python3
"""banjo-nx (2026-11-19, the rebase on the ultimate base): re-keys an indirect_promotions.txt made with the old site
key (a named function's sites counted as the previous sub_ function's, tools/indirect_calls.py before this date) to
the new one (every site by its own function's address and its ordinal there).

The old key is read from a generated tree that has the old tags in it (the tree the list was used with: each site's
line holds REX_CALL_INDIRECT_FUNC_AT(0x<function>, <site>, ...)), and each tag is mapped to its real function there.

  python tools/indirect_promotions_rekey.py --old-gen <old generated folder> --list app/indirect_promotions.txt [--write]
"""
import argparse
import os
import re
import sys

ANY_FUNC = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
SUB = re.compile(r'^sub_([0-9A-F]{8})$')
INIT_ENTRY = re.compile(r'^\s*\{ 0x([0-9A-F]{8}), (\w+) \},\s*$')
TAG = re.compile(r'REX_CALL_INDIRECT_FUNC_AT\(0x([0-9A-F]{8}), (\d+), ')
LINE = re.compile(r'^\t(REX_CALL_INDIRECT_FUNC_AT\(|REX_CALL_INDIRECT_FUNC\(|if \(REX_PROMOTED_IS\()')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--old-gen', required=True)
    ap.add_argument('--list', required=True)
    ap.add_argument('--write', action='store_true')
    a = ap.parse_args()
    names = {}
    for f in os.listdir(a.old_gen):
        if f.endswith('_init.cpp'):
            for line in open(os.path.join(a.old_gen, f), encoding='utf-8'):
                m = INIT_ENTRY.match(line)
                if m:
                    names.setdefault(m.group(2), m.group(1))
    remap = {}  # (old function, old site) -> (function, site)
    for f in sorted(os.listdir(a.old_gen)):
        if not re.match(r'\w+_recomp\.\d+\.cpp$', f):
            continue
        lines = open(os.path.join(a.old_gen, f), encoding='utf-8').read().split('\n')
        function, ordinal = None, 0
        for i, line in enumerate(lines):
            m = ANY_FUNC.match(line)
            if m:
                s = SUB.match(m.group(1))
                function = s.group(1) if s else names.get(m.group(1))
                ordinal = 0
                continue
            if function is None or not LINE.match(line) or i == 0 or not lines[i - 1].startswith('\t// bctrl'):
                continue
            t = TAG.search(line)
            if t:
                remap[(t.group(1), int(t.group(2)))] = (function, ordinal)
            ordinal += 1
    out, moved, lost = [], 0, []
    for raw in open(a.list, encoding='utf-8').read().split('\n'):
        body, hash_, comment = raw.partition('#')
        parts = body.split()
        if len(parts) < 3:
            out.append(raw)
            continue
        key = (parts[0].upper(), int(parts[1]))
        new = remap.get(key)
        if new is None:
            lost.append(key)
            out.append(raw)
            continue
        if new != key:
            moved += 1
        rest = ' '.join(parts[2:])
        out.append('%s %d %s%s' % (new[0], new[1], rest, ('   #' + comment) if hash_ else ''))
    print('%d sites in the old tree; %d list entries moved to their own function, %d not found in the old tree%s' %
          (len(remap), moved, len(lost), (': first ' + str(lost[0])) if lost else ''))
    if a.write:
        open(a.list, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    return 1 if lost else 0


if __name__ == '__main__':
    sys.exit(main())
