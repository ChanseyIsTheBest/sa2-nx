#!/usr/bin/env python3
"""blur-nx (2026-11-13, Blur's docs/cpu-near-exact-50-1113.md; the user chose the near-exact ideas to do): the generated
code's forms that a near-exact switch changes, rewritten to helpers whose default is the expression the generator
wrote (so with every switch off the code computes what it did).

  #42  vmaxfp / vmaxfp128 / vminfp / vminfp128:  simde_mm_max_ps(A, B) -> rex::ppc::VMaxFp(A, B)
                                                 simde_mm_min_ps(A, B) -> rex::ppc::VMinFp(A, B)
       (sdk/include/rex/ppc/diet_ops.h: SSE's max / min by default, Arm's fmax / fmin with REX_NX_VMINMAX_FAST;
       the PC census counts the lanes where the two differ.)
  #19  `if (C) ppc_trap(ctx, base, 0|22);` -> `if (REX_NX_TRAPS_ON && (C)) ppc_trap(ctx, base, 0|22);`
       (the divide-by-zero traps before divisions and the twi asserts: on a hit ppc_trap only logs a warning and the
       game goes on, so with REX_NX_NO_TRAPS (REX_NX_TRAPS_ON 0) the compiler drops the test and nothing but the log
       line changes; the call's text stays for the steps that read it. The service trap, type 20, stays.)

A post-codegen step before the copies (tools/localize_native.py), so they take the same forms.

  python tools/near_exact.py --gen app/generated [--check]
Idempotent (the rewritten text no longer matches). No restore: the helpers' defaults are the expressions themselves.
"""
import argparse
import glob
import os
import re
import sys

INSN = re.compile(r'^\t// (\S+)')
FORMS = {
    'vmaxfp': ('simde_mm_max_ps(', 'rex::ppc::VMaxFp('),
    'vmaxfp128': ('simde_mm_max_ps(', 'rex::ppc::VMaxFp('),
    'vminfp': ('simde_mm_min_ps(', 'rex::ppc::VMinFp('),
    'vminfp128': ('simde_mm_min_ps(', 'rex::ppc::VMinFp('),
}


TRAP = re.compile(r'^(\tif \()(.*)(\) ppc_trap\(ctx, base, (?:0|22)\);.*)$', re.S)


def rewrite(text, counts):
    out = []
    current = None
    for line in text.splitlines(keepends=True):
        t = TRAP.match(line)
        if t and 'REX_NX_TRAPS_ON' not in line:
            line = t.group(1) + 'REX_NX_TRAPS_ON && (' + t.group(2) + ')' + t.group(3)
            counts['REX_NX_TRAPS_ON'] = counts.get('REX_NX_TRAPS_ON', 0) + 1
        m = INSN.match(line)
        if m:
            current = FORMS.get(m.group(1))
            out.append(line)
            continue
        if current and current[0] in line:
            n = line.count(current[0])
            line = line.replace(current[0], current[1])
            counts[current[1]] = counts.get(current[1], 0) + n
        out.append(line)
    return ''.join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--check', action='store_true', help='report only')
    args = ap.parse_args()
    files = sorted(glob.glob(os.path.join(args.gen, '*_recomp.*.cpp')))  # port 186: any project's name
    if not files:
        sys.exit('near_exact: no generated sources in %s' % args.gen)
    counts = {}
    changed = 0
    for path in files:
        text = open(path, encoding='utf-8').read()
        new = rewrite(text, counts)
        if new != text:
            changed += 1
            if not args.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    print('near_exact: %s in %d files%s' % (', '.join('%d %s' % (v, k.rstrip('(')) for k, v in sorted(counts.items()))
                                            or 'nothing to rewrite', changed, ' (check only)' if args.check else ''))


if __name__ == '__main__':
    main()
