#!/usr/bin/env python3
"""Regenerates the hand-written natives that are made from localized copies (app/src/cpu/hot/n_*.h written by the
make_*.py beside this file), after tools/post_codegen.py has regenerated the copies (app/generated/hot).

Each generator takes its function's localized copy, checks the statements of the loop(s) it replaces against the
text it was written for (it stops with an AssertionError when the copy changed: the fast loop must then be audited
again against the new copy), and writes the native: the copy with fast loops that apply under conditions they test
and fall back to the copy's loops (fastloop.py; their verify-mode self-checks are app/src/cpu/hot/fast_loop_check.h).

  python tools/hand_natives/run_all.py
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
failed = []
for script in sorted(glob.glob(os.path.join(HERE, 'make_*.py'))):
    r = subprocess.run([sys.executable, script], capture_output=True, text=True)
    name = os.path.basename(script)
    if r.returncode:
        failed.append(name)
        print(f'{name}: FAILED\n{r.stderr.strip()[-2000:]}')
    else:
        print(f'{name}: {r.stdout.strip().splitlines()[-1]}')
if failed:
    sys.exit(f'{len(failed)} generator(s) failed: {", ".join(failed)}')
