#!/usr/bin/env python3
"""katamari-nx (2026-10-30): declares the complete functions that sit in unclaimed gaps of the generated code even when
no pointer to them is stored or built where tools/pointer_fixpoint.py looks (the first PC boot stopped on
"Call to invalid or unregistered function at guest address 0x82689770": a 10-instruction table accessor between two
registered ones, reached through a pointer computed elsewhere).

Candidates: in every gap (tools/find_gaps.py), the first word after the padding, and the word after each blr / bctr
(and its padding) inside the gap. The checks are pointer_fixpoint.py's (a scratch codegen with the candidates
declared, then: the listing fits in the gap or continues only into a registered tail it branches to, ends in a
terminator, has no padding or invalid words, reads no register that is undefined at a function entry, and a tail
branch lands on a function start). The accepted ones go to app/overrides.toml with "gap sweep" as their evidence;
then the codegen runs again and the sweep repeats until nothing new qualifies.

  python tools/gap_sweep.py [--image out/renut_image.bin] [--scratch out/gapsweep] [--dry-run]
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pointer_fixpoint as pf  # noqa: E402

TERMINATORS = {0x4E800020, 0x4E800420}  # blr, bctr
SWITCH_SHADOW = 0x800  # no candidate this far after a jump-table bctr: those blocks are its cases


def table_bctrs(image, lo, hi):
    """bctr addresses of jump tables: mtctr right before, an indexed load (lwzx / lhzx / lbzx) in the 8 before. The
    first run of this sweep took 2-instruction cases after such a bctr for functions (the codegen then reported
    "Jump target ... unresolved at bctr" for 0x8230DCD8, 0x8240305C, 0x8255BCD0, 0x82707584)."""
    out = []
    for off in range(lo - pf.IMAGE_BASE + 32, hi - pf.IMAGE_BASE, 4):
        if struct.unpack_from('>I', image, off)[0] != 0x4E800420:
            continue
        prev = [struct.unpack_from('>I', image, off - 4 * k)[0] for k in range(1, 9)]
        if prev[0] >> 26 != 31 or (prev[0] >> 1) & 0x3FF != 467:  # mtspr (mtctr)
            continue
        ctr_src = (prev[0] >> 21) & 31
        for w in prev[1:]:
            if w in TERMINATORS:  # the previous function's blr: no table here (a vtable thunk's bctr)
                break
            # the indexed load (lwzx / lhzx / lbzx) or the add of the table base that gives mtctr its register
            if w >> 26 == 31 and (w >> 1) & 0x3FF in (23, 279, 87, 266) and (w >> 21) & 31 == ctr_src:
                out.append(pf.IMAGE_BASE + off)
                break
    return out


def candidates(image, gaps, lo, hi):
    import bisect
    tables = table_bctrs(image, lo, hi)
    out = {}
    for start, size in gaps:
        end = start + size
        a = start
        at_entry = True
        while a < end:
            w = struct.unpack_from('>I', image, a - pf.IMAGE_BASE)[0]
            if w == 0:
                a += 4
                at_entry = True
                continue
            if at_entry and lo <= a < hi:
                i = bisect.bisect_right(tables, a) - 1
                if not (i >= 0 and a - tables[i] < SWITCH_SHADOW):
                    out[a] = (end - a, [start])
                at_entry = False
            if w in TERMINATORS:
                at_entry = True
            a += 4
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', default=os.path.join(pf.ROOT, 'out', 'renut_image.bin'))
    ap.add_argument('--scratch', default=os.path.join(pf.ROOT, 'out', 'gapsweep'))
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    rexglue = pf.find_rexglue()
    image = open(a.image, 'rb').read()
    lo, hi = pf.code_range()
    pf.CODE_LO, pf.CODE_HI = lo, hi
    examined = set()
    for n in range(1, 10):
        d = os.path.join(a.scratch, f'pass{n}')
        os.makedirs(d, exist_ok=True)
        gaps = pf.read_gaps(d)
        cands = {t: v for t, v in candidates(image, gaps, lo, hi).items() if t not in examined}
        examined |= set(cands)
        if not cands:
            print(f'pass {n}: no new candidates; fixpoint')
            break
        gen, log = pf.scratch_codegen(rexglue, d, cands)
        pf._owner_cache.clear()
        accepted, rejected, notes = pf.classify(gen, log, cands)
        reasons = {}
        for why in rejected.values():
            reasons[why] = reasons.get(why, 0) + 1
        print(f'pass {n}: {len(gaps)} gaps, {len(cands)} candidates, {len(accepted)} accepted, rejected {reasons}')
        with open(os.path.join(d, 'accepted.txt'), 'w') as f:
            for t in sorted(accepted):
                f.write(f'0x{t:08X} | ' + ' ; '.join(accepted[t]) + '\n')
        if a.dry_run or not accepted:
            break
        lines = [f'\n# Complete functions in unclaimed gaps without a pointer the pointer sweep sees (tools/gap_sweep.py pass '
                 f'{n}): the scratch listing fits in the gap (or continues only into a registered tail it branches\n'
                 f'# to), ends in a terminator, reads no register that is undefined at a function entry. First case:\n'
                 f'# 0x82689770, the first PC boot\'s "Call to invalid or unregistered function".\n']
        for t in sorted(accepted):
            size = notes.get(t) or f'{len(accepted[t])} instructions'
            lines.append(f'"0x{t:08X}" = {{ }}  # {size}, gap sweep\n')
        pf.insert_in_functions(lines)
        r = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                            os.path.join(pf.TOOLS, 'codegen.ps1'), '-Rexglue', rexglue], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f'codegen failed after pass {n}:\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')


if __name__ == '__main__':
    main()
