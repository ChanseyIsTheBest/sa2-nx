#!/usr/bin/env python3
"""sa2-nx-ult (2026-11-19): the post-codegen steps of a game's second code module (a guest DLL of the manifest's
[[modules]]: the generator writes it into a folder of its own under app/generated, here generated/data_dll, SA2's
stage DLL). tools/post_codegen.py runs the base's whole sequence on the main module's flat tree, then this on every
module folder it finds (a subfolder of app/generated with a <project>_pch.h).

Which steps, and why (sa2-nx 1.0.6's tools/post_codegen.py ran the same on its stage DLL):
  - the ones each file needs to be right or that the generated pch's macros assume: pch_no_volatile.py WITH
    loop_barriers.py (without barriers GCC turns guest polling loops into infinite loops), pch_cold_traps.py,
    direct_calls.py --redo, guest_cas.py, guest_fences.py, flush_mode_flow.py (without the image: its entry modes are
    the main module's), vec_byteswap.py, pch_neon_shuffles.py, float_loads.py, frame_traffic.py (marks only),
    loop_locals.py, leaf_locals.py and store_cards.py last;
  - NOT the whole-program analyses (leaf_dead_stores.py, dead_ctx_defs.py, reg_liveness.py, single_floats.py,
    ro_constants*.py, lr / r1 proofs, inline_leaves.py) nor the machine copies: they read one module's call graph and
    one image, and a module's functions are reached from the other through pointers only (sa2-nx's audit: the stage
    DLL's data holds no pointer into default.xex's code, and none of the game's indirect calls leaves the main image).

Each step is idempotent, as in post_codegen.py; store_cards.py's marks are taken out first so a second run sees the
text the first one did.

  python tools/post_codegen_modules.py [--gen <generated folder>]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, 'app')
GEN = os.path.join(APP, 'generated')
HOOK_TOMLS = [APP, os.path.join(APP, 'config', 'renut_hooks.toml'), os.path.join(APP, 'config', 'renut_gpu_funcs.toml'),
              os.path.join(APP, 'config', 'renut_crt.toml')]


def run(*args):
    result = subprocess.run([sys.executable, *args], cwd=ROOT)
    if result.returncode != 0:
        sys.exit(f'post_codegen_modules: {os.path.basename(args[0])} failed ({result.returncode})')


def tool(name):
    return os.path.join(ROOT, 'tools', name)


def module_folders(gen):
    out = []
    for name in sorted(os.listdir(gen)):
        folder = os.path.join(gen, name)
        if os.path.isdir(folder) and name != 'hot' and any(re.fullmatch(r'\w+_pch\.h', f) for f in os.listdir(folder)):
            out.append(folder)
    return out


def pch_of(folder):
    for f in sorted(os.listdir(folder)):
        if re.fullmatch(r'\w+_pch\.h', f):
            return os.path.join(folder, f)
    sys.exit(f'post_codegen_modules: no *_pch.h in {folder}')


def main():
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    for folder in module_folders(gen):
        pch = pch_of(folder)
        tomls = [p for p in HOOK_TOMLS if os.path.exists(p)]
        run(tool('store_cards.py'), '--gen', folder, '--restore')
        run(tool('pch_no_volatile.py'), pch)
        run(tool('loop_barriers.py'), folder)
        run(tool('pch_cold_traps.py'), pch)
        run(tool('direct_calls.py'), '--redo', '--gen', folder)
        run(tool('guest_cas.py'), '--gen', folder)
        run(tool('guest_fences.py'), '--gen', folder)
        run(tool('flush_mode_flow.py'), '--redo', '--gen', folder, '--tomls', *tomls)
        run(tool('vec_byteswap.py'), '--gen', folder)
        run(tool('pch_neon_shuffles.py'), pch)
        run(tool('float_loads.py'), '--gen', folder)
        run(tool('frame_traffic.py'), '--gen', folder)
        run(tool('loop_locals.py'), '--gen', folder)
        run(tool('leaf_locals.py'), '--gen', folder)
        run(tool('store_cards.py'), '--gen', folder)
        print(f'post_codegen_modules: {os.path.relpath(folder, ROOT)} done')


if __name__ == '__main__':
    main()
