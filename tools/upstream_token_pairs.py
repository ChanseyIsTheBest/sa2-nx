#!/usr/bin/env python3
"""banjo-nx (2026-10-29, the title update): the hex constants reNut upstream changed in its title-update commit, as
old -> new pairs, from `git diff` of the commit: each hunk's removed and added lines are paired in order, and two
lines that differ only in their hex constants give their constants as pairs. Used to check tools/remap_addresses.py
and the device-offset shift against upstream's hand edits.

  git -C reNut diff 99cb2f4 ab60251 -- src | python tools/upstream_token_pairs.py > pairs.tsv
"""
import re
import sys

HEX = re.compile(r'(?<![0-9A-Za-z_])(?:0x|sub_|loc_|dword_|byte_|word_|unk_|off_)?([0-9A-Fa-f]{4,8})(?![0-9A-Za-z])')
TOK = re.compile(r'(?<![0-9A-Za-z_])((?:0x|sub_|loc_|dword_|byte_|word_|unk_|off_)?)([0-9A-Fa-f]{3,8})(?![0-9A-Za-z])')


def skeleton(line):
    return TOK.sub(lambda m: m.group(1) + '#', line)


def main():
    pairs = {}
    path = None
    minus, plus = [], []

    def flush():
        for a, b in zip(minus, plus):
            if skeleton(a) != skeleton(b):
                continue
            ta = [m.group(1) + m.group(2) for m in TOK.finditer(a)]
            tb = [m.group(1) + m.group(2) for m in TOK.finditer(b)]
            for x, y in zip(ta, tb):
                if x.lower() != y.lower():
                    pairs.setdefault((x.lower(), y.lower()), set()).add(path)
        minus.clear()
        plus.clear()

    for line in sys.stdin:
        line = line.rstrip('\n')
        if line.startswith('diff --git'):
            flush()
            path = line.split(' b/')[-1]
        elif line.startswith('---') or line.startswith('+++'):
            continue
        elif line.startswith('-'):
            if plus:
                flush()
            minus.append(line[1:])
        elif line.startswith('+'):
            plus.append(line[1:])
        else:
            flush()
    flush()
    for (x, y), files in sorted(pairs.items()):
        print('%s\t%s\t%s' % (x, y, ','.join(sorted(f.split('/')[-1] for f in files))))


if __name__ == '__main__':
    main()
