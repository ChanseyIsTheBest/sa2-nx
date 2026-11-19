#!/usr/bin/env python3
"""renut-ultimate (2026-11-16, port 147; tw2de_nx's tools/ppc_tests of 2026-11-01): the PowerPC instruction tests as a Switch homebrew (renut_ppc_tests.nro).

Xenia's instruction tests (src/xenia/cpu/ppc/testing of xenia-canary, BSD license: inputs, a few instructions, the
results the Xbox 360 gives; the instr__gen_* files were recorded on the console) go through this SDK's code
generator with the project's code generator options and the Switch build's compiler and flags, and run on the
console. Every failure is an instruction the game's code computes differently from the 360.

    python tools/ppc_tests/build_ppc_tests.py [--xenia DIR] [--out DIR] [--jobs N]

Needs: git (to fetch the tests, unless --xenia names a checkout's src/xenia/cpu/ppc/testing), a C compiler for the
small assembler (clang, gcc or cc), the code generator (sdk/out/win-amd64/rexglue, as tools\build_pc.bat builds it), CMake,
Ninja and devkitA64 (DEVKITPRO). The tests: xenia-canary at a fixed commit, with xenia_tests_pr1342.patch applied
(the corrected expectations and the new tests of xenia-canary pull request #1342, AdrianCassar, from the same
project and license: XENIA_LICENSE). Writes out/ppc_tests/renut_ppc_tests.nro: copy it to the SD card (/switch/) and start it
from the homebrew menu. Its results: on screen, and the failures in renut_ppc_tests.txt next to the NRO.

The code is generated with exact_fp on (the 360's NaN results: the game's build leaves it off) so that every test
can pass; known.txt lists the cases whose expectation is not the hardware's.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SDK = os.path.join(ROOT, 'sdk')
EXE = '.exe' if os.name == 'nt' else ''
XENIA_URL = 'https://github.com/xenia-canary/xenia-canary.git'
XENIA_COMMIT = '96ce1ac47ee7646deac2a2c9c912351e8ff0edee'
TESTING = 'src/xenia/cpu/ppc/testing'
OWN = os.path.join(HERE, 'own')  # this project's own test files (tw2de_nx, 2026-11-02)
# Not tests (helpers of Xenia's own runner) or forms the small assembler does not take (@h / @l, local labels).
SKIP_FILES = {'ppc_testing_native_thunks', 'seq_jumptable_constants'}
CHUNK = 4000

# The cases whose expectation is not what the hardware gives, with the reason (the runner counts them apart).
KNOWN = {
    'fnmsubx.test_3': 'expects +0 for -(0.5 * 0.5 - 0.25); the hardware-recorded fnmsub cases give -0 for a zero '
                      'difference (instr__gen_fnmsub: 80 cases), as IEEE negation does',
    'fnmsubsx.test_3': 'as fnmsubx.test_3',
    '_gen_fctiwz.test_fctiwz_12_cr_GEN': 'a recording anomaly: the same input without the record form '
                                         '(test_fctiwz_12_GEN) and fctiw. of it give 0xFFFFFFFF80000000 and CR1 '
                                         'FX VX; xenia-canary pull request #1342 skips it too',
}


def run(cmd, **kw):
    print('  ' + ' '.join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def fetch_tests(out):
    dest = os.path.join(out, 'xenia')
    testing = os.path.join(dest, *TESTING.split('/'))
    if os.path.isdir(testing) and os.path.isfile(os.path.join(out, '.tests_ready')):
        return testing
    if os.path.isdir(dest):
        shutil.rmtree(dest)
    run(['git', 'clone', '--filter=blob:none', '--no-checkout', XENIA_URL, dest])
    run(['git', '-C', dest, 'sparse-checkout', 'set', '--no-cone', TESTING])
    run(['git', '-C', dest, 'checkout', XENIA_COMMIT])
    run(['git', '-C', dest, 'apply', '--whitespace=nowarn', os.path.join(HERE, 'xenia_tests_pr1342.patch')])
    open(os.path.join(out, '.tests_ready'), 'w').close()
    return testing


def find_cc():
    for cc in ('clang', 'gcc', 'cc'):
        if shutil.which(cc):
            return cc
    sys.exit('no C compiler (clang, gcc or cc) for the assembler')


def build_assembler(out):
    exe = os.path.join(out, 'miniasm' + EXE)
    disasm = os.path.join(SDK, 'thirdparty', 'disasm')
    run([find_cc(), '-O1', '-w', '-I', disasm, os.path.join(HERE, 'miniasm.c'), os.path.join(disasm, 'disasm.c'),
         '-o', exe])
    return exe


def assemble(testing, out, miniasm):
    asm, pre, binr = (os.path.join(out, d) for d in ('asm', 'pre', 'bin'))
    for d in (asm, pre, binr):
        if os.path.isdir(d):
            shutil.rmtree(d)
        os.makedirs(d)
    count = 0
    # tw2de_nx (2026-11-02): with Xenia's tests, this project's own (tools/ppc_tests/own: cases the SDK got wrong
    # in the game, such as a vector pack whose destination is also a source).
    files = [(testing, n) for n in sorted(os.listdir(testing))]
    files += [(OWN, n) for n in sorted(os.listdir(OWN))]
    for folder, name in files:
        if not name.endswith('.s'):
            continue
        stem = name[:-2]
        if stem in SKIP_FILES:
            continue
        shutil.copy(os.path.join(folder, name), os.path.join(asm, name))
        with open(os.path.join(pre, name), 'w') as f:
            subprocess.run([sys.executable, os.path.join(HERE, 'prep.py'), os.path.join(asm, name)], stdout=f,
                           check=True)
        r = subprocess.run([miniasm, os.path.join(pre, name), os.path.join(binr, stem)], capture_output=True,
                           text=True)
        bad = [l for l in r.stderr.splitlines() if 'decodes as' not in l]
        if r.returncode != 0 or bad:
            sys.exit('assembling %s:\n%s' % (name, '\n'.join(bad[:20])))
        count += 1
    print('  %d test files assembled' % count)
    return asm, binr


def write_regs(gen):
    acc = ['#include "ppc_config.h"\n#include <rex/ppc.h>\n']
    acc.append('PPCRegister* R(PPCContext& c, int n) { switch (n) {' +
               ''.join('case %d: return &c.r%d;' % (i, i) for i in range(32)) + '} return nullptr; }\n')
    acc.append('PPCRegister* F(PPCContext& c, int n) { switch (n) {' +
               ''.join('case %d: return &c.f%d;' % (i, i) for i in range(32)) + '} return nullptr; }\n')
    acc.append('PPCVRegister* V(PPCContext& c, int n) { switch (n) {' +
               ''.join('case %d: return &c.v%d;' % (i, i) for i in range(128)) + '} return nullptr; }\n')
    acc.append('uint32_t CR(PPCContext& c) { return (c.cr0.raw() << 28) | (c.cr1.raw() << 24) | '
               '(c.cr2.raw() << 20) | (c.cr3.raw() << 16) | (c.cr4.raw() << 12) | (c.cr5.raw() << 8) | '
               '(c.cr6.raw() << 4) | c.cr7.raw(); }\n')
    open(os.path.join(gen, 'regs.cpp'), 'w').write(''.join(acc))


def split_functions(gen):
    src = open(os.path.join(gen, 'ppc_test_functions.cpp')).read()
    blocks = re.split(r'\n(?=DEFINE_REX_FUNC\()', src[src.index('// Function implementations'):])
    funcs = [b for b in blocks if b.startswith('DEFINE_REX_FUNC(')]
    hdr = '#include "ppc_config.h"\n#include <rex/ppc.h>\n#include "ppc_test_decls.h"\n'
    for f in os.listdir(gen):
        if re.match(r'f_\d+\.cpp$', f):
            os.remove(os.path.join(gen, f))
    for i in range(0, len(funcs), CHUNK):
        open(os.path.join(gen, 'f_%04d.cpp' % (i // CHUNK)), 'w').write(hdr + '\n'.join(funcs[i:i + CHUNK]) + '\n')
    print('  %d functions in %d files' % (len(funcs), (len(funcs) + CHUNK - 1) // CHUNK))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--xenia', help='an existing src/xenia/cpu/ppc/testing (instead of fetching it)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'ppc_tests'))
    # renut-ultimate (port 147): the base's code generator (toolsuild_pc.bat builds it) and its manifest.
    ap.add_argument('--rexglue', default=os.path.join(ROOT, 'sdk', 'out', 'win-amd64', 'rexglue' + EXE))
    ap.add_argument('--manifest', default=os.path.join(ROOT, 'app', 'renut_manifest.toml'))
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 2)
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    if not os.path.isfile(args.rexglue):
        sys.exit('no code generator at %s (tools\build_pc.bat builds it)' % args.rexglue)
    dkp = os.environ.get('DEVKITPRO') or ('C:/devkitPro' if os.name == 'nt' else '/opt/devkitpro')

    print('1/6 The tests')
    testing = os.path.abspath(args.xenia) if args.xenia else fetch_tests(out)
    print('2/6 The assembler')
    miniasm = build_assembler(out)
    print('3/6 Assembling')
    asm, binr = assemble(testing, out, miniasm)
    print('4/6 Generating the code (exact_fp on)')
    gen = os.path.join(out, 'gen')
    if os.path.isdir(gen):
        shutil.rmtree(gen)
    os.makedirs(gen)
    run([args.rexglue, 'recompile-tests', '--output=' + gen, '--bin-dir=' + binr, '--asm-dir=' + asm,
         '--config=' + os.path.abspath(args.manifest), '--exact-fp'])
    romfs = os.path.join(gen, 'romfs')
    os.makedirs(romfs)
    run([sys.executable, os.path.join(HERE, 'gen_data.py'), gen, gen])
    for f in ('tests.bin', 'names.txt'):
        shutil.move(os.path.join(gen, f), os.path.join(romfs, f))
    with open(os.path.join(romfs, 'known.txt'), 'w') as f:
        f.write(''.join('%s\t%s\n' % kv for kv in KNOWN.items()))
    write_regs(gen)
    split_functions(gen)
    print('5/6 Building the homebrew (devkitA64)')
    build = os.path.join(out, 'build')
    if not os.path.isfile(os.path.join(build, 'build.ninja')):
        run(['cmake', '-S', os.path.join(HERE, 'nro'), '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_TOOLCHAIN_FILE=' + os.path.join(ROOT, 'tools', 'switch', 'cmake', 'switch-devkitA64.cmake'),
             '-DDEVKITPRO=' + dkp, '-DGEN_DIR=' + gen, '-DREXSDK_DIR=' + SDK])
    else:
        run(['cmake', build])  # the generated file list may have changed
    run(['cmake', '--build', build, '-j', str(args.jobs)])
    print('6/6 Done')
    nro = os.path.join(out, 'renut_ppc_tests.nro')
    shutil.copy(os.path.join(build, 'renut_ppc_tests.nro'), nro)
    print('  ' + nro + '\n  Copy it to the SD card (/switch/) and start it from the homebrew menu.')


if __name__ == '__main__':
    main()
