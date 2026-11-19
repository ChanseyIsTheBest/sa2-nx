/**
 * @file        fpu_fma.h
 * @brief       fnmadd / fnmsub (and their single forms): the negation of a fused multiply-add with the sign of zero
 *              and NaN the PowerPC gives
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * renut-ultimate (2026-11-16, port 144; tw2de_nx's fpu_fma.h of 2026-11-01): fnmadd is -(a * c + b) and fnmsub
 * -(a * c - b), and a NaN result keeps its sign (it is not negated). The generated code is -std::fma(a, c, b) and
 * -std::fma(a, c, -b) (and (-REX_FMADDS(...)) for the single forms, tools/fma_single.py). GCC (devkitA64) folds that
 * negation into the AArch64 instructions FNMADD (-(a * c) - b) and FMSUB (b - a * c), which are not the same on zeros:
 * with a * c = +0 and b = +0, fnmsub gives -0 on the PowerPC and FMSUB gives +0 (and fnmadd with b = -0 the same way).
 * Xenia's PowerPC instruction tests (results recorded on the hardware) showed 80 fnmadd and 80 fnmsub cases giving +0
 * where the hardware gives -0. FusedNeg keeps the fused result in its register (an empty asm with the value as
 * operand: the compiler cannot fold across it) and then negates it, unless it is NaN.
 *
 * Used through REX_FNEG_FUSED (tools/fnm_sign.py), whose default is the plain negation; the CMake option
 * TW2DE_FNMADD_SIGN (REX_FNMADD_SIGN_EXACT, off by default) selects FusedNeg.
 */
#pragma once

namespace rex::ppc {

namespace fpu_fma {
[[gnu::always_inline]] inline double keep(double t) {
#if defined(__aarch64__) || defined(_M_ARM64)
  __asm__("" : "+w"(t));
#elif defined(__x86_64__) || defined(__i386__)
  __asm__("" : "+x"(t));
#endif
  return t;
}
}  // namespace fpu_fma

// -t for a fused multiply-add's result t, with no fold into FNMADD / FMSUB; a NaN is returned as it is.
[[gnu::always_inline]] inline double FusedNeg(double t) {
  t = fpu_fma::keep(t);
  return t != t ? t : -t;
}

}  // namespace rex::ppc
