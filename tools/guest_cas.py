#!/usr/bin/env python3
"""Guest compare-and-swap without the legacy builtin's trailing barrier (post-codegen step of the "optimised" codegen
flavour; docs/enhancements.md, "Build-time"). Works on any ReXGlue port's generated code.

The generator writes a `stwcx.` / `stdcx.` as `__sync_bool_compare_and_swap(...)`. On AArch64 without LSE (the
Switch's Cortex-A57) GCC compiles that legacy builtin as a load-exclusive / store-exclusive loop FOLLOWED BY
`dmb ish`, because the __sync builtins promise a full barrier (GCC PR 65697). A `dmb` drains the store buffer: tens of
cycles on an A57, after every interlocked increment, decrement and exchange the game does (reference counts, lock
words, lock-free lists).

The guest never had that barrier: on PowerPC lwarx/stwcx. order nothing, and where the game needs ordering it has its
own lwsync / sync / eieio, which the generator translates separately (std::atomic_thread_fence). So this step
rewrites the call into REX_GUEST_CAS(...), which the generated pch defines as __atomic_compare_exchange_n with
sequentially consistent ordering: the same loop with acquire/release exclusives (ldaxr / stlxr) and no trailing
barrier. That is still stronger than what the PowerPC code had.

  - REX_GUEST_CAS_LEGACY defined: the old builtin (build-time off switch).
  - On AArch64 the byte g_rex_atomic_legacy_barrier (defined by the runtime, sdk/src/core/atomic_posix.cpp; the app
    sets it from its key) puts the `dmb ish` back at run time: the A/B on the console without a second build.
  - On x86 both forms are `lock cmpxchg`: the PC build tests the rewritten expression, not the barrier.

  python tools/guest_cas.py --gen app/generated/default [--check] [--undo]
Idempotent.
"""
import argparse
import os
import re
import sys

OLD = '__sync_bool_compare_and_swap('
NEW = 'REX_GUEST_CAS('
MARKER = '// rexglue ports: guest compare-and-swap without the trailing barrier (tools/guest_cas.py)'
MACROS = MARKER + '''
#ifndef REX_GUEST_CAS
#if defined(REX_GUEST_CAS_LEGACY)
#define REX_GUEST_CAS(p, e, d) __sync_bool_compare_and_swap((p), (e), (d))
#else
#if defined(__aarch64__)
extern "C" unsigned char g_rex_atomic_legacy_barrier;
#endif
#if defined(REX_NX_CENSUS_ON)
// blur-nx (2026-11-13, #32 of Blur's docs/cpu-near-exact-50-1113.md): the PC census of which threads use each word a
// guest compare-and-swap writes (app/src/cpu/banjo_codegen_checks.cpp).
extern "C" void RexNxCasCensus(const void* word);
#endif
#if defined(__aarch64__)
// blur-nx (2026-11-16, #163 of docs/cpu-codegen-recheck-360-precision-1114.md): 1 = the sequentially consistent loop
// (ldaxr / stlxr) instead of the relaxed one (sdk/src/core/atomic_posix.cpp; the app's key blur_guest_cas_relaxed).
extern "C" unsigned char g_rex_guest_cas_seq_cst;
#endif
#ifndef REX_GUEST_CAS_ORDER
// renut-ultimate (2026-11-17, port 150): the order of the relaxed form, written here by tools/guest_cas.py from the
// tree. Relaxed for a game with no isync. Acquire (ldaxr / stxr) for one with any: the PowerPC acquire idiom
// `lwarx; stwcx.; bne-; isync` orders every later access after the lwarx, and the generator writes isync as nothing,
// so the exclusive load must give that order itself. (lwsync, sync and eieio are fences of their own: guest_fences.py.)
#define REX_GUEST_CAS_ORDER @ORDER@
#endif
template <typename T, typename E, typename D>
[[gnu::always_inline]] static inline bool RexGuestCas(T* p, E e, D d) noexcept {
#if defined(REX_NX_CENSUS_ON)
  RexNxCasCensus(static_cast<const void*>(p));
#endif
  T expected = static_cast<T>(e);
#if defined(__aarch64__) && !defined(REX_GUEST_CAS_SEQ_CST)
  // blur-nx (2026-11-16, #163): on the 360 lwarx / stwcx. order no other access; the game orders with its own
  // lwsync / sync / eieio (REX_GUEST_LWSYNC and kin, tools/guest_fences.py; Blur has no isync), so the exclusives
  // need no acquire or release of their own: ldxr / stxr. Exact with respect to the 360's memory model.
  // renut-ultimate (2026-11-17): REX_GUEST_CAS_ORDER, acquire in a game with isync (above).
  if (!g_rex_guest_cas_seq_cst) [[likely]]
    return __atomic_compare_exchange_n(p, &expected, static_cast<T>(d), false, REX_GUEST_CAS_ORDER, __ATOMIC_RELAXED);
#endif
  const bool swapped = __atomic_compare_exchange_n(p, &expected, static_cast<T>(d), false, __ATOMIC_SEQ_CST,
                                                   __ATOMIC_SEQ_CST);
#if defined(__aarch64__)
  if (g_rex_atomic_legacy_barrier) [[unlikely]] __asm__ __volatile__("dmb ish" ::: "memory");
#endif
  return swapped;
}
#define REX_GUEST_CAS(p, e, d) RexGuestCas((p), (e), (d))
#endif
#endif
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True, help='generated folder (*_recomp.N.cpp and *_pch.h)')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--undo', action='store_true')
    a = ap.parse_args()
    files = sorted(f for f in os.listdir(a.gen) if re.match(r'\w+_recomp\.\d+\.cpp$', f))
    pchs = [f for f in os.listdir(a.gen) if f.endswith('_pch.h')]
    if not files or len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): guest_cas: no *_recomp.N.cpp or no single *_pch.h in %s' % a.gen)
    old, new = (NEW, OLD) if a.undo else (OLD, NEW)
    sites = present = changed = 0
    for f in files:
        path = os.path.join(a.gen, f)
        text = open(path, encoding='utf-8').read()
        n = text.count(old)
        present += text.count(new)
        if n:
            sites += n
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(text.replace(old, new))
    # renut-ultimate (2026-11-17, port 150): the game's isync instructions (the generator's comment line before each
    # statement; isync's statement is nothing) decide the relaxed form's order (REX_GUEST_CAS_ORDER above).
    isyncs = sum(len(re.findall(r'(?m)^\s*// isync\s*$', open(os.path.join(a.gen, f), encoding='utf-8').read()))
                 for f in files)
    block = MACROS.replace('@ORDER@', '__ATOMIC_ACQUIRE' if isyncs else '__ATOMIC_RELAXED')
    pch = os.path.join(a.gen, pchs[0])
    text = open(pch, encoding='utf-8').read()
    if not a.undo and block not in text:
        if MARKER in text:
            # An older form of the block (or the other order) in place of, as loop_locals.py does with its own.
            start = text.index(MARKER)
            tail = '#define REX_GUEST_CAS(p, e, d) RexGuestCas((p), (e), (d))\n#endif\n#endif\n'
            end = text.index(tail, start) + len(tail)
            new_text = text[:start] + block + text[end:]
            print('guest_cas: the macros in %s were replaced by the current ones' % pch)
        else:
            anchor = '#define REX_LOAD_U8(x)'
            if anchor not in text:
                sys.exit('ERROR (post-codegen patch): guest_cas: no REX_LOAD_U8 in %s: the generated pch changed' % pch)
            new_text = text.replace(anchor, block + anchor, 1)
        if not a.check:
            open(pch, 'w', encoding='utf-8', newline='\n').write(new_text)
    print('guest_cas%s: %d compare-and-swap sites %s in %d files (%d already in that form); %d isync: the relaxed '
          'form is %s' % (' (check only)' if a.check else '', sites, 'restored' if a.undo else 'rewritten', changed,
                          present, isyncs, 'acquire (ldaxr / stxr)' if isyncs else 'relaxed (ldxr / stxr)'))


if __name__ == '__main__':
    main()
