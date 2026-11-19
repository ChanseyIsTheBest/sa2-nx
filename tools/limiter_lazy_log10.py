#!/usr/bin/env python3
"""cpu_aggressive (proposed step, right after tools/localize_native.py): the XAudio limiter's localized copy
(app/generated/hot/n_82B6A678.h, f_sub_82B6A678) computes log10 only where its result is used.

The limiter (guest sub_82B6A678, 256 samples per channel per call) does per sample:
    env = (c76 * env + |x|) / c72;   db = log10(env);   if (env > T [r31+12]) gain = pow(10, -(db*20 - T)*c68*0.05)
    else gain = 1.0;  ... smoothing, output.
The dB value (f1 after `bl rex_log10`, then f13 = float(f1) * 20) is read ONLY on the env > T path: on the other
path f13 is reloaded from [r31+52] before any read. On the console (package 21b) the copy is 6.3 % of the running
guest-thread samples, 88 % of them inside the inlined log10 / log (two dependent double divisions), and pow's code
has 1 sample in 13,483: the env > T path is (almost) never taken. So nearly every log10 is dead work.

Rewrite of the copy's loop body (the recompiled original sub_82B6A678, which the hook's guard compares against, is
NOT changed):
    l_f1.f64 = f27.f64;
    if (BANJO_LAZY_LOG10_ON && !(f27 > [r31+12])) { lz.Skip(f27); }          // the same compare as the fcmpu below
    else { lz.Replay(...); <the original call block of f_rex_log10, unchanged> }
and at the function's single exit, before its final write-back: lz.Replay(...).

Why the result is the same (registers, guest memory, flush mode):
  - On the skipped path nothing reads what log10 would have produced before overwriting it: l_f13 is recomputed
    from l_f1 and then reloaded at loc_82B0D1A4; l_f11 / l_f12 are reloaded at loc_82B0D1BC; f0 and cr6 come from
    the reload of [r31+12] and the compare, which do not depend on the call; [r31+12] is not written by log10
    (its only stores are in its own frame below the limiter's r1: the link-register slot r1-8, the back chain at
    r1-96, rex_log's 16(r1) and -16(r1)).
  - What log10 leaves behind (ctx.f1, f3-f13, r6-r10, the four stack words) is reproduced exactly when it can next
    be seen: before a real call (env > T, the pow path, which can read its frame) and at the function's exit,
    lz.Replay runs the skipped calls again, but only the LAST skipped call of each of rex_log's four paths (x == 1,
    inf / nan, x <= 0, the main path), in the order those last calls happened. Each path writes a fixed set of
    registers and stack words whatever x is, and the main path's set contains the other three, so the state after
    replaying those (at most four) calls is the state after the whole skipped sequence. The flush mode is off
    throughout (the loop's checks) and log10 keeps it.
  - The flow uses only the copy's locals and ctx exactly as the call block does (the same hand-over).
Check: the hook's guard (banjo_hot_guest.cpp) runs the copy and the recompiled original from the same inputs and
compares every register and every byte either wrote: with banjo_cpu_verify on the PC every call is compared.
Switch back: BANJO_LAZY_LOG10_ON 0 (a define in the copy's header; the copy is then the old copy) or remove the step.

  python limiter_lazy_log10.py --hot <generated hot folder> [--check]
Idempotent; leaves the file unchanged (and says so) if the generated statements are not the expected ones.
"""
import argparse
import os
import re
import sys

MARK = '// cpu_aggressive: lazy log10 (limiter_lazy_log10.py)'
SUPPORT = MARK + r'''
#ifndef BANJO_LAZY_LOG10_ON
#define BANJO_LAZY_LOG10_ON 1
#endif
// The skipped log10 calls that still matter: the last one of each of rex_log's paths, in the order they happened.
struct LazyLog10 {
  double x[4];
  uint8_t path[4];
  uint32_t n = 0;
  // rex_log (0x821F92A8): x == 1.0 / exponent all ones (inf, nan) / x <= 0 / the main path; the same tests in the same
  // order (fcmpu with 1.0, the 0x7FF0 mask of the top half-word, fcmpu with 0.0 and bgt).
  [[gnu::always_inline]] static uint32_t Path(double v) {
    if (v == 1.0) return 0;
    uint64_t b;
    std::memcpy(&b, &v, 8);
    if (((b >> 48) & 0x7FF0u) == 0x7FF0u) return 1;
    if (!(v > 0.0)) return 2;
    return 3;
  }
  [[gnu::always_inline]] void Skip(double v) {
    const uint32_t p = Path(v);
    if (n && path[n - 1] == p) {  // the common case: the same path as the last skipped call
      x[n - 1] = v;
      return;
    }
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; ++i)
      if (path[i] != p) {
        x[k] = x[i];
        path[k] = path[i];
        ++k;
      }
    x[k] = v;
    path[k] = uint8_t(p);
    n = k + 1;
  }
};
'''
CALL = ('\t{ ctx.f1 = l_f1; ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r8 = l_r8; '
        'ctx.r9 = l_r9; ctx.r10 = l_r10; f_rex_log10(ctx, base, s); if (s.Stopped()) return; l_f1 = ctx.f1; '
        'l_f11 = ctx.f11; l_f12 = ctx.f12; l_f13 = ctx.f13; l_r1 = ctx.r1; l_r8 = ctx.r8; l_r9 = ctx.r9; '
        'l_r10 = ctx.r10; }')
BEFORE = ['\tl_f1.f64 = f27.f64;']
AFTER = ['\tctx.fpscr.disableFlushMode();', '\tl_f13.f64 = double(float(l_f1.f64));',
         '\tf0.f64 = double(REX_LOAD_D_F32(r31.u32, 12));', '\tcr6.compare(f27.f64, f0.f64);',
         '\tl_f13.f64 = double(float(l_f13.f64 * f29.f64));', '\tif (!cr6.gt) goto loc_82B6A758;']
EXIT = '\t{ ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r3 = l_r3; return; }'
# Replay: the call block's own hand-over for each kept skipped call. Only ctx.f1 (the argument) differs from what the
# original handed over at that call; log10 reads no other register (rex_log10 and rex_log read f1 and r1 only), and
# the locals the limiter keeps are not taken back (the original overwrites them before reading them), except r8-r10,
# which the next hand-over passes on.
REPLAY = ('\tif (BANJO_LAZY_LOG10_ON) { for (uint32_t lz_i = 0; lz_i < lz.n; ++lz_i) { ctx.f1.f64 = lz.x[lz_i]; '
          'ctx.f11 = l_f11; ctx.f12 = l_f12; ctx.f13 = l_f13; ctx.r1 = l_r1; ctx.r8 = l_r8; ctx.r9 = l_r9; '
          'ctx.r10 = l_r10; f_rex_log10(ctx, base, s); if (s.Stopped()) return; l_r8 = ctx.r8; l_r9 = ctx.r9; '
          'l_r10 = ctx.r10; } lz.n = 0; }')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--hot', required=True)
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    path = os.path.join(a.hot, 'n_82B6A678.h')
    if not os.path.isfile(path):
        print('limiter_lazy_log10: %s missing: nothing done' % path)
        return
    text = open(path, encoding='utf-8').read()
    if MARK in text:
        print('limiter_lazy_log10: already done')
        return
    head = 'static void f_sub_82B6A678(PPCContext& __restrict ctx, uint8_t* base, [[maybe_unused]] S& s) {\n'
    i = text.find(head)
    j = text.find('\n}\n', i) if i >= 0 else -1
    if i < 0 or j < 0:
        print('limiter_lazy_log10: f_sub_82B6A678 not found: nothing done')
        return
    body = text[i:j].split('\n')
    sites = [k for k, l in enumerate(body) if l == CALL]
    exits = [k for k, l in enumerate(body) if l == EXIT]
    if len(sites) != 1 or len(exits) != 1 or body[sites[0] - 1] != BEFORE[0] or \
            body[sites[0] + 1:sites[0] + 1 + len(AFTER)] != AFTER or 'return;' in ' '.join(body[exits[0] + 1:]) or \
            sum(1 for l in body if 'return;' in l and 'Stopped' not in l) != 1:
        print('limiter_lazy_log10: the statements are not the expected ones (%d call sites, %d exits): nothing done' %
              (len(sites), len(exits)))
        return
    # The exit's replay runs before the epilogue gives r1 back (fixed 2026-10-16: placed after it, the replayed calls
    # built their frames 192 bytes higher, in the caller's frame: the guard's first compare caught a stack word).
    epilogue = None
    for e in range(exits[0] - 1, sites[0], -1):
        if re.match(r'^\tl_r1\.s64 = l_r1\.s64 \+ \d+;$', body[e]):
            epilogue = e
            break
        if body[e].startswith('loc_') or 'goto' in body[e]:
            break
    if epilogue is None:
        print('limiter_lazy_log10: the epilogue that gives r1 back was not found before the exit: nothing done')
        return
    k = sites[0]
    new = body[:k] + [
        '\tif (BANJO_LAZY_LOG10_ON && !(f27.f64 > double(REX_LOAD_D_F32(r31.u32, 12)))) {',
        '\t\tlz.Skip(f27.f64);  // the dB value is read only on the env > T path (loc_82B6A758 overwrites it; the base: loc_82B0D1A0)',
        '\t} else {',
        '\t' + REPLAY,
        '\t' + CALL,
        '\t}'] + body[k + 1:epilogue] + [REPLAY] + body[epilogue:]
    # the state object, after the copy's locals
    p = new.index('\tREX_FUNC_PROLOGUE();')
    new.insert(p + 1, '\t[[maybe_unused]] LazyLog10 lz;')
    text = text[:i] + '\n'.join(new) + text[j:]
    proto = 'template <class S>\nstatic void f_sub_82B6A678(PPCContext& __restrict ctx, uint8_t* base, [[maybe_unused]] S& s);'
    q = text.find(proto)
    if q < 0:
        print('limiter_lazy_log10: prototype not found: nothing done')
        return
    text = text[:q] + SUPPORT + '\n' + text[q:]
    if not a.check:
        open(path, 'w', encoding='utf-8', newline='\n').write(text)
    print('limiter_lazy_log10: f_sub_82B6A678 rewritten (1 call site, 1 exit)')


if __name__ == '__main__':
    main()
