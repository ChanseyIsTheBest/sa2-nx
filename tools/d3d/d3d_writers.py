"""Static scan of the generated code for Direct3D's command-buffer (PM4) writers: the functions that call one of
Direct3D's two "space" functions, or load the device's write pointer (+48), store words after it with `stwu rX,4(rY)`
and store the pointer back. Banjo's Direct3D is not one address range (the game was linked with profile-guided
function order: the hot Direct3D functions sit between hot game functions at 0x8220-0x8229, the rest at
0x8264D-0x82658), so the census build (tools/d3d/d3d_census_build.py) hooks this list instead of a range.

   python tools/d3d/d3d_writers.py [--callers]     prints one line per function: address, instructions, why
   --callers: also the direct callers of every writer (one level), to see the entry points.
"""
import glob, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
GEN = os.path.join(ROOT, 'app', 'generated')
# Direct3D's space functions: the kick / next segment (soft limit, +56) and the "more space" of the flush writers
# (hard limit, +52), and the inline-data allocator.
SPACE = {'82210ab0': 'kick', '821e2940': 'more space', '8222d240': 'inline data'}
DEF = re.compile(r'^DEFINE_REX_FUNC\((?:__imp__)?(\w+)\) \{$')
BL = re.compile(r'^\s*// bl 0x([0-9a-f]{8})$')
LOAD48 = re.compile(r'^\s*// lwz r(\d+),48\(r(\d+)\)$')
STORE48 = re.compile(r'^\s*// stw r(\d+),48\(r(\d+)\)$')
STWU4 = re.compile(r'^\s*// stwu r(\d+),4\(r(\d+)\)$')
INSN = re.compile(r'^\s*// [a-z]')


def scan():
    funcs = {}
    for f in sorted(glob.glob(GEN + '/renut_recomp.*.cpp')):
        cur = None
        for ln in open(f, encoding='utf-8', errors='ignore'):
            m = DEF.match(ln)
            if m:
                cur = {'name': m.group(1), 'calls': [], 'n': 0, 'load48': 0, 'store48': 0, 'stwu': 0}
                funcs[m.group(1)] = cur
                continue
            if cur is None:
                continue
            if ln.startswith('}'):
                cur = None
                continue
            if INSN.match(ln):
                cur['n'] += 1
            m = BL.match(ln)
            if m:
                cur['calls'].append(m.group(1))
                continue
            if LOAD48.match(ln):
                cur['load48'] += 1
            elif STORE48.match(ln):
                cur['store48'] += 1
            elif STWU4.match(ln):
                cur['stwu'] += 1
    return funcs


def writers(funcs):
    out = {}
    for name, f in funcs.items():
        why = []
        for c in f['calls']:
            if c in SPACE and SPACE[c] not in why:
                why.append(SPACE[c])
        if f['load48'] and f['store48'] and f['stwu']:
            why.append('writes after +48 (%d words in the listing)' % f['stwu'])
        if why:
            out[name] = why
    return out


def main():
    funcs = scan()
    w = writers(funcs)
    by_addr = {}
    for name in funcs:
        m = re.match(r'sub_([0-9A-F]{8})$', name)
        if m:
            by_addr[m.group(1).lower()] = name
    print('%d functions write the command buffer (of %d)' % (len(w), len(funcs)))
    for name in sorted(w):
        print('%s  %4d instructions  %s' % (name, funcs[name]['n'], '; '.join(w[name])))
    if '--callers' in sys.argv:
        print()
        targets = {n[4:].lower() for n in w if n.startswith('sub_')}
        callers = {}
        for name, f in funcs.items():
            for c in f['calls']:
                if c in targets and name not in w:
                    callers.setdefault(name, set()).add(by_addr.get(c, c))
        print('%d functions call a writer without being one' % len(callers))
        for name in sorted(callers):
            print('%s -> %s' % (name, ' '.join(sorted(callers[name]))))


if __name__ == '__main__':
    main()
