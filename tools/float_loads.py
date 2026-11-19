#!/usr/bin/env python3
"""Guest single-precision loads byte-swapped in the vector unit on AArch64 (post-codegen step of the "optimised"
codegen flavour; docs/enhancements.md, "The second CPU round of 2026-10-12"). Works on any ReXGlue port.

Guest memory is big-endian, so an `lfs` is a 32-bit load, a byte swap and a conversion to double. The generator
writes it through an integer:
    temp.u32 = REX_LOAD_D_U32(r3.u32, 172);      ldr w0, [..] ; rev w0, w0 ; fmov s0, w0
    f13.f64 = double(temp.f32);                  fcvt d13, s0
and the move from the general registers to the vector unit (`fmov s0, w0`) is the slow step on the Cortex-A57: 5
cycles, after the load's 4 and the swap's 1. Loaded straight into the vector unit and swapped there it is
    ldr s0, [..] ; rev32 v0.8b, v0.8b            5 + 3 cycles, one instruction less
The same four bytes in the same order: REX_F32_FROM_BE() below, checked by sdk/tests/float_loads_test.cpp against the
integer form through simde's model of the instruction.

This step rewrites the two statements into
    f13.f64 = double(REX_LOAD_D_F32(r3.u32, 172));
The generated pch defines REX_LOAD_D_F32 / REX_LOAD_F32 as the vector form on AArch64 and as the integer form
elsewhere (the PC build is unchanged) or with REX_FLOAT_LOADS_INTEGER defined.

Not rewritten: loads that feed an any-mode helper (they test the bits as an integer), stores (the value comes from
a conversion in the vector unit and needs the same number of instructions either way), and a load from an address a
float was stored to earlier in the same straight line of code (no label, call or loop barrier in between): there the
compiler hands the stored value on without any load, which it can only do while both sides are the integer form.

blur-nx (2026-11-13, M-23 of docs/cpu-moonshots-50-1113.md): with CG-13 (REX_FPCR_ONE_MODE: one flush mode) the
any-mode helpers are the plain conversions, so the loads that feed SingleToDoubleAnyMode and the stores of a
DoubleToSingleAnyMode result go through the vector unit too:
    temp.u32 = REX_LOAD_D_U32(r31.u32, 13392);         ->  ctx.f1.f64 = REX_LOAD_D_F64ANY(r31.u32, 13392);
    ctx.f1.f64 = rex::ppc::SingleToDoubleAnyMode(temp.u32);
    temp.f32 = rex::ppc::DoubleToSingleAnyMode(f31.f64);  ->  REX_STORE_D_F32ANY_C(r29.u32, 1180, f31.f64);
    REX_STORE_D_U32_C(r29.u32, 1180, temp.u32);
(`ldr s; rev32` / `rev32; str s` for `ldr w; rev w; fmov s, w` / `fmov w, s; rev w; str w`: the 5-cycle moves between
the register files gone). The macros are the vector forms only with REX_FPCR_ONE_MODE on AArch64, and today's
integer forms otherwise (the helpers then test the bits as before), so the generated tree serves both. The same four
bytes, the same conversion (fcvt under the same FPCR). Loads from an address an integer store just wrote stay as they
are (the forwarding rule above).
blur-nx (2026-11-17, #231 of docs/cpu-codegen-universal-30-1116.md): --doubles does the same for `lfd`:
    ctx.f13.u64 = REX_LOAD_D_U64(r3.u32, 8);     ->  ctx.f13.f64 = REX_LOAD_D_F64(r3.u32, 8);
(`ldr d; rev64 v.8b` for `ldr x; rev x; fmov d, x`; rex::ppc::DoubleFromBigEndian, only bits move), where the loaded
register's next use in the same straight line reads it as a double (not where it is stored again as an integer, which
the compiler turns into a plain copy, nor where it is converted as a 64-bit integer: fcfid), and not after a 64-bit
integer store to the same address (the forwarding rule).
  python tools/float_loads.py --gen app/generated/default [--check] [--undo] [--any-mode] [--doubles]
Idempotent.
"""
import argparse
import os
import re
import sys

STORE = re.compile(r'^\tREX_STORE(?:_D)?_U32\((.*), (?:ctx\.)?temp\.u32\);$')
FENCE = re.compile(r'^loc_|\(ctx, base[,)]|REX_CALL_INDIRECT_|REX_LOOP_BARRIER|^DEFINE_REX_FUNC|^\}')
LOAD = re.compile(r'^\t((?:ctx\.)?temp)\.u32 = (REX_LOAD(?:_D)?)_U32\((.*)\);$')
USE = re.compile(r'^\t((?:ctx\.)?[fl_]+\d+)\.f64 = double\(((?:ctx\.)?temp)\.f32\);$')
NEW = re.compile(r'^\t((?:ctx\.)?[fl_]+\d+)\.f64 = double\((REX_LOAD(?:_D)?)_F32\((.*)\)\);$')
# blur-nx (2026-11-13, M-23): the any-mode pairs and their rewritten forms
USE_ANY = re.compile(r'^\t((?:ctx\.)?[fl_]+\d+)\.f64 = rex::ppc::SingleToDoubleAnyMode\(((?:ctx\.)?temp)\.u32\);$')
NEW_ANY = re.compile(r'^\t((?:ctx\.)?[fl_]+\d+)\.f64 = (REX_LOAD(?:_D)?)_F64ANY\((.*)\);$')
TO_SINGLE = re.compile(r'^\t((?:ctx\.)?temp)\.f32 = rex::ppc::DoubleToSingleAnyMode\(([^,()]+)\);$')
STORE_ANY = re.compile(r'^\t(REX_STORE(?:_D)?)_U32(_C)?\((.*), ((?:ctx\.)?temp)\.u32\);$')
NEW_STORE_ANY = re.compile(r'^\t(REX_STORE(?:_D)?)_F32ANY(_C)?\((.*), ([^,]+)\);$')
# blur-nx (2026-11-17, #231): double loads
LOAD64 = re.compile(r'^\t((?:ctx\.)?[fl_]+\d+)\.u64 = (REX_LOAD(?:_D)?)_U64\((.*)\);$')
NEW64 = re.compile(r'^\t((?:ctx\.)?[fl_]+\d+)\.f64 = (REX_LOAD(?:_D)?)_F64\((.*)\);$')
STORE64 = re.compile(r'^\tREX_STORE(?:_D)?_U64\((.*), [^,()]+\);$')
FENCE64 = re.compile(r'^loc_|\(ctx, base[,)]|REX_CALL_INDIRECT_|REX_LOOP_BARRIER|^DEFINE_REX_FUNC|^\}|\breturn\b|\bgoto\b')
MARKER64 = '// blur-nx: double loads swapped in the vector unit (tools/float_loads.py --doubles, #231)'
MACROS64 = MARKER64 + '''
#include <rex/ppc/float_be.h>
#ifndef REX_LOAD_D_F64
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_FLOAT_LOADS_INTEGER)
#define REX_LOAD_F64(x) rex::ppc::DoubleFromBigEndian(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))
#define REX_LOAD_D_F64(r, d) rex::ppc::DoubleFromBigEndian(REX_ADDR_D(r, d))
#else
#define REX_LOAD_F64(x) std::bit_cast<double>(u64(REX_LOAD_U64(x)))
#define REX_LOAD_D_F64(r, d) std::bit_cast<double>(u64(REX_LOAD_D_U64(r, d)))
#endif
#endif
'''


def next_use_is_double(lines, i, reg):
    """The next statement of the straight line after lines[i] that names reg reads it as .f64 only."""
    tok = re.compile(r'(?<![\w.])' + re.escape(reg) + r'\.(\w+)')
    for j in range(i + 1, min(i + 80, len(lines))):
        line = lines[j]
        if line.lstrip().startswith('//'):
            continue
        if FENCE64.search(line):
            return False
        uses = tok.findall(line)
        if not uses:
            continue
        m = re.match(r'^\t' + re.escape(reg) + r'\.\w+ = (.*)$', line)
        rhs = tok.findall(m.group(1)) if m else uses
        if m and not rhs:
            return False  # overwritten before any read
        return all(u == 'f64' for u in rhs)
    return False


MARKER = '// rexglue ports: single-precision loads swapped in the vector unit (tools/float_loads.py)'
MACROS = MARKER + '''
#ifndef REX_LOAD_F32
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_FLOAT_LOADS_INTEGER)
[[gnu::always_inline]] static inline float RexFloatFromBigEndian(const void* p) noexcept {
  float f;
  __builtin_memcpy(&f, p, 4);  // ldr s
  const float32x2_t v = vset_lane_f32(f, vdup_n_f32(0.0f), 0);
  return vget_lane_f32(vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_f32(v))), 0);  // rev32
}
#define REX_LOAD_F32(x) RexFloatFromBigEndian(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))
#define REX_LOAD_D_F32(r, d) RexFloatFromBigEndian(REX_ADDR_D(r, d))
#else
#define REX_LOAD_F32(x) std::bit_cast<float>(u32(REX_LOAD_U32(x)))
#define REX_LOAD_D_F32(r, d) std::bit_cast<float>(u32(REX_LOAD_D_U32(r, d)))
#endif
#endif
'''
MARKER_ANY = '// blur-nx: the any-mode single loads and stores of CG-13 in the vector unit (tools/float_loads.py, M-23)'
MACROS_ANY = MARKER_ANY + '''
#include <rex/ppc/float_be.h>
#ifndef REX_LOAD_D_F64ANY
#if defined(REX_FPCR_ONE_MODE) && (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_FLOAT_LOADS_INTEGER)
#define REX_LOAD_F64ANY(x) double(REX_LOAD_F32(x))
#define REX_LOAD_D_F64ANY(r, d) double(REX_LOAD_D_F32(r, d))
#define REX_STORE_F32ANY(x, v) rex::ppc::FloatToBigEndian(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x), float(v))
#define REX_STORE_D_F32ANY(r, d, v) rex::ppc::FloatToBigEndian(REX_ADDR_D(r, d), float(v))
#else
#define REX_LOAD_F64ANY(x) rex::ppc::SingleToDoubleAnyMode(u32(REX_LOAD_U32(x)))
#define REX_LOAD_D_F64ANY(r, d) rex::ppc::SingleToDoubleAnyMode(u32(REX_LOAD_D_U32(r, d)))
#define REX_STORE_F32ANY(x, v) REX_STORE_U32(x, std::bit_cast<u32>(rex::ppc::DoubleToSingleAnyMode(v)))
#define REX_STORE_D_F32ANY(r, d, v) REX_STORE_D_U32(r, d, std::bit_cast<u32>(rex::ppc::DoubleToSingleAnyMode(v)))
#endif
#define REX_STORE_F32ANY_C(x, v) (REX_STORE_F32ANY(x, v), REX_CARD(x))
#define REX_STORE_D_F32ANY_C(r, d, v) (REX_STORE_D_F32ANY(r, d, v), REX_CARD((u32)(r) + (u32)(d)))
#endif
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--undo', action='store_true')
    # blur-nx (2026-11-13, M-23): the any-mode pairs too. tools/post_codegen.py's second run, after single_floats.py
    # (which reads the integer pairs for its proofs), ro_constants_cfg.py (which folds integer loads of read-only data)
    # and dead_ctx_defs.py; before the copies and store_cards.py (which marks the new stores' cards too).
    ap.add_argument('--any-mode', action='store_true')
    ap.add_argument('--doubles', action='store_true', help='blur-nx #231: lfd into the vector unit too')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): float_loads: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    sites = present = changed = forwarded = 0
    any_loads = any_stores = any_present = any_forwarded = 0  # blur-nx (2026-11-13, M-23)
    dbl_loads = dbl_present = dbl_forwarded = 0  # blur-nx (2026-11-17, #231)
    for f in files:
        path = os.path.join(a.gen, f)
        lines = open(path, encoding='utf-8').read().split('\n')
        out = []
        i = 0
        n = 0
        stored = set()  # addresses of the float stores since the last label, call or barrier
        stored64 = set()  # #231: the 64-bit integer stores' addresses, the same way
        while i < len(lines):
            line = lines[i]
            if FENCE.search(line):
                stored.clear()
                stored64.clear()
            else:
                st = STORE.match(line)
                if st:
                    stored.add(st.group(1))
                st = STORE64.match(line)
                if st:
                    stored64.add(st.group(1))
            if a.undo:
                m = NEW64.match(line)
                if m:
                    out.append('\t%s.u64 = %s_U64(%s);' % (m.group(1), m.group(2), m.group(3)))
                    n += 1
                    i += 1
                    continue
                m = NEW.match(line)
                if m:
                    out.append('\ttemp.u32 = %s_U32(%s);' % (m.group(2), m.group(3)))
                    out.append('\t%s.f64 = double(temp.f32);' % m.group(1))
                    n += 1
                    i += 1
                    continue
                m = NEW_ANY.match(line)
                if m:
                    out.append('\ttemp.u32 = %s_U32(%s);' % (m.group(2), m.group(3)))
                    out.append('\t%s.f64 = rex::ppc::SingleToDoubleAnyMode(temp.u32);' % m.group(1))
                    n += 1
                    i += 1
                    continue
                m = NEW_STORE_ANY.match(line)
                if m:
                    out.append('\ttemp.f32 = rex::ppc::DoubleToSingleAnyMode(%s);' % m.group(4))
                    out.append('\t%s_U32%s(%s, temp.u32);' % (m.group(1), m.group(2) or '', m.group(3)))
                    n += 1
                    i += 1
                    continue
            else:
                if NEW64.match(line):
                    dbl_present += 1
                m = LOAD64.match(line) if a.doubles else None
                if m and next_use_is_double(lines, i, m.group(1)):
                    if m.group(3) in stored64:
                        dbl_forwarded += 1
                    else:
                        out.append('\t%s.f64 = %s_F64(%s);' % (m.group(1), m.group(2), m.group(3)))
                        dbl_loads += 1
                        n += 1
                        i += 1
                        continue
                if NEW_ANY.match(line) or NEW_STORE_ANY.match(line):
                    any_present += 1
                # M-23: the store of a DoubleToSingleAnyMode result (its address is not an integer store's: no entry
                # in 'stored', so a later load from it may take the vector form too).
                t = TO_SINGLE.match(line) if a.any_mode else None
                sa = STORE_ANY.match(lines[i + 1]) if t and i + 1 < len(lines) else None
                if t and sa and sa.group(4) == t.group(1):
                    out.append('\t%s_F32ANY%s(%s, %s);' % (sa.group(1), sa.group(2) or '', sa.group(3), t.group(2)))
                    stored.discard(sa.group(3))
                    any_stores += 1
                    n += 1
                    i += 2
                    continue
                # M-23: the load that feeds SingleToDoubleAnyMode (kept as it is after an integer store to the address)
                m = LOAD.match(line)
                ua = USE_ANY.match(lines[i + 1]) if m and a.any_mode and i + 1 < len(lines) else None
                if m and ua and ua.group(2) == m.group(1):
                    if m.group(3) in stored:
                        any_forwarded += 1
                    else:
                        out.append('\t%s.f64 = %s_F64ANY(%s);' % (ua.group(1), m.group(2), m.group(3)))
                        any_loads += 1
                        n += 1
                        i += 2
                        continue
                if NEW.match(line):
                    present += 1
                m = LOAD.match(line)
                u = USE.match(lines[i + 1]) if m and i + 1 < len(lines) else None
                if m and u and u.group(2) == m.group(1) and m.group(3) in stored:
                    forwarded += 1
                elif m and u and u.group(2) == m.group(1):
                    out.append('\t%s.f64 = double(%s_F32(%s));' % (u.group(1), m.group(2), m.group(3)))
                    n += 1
                    i += 2
                    continue
            out.append(line)
            i += 1
        if n:
            sites += n
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if not a.undo and MARKER not in text:
        # After the load macros it builds on (REX_ADDR_D, REX_LOAD_D_U32).
        anchor = '#define REX_MEMORY_SIZE'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): float_loads: no REX_MEMORY_SIZE in %s: the generated pch changed' % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    # blur-nx (2026-11-13, M-23): the any-mode forms (after the block above, which they build on)
    text = open(pch, encoding='utf-8').read()
    if not a.undo and MARKER_ANY not in text:
        anchor = '#define REX_MEMORY_SIZE'
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS_ANY + anchor, 1))
    text = open(pch, encoding='utf-8').read()
    if not a.undo and a.doubles and MARKER64 not in text:
        anchor = '#define REX_MEMORY_SIZE'
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS64 + anchor, 1))
    if a.doubles:
        print('float_loads (#231, doubles): %d loads %s (%d already; %d kept after a 64-bit integer store to the same '
              'address)' % (dbl_loads, 'to rewrite' if a.check else 'rewritten', dbl_present, dbl_forwarded))
    print('float_loads%s: %d single-precision loads %s in %d files (%d already in that form; %d kept as integers '
          'because a float was just stored to the same address)' %
          (' (check only)' if a.check else '', sites - dbl_loads, 'restored' if a.undo else 'rewritten', changed, present,
           forwarded))
    if not a.undo:
        print('float_loads (M-23, the any-mode forms of CG-13): %d loads and %d stores %s (%d already; %d loads kept '
              'after an integer store to the same address)' % (any_loads, any_stores,
                                                               'to rewrite' if a.check else 'rewritten', any_present,
                                                               any_forwarded))


if __name__ == '__main__':
    main()
