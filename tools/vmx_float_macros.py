#!/usr/bin/env python3
"""renut-ultimate port 72 (2026-11-11): sasrt-nx's tool as it was that day (its post_codegen.py runs it after
flush_hoist.py and vupkd3d_neon.py, before localize_native.py). sasrt-nx (2026-11-11, machine-copy idea 43, F1 and F2): three VMX float operations as macros whose default is the
expression the generator writes today, so a build can switch each to a faster AArch64 form that is NOT bit-identical
(post-codegen step, before tools/localize_native.py).

  vrsqrtefp  simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(X))  -> REX_VRSQRTEFP(X)
  vrefp      simde_mm_div_ps(simde_mm_set1_ps(1), X)                     -> REX_VREFP(X)
  vmaddfp    simde_mm_add_ps(simde_mm_mul_ps(A, B), C)                   -> REX_VMADDFP(A, B, C)
  vnmsubfp   simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(A, B), C), <sign mask>) -> REX_VNMSUBFP(A, B, C)
  vlogefp    D.f32[0..3] = log2f(A.f32[0..3]); (four lines)       -> simde_mm_store_ps(D.f32, REX_VLOGEFP(simde_mm_load_ps(A.f32)))

sasrt-nx (2026-11-11 ~10:00, the user: "add these please"): vnmsubfp and vlogefp too. vnmsubfp is vmaddfp's mirror: the
360 computes -(a*b - c) with one rounding, the port multiplied and subtracted (two); fused: -(fma(a, b, -c)) (the same
-0 as before where a*b == c, as the 360's). vlogefp is an estimate on the 360 (the AltiVec bound: 1/32 absolute); the
port called log2f four times an instruction; the fast form is a NEON log2 (exponent + a degree-7 polynomial of the
mantissa reduced to [sqrt(0.5), sqrt(2)): ~1.2e-7 absolute, exact at powers of two; zero and denormals (read as zero in
the flush mode, as the 360's VMX does) -inf, negatives NaN, +inf +inf, NaN kept).

The port computes the estimates exactly (`fsqrt` + `fdiv`, neither pipelined on the Cortex-A57; 32 of WaterThread's
207 samples in sub_8286C230) and the multiply-add with two roundings (`fmul` + `fadd`; -ffp-contract=off). The 360's
vrsqrtefp / vrefp are estimates themselves and its vmaddfp rounds once, so the faster forms are not further from the
console than today's, but they are not today's results:
  - REX_VMX_FAST_ESTIMATES (CMake SASRT_VMX_FAST_ESTIMATES, ON since 2026-11-11, the user's call): `frsqrte` /
    `frecpe` and two Newton steps
    (`frsqrts` / `frecps`);
  - REX_VMX_FUSED_MADD (CMake SASRT_VMX_FUSED_MADD, ON since 2026-11-11, the user's call): one `fmla` (vmaddfp), and
    -(fma(a, b, -c)) for vnmsubfp; REX_VMX_FAST_ESTIMATES also gives vlogefp's NEON estimate.
Without them the macros are the expressions above (the same code as before). sasrt_test_vmx_fuzz (the app) compares
each fast form with the exact one on the console itself (x86 cannot reproduce NEON's estimates).

  python tools/vmx_float_macros.py --gen app/generated/default [--check]
Idempotent (the rewritten text no longer matches). No restore: the macros' defaults are the expressions themselves.
"""
import argparse
import glob
import os
import re
import sys

MARKER = '// rexglue ports: VMX float estimates and multiply-add as macros (tools/vmx_float_macros.py)'
END = '// (end of tools/vmx_float_macros.py)\n'
MACROS = MARKER + '''
#ifndef REX_VRSQRTEFP
#if (defined(__aarch64__) || defined(_M_ARM64))
#include <arm_neon.h>
#define REX_VMX_FLOAT_HELPERS 1  // the fast forms below exist (sasrt_test_vmx_fuzz tests them)
[[gnu::always_inline]] static inline simde__m128 rex_vrsqrtefp_fast_(simde__m128 x) {
  const float32x4_t v = simde__m128_to_neon_f32(x);
  const float32x4_t e0 = vrsqrteq_f32(v);
  float32x4_t e = vmulq_f32(e0, vrsqrtsq_f32(vmulq_f32(v, e0), e0));
  e = vmulq_f32(e, vrsqrtsq_f32(vmulq_f32(v, e), e));
  // (Batch 3's audit, finding 5: +-0 -> +-inf and +inf -> +0 come from the estimate itself; the Newton steps would
  // turn them into NaN through 0 * inf.)
  const uint32x4_t special = vorrq_u32(vceqq_f32(e0, vdupq_n_f32(0.0f)),
                                       vceqq_f32(vabsq_f32(e0), vdupq_n_f32(__builtin_inff())));
  return simde__m128_from_neon_f32(vbslq_f32(special, e0, e));
}
[[gnu::always_inline]] static inline simde__m128 rex_vrefp_fast_(simde__m128 x) {
  const float32x4_t v = simde__m128_to_neon_f32(x);
  float32x4_t e = vrecpeq_f32(v);
  e = vmulq_f32(e, vrecpsq_f32(v, e));
  e = vmulq_f32(e, vrecpsq_f32(v, e));
  return simde__m128_from_neon_f32(e);
}
[[gnu::always_inline]] static inline simde__m128 rex_vmaddfp_fused_(simde__m128 a, simde__m128 b, simde__m128 c) {
  return simde__m128_from_neon_f32(
      vfmaq_f32(simde__m128_to_neon_f32(c), simde__m128_to_neon_f32(a), simde__m128_to_neon_f32(b)));
}
// -(a*b - c), one rounding (the 360's vnmsubfp); -0 where a*b == c, as the unfused form and the 360 give.
[[gnu::always_inline]] static inline simde__m128 rex_vnmsubfp_fused_(simde__m128 a, simde__m128 b, simde__m128 c) {
  return simde__m128_from_neon_f32(vnegq_f32(vfmaq_f32(vnegq_f32(simde__m128_to_neon_f32(c)),
                                                       simde__m128_to_neon_f32(a), simde__m128_to_neon_f32(b))));
}
// log2 estimate: the exponent plus log2 of the mantissa reduced to [sqrt(0.5), sqrt(2)) as t * q(t), t = m - 1, q of
// degree 7 (a minimax fit, ~1.2e-7 absolute in float32; 0 at t = 0, so powers of two are exact). The 360's vlogefp is
// an estimate good to 1/32. Zero and denormals (the flush mode reads them as zero) -> -inf, negatives -> NaN,
// +inf -> +inf, a NaN -> itself made quiet.
[[gnu::always_inline]] static inline simde__m128 rex_vlogefp_fast_(simde__m128 x) {
  const float32x4_t v = simde__m128_to_neon_f32(x);
  const uint32x4_t bits = vreinterpretq_u32_f32(v);
  const uint32x4_t expo = vandq_u32(vshrq_n_u32(bits, 23), vdupq_n_u32(0xFFu));
  float32x4_t m = vreinterpretq_f32_u32(vorrq_u32(vandq_u32(bits, vdupq_n_u32(0x007FFFFFu)), vdupq_n_u32(0x3F800000u)));
  const uint32x4_t big = vcgtq_f32(m, vdupq_n_f32(1.41421356f));
  m = vbslq_f32(big, vmulq_n_f32(m, 0.5f), m);
  const int32x4_t e = vsubq_s32(vsubq_s32(vreinterpretq_s32_u32(expo), vdupq_n_s32(127)), vreinterpretq_s32_u32(big));
  const float32x4_t t = vsubq_f32(m, vdupq_n_f32(1.0f));
  float32x4_t p = vdupq_n_f32(-1.457423121e-01f);
  p = vfmaq_f32(vdupq_n_f32(2.368895411e-01f), p, t);
  p = vfmaq_f32(vdupq_n_f32(-2.500694096e-01f), p, t);
  p = vfmaq_f32(vdupq_n_f32(2.867075801e-01f), p, t);
  p = vfmaq_f32(vdupq_n_f32(-3.600871861e-01f), p, t);
  p = vfmaq_f32(vdupq_n_f32(4.809394479e-01f), p, t);
  p = vfmaq_f32(vdupq_n_f32(-7.213571668e-01f), p, t);
  p = vfmaq_f32(vdupq_n_f32(1.442694783e+00f), p, t);
  float32x4_t r = vaddq_f32(vcvtq_f32_s32(e), vmulq_f32(p, t));
  const uint32x4_t zero = vceqq_u32(expo, vdupq_n_u32(0u));
  const uint32x4_t neg = vtstq_u32(bits, vdupq_n_u32(0x80000000u));
  const uint32x4_t top = vceqq_u32(expo, vdupq_n_u32(0xFFu));
  const uint32x4_t nan = vmvnq_u32(vceqq_f32(v, v));
  r = vbslq_f32(zero, vdupq_n_f32(-__builtin_inff()), r);
  r = vbslq_f32(vbicq_u32(neg, zero), vreinterpretq_f32_u32(vdupq_n_u32(0x7FC00000u)), r);
  r = vbslq_f32(vbicq_u32(top, neg), v, r);
  r = vbslq_f32(nan, vreinterpretq_f32_u32(vorrq_u32(bits, vdupq_n_u32(0x00400000u))), r);
  return simde__m128_from_neon_f32(r);
}
#endif
// vlogefp as the port computed it before (log2f lane by lane): the macro's default on every target.
static inline simde__m128 rex_vlogefp_exact_(simde__m128 x) {
  alignas(16) float f[4];
  simde_mm_store_ps(f, x);
  for (int i = 0; i < 4; ++i)
    f[i] = log2f(f[i]);
  return simde_mm_load_ps(f);
}
#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(REX_VMX_FAST_ESTIMATES)
#define REX_VRSQRTEFP(x) rex_vrsqrtefp_fast_(x)
#define REX_VREFP(x) rex_vrefp_fast_(x)
#define REX_VLOGEFP(x) rex_vlogefp_fast_(x)
#else
#define REX_VRSQRTEFP(x) simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(x))
#define REX_VREFP(x) simde_mm_div_ps(simde_mm_set1_ps(1), x)
#define REX_VLOGEFP(x) rex_vlogefp_exact_(x)
#endif
#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(REX_VMX_FUSED_MADD)
#define REX_VMADDFP(a, b, c) rex_vmaddfp_fused_(a, b, c)
#define REX_VNMSUBFP(a, b, c) rex_vnmsubfp_fused_(a, b, c)
#else
#define REX_VMADDFP(a, b, c) simde_mm_add_ps(simde_mm_mul_ps(a, b), c)
#define REX_VNMSUBFP(a, b, c) simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(a, b), c), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))))
#endif
#endif
''' + END
ANCHOR = '#define REX_LOAD_U8(x)'


def args_of(text, i):
    """The comma-separated top-level arguments of the call whose '(' is text[i]; (list, index after ')')."""
    depth, start, out = 0, i + 1, []
    for k in range(i, len(text)):
        ch = text[k]
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
            if depth == 0:
                out.append(text[start:k].strip())
                return out, k + 1
        elif ch == ',' and depth == 1:
            out.append(text[start:k].strip())
            start = k + 1
        elif ch in ';\n':
            return None, i
    return None, i


def rewrite(line, counts):
    # vrsqrtefp first (its inner form is also a vrefp's).
    for name, head, build in (
            ('rsqrte', 'simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(', None),
            ('re', 'simde_mm_div_ps(simde_mm_set1_ps(1), ', None),
            ('madd', 'simde_mm_add_ps(simde_mm_mul_ps(', None),
            ('nmsub', 'simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(', None)):
        pos = 0
        while True:
            i = line.find(head, pos)
            if i < 0:
                break
            if name == 'rsqrte':
                inner, end = args_of(line, i + len(head) - 1)  # the sqrt's argument
                outer_ok = inner is not None and len(inner) == 1 and line[end:end + 1] == ')'
                if outer_ok:
                    line = line[:i] + 'REX_VRSQRTEFP(' + inner[0] + ')' + line[end + 1:]
                    counts[name] += 1
                    pos = i + 1
                    continue
            elif name == 're':
                a, end = args_of(line, i + len('simde_mm_div_ps'))
                if a is not None and len(a) == 2 and a[0] == 'simde_mm_set1_ps(1)':
                    line = line[:i] + 'REX_VREFP(' + a[1] + ')' + line[end:]
                    counts[name] += 1
                    pos = i + 1
                    continue
            elif name == 'nmsub':
                a, end = args_of(line, i + len('simde_mm_xor_ps'))
                if a is not None and len(a) == 2 and a[1] == SIGN_MASK and a[0].startswith('simde_mm_sub_ps('):
                    sb, send = args_of(a[0], len('simde_mm_sub_ps'))
                    if sb is not None and len(sb) == 2 and send == len(a[0]) and sb[0].startswith('simde_mm_mul_ps('):
                        m, mend = args_of(sb[0], len('simde_mm_mul_ps'))
                        if m is not None and len(m) == 2 and mend == len(sb[0]):
                            line = line[:i] + 'REX_VNMSUBFP(%s, %s, %s)' % (m[0], m[1], sb[1]) + line[end:]
                            counts[name] += 1
                            pos = i + 1
                            continue
            else:
                a, end = args_of(line, i + len('simde_mm_add_ps'))
                if a is not None and len(a) == 2 and a[0].startswith('simde_mm_mul_ps('):
                    m, mend = args_of(a[0], len('simde_mm_mul_ps'))
                    if m is not None and len(m) == 2 and mend == len(a[0]):
                        line = line[:i] + 'REX_VMADDFP(%s, %s, %s)' % (m[0], m[1], a[1]) + line[end:]
                        counts[name] += 1
                        pos = i + 1
                        continue
            pos = i + 1
    return line


SIGN_MASK = 'simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000)))'
LOG_LANE = re.compile(r'^(\s*)(\S+)\.f32\[(\d)\] = log2f\((\S+)\.f32\[(\d)\]\);$')


def vlogefp_lines(lines, counts):
    """vlogefp's four lane lines (D.f32[k] = log2f(A.f32[k]); k = 0..3, D and A the same in all four) as one store."""
    out, j = [], 0
    while j < len(lines):
        ms = [LOG_LANE.match(lines[j + k]) if j + k < len(lines) else None for k in range(4)]
        if all(ms) and all(m.group(3) == m.group(5) == str(k) for k, m in enumerate(ms)) and \
                len({(m.group(1), m.group(2), m.group(4)) for m in ms}) == 1:
            ind, d, a = ms[0].group(1), ms[0].group(2), ms[0].group(4)
            out.append('%ssimde_mm_store_ps(%s.f32, REX_VLOGEFP(simde_mm_load_ps(%s.f32)));' % (ind, d, a))
            counts['loge'] += 1
            j += 4
            continue
        out.append(lines[j])
        j += 1
    return out


RESTORE = [
    (re.compile(r'REX_VRSQRTEFP\('), 'simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps('),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', required=True)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--restore', action='store_true')
    a = ap.parse_args()
    pchs = glob.glob(os.path.join(a.gen, '*_pch.h'))
    if len(pchs) != 1:
        sys.exit('ERROR (post-codegen patch): vmx_float_macros: no single *_pch.h in %s' % a.gen)
    if a.restore:
        sys.exit('vmx_float_macros: --restore is a codegen away (tools/codegen.ps1): the macros\' defaults are the '
                 'expressions themselves, so nothing needs restoring for a plain build')
    counts = {'rsqrte': 0, 're': 0, 'madd': 0, 'nmsub': 0, 'loge': 0}
    changed = 0
    for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
        text = open(path, encoding='utf-8').read()
        if ('simde_mm_set1_ps(1)' not in text and 'simde_mm_add_ps(simde_mm_mul_ps(' not in text and
                'simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(' not in text and 'log2f(' not in text):
            continue
        lines = [rewrite(l, counts) if ('simde_mm_div_ps(' in l or 'simde_mm_add_ps(' in l or 'simde_mm_xor_ps(' in l)
                 else l for l in text.split('\n')]
        new = '\n'.join(vlogefp_lines(lines, counts) if 'log2f(' in text else lines)
        if new != text:
            changed += 1
            if not a.check:
                open(path, 'w', encoding='utf-8', newline='\n').write(new)
    pch = pchs[0]
    text = open(pch, encoding='utf-8').read()
    if MARKER in text:
        start = text.index(MARKER)
        text = text[:start] + text[text.index(END, start) + len(END):]
    if ANCHOR not in text:
        sys.exit('ERROR (post-codegen patch): vmx_float_macros: no %s in %s' % (ANCHOR, pch))
    text = text.replace(ANCHOR, MACROS + ANCHOR, 1)
    if not a.check and text != open(pch, encoding='utf-8').read():
        open(pch, 'w', encoding='utf-8', newline='\n').write(text)
    print('vmx_float_macros: %d vrsqrtefp, %d vrefp, %d vmaddfp, %d vnmsubfp, %d vlogefp as macros in %d files%s' % (
        counts['rsqrte'], counts['re'], counts['madd'], counts['nmsub'], counts['loge'], changed,
        ' (check only)' if a.check else ''))


if __name__ == '__main__':
    main()
