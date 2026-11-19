#!/usr/bin/env python3
"""katamari-nx (2026-10-30): a test toml: a package toml plus test keys (the scripted pad, captures, logs), each set
above the [input_p*] sections (tools/make_katamari_tomls.py's rule).

  python tools/make_test_toml.py --base dist/switch/katamari-nx/configs/renut_verify.toml --out out/emu_test.toml
         --set input_script="..." --set nfsmw_captura_cada_s=15 ...
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_katamari_tomls import apply  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--set', action='append', default=[], help='key=value (TOML value; strings quoted)')
    a = ap.parse_args()
    text = open(a.base, encoding='utf-8').read()
    keys = {}
    for kv in a.set:
        k, v = kv.split('=', 1)
        keys[k.strip()] = v.strip()
    text = apply(text, keys)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'{a.out}: {len(keys)} keys set')


if __name__ == '__main__':
    main()
