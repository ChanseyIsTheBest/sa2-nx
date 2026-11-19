#!/usr/bin/env python3
"""blur-nx (2026-11-16, the 15 ms batch, #164 / #165 of docs/cpu-codegen-recheck-360-precision-1114.md): the guest's
sync, lwsync and eieio fences with the PowerPC's own orderings (a post-codegen step, before every tool that reads
the statements).

The generator writes every fence as a C++ fence: sync and eieio `std::atomic_thread_fence(std::memory_order_seq_cst)`,
lwsync `std::atomic_thread_fence(std::memory_order_acq_rel)`. On AArch64 GCC makes all three one full `dmb ish`, which
also orders a store before a later load: the one ordering lwsync does not give, and the costly one (it drains the
store buffer). What the PowerPC fences order:

  - sync: everything. `dmb ish`, as before.
  - lwsync: load-load, load-store, store-store (not store-load). `dmb ishld` (a load before any later access) plus
    `dmb ishst` (a store before a later store): exactly those three.
  - eieio, on cacheable memory: stores before later stores. `dmb ishst`. (The eieio of an MMIO access sequence is the
    generator's MMIO path, not one of these fences.)

So each is exact with respect to the 360's memory model. The x86 PC build keeps the C++ fences (x86 orders all but
store-load by itself). On AArch64 the byte g_rex_guest_fence_full (sdk/src/core/atomic_posix.cpp; the app's key
blur_guest_fences_split false sets it) gives lwsync and eieio the full `dmb ish` back at run time: the A/B on the
console without a second build.

The generator writes each instruction's text as a comment on the line before its statement (`// lwsync`); that is how
the three are told apart (sync and eieio are the same statement). Rewrites:
  // sync    + std::atomic_thread_fence(std::memory_order_seq_cst);  -> REX_GUEST_SYNC();
  // lwsync  + std::atomic_thread_fence(std::memory_order_acq_rel);  -> REX_GUEST_LWSYNC();
  // eieio   + std::atomic_thread_fence(std::memory_order_seq_cst);  -> REX_GUEST_EIEIO();
tools/localize_native.py knows the three macros as fences (the guard's stop in front, the card run flushed).

  python tools/guest_fences.py --gen app/generated [--check] [--undo]
Idempotent.
"""
import argparse
import os
import re
import sys

FENCES = {
    'sync': ('std::atomic_thread_fence(std::memory_order_seq_cst);', 'REX_GUEST_SYNC();'),
    'lwsync': ('std::atomic_thread_fence(std::memory_order_acq_rel);', 'REX_GUEST_LWSYNC();'),
    'eieio': ('std::atomic_thread_fence(std::memory_order_seq_cst);', 'REX_GUEST_EIEIO();'),
}
COMMENT = re.compile(r'^\s*// (sync|lwsync|eieio)\s*$')
MARKER = '// rexglue ports: guest fences with the PowerPC orderings (tools/guest_fences.py)'
MACROS = MARKER + '''
#ifndef REX_GUEST_LWSYNC
#if defined(REX_GUEST_FENCES_LEGACY) || !defined(__aarch64__)
#define REX_GUEST_SYNC() std::atomic_thread_fence(std::memory_order_seq_cst)
#define REX_GUEST_LWSYNC() std::atomic_thread_fence(std::memory_order_acq_rel)
#define REX_GUEST_EIEIO() std::atomic_thread_fence(std::memory_order_release)
#else
extern "C" unsigned char g_rex_guest_fence_full;  // 1: lwsync and eieio as a full dmb ish (sdk/src/core/atomic_posix.cpp)
#define REX_GUEST_SYNC() __asm__ __volatile__("dmb ish" ::: "memory")
#define REX_GUEST_LWSYNC()                                                          \\
  do {                                                                              \\
    if (g_rex_guest_fence_full) [[unlikely]]                                        \\
      __asm__ __volatile__("dmb ish" ::: "memory");                                 \\
    else                                                                            \\
      __asm__ __volatile__("dmb ishld\\n\\tdmb ishst" ::: "memory");                  \\
  } while (0)
#define REX_GUEST_EIEIO()                                                           \\
  do {                                                                              \\
    if (g_rex_guest_fence_full) [[unlikely]]                                        \\
      __asm__ __volatile__("dmb ish" ::: "memory");                                 \\
    else                                                                            \\
      __asm__ __volatile__("dmb ishst" ::: "memory");                               \\
  } while (0)
#endif
#endif
'''


def rewrite(text, undo):
    lines = text.split('\n')
    counts = {'sync': 0, 'lwsync': 0, 'eieio': 0}
    for i in range(1, len(lines)):
        m = COMMENT.match(lines[i - 1])
        if not m:
            continue
        kind = m.group(1)
        old, new = FENCES[kind]
        if undo:
            old, new = new, old
        stripped = lines[i].strip()
        if stripped == old:
            lines[i] = lines[i][:len(lines[i]) - len(lines[i].lstrip())] + new
            counts[kind] += 1
    return '\n'.join(lines), counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--undo', action='store_true')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): guest_fences: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    total = {'sync': 0, 'lwsync': 0, 'eieio': 0}
    changed = 0
    for f in files:
        path = os.path.join(a.gen, f)
        text = open(path, encoding='utf-8').read()
        if 'atomic_thread_fence' not in text and 'REX_GUEST_' not in text:
            continue
        new, counts = rewrite(text, a.undo)
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
        for k in total:
            total[k] += counts[k]
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if not a.undo and MARKER not in text:
        anchor = '#define REX_LOAD_U8(x)'
        if anchor not in text:
            sys.exit('ERROR (post-codegen patch): guest_fences: no REX_LOAD_U8 in %s: the generated pch changed' % pch)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(text.replace(anchor, MACROS + anchor, 1))
    print('guest_fences%s: %d sync, %d lwsync, %d eieio %s in %d files' % (
        ' (check only)' if a.check else '', total['sync'], total['lwsync'], total['eieio'],
        'restored' if a.undo else 'rewritten', changed))


if __name__ == '__main__':
    main()
