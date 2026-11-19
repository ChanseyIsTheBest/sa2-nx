"""The shipped GLSL against an earlier package's, shader by shader, with each shader's first logged variant's
constants (911 = 0 and 1 where the shader has it). python verify_final.py <old glsl dir> <new glsl dir> <hashes> [trials]"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VARIANTS = 'D:/New folder/ryu-data/banjo/sdcard/switch/banjo-nx/deko3d_variants.txt'
old_dir, new_dir, hashes = sys.argv[1], sys.argv[2], sys.argv[3].split(',')
trials = sys.argv[4] if len(sys.argv) > 4 else '300'
spec = {}
for ln in open(VARIANTS, encoding='utf-8', errors='replace'):
    m = re.match(r'vs \w+ ps (\w+) (.*?) \|', ln)
    if m and m.group(1) in hashes and m.group(1) not in spec:
        s = [x for x in m.group(2).split() if re.fullmatch(r'\d+=\d+', x)]
        if any(x.startswith('903=') for x in s):
            spec[m.group(1)] = s
bad = 0
for h in hashes:
    new = os.path.join(new_dir, f'ps_{h}.glsl')
    extra = [[]]
    if 'SPIRV_CROSS_CONSTANT_ID_911' in open(new, encoding='utf-8').read():
        extra = [['911=0'], ['911=1']]
    for e in extra:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'difftest_game.py'), os.path.join(old_dir, f'ps_{h}.glsl'),
                            new, trials] + spec.get(h, ['901=65520']) + e, capture_output=True, text=True)
        last = (r.stdout + r.stderr).strip().split('\n')[-1]
        print(h, ' '.join(e), last[-90:])
        bad += 'MISMATCH' in last or 'Error' in last
print('bad', bad)
