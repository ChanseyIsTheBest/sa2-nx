"""A57 instructions per source line of the native Direct3D draw path and of the recompiled originals, without a
console: compiles this tree's files with the Switch compiler and the flags of a Switch build.ninja (read only; the
include paths are pointed at this tree), with line tables (-g1, which does not change the code), and counts the
AArch64 instructions each source line became (through the lines it was inlined from).

   python tools/d3d/d3d_arm_lines.py --ninja <a Switch build.ninja> --scratch <folder> native
        the native draw path (app/src/me/banjo_me_hooks.cpp): per function, instructions per line of
        app/src/me/banjo_d3d_native.h
   python tools/d3d/d3d_arm_lines.py --ninja ... --scratch ... original 826567C8 [8222C7A0 ...]
        the recompiled functions: instructions per PPC instruction block (between labels), in listing order

A count of instructions says what a path costs to decode and retire, not what it costs in cache misses; it is what
showed sa2-nx's and this port's ARM regressions before any console run (docs/d3d-native.md).
"""
import collections, glob, os, re, shlex, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BIN = r'C:\devkitPro\devkitA64\bin'
GXX = os.path.join(BIN, 'aarch64-none-elf-g++.exe')
OBJDUMP = os.path.join(BIN, 'aarch64-none-elf-objdump.exe')
GEN = os.path.join(ROOT, 'app', 'generated')
INSN = re.compile(r'^\s*([0-9a-f]+):\t(\S+)\s*(.*)$')
RECORD = re.compile(r'^(.+):(\d+)(?: \(discriminator \d+\))?$')
FUNC = re.compile(r'^([0-9a-f]+) <(.+)>:$')


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def ninja_flags(ninja, obj, tree):
    text = open(ninja, encoding='utf-8', errors='replace').read()
    m = re.search(r'^build [^\n]*' + re.escape(obj) + r':[^\n]*\n((?:  [^\n]*\n)+)', text, re.M)
    if not m:
        sys.exit('no rule for %s in %s' % (obj, ninja))
    vals = {}
    for line in m.group(1).splitlines():
        k, _, v = line.strip().partition(' = ')
        vals[k] = v
    # This tree's sources and SDK headers instead of the build folder's tree; its generated headers stay.
    their = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(ninja))))).replace('\\', '/')
    mine = ROOT.replace('\\', '/')
    out = []
    skip = False
    for f in shlex.split(vals.get('DEFINES', '') + ' ' + vals.get('INCLUDES', '') + ' ' + vals['FLAGS'], posix=True):
        if skip:
            skip = False
            continue
        if f == '-include':
            skip = True
            continue
        if f == '-Winvalid-pch':
            continue
        if f.startswith('-I') and '/out/' not in f:
            f = f.replace(their, mine)
        out.append(f)
    return out


def compile_one(flags, source, out, extra=()):
    if os.path.isfile(out) and os.path.getmtime(out) >= newest(source):
        return
    cmd = [GXX] + flags + ['-g1', '-w'] + list(extra) + ['-c', source, '-o', out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit('compiling %s failed:\n%s' % (source, r.stderr[-3000:]))


def newest(source):
    t = os.path.getmtime(source)
    if source.endswith('banjo_me_hooks.cpp'):
        for h in ('banjo_d3d_native.h', 'banjo_d3d_census.h'):
            t = max(t, os.path.getmtime(os.path.join(os.path.dirname(source), h)))
    return t


def disassemble(obj):
    """[(function, [(mnemonic, operands, file, line, [inlined-by lines])])]"""
    text = subprocess.run([OBJDUMP, '-d', '-C', '-l', '--inlines', '--no-show-raw-insn', obj], capture_output=True, text=True).stdout
    funcs = []
    cur = None
    where = (None, 0)
    chain = []
    for ln in text.splitlines():
        m = FUNC.match(ln)
        if m:
            cur = (m.group(2), [])
            funcs.append(cur)
            where = (None, 0)
            chain = []
            continue
        if cur is None:
            continue
        m = INSN.match(ln)
        if m:
            cur[1].append((m.group(2), m.group(3), where[0], where[1], list(chain)))
            continue
        s = ln.strip()
        if s.startswith('inlined by '):
            mm = re.match(r'inlined by (.+):(\d+)', s)
            if mm:
                chain.append((os.path.basename(mm.group(1)), int(mm.group(2))))
            continue
        m = RECORD.match(s)
        if m and not s.endswith('():'):
            where = (os.path.basename(m.group(1)), int(m.group(2)))
            chain = []
    return funcs


def native(flags, scratch):
    src = os.path.join(ROOT, 'app', 'src', 'me', 'banjo_me_hooks.cpp')
    out = os.path.join(scratch, 'banjo_me_hooks.o')
    compile_one(flags, src, out)
    want = arg('--function')
    for name, insns in disassemble(out):
        if 'banjo::d3d' not in name and 'sub_826567C8' not in name and 'sub_8222C7A0' not in name:
            continue
        if want and want not in name:
            continue
        per = collections.Counter()
        calls = collections.Counter()
        for mn, ops, f, line, chain in insns:
            # The line of banjo_d3d_native.h this instruction belongs to: its own, or the nearest caller's in that file.
            at = None
            if f == 'banjo_d3d_native.h':
                at = line
            for cf, cl in chain:
                if cf == 'banjo_d3d_native.h':
                    at = cl if at is None or '--outer' in sys.argv else at
            per[at if at is not None else (f, line)] += 1
            if mn in ('bl', 'blr'):
                calls[at if at is not None else (f, line)] += 1
        print('== %s: %d instructions' % (name, len(insns)))
        for k in sorted(per, key=lambda x: (isinstance(x, tuple), x)):
            print('   %-28s %4d%s' % (('line %d' % k) if not isinstance(k, tuple) else '%s:%s' % k, per[k],
                                       ('  (%d calls)' % calls[k]) if calls[k] else ''))


def original(flags, scratch, addresses):
    pch = ['-include', os.path.join(GEN, 'renut_pch.h')]
    for a in addresses:
        a = a.upper()
        src = None
        for f in glob.glob(GEN + '/renut_recomp.*.cpp'):
            if ('DEFINE_REX_FUNC(sub_%s)' % a) in open(f, encoding='utf-8', errors='ignore').read():
                src = f
                break
        if not src:
            print('== sub_%s: not found' % a)
            continue
        out = os.path.join(scratch, os.path.basename(src) + '.o')
        compile_one(flags, src, out, pch)
        # The function's lines, split into blocks at its labels.
        lines = open(src, encoding='utf-8', errors='ignore').read().split('\n')
        start = next(i for i, l in enumerate(lines) if l.startswith('DEFINE_REX_FUNC(sub_%s)' % a)) + 1
        end = next(i for i in range(start, len(lines)) if lines[i].startswith('}')) + 1
        block_of = {}
        label = 'entry'
        order = ['entry']
        ppc = collections.Counter()
        for i in range(start, end + 1):
            m = re.match(r'^loc_([0-9A-F]{8}):', lines[i - 1].strip())
            if m:
                label = m.group(1)
                order.append(label)
            block_of[i] = label
            if re.match(r'^\s*// [a-z]', lines[i - 1]):
                ppc[label] += 1
        base = os.path.basename(src)
        for name, insns in disassemble(out):
            if name not in ('__imp__sub_%s' % a, 'sub_%s' % a):
                continue
            per = collections.Counter()
            calls = collections.Counter()
            other = 0
            for mn, ops, f, line, chain in insns:
                at = None
                if f == base and line in block_of:
                    at = block_of[line]
                for cf, cl in chain:
                    if cf == base and cl in block_of:
                        at = block_of[cl]
                if at is None:
                    other += 1
                    continue
                per[at] += 1
                if mn in ('bl', 'blr', 'b') and ('sub_' in ops or 'rex_' in ops):
                    calls[at] += 1
            print('== %s: %d instructions (%d not from the function\'s own lines), %d PPC instructions' % (name, len(insns), other, sum(ppc.values())))
            for lb in order:
                print('   %-10s PPC %3d  A57 %4d%s' % (lb, ppc[lb], per[lb], ('  (%d calls)' % calls[lb]) if calls[lb] else ''))


def main():
    ninja = arg('--ninja')
    scratch = arg('--scratch')
    if not ninja or not scratch:
        sys.exit(__doc__)
    os.makedirs(scratch, exist_ok=True)
    rest = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and sys.argv[i - 1] not in ('--ninja', '--scratch', '--function')]
    if rest and rest[0] == 'native':
        native(ninja_flags(ninja, 'banjo_me_hooks.cpp.obj', ROOT), scratch)
    elif rest and rest[0] == 'original':
        original(ninja_flags(ninja, 'renut_recomp.0.cpp.obj', ROOT), scratch, rest[1:])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
