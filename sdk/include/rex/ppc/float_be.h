/**
 * blur-nx (2026-11-13, M-23 of docs/cpu-moonshots-50-1113.md): a guest single-precision value stored big-endian from
 * the vector unit. The generated code's single stores (stfs) go `fmov w, s; rev w; str w`: the move from the vector
 * unit to the general registers is the slow step on the Cortex-A57 (5 cycles). Swapped in the vector unit and stored
 * from there it is `rev32 v.8b; str s`: the same four bytes. Used by tools/float_loads.py's REX_STORE_*F32ANY forms
 * (with CG-13 on: REX_FPCR_ONE_MODE) and checked against the integer form by app/src/cpu/banjo_forms_selfcheck.cpp.
 * Other targets: the integer form.
 */
#pragma once

#include <cstdint>
#include <cstring>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace rex::ppc {

[[gnu::always_inline]] inline void FloatToBigEndian(void* p, float f) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  const float32x2_t v = vset_lane_f32(f, vdup_n_f32(0.0f), 0);
  const float s = vget_lane_f32(vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_f32(v))), 0);  // rev32
  std::memcpy(p, &s, 4);                                                                   // str s
#else
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(p, &u, 4);
#endif
}

// The load's twin (tools/float_loads.py's RexFloatFromBigEndian, in the generated pch): for the self-check.
[[gnu::always_inline]] inline float FloatFromBigEndian(const void* p) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  float f;
  std::memcpy(&f, p, 4);  // ldr s
  const float32x2_t v = vset_lane_f32(f, vdup_n_f32(0.0f), 0);
  return vget_lane_f32(vreinterpret_f32_u8(vrev32_u8(vreinterpret_u8_f32(v))), 0);  // rev32
#else
  uint32_t u;
  std::memcpy(&u, p, 4);
  u = __builtin_bswap32(u);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
#endif
}

// blur-nx (2026-11-17, #231 of docs/cpu-codegen-universal-30-1116.md): a guest double loaded big-endian into the vector
// unit, `ldr d; rev64 v.8b` instead of `ldr x; rev x; fmov d, x` (the 5-cycle move between the register files). Only
// the bits move (no floating-point operation, so no NaN is quieted). tools/float_loads.py --doubles writes it where the
// loaded register's next use is double arithmetic; checked against the integer form by banjo_forms_selfcheck.cpp.
[[gnu::always_inline]] inline double DoubleFromBigEndian(const void* p) noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
  double d;
  std::memcpy(&d, p, 8);                                                                  // ldr d
  return vget_lane_f64(vreinterpret_f64_u8(vrev64_u8(vreinterpret_u8_f64(vdup_n_f64(d)))), 0);  // rev64
#else
  uint64_t u;
  std::memcpy(&u, p, 8);
  u = __builtin_bswap64(u);
  double d;
  std::memcpy(&d, &u, 8);
  return d;
#endif
}

}  // namespace rex::ppc
