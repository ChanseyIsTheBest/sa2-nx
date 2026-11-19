#!/usr/bin/env python3
"""Take the ultimate base's ports into a game's port one base commit at a time (sasrt-nx's tool of 2026-11-13, made
general for the base: sasrt-nx took ports 45-129 with it, 130 files, 136 conflict blocks resolved by hand).

For each port commit C, for each file it changes: a three-way merge (git merge-file) of the port's file (ours) with the
base's file before C (the ancestor) and after C (theirs), so exactly C's change comes in and everything the port did
since stays. Files C adds and the port lacks are copied; files the port has under another name, files C deletes and the
per-XDK files (--hand) are listed for a hand port; the docs, Katamari's own files and dist/ are skipped. Line ends: compared
without CR, written back in the file's own style. Leave out of the list the base ports that are copies of the port's own
work (the base's form of them came from it).

    python tools/ult_port_sync.py --base <BASE_DIR> [--port <PORT_DIR>] PORTS.json [--dry-run] [--hand PREFIX ...]
    python tools/ult_port_sync.py --base <BASE_DIR> [--port <PORT_DIR>] --range A..B [--dry-run] [--skip N ...]

PORTS.json: [[port number, commit], ...] in the order to apply. --range: the base's "Port N: ..." commits in A..B, oldest
first (its "docs: port N" commits are docs/ only and skipped anyway); --skip leaves out port numbers. <PORT_DIR> is the
current directory by default; the base is only read. Writes <PORT_DIR>/out/ult_port_sync.txt (what happened to each file).
A file with conflicts gets diff3 markers (<<<<<<< ours / ||||||| base before / ======= / >>>>>>> base port N) in place:
resolve them by hand, then build and test.
"""
import argparse
import json
import os
import re
import subprocess
import tempfile

SKIP_PREFIX = ('docs/', 'dist/', 'pgo/', 'app/katamari', 'app/src/katamari', 'README.md', 'THIRD_PARTY_NOTICES.md',
               'LICENSE', 'app/renut_manifest.toml')
# Files a port at another XDK keeps in its own form (docs/xdk-notes.md): their fixes are carried by hand.
DEFAULT_HAND = ('app/src/renut_engine/gpu/d3d/hooks/', 'app/src/renut_engine/gpu/d3d/d3d.h',
                'app/src/renut_engine/gpu/d3d/d3d_native.h', 'app/src/renut_engine/gpu/d3d/d3d_native_state.cpp',
                'app/src/renut_engine/gpu/banjo_shared_hooks.h')


def git(base, *args, text=False):
    return subprocess.run(['git', '-C', base] + list(args), capture_output=True, text=text)


def git_show(base, rev, path):
    r = git(base, 'show', '%s:%s' % (rev, path))
    return r.stdout if r.returncode == 0 else None


def name_status(base, c):
    rows = []
    for line in git(base, 'show', '--name-status', '--format=', c, text=True).stdout.splitlines():
        p = line.split('\t')
        if p[0].startswith('R'):
            rows.append(('R', p[2], p[1]))
        elif p[0]:
            rows.append((p[0][0], p[1], None))
    return rows


def ports_in_range(base, rng, skip):
    out = git(base, 'log', '--reverse', '--format=%H %s', rng, text=True).stdout
    ports = []
    for line in out.splitlines():
        h, _, subject = line.partition(' ')
        m = re.match(r'Port (\d+)\b', subject)
        if m and int(m.group(1)) not in skip:
            ports.append([int(m.group(1)), h])
    return ports


def lf(b):
    return b.replace(b'\r\n', b'\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('ports_json', nargs='?')
    ap.add_argument('--base', required=True, help='the ultimate base checkout (read only)')
    ap.add_argument('--port', default=os.getcwd(), help="the game's port checkout (default: the current directory)")
    ap.add_argument('--range', help='base commits A..B: their "Port N:" commits, oldest first')
    ap.add_argument('--skip', type=int, nargs='*', default=[], help='port numbers to leave out (with --range)')
    ap.add_argument('--hand', nargs='*', help='path prefixes the port keeps in its own form (default: the per-XDK files)')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    if bool(a.ports_json) == bool(a.range):
        ap.error('give one of PORTS.json and --range')
    ports = json.load(open(a.ports_json)) if a.ports_json else ports_in_range(a.base, a.range, set(a.skip))
    hand = tuple(a.hand) if a.hand is not None else DEFAULT_HAND
    log = []
    totals = {'clean': 0, 'conflict': 0, 'copied': 0, 'same': 0, 'hand': 0, 'skipped': 0}
    for n, c in ports:
        for st, path, old in name_status(a.base, c):
            tag = 'port %d %s' % (n, c[:9])
            if path.startswith(SKIP_PREFIX):
                log.append('%s skip (docs / Katamari / dist) %s' % (tag, path))
                totals['skipped'] += 1
                continue
            if hand and path.startswith(hand):
                log.append('%s HAND (the port keeps its own form) %s' % (tag, path))
                totals['hand'] += 1
                continue
            if st == 'D':
                log.append('%s HAND (deleted in the base) %s' % (tag, path))
                totals['hand'] += 1
                continue
            mine = os.path.join(a.port, path)
            after = git_show(a.base, c, path)
            before = git_show(a.base, c + '^', old or path) if st != 'A' else None
            if not os.path.exists(mine):
                if st == 'A':
                    if not a.dry_run:
                        os.makedirs(os.path.dirname(mine), exist_ok=True)
                        open(mine, 'wb').write(after)
                    log.append('%s copied (new) %s' % (tag, path))
                    totals['copied'] += 1
                else:
                    log.append('%s HAND (not in the port under this path) %s' % (tag, path))
                    totals['hand'] += 1
                continue
            ours_raw = open(mine, 'rb').read()
            crlf = b'\r\n' in ours_raw
            ours, base, theirs = lf(ours_raw), lf(before or b''), lf(after)
            if ours == theirs:
                log.append('%s same already %s' % (tag, path))
                totals['same'] += 1
                continue
            with tempfile.TemporaryDirectory() as d:
                po, pb, pt = (os.path.join(d, x) for x in ('ours', 'base', 'theirs'))
                open(po, 'wb').write(ours)
                open(pb, 'wb').write(base)
                open(pt, 'wb').write(theirs)
                r = subprocess.run(['git', 'merge-file', '-p', '--diff3', '-L', 'ours', '-L', 'base before',
                                    '-L', 'base port %d' % n, po, pb, pt], capture_output=True)
                merged, conflicts = r.stdout, r.returncode
            if conflicts < 0:
                log.append('%s HAND (merge-file failed) %s' % (tag, path))
                totals['hand'] += 1
                continue
            if not a.dry_run:
                open(mine, 'wb').write(merged.replace(b'\n', b'\r\n') if crlf else merged)
            if conflicts:
                log.append('%s CONFLICT x%d %s' % (tag, conflicts, path))
                totals['conflict'] += 1
            else:
                log.append('%s clean %s' % (tag, path))
                totals['clean'] += 1
    os.makedirs(os.path.join(a.port, 'out'), exist_ok=True)
    out = os.path.join(a.port, 'out', 'ult_port_sync.txt')
    open(out, 'w', encoding='utf-8').write('\n'.join(log) + '\n')
    print('\n'.join(l for l in log if 'CONFLICT' in l or 'HAND' in l))
    print(totals, '->', out)


if __name__ == '__main__':
    main()
