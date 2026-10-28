#!/usr/bin/env python3
"""Ranks the game's functions by what a console PGO session executed (pgo/*.gcda), without a profiler: for each
recompiled function, the sum of its statements' execution counts (the generated code has one statement per guest
instruction, so this is its guest instructions executed) and its call count. sa2-nx, 2026-10-28: the SDK's sampling
profiler does not run under Ryujinx (Banjo's finding too), and the console session of the release's PGO profile is the
newest record of the game on the console.

The .gcda files say how often each instrumented arc ran; the names, lines and control flow are in the .gcno notes the
instrumented compile wrote (not kept). This compiles each sa2_recomp.N.cpp again as the generate build did (the
release tree's flags with -fprofile-use replaced by the generate flags, plus -ftest-coverage) into out/pgo_counts/,
puts the session's .gcda beside each, and reads gcov's JSON. Functions whose code changed since the session would not
match (gcov skips them); the release's generated code is the session's.

  python tools/pgo_function_counts.py [--jobs 8] [--build out/nx-renut-pgo] [--top 300] [--only N ...]
Writes out/pgo_counts/functions.tsv (address, guest instructions executed, calls, guest instructions of the function)
sorted by the first, and prints the top.
"""
import argparse
import concurrent.futures
import glob
import gzip
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
USE_FLAGS = re.compile(r'^-fprofile-(?:use|prefix-path|correction|partial-training)\b|^-Wno-(?:missing-profile|coverage-mismatch)$')


def ninja_vars(build, n):
    text = open(os.path.join(build, 'build.ninja'), encoding='utf-8').read()
    m = re.search(r'^build CMakeFiles/sa2_recomp\.dir/generated/default/sa2_recomp\.%s\.cpp\.obj: .*?\n((?:  .*\n)+)' % n,
                  text, re.M)
    if not m:
        return None
    v = {}
    for line in m.group(1).splitlines():
        k, _, val = line.strip().partition(' = ')
        v[k] = val
    return v


def compile_one(build, out, n):
    v = ninja_vars(build, n)
    if v is None:
        return n, 'not in build.ninja'
    # The source path spelled as build.ninja spells it (forward slashes): it is part of every function's checksum.
    src = os.path.join(ROOT, 'app', 'generated', 'default', f'sa2_recomp.{n}.cpp').replace(chr(92), '/')
    flags =[f for f in shlex.split(v.get('FLAGS', '').replace(chr(92), '/')) if not USE_FLAGS.match(f)]
    flags += ['-fprofile-generate', '-fno-profile-values', '-fprofile-update=single', '-ftest-coverage',
              '-fprofile-info-section=gcov_info']
    obj = os.path.join(out, f'sa2_recomp.{n}.o')
    cmd = ([DEVKITA64 + 'aarch64-none-elf-g++' + EXE] + shlex.split(v.get('DEFINES', '')) +
           shlex.split(v.get('INCLUDES', '').replace(chr(92), '/')) + flags + ['-w', '-c', src, '-o', obj])
    r = subprocess.run(cmd, cwd=build, capture_output=True, text=True)
    if r.returncode != 0:
        return n, 'compile failed: ' + (r.stderr or r.stdout)[-600:]
    gcda = os.path.join(ROOT, 'pgo', f'CMakeFiles#sa2_recomp.dir#generated#default#sa2_recomp.{n}.cpp.gcda')
    if not os.path.exists(gcda):
        return n, 'no .gcda'
    # gcov takes counts only with the notes' stamp (the instrumented compile's own); every function is still matched
    # by its line and control-flow checksums, so the stamp of this compile is put into the session's counts.
    gcno = open(os.path.join(out, f'sa2_recomp.{n}.gcno'), 'rb').read()
    data = bytearray(open(gcda, 'rb').read())
    data[8:12] = gcno[8:12]
    open(os.path.join(out, f'sa2_recomp.{n}.gcda'), 'wb').write(bytes(data))
    r = subprocess.run([DEVKITA64 + 'aarch64-none-elf-gcov' + EXE, '-j', '-o', out, src], cwd=out,
                       capture_output=True, text=True)
    js = os.path.join(out, f'sa2_recomp.{n}.gcov.json.gz')
    if not os.path.exists(js):
        return n, 'gcov wrote nothing: ' + (r.stderr or r.stdout)[-400:]
    return n, None


def functions_of(out, n):
    js = os.path.join(out, f'sa2_recomp.{n}.gcov.json.gz')
    data = json.load(gzip.open(js, 'rt', encoding='utf-8'))
    rows = []
    for f in data['files']:
        if not f['file'].replace('\\', '/').endswith(f'sa2_recomp.{n}.cpp'):
            continue
        counts = {l['line_number']: l['count'] for l in f['lines']}
        for fn in f['functions']:
            name = fn['name']
            m = re.fullmatch(r'_?Z?\w*?(sub_[0-9A-F]{8})\w*', name) or re.search(r'(sub_[0-9A-F]{8})', name)
            if not m:
                continue
            lines = [counts[i] for i in range(fn['start_line'], fn['end_line'] + 1) if i in counts]
            rows.append((m.group(1)[4:], sum(lines), fn['execution_count'], len(lines)))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', default=os.path.join(ROOT, 'out', 'nx-renut-pgo'))
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--reuse', action='store_true', help='read the JSON already in out/pgo_counts')
    a = ap.parse_args()
    out = os.path.join(ROOT, 'out', 'pgo_counts')
    os.makedirs(out, exist_ok=True)
    files = sorted(glob.glob(os.path.join(ROOT, 'app', 'generated', 'default', 'sa2_recomp.*.cpp')))
    ns = a.only or [re.search(r'sa2_recomp\.(\d+)\.cpp$', f).group(1) for f in files]
    if not a.reuse:
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            for n, err in ex.map(lambda n: compile_one(a.build, out, n), ns):
                if err:
                    print(f'sa2_recomp.{n}: {err}')
    rows = []
    for n in ns:
        if os.path.exists(os.path.join(out, f'sa2_recomp.{n}.gcov.json.gz')):
            rows += functions_of(out, n)
    rows.sort(key=lambda r: -r[1])
    total = sum(r[1] for r in rows)
    with open(os.path.join(out, 'functions.tsv'), 'w', encoding='utf-8') as f:
        f.write('address\tstatements_executed\tcalls\tstatements\n')
        for r in rows:
            f.write('%s\t%d\t%d\t%d\n' % r)
    print(f'{len(rows)} functions, {total:,} statements executed in all')
    for r in rows[:a.top]:
        print(f'  sub_{r[0]}  {100.0 * r[1] / max(total, 1):5.2f} %  {r[1]:>14,}  calls {r[2]:>12,}  ({r[3]} statements)')


if __name__ == '__main__':
    main()
