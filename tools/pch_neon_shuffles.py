#!/usr/bin/env python3
"""Constant 4-lane shuffles as one permutation on AArch64 (post-codegen step of the "optimised" codegen flavour;
docs/enhancements.md, "Build-time"). Patches the generated pch only. Works on any ReXGlue port.

The generator writes the VMX word shuffles (vspltw, vpermwi128, vrlimi128, the word rotations ...) as
simde_mm_shuffle_epi32(v, imm) and simde_mm_permute_ps(v, imm): one pshufd / vpermilps on x86. simde's NEON versions
build the result lane by lane:
  - simde_mm_shuffle_epi32: vmovq_n(lane a) then three vsetq_lane(vgetq_lane(...)), which GCC compiles to a DUP and
    three element-to-element INS in a dependency chain (3 cycles each on the Cortex-A57: 12 cycles for what x86 does
    in one) unless all four lanes are the same;
  - simde_mm_permute_ps: a C loop over the four lanes.
The compiler does not see these as a permutation, so it cannot merge them with the permutations around them either
(the byte reversal of a vector load, tools/vec_byteswap.py).

This step redefines both, on AArch64 only, with simde's own generic form (the one it uses for targets without a
native shuffle): SIMDE_SHUFFLE_VECTOR_, GCC's __builtin_shuffle with constant indices. GCC then picks the best single
instruction for each constant (DUP, EXT, REV, ZIP / UZP / TRN, or one TBL with a literal mask) and folds neighbouring
constant permutations into one. The lanes are the same by definition (lane i = a[(imm >> 2i) & 3]):
sdk/tests/vec_byteswap_test.cpp's sibling check is not needed for an index formula simde already uses elsewhere.
REX_SIMDE_SHUFFLES defined keeps simde's NEON versions (the off switch). x86 builds are not touched.

  python tools/pch_neon_shuffles.py <generated pch> [--check]
Idempotent.
"""
import sys

MARKER = '// rexglue ports: constant word shuffles as one permutation on AArch64 (tools/pch_neon_shuffles.py)'
BLOCK = MARKER + '''
#if (defined(__aarch64__) || defined(_M_ARM64)) && defined(SIMDE_SHUFFLE_VECTOR_) && !defined(REX_SIMDE_SHUFFLES)
// sasrt-nx (2026-11-11, port 73; its machine-copy idea 43, B1): the word shuffle in the float lanes. A lane copy has no float
// semantics (the same bits either way), but in the integer lanes the reinterpretation hid a splat (vspltw) from the
// combiner, so a splat feeding vmulfp stayed a `dup` + `fmul` (3,452 such pairs in the copies) instead of one
// `fmul v, v, v.s[k]`. REX_SHUFFLE_INT_DOMAIN: the integer lanes, as before.
#if defined(REX_SHUFFLE_INT_DOMAIN)
#define REX_SHUFFLE_LANES_ i32
#else
#define REX_SHUFFLE_LANES_ f32
#endif
#undef simde_mm_shuffle_epi32
#define simde_mm_shuffle_epi32(a, imm8)                                                        \\
  (__extension__({                                                                             \\
    const simde__m128i_private rex_shuffle_a_ = simde__m128i_to_private(a);                    \\
    simde__m128i_from_private((simde__m128i_private){                                          \\
        .REX_SHUFFLE_LANES_ = SIMDE_SHUFFLE_VECTOR_(32, 16, (rex_shuffle_a_).REX_SHUFFLE_LANES_,                \\
                                     (rex_shuffle_a_).REX_SHUFFLE_LANES_,                       \\
                                     ((imm8)) & 3, ((imm8) >> 2) & 3, ((imm8) >> 4) & 3,       \\
                                     ((imm8) >> 6) & 3)});                                     \\
  }))
#undef simde_mm_permute_ps
#define simde_mm_permute_ps(a, imm8)                                                           \\
  (__extension__({                                                                             \\
    const simde__m128_private rex_permute_a_ = simde__m128_to_private(a);                      \\
    simde__m128_from_private((simde__m128_private){                                            \\
        .f32 = SIMDE_SHUFFLE_VECTOR_(32, 16, (rex_permute_a_).f32, (rex_permute_a_).f32,       \\
                                     ((imm8)) & 3, ((imm8) >> 2) & 3, ((imm8) >> 4) & 3,       \\
                                     ((imm8) >> 6) & 3)});                                     \\
  }))
// sasrt-nx (2026-11-11, port 73; its machine-copy idea 43, B2): vmsum3fp128 / vmsum4fp128 (simde_mm_dp_ps): the same multiply, mask
// and pairwise sums as SIMDe's NEON form ((p0 + p1) + (p2 + p3), every operand in the same order), without the trailing
// `dup`: the second pairwise add already holds the sum in every lane. REX_SIMDE_DP_PS: SIMDe's, as before.
#if !defined(REX_SIMDE_DP_PS)
[[gnu::always_inline]] static inline simde__m128 rex_dp_ps_(simde__m128 a, simde__m128 b, const int imm8) {
  float32x4_t p = vmulq_f32(simde__m128_to_neon_f32(a), simde__m128_to_neon_f32(b));
  if ((imm8 & 0xF0) != 0xF0) {
    const uint32x4_t m = {(imm8 & 0x10) ? ~0u : 0u, (imm8 & 0x20) ? ~0u : 0u, (imm8 & 0x40) ? ~0u : 0u,
                          (imm8 & 0x80) ? ~0u : 0u};
    p = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(p), m));
  }
  p = vpaddq_f32(p, p);
  p = vpaddq_f32(p, p);
  if ((imm8 & 0x0F) != 0x0F) {
    const uint32x4_t m = {(imm8 & 1) ? ~0u : 0u, (imm8 & 2) ? ~0u : 0u, (imm8 & 4) ? ~0u : 0u, (imm8 & 8) ? ~0u : 0u};
    p = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(p), m));
  }
  return simde__m128_from_neon_f32(p);
}
#undef simde_mm_dp_ps
#define simde_mm_dp_ps(a, b, imm8) rex_dp_ps_((a), (b), (imm8))
#endif
#endif
'''
ANCHOR = '#define REX_LOAD_U8(x)'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        sys.exit(__doc__)
    check = '--check' in sys.argv
    text = open(args[0], encoding='utf-8').read()
    if MARKER in text:
        # sasrt-nx (2026-11-11, port 73): an older block of this tool is replaced (it lies before ANCHOR).
        # renut-ultimate (2026-11-17, port 150): only this tool's block, up to the next tool's marker (every block
        # starts with one) or ANCHOR. It took everything up to ANCHOR before: in a tree that had been through the
        # steps once, a block inserted before ANCHOR by a step that runs earlier (guest_fences.py, new in port 150)
        # went with it, and the build then stopped on REX_GUEST_EIEIO undeclared.
        if BLOCK in text:
            print('pch_neon_shuffles: already in %s' % args[0])
            return
        start = text.index(MARKER)
        ends = [text.index(ANCHOR, start)]
        for next_marker in ('\n// rexglue ports: ', '\n// blur-nx: '):
            k = text.find(next_marker, start + len(MARKER))
            if k >= 0:
                ends.append(k + 1)
        text = text[:start] + text[min(ends):]
    if ANCHOR not in text:
        sys.exit('ERROR (post-codegen patch): pch_neon_shuffles: no REX_LOAD_U8 in %s: the generated pch changed'
                 % args[0])
    if not check:
        open(args[0], 'w', encoding='utf-8', newline='\n').write(text.replace(ANCHOR, BLOCK + ANCHOR, 1))
    print('pch_neon_shuffles%s: simde_mm_shuffle_epi32 and simde_mm_permute_ps as constant permutations on AArch64'
          % (' (check only)' if check else ''))


if __name__ == '__main__':
    main()
