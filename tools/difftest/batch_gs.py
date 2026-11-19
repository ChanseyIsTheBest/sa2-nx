"""guardsel (the tool's current version) on the heavy pixel shaders, each with up to two of its logged variants'
constants, differential-tested against the package 36 text."""
import os
import re
import subprocess
import sys

sys.path.insert(0, 'D:/Banjo kazooie port/banjo-nx/tools')
import deko3d_glsl_opt as o  # noqa: E402

BASE = 'D:/Banjo kazooie port/banjo-nx/out/deko3d_shaders/glsl_opt_36'
VARIANTS = 'D:/New folder/ryu-data/banjo/sdcard/switch/banjo-nx/deko3d_variants.txt'
SHADERS = sys.argv[1].split(',')
TRIALS = sys.argv[2] if len(sys.argv) > 2 else '300'
specs = {}
for ln in open(VARIANTS, encoding='utf-8', errors='replace'):
    m = re.match(r'vs \w+ ps (\w+) (.*?) \|', ln)
    if m and m.group(1) in SHADERS:
        s = ' '.join(x for x in m.group(2).split() if re.fullmatch(r'\d+=\d+', x))
        lst = specs.setdefault(m.group(1), [])
        if s not in lst and len(lst) < 2:
            lst.append(s)
bad = 0
for sh in SHADERS:
    src = open(f'{BASE}/ps_{sh}.glsl', encoding='utf-8').read()
    new = o.pass_guardsel(src)
    path = os.path.abspath(f'gs_{sh}.glsl')
    open(path, 'w', encoding='utf-8', newline='\n').write(new)
    removed = src.count('dk_sel(') - new.count('dk_sel(') + src.count('mix(') - new.count('mix(')
    for s in specs.get(sh, ['901=65520']):
        r = subprocess.run([sys.executable, 'difftest_game.py', f'{BASE}/ps_{sh}.glsl', path, TRIALS] + s.split(),
                           capture_output=True, text=True)
        out = (r.stdout + r.stderr).strip().split('\n')
        print(f'{sh} ({removed} selects) [{s[:60]}]: {out[-1][:160]}')
        bad += 'MISMATCH' in out[-1]
print('bad', bad)
