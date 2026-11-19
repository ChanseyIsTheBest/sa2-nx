#!/usr/bin/env python3
"""Code addresses BUILT in code that no translated function starts at: the check behind S4ep1-nx's Mad Gear crash fix.

renut-ultimate (2026-11-20, port 191): from S4ep1-nx (github.com/FurryLilManChild/S4ep1-nx, GPL-3.0, release 0.5.1,
commit 30149407, its tools/built_ptr_scan.py of 2026-11-18, GitHub issue 1). Sonic 4's Mad Gear zone stopped with
"Call to invalid or unregistered function at guest address 0x8210C638": a 10-instruction leaf callback in a gap with no
.pdata entry, whose address the game builds (lis r10,0x8211 at 0x8210C0BC, addi r6,r10,-0x39C8 at 0x8210C0C8) and
stores in an object at run time. No data word holds it, the generator never made it a function, and the first call
through the stored pointer stopped the game. S4ep1-nx registered it in its overrides.toml and ran this over the whole
image to find any other.

What it reports: every `lis rX,hi` followed within 24 instructions by `addi rY,rX,lo` or `ori rY,rX,lo` whose value is a
word-aligned address in .text that lies outside every .pdata function and that the generated code does not register
as a function. Switch-table bases (an mtctr soon after, or an address table at the target) and the import
thunks the table registers are left out. tools/pointer_fixpoint.py looks for the same
built addresses (within 32 instructions), but takes a candidate only when its gap passes its checks; this scan is the
independent count by .pdata and by the generated table, so a candidate pointer_fixpoint turned down, or a codegen run
after it, still shows. Check every hit by hand (tools/ppc_dis.py): a real callback is stored, passed or called
through CTR; register it in app/overrides.toml with its evidence, then run code generation and post_codegen again.

    python tools/built_ptr_scan.py [--image out/renut_image.bin] [--gen app/generated] [--overrides app/overrides.toml]

--image: the generator's image dump (REXGLUE_DUMP_IMAGE; tools/codegen.ps1 writes out/renut_image.bin): the XEX's PE
image as loaded at 0x82000000. --gen: the generated folder, whose <project>_init.cpp lists the registered functions
(found by the project's name, as port 186's passes do). Prints one line per candidate; the count goes to stderr.
"""
import argparse
import bisect
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = 0x82000000
WINDOW = 24  # S4ep1-nx's window: lis and its addi/ori up to 23 instructions apart


def gen_file(gen, suffix):
    """The generator names its files after the project (port 186): <project>_init.cpp."""
    for name in sorted(os.listdir(gen)):
        m = re.fullmatch(r'(\w+)_recomp\.\d+\.cpp', name)
        if m:
            return os.path.join(gen, m.group(1) + '_' + suffix)
    return os.path.join(gen, 'renut_' + suffix)


def sections(d):
    e = struct.unpack('<I', d[0x3c:0x40])[0]
    nsec = struct.unpack('<H', d[e + 6:e + 8])[0]
    opt = struct.unpack('<H', d[e + 20:e + 22])[0]
    secs = {}
    for i in range(nsec):
        s = d[e + 24 + opt + 40 * i:e + 24 + opt + 40 * i + 40]
        name = s[:8].rstrip(b'\0').decode('ascii', 'replace')
        secs.setdefault(name, (struct.unpack('<I', s[12:16])[0], struct.unpack('<I', s[8:12])[0]))
    return secs


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--image', default=os.path.join(ROOT, 'out', 'renut_image.bin'))
    ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated'))
    ap.add_argument('--overrides', default=os.path.join(ROOT, 'app', 'overrides.toml'))
    a = ap.parse_args()

    d = open(a.image, 'rb').read()
    secs = sections(d)
    tva, tvs = secs['.text']
    pva, pvs = secs['.pdata']
    funcs = []
    for j in range(pva, pva + pvs, 8):
        b, w = struct.unpack('>II', d[j:j + 8])
        if b == 0:
            break
        funcs.append((b, b + ((w >> 8) & 0x3FFFFF) * 4))
    funcs.sort()
    starts = [f[0] for f in funcs]

    def covered(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and funcs[i][0] <= addr < funcs[i][1]

    declared = set()
    if os.path.exists(a.overrides):
        declared = set(int(m, 16) for m in re.findall(r'"0x([0-9A-Fa-f]{8})"', open(a.overrides, encoding='utf-8').read()))
    init = gen_file(a.gen, 'init.cpp')
    # Every address the table registers: the sub_ functions and the import thunks (__imp__ entries).
    registered = set(int(m, 16) for m in re.findall(r'\{ 0x([0-9A-Fa-f]{8}),', open(init, encoding='utf-8').read()))
    if not registered:
        sys.exit('built_ptr_scan: no registered functions in %s' % init)

    t_lo, t_hi = BASE + tva, BASE + tva + tvs
    hits = {}
    for off in range(tva, tva + tvs - 4, 4):
        w = struct.unpack_from('>I', d, off)[0]
        if w >> 26 != 15 or (w >> 16) & 31 != 0:  # lis rD,imm is addis rD,0,imm
            continue
        rd = (w >> 21) & 31
        hi = (w & 0xFFFF) << 16
        for k in range(1, WINDOW):
            o2 = off + 4 * k
            if o2 + 4 > tva + tvs:
                break
            w2 = struct.unpack_from('>I', d, o2)[0]
            op, rt, ra = w2 >> 26, (w2 >> 21) & 31, (w2 >> 16) & 31
            if (op == 14 and ra == rd) or (op == 24 and rt == rd):  # addi rY,rD,lo / ori rY,rD,lo (ori: rS is bits 6-10)
                lo = w2 & 0xFFFF
                target = (hi + (lo - 0x10000 if (op == 14 and lo & 0x8000) else lo)) & 0xFFFFFFFF
                follow = [struct.unpack_from('>I', d, o2 + 4 * j)[0] for j in range(1, 7) if o2 + 4 * j + 4 <= len(d)]
                # A switch table's base: an mtctr (any register) within the next six instructions, or a table of
                # absolute addresses placed in .text at the target (S4ep1-nx's form: mtctr r12 in the next four;
                # Katamari's 95 such tables use other registers and an index scale first).
                first = struct.unpack_from('>I', d, target - BASE)[0] if t_lo <= target < t_hi else 0
                switch_base = (any((f & 0xFC1FFFFF) == 0x7C0903A6 for f in follow)
                               or BASE <= first < BASE + len(d))
                if (t_lo <= target < t_hi and target % 4 == 0 and not covered(target) and target not in starts
                        and target not in declared and target not in registered and not switch_base):
                    hits.setdefault(target, []).append(BASE + o2)
            # rD overwritten (a D-form writing it other than the uses above): the pair ends
            if op in (14, 15, 32, 24, 25) and rt == rd and not (op == 14 and ra == rd):
                break
            # culdcept-nx (2026-11-20): the other forms that write rD end it too. Culdcept Saga's one candidate was
            # `lis r11,0x8218` at 0x82182E4C, `lbz r11,10810(r31)`, then `ori r11,r11,0x4` (a flag bit set in the byte
            # it loaded): "0x82180004", the middle of a function. D-form loads and mulli / subfic write rT; the logical
            # immediates and the rotates write rA (rS / rT is their source). X-form (op 31) is left alone: telling its
            # writes from its reads needs the extended opcode, and ending a pair too early could hide a real callback.
            if op in (7, 8, 33, 34, 35, 40, 41, 42, 43, 46, 58) and rt == rd:
                break
            if op in (20, 21, 23, 26, 27, 28, 29, 30) and ra == rd:
                break
            if op in (24, 25) and ra == rd and rt != rd:  # ori / oris into rD from another register
                break
            # renut-ultimate (port 191, with culdcept-nx's handoff): addic / addic. write rT too, and the update forms
            # (lwzu, lbzu, lhzu, lhau, the stores with update, the float ones) write their base register rA.
            if op in (12, 13) and rt == rd:
                break
            if op in (33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55) and ra == rd:
                break
            if w2 == 0x4E800020 or op == 18:  # blr / b
                break
    for target in sorted(hits):
        print('0x%08X built at %s' % (target, ' '.join('0x%08X' % x for x in hits[target])))
    print('built_ptr_scan: %d candidates (%d functions registered, %d .pdata functions)'
          % (len(hits), len(registered), len(funcs)), file=sys.stderr)


if __name__ == '__main__':
    main()
