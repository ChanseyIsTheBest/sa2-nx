#!/usr/bin/env python3
"""Where the generated code's machine time goes, for any ReXGlue port on the Switch (blur-nx, 2026-11-17; #254 and #255
of docs/cpu-codegen-universal-30-1116.md). Two views, both from a console profile (rex_profile.log, the profiler's
per-thread pc histograms) and the build's ELF with symbols:

  samples   every sampled pc's instruction decoded into a class (guest-memory load / store, byte swap, other load and
            store, branch, call, integer, float / vector, system), per thread and in all. Tells which overhead of the
            generated code's forms costs time, wherever it sits.
  copies    the N hottest functions (by samples) disassembled (objdump): their size, host stack frame, vector spills
            and reloads, byte swaps, guest accesses through a 32-bit index (uxtw), calls, and the samples on loads.
            Tells which copies have more machine code than their guest work explains.

  python tools/codegen_census.py samples PROFILE ELF [--from N --to M]
  python tools/codegen_census.py copies PROFILE ELF [--from N --to M] [--top 20]

Reports FROM-TO are the profiler's numbered windows (the racing stretch of a measured race: 9-20). The decoding is by
the A64 encoding groups (the top-level op0 field), with the byte reversals (REV, REV16, REV32) picked out; it reads the
ELF's .text from its file offset (readelf -S).
"""
import argparse
import bisect
import collections
import re
import struct
import subprocess
import sys

DKP = 'C:/devkitPro/devkitA64/bin/aarch64-none-elf-'
WIN = re.compile(r'^==== ([0-9.]+) s \| mode: (\w+)')
THREAD = re.compile(r'^-- thread \d+ "([^"]*)"')
PC = re.compile(r'^\s+[0-9.]+%\s+pc image\+0x([0-9a-f]+) x(\d+)')


def text_section(elf):
    out = subprocess.run([DKP + 'readelf', '-SW', elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.search(r'\]\s+\.text\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
        if m:
            return int(m.group(1), 16), int(m.group(3), 16), int(m.group(2), 16)  # address, size, file offset
    sys.exit('no .text in ' + elf)


def symbols(elf):
    starts, names = [], []
    out = subprocess.run([DKP + 'nm', '-n', '--defined-only', elf], capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'TtWw':
            starts.append(int(p[0], 16))
            names.append(p[2])
    return starts, names


def samples_of(profile, frm, to):
    """{thread: Counter(pc -> samples)} over windows frm..to."""
    per = collections.defaultdict(collections.Counter)
    n, thread = -1, None
    for line in open(profile, encoding='utf-8', errors='replace'):
        if WIN.match(line):
            n += 1
            continue
        if not frm <= n <= to:
            continue
        m = THREAD.match(line)
        if m:
            thread = m.group(1)
            continue
        m = PC.match(line)
        if m and thread:
            per[thread][int(m.group(1), 16)] += int(m.group(2))
    return per


def classify(w):
    """The A64 encoding group of an instruction word, with byte reversals and calls picked out."""
    if w is None:
        return 'outside .text'
    op0 = (w >> 25) & 0xF
    if (w & 0x7FFFF800) == 0x5AC00800 or (w & 0xFFFFF800) == 0xDAC00800 or (w & 0x7FFFF800) == 0x5AC00400 or \
            (w & 0xFFFFFC00) == 0xDAC00C00:
        return 'byte swap (rev)'
    if (op0 & 0b0101) == 0b0100:  # loads and stores
        is_load = bool(w & (1 << 22)) if (op0 & 0b1010) == 0b1000 or (op0 & 0b1010) == 0b1010 else bool(w & (1 << 22))
        # register-offset form with an extend (the guest window's base + zext(index)): option field bits 13-15
        reg_off = (w & 0x3B200C00) == 0x38200800
        uxtw = reg_off and ((w >> 13) & 7) == 0b010
        kind = 'load' if is_load else 'store'
        return ('guest ' if uxtw else '') + kind + (' (uxtw index)' if uxtw else '')
    if (op0 & 0b1110) == 0b1010:  # branches, exceptions, system
        if (w & 0xFC000000) == 0x94000000 or (w & 0xFFFFFC1F) == 0xD63F0000:
            return 'call (bl / blr)'
        if (w & 0xFFC00000) == 0xD5000000:
            return 'system (msr / mrs / barriers / hints)'
        return 'branch'
    if (op0 & 0b0111) == 0b0111:
        return 'float / vector'
    return 'integer'


def cmd_samples(a):
    vaddr, size, off = text_section(a.elf)
    data = open(a.elf, 'rb').read()

    def word(pc):
        o = off + pc - vaddr
        return struct.unpack_from('<I', data, o)[0] if vaddr <= pc < vaddr + size else None

    per = samples_of(a.profile, a.frm, a.to)
    total = collections.Counter()
    for thread, pcs in sorted(per.items(), key=lambda kv: -sum(kv[1].values())):
        c = collections.Counter()
        for pc, n in pcs.items():
            c[classify(word(pc))] += n
        total.update(c)
        n = sum(c.values())
        if n < 50:
            continue
        print('== %s: %d samples' % (thread, n))
        for k, v in c.most_common():
            print('   %5.1f %%  %s' % (100.0 * v / n, k))
    n = sum(total.values())
    print('== all threads: %d samples' % n)
    for k, v in total.most_common():
        print('   %5.1f %%  %s' % (100.0 * v / n, k))
    print('(A sample is the oldest instruction not yet retired: a load that misses holds the samples itself.)')


def cmd_copies(a):
    starts, names = symbols(a.elf)
    per = samples_of(a.profile, a.frm, a.to)
    by_fn = collections.Counter()
    for pcs in per.values():
        for pc, n in pcs.items():
            i = bisect.bisect_right(starts, pc) - 1
            if i >= 0:
                by_fn[i] += n
    print('| function | bytes | samples | frame | q spills / reloads | rev | uxtw accesses | calls | samples on loads |')
    print('|---|---|---|---|---|---|---|---|---|')
    for i, n in by_fn.most_common(a.top):
        lo, hi = starts[i], starts[i + 1] if i + 1 < len(starts) else starts[i] + 4
        dis = subprocess.run([DKP + 'objdump', '-d', '--no-show-raw-insn', '--start-address=0x%x' % lo,
                              '--stop-address=0x%x' % hi, a.elf], capture_output=True, text=True).stdout
        frame = spills = reloads = rev = uxtw = calls = 0
        load_pcs = set()
        for line in dis.splitlines():
            m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
            if not m:
                continue
            pc, op, args = int(m.group(1), 16), m.group(2), m.group(3)
            if op == 'sub' and args.startswith('sp, sp, #') and not frame:
                frame = int(args.split('#')[1].split()[0], 0)
            if re.match(r'(str|stp)$', op) and re.match(r'q\d+', args) and '[sp' in args:
                spills += 1
            if re.match(r'(ldr|ldp)$', op) and re.match(r'q\d+', args) and '[sp' in args:
                reloads += 1
            if op.startswith('rev'):
                rev += 1
            if 'uxtw]' in args:
                uxtw += 1
            if op in ('bl', 'blr'):
                calls += 1
            if op.startswith('ld'):
                load_pcs.add(pc)
        on_loads = sum(c for pcs in per.values() for pc, c in pcs.items() if pc in load_pcs)
        name = re.sub(r'^_ZN5banjo3hot\d+n_([0-9A-F]{8}).*', r'copy \1', names[i])[:40]
        print('| %s | %d | %d | %d | %d / %d | %d | %d | %d | %.0f %% |' % (
            name, hi - lo, n, frame, spills, reloads, rev, uxtw, calls, 100.0 * on_loads / max(n, 1)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', choices=['samples', 'copies'])
    ap.add_argument('profile')
    ap.add_argument('elf')
    ap.add_argument('--from', dest='frm', type=int, default=9)
    ap.add_argument('--to', type=int, default=20)
    ap.add_argument('--top', type=int, default=20)
    a = ap.parse_args()
    (cmd_samples if a.what == 'samples' else cmd_copies)(a)


if __name__ == '__main__':
    main()
