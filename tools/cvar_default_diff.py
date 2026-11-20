#!/usr/bin/env python3
"""banjo-nx (2026-11-19, the rebase): every cvar whose default differs between two trees (a git revision and the
working tree), from their REXCVAR_DEFINE_* lines in app/src and sdk/src, so a key one tree had on and the other off is
seen. Keys only in one tree are counted, not listed.

  python tools/cvar_default_diff.py --rev master [--filter PATTERN]
"""
import argparse
import re
import subprocess
import sys

DEF = re.compile(r'REXCVAR_DEFINE_(BOOL|INT32|INT64|UINT32|UINT64|DOUBLE|FLOAT|STRING)\(\s*(\w+)\s*,\s*((?:[^,"()]|"[^"]*"|\([^()]*\))+)\s*,')


def defaults_from_texts(texts):
    out = {}
    for path, text in texts:
        flat = re.sub(r'\s+', ' ', text)
        for m in DEF.finditer(flat):
            out.setdefault(m.group(2), (m.group(3).strip(), path))
    return out


def rev_texts(rev):
    files = subprocess.run(['git', 'ls-tree', '-r', '--name-only', rev, '--', 'app/src', 'sdk/src'],
                           capture_output=True, text=True).stdout.split()
    for f in files:
        if f.endswith(('.cpp', '.h', '.c', '.inc')):
            r = subprocess.run(['git', 'show', '%s:%s' % (rev, f)], capture_output=True)
            yield f, r.stdout.decode('utf-8', 'replace')


def tree_texts():
    files = subprocess.run(['git', 'ls-files', '--', 'app/src', 'sdk/src'], capture_output=True, text=True).stdout.split()
    for f in files:
        if f.endswith(('.cpp', '.h', '.c', '.inc')):
            try:
                yield f, open(f, encoding='utf-8', errors='replace').read()
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rev', default='master')
    ap.add_argument('--filter', default='')
    a = ap.parse_args()
    old = defaults_from_texts(rev_texts(a.rev))
    new = defaults_from_texts(tree_texts())
    both = sorted(set(old) & set(new))
    diff = [(k, old[k][0], new[k][0], new[k][1]) for k in both if old[k][0].replace(' ', '') != new[k][0].replace(' ', '')]
    print('%d keys in %s, %d in the tree, %d in both; %d with another default:' % (len(old), a.rev, len(new), len(both),
                                                                                  len(diff)))
    for k, o, n, path in diff:
        if a.filter and not re.search(a.filter, k):
            continue
        print('  %-48s %-28s -> %-28s %s' % (k, o[:28], n[:28], path))
    gone = sorted(set(old) - set(new))
    print('only in %s: %d%s' % (a.rev, len(gone), (': ' + ', '.join(gone[:40])) if gone else ''))


if __name__ == '__main__':
    sys.exit(main())
