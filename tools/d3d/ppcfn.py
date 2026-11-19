"""Print the PPC listing of recompiled functions (from the generated code's comments), compactly.
   python tools/d3d/ppcfn.py 826567C8 [8222BF10 ...] [--max=N] [--out=FILE]
From sa2-nx's tools/d3d/ppcfn.py (GPL-3.0), with Banjo's generated file names."""
import glob, os, re, sys
GEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'app', 'generated')
want = [a.upper().replace('0X', '') for a in sys.argv[1:] if not a.startswith('--')]
mx = 4000
out = None
for a in sys.argv[1:]:
    if a.startswith('--max='): mx = int(a.split('=')[1])
    if a.startswith('--out='): out = a.split('=', 1)[1]
DEF = re.compile(r'^DEFINE_REX_FUNC\((?:__imp__)?sub_([0-9A-F]{8})\) \{$')
found = {}
for f in sorted(glob.glob(GEN + '/renut_recomp.*.cpp')):
    cur = None
    for ln in open(f, encoding='utf-8', errors='ignore'):
        m = DEF.match(ln)
        if m:
            cur = m.group(1) if m.group(1) in want else None
            if cur: found[cur] = []
            continue
        if cur is None: continue
        if ln.startswith('}'):
            cur = None; continue
        s = ln.strip()
        if s.startswith('// '):
            found[cur].append('    ' + s[3:])
        elif s.startswith('loc_'):
            found[cur].append(s)
res = []
for a in want:
    lines = found.get(a)
    if lines is None:
        res.append(f'== sub_{a}: not found'); continue
    n = sum(1 for l in lines if l.startswith('    '))
    res.append(f'== sub_{a} ({n} instructions)')
    res.extend(lines[:mx])
    if len(lines) > mx: res.append(f'    ... {len(lines) - mx} more')
text = '\n'.join(res) + '\n'
if out:
    open(out, 'w', newline='\n').write(text)
    print(f'{len(res)} lines -> {out}')
else:
    sys.stdout.write(text)
