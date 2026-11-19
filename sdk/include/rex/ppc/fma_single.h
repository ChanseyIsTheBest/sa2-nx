// blur-nx (2026-11-12, the user: "Please do the fmadds change"): the single-precision multiply-adds (fmadds, fmsubs,
// fnmadds, fnmsubs) rounded once, as the Xenon does, instead of twice.
//
// The Xenon computes a * b + c exactly and rounds the result once to single precision. The generated code computed it in
// double (std::fma: rounded to 53 bits) and then rounded that to single: a double rounding, which in rare cases (the
// double result exactly halfway between two floats while the exact one was not; for random inputs about one operation in
// a few hundred million) is one unit in the last place off the Xenon's. It also kept every single-precision chain in
// double registers: an fcvt before and two after each such operation.
//
// FmaddsSingle: when all three inputs are exact floats (the case for values that come from lfs and from single-precision
// operations), one single-precision fused multiply-add (fmaf: one `fmadd s` on the AArch64), which is the Xenon's result
// exactly; GCC folds the test away where it sees the inputs' origin (a chain of single-precision statements then stays
// in float registers). An input that is not a float (a double value in the register) keeps the double form, as before.
// REX_FMADDS_DOUBLE_ROUNDING (compile time; CMake BLUR_FMADDS_SINGLE=OFF): the double form everywhere, as before.
//
// Checked on the device by the app's forms self-check against a quad-precision reference (long double).
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace rex::ppc {

[[gnu::always_inline]] inline bool IsExactSingle(double x) {
  return std::bit_cast<uint64_t>(double(float(x))) == std::bit_cast<uint64_t>(x);
}

// The double form the generator wrote (two roundings).
[[gnu::always_inline]] inline double FmaddsDouble(double a, double b, double c) { return double(float(std::fma(a, b, c))); }

// a * b + c rounded once to single precision when a, b and c are floats; otherwise the double form.
[[gnu::always_inline]] inline double FmaddsSingle(double a, double b, double c) {
#if !defined(REX_FMADDS_DOUBLE_ROUNDING)
  if (__builtin_expect(IsExactSingle(a) & IsExactSingle(b) & IsExactSingle(c), 1))
    return double(std::fmaf(float(a), float(b), float(c)));
#endif
  return FmaddsDouble(a, b, c);
}

// blur-nx (2026-11-13, M-24 of docs/cpu-moonshots-50-1113.md): the fused forms with the three inputs proven single by
// tools/single_floats.py (a forward must-analysis over the guest code: CG-14's): FmaddsSingle's test is always true
// there, so only its true branch (one single-precision fused multiply-add). REX_FMADDS_DOUBLE_ROUNDING: the double
// form, as FmaddsSingle gives then.
[[gnu::always_inline]] inline double FmaddsSingleKnown(double a, double b, double c) {
#if !defined(REX_FMADDS_DOUBLE_ROUNDING)
  return double(__builtin_fmaf(float(a), float(b), float(c)));
#else
  return double(float(__builtin_fma(a, b, c)));
#endif
}

}  // namespace rex::ppc
