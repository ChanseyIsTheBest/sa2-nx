/**
 * @file        vmx_est360.h
 * @brief       The Xbox 360's estimate instructions and dot products, as the console computes them
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * renut-ultimate (2026-11-16, port 144; tw2de_nx's rex/ppc/vmx_fp.h and fpu_est.h of 2026-10-24 and 2026-11-01, the
 * parts for the codegen keys vmx_360_estimates and vmx_dot_360, both off by default at the user's word):
 *  - vrefp, vexptefp, vlogefp and fres: the 360's own tables, bit for bit (rex/ppc/estimates.h, sdk/src/core/
 *    ppc_estimates.cpp: Xenia's, read from a retail console; xenia-canary pull request #1342, BSD);
 *  - vrsqrtefp: xenia-edge's hardware-matched table (BSD 3-Clause, https://github.com/has207/xenia-edge, commit b3e8fda2,
 *    kVRsqrteCoefficients in src/xenia/cpu/backend/vrsqrte_table.cc), NEON on AArch64 and the same per lane elsewhere;
 *  - frsqrte: xenia-edge's table (kEstimates in EmitFrsqrteHelper, a64_backend.cc): frsqrte(1.0) is 0.970703125;
 *  - vmsum3fp128 / vmsum4fp128: float64 products summed as (x + z) + y or (x + z) + (y + w), then truncated to single
 *    precision (xenia-canary pull request #1342's x64 backend, which matches the console: 1.0 * 2.0 + 1.5 * 3.75 +
 *    1.1 * 2.31 gives 4122A7EF there, not 4122A7F0).
 * The base's defaults stay as they were: the exact division and square root (and port 72's NEON estimates with
 * SASRT_VMX_FAST_ESTIMATES), and the float32 dot product (vmx_dot_overflow_qnan's float64 path on overflow). These are
 * slower than those (a table look-up per lane, a scalar double sum) and match the console's bits.
 * The vector unit is taken to be in non-Java mode (NJ = 1: denormal inputs and outputs are zero), as the 360's games run.
 */
#pragma once

#include <rex/ppc/estimates.h>

#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#if (defined(__aarch64__) || defined(_M_ARM64))
#include <arm_neon.h>
#define REX_VMX_EST360_NEON 1
#endif

namespace rex::ppc {

namespace vmx_est360 {
// vrsqrtefp: 32 segments by (exponent parity, mantissa bits 22..19): base, slope, and an 8-bit pattern that takes one
// more ulp off for some values of the low 3 bits of the interpolation key (mantissa bits 18..9). The three tables are
// computed from xenia-edge's coefficients at compile time (each a slope in the high 16 bits and a base in the low 16).
inline constexpr uint32_t kVRsqrteCoefficients[32] = {
    0x0568B4FD, 0x04F3AF97, 0x048DAAA5, 0x0435A618, 0x03E7A1E4, 0x03A29DFE, 0x03659A5C, 0x032E96F8,
    0x02FC93CA, 0x02D090CE, 0x02A88DFE, 0x02838B57, 0x026188D4, 0x02438673, 0x02268431, 0x020B820B,
    0x03D27FFA, 0x03807C29, 0x033878AA, 0x02F97572, 0x02C27279, 0x02926FB7, 0x02666D26, 0x023F6AC0,
    0x021D6881, 0x01FD6665, 0x01E16468, 0x01C76287, 0x01AF60C1, 0x01995F12, 0x01855D79, 0x01735BF4};

// xenia-edge's interpolation for a positive normal input: the result's bits with the canonical exponent of the
// input's parity (0x3F800000 plus the adjustment).
constexpr uint32_t XeniaVRsqrte(uint32_t input) {
  const uint32_t mantissa = input & 0x7FFFFF;
  const uint32_t coefficient = kVRsqrteCoefficients[(((input >> 23) & 1) << 4) | (mantissa >> 19)];
  uint32_t estimate = ((coefficient << 10) & 0x3FFFC00) - (((mantissa >> 9) & 1023) * (coefficient >> 16));
  int32_t adjustment = 0;
  if (!(estimate & 0x02000000)) {
    const uint32_t normalized = estimate & 0x1FFFFFF;
    uint32_t leading = 0;
    for (uint32_t bit = 0x80000000u; !(normalized & bit); bit >>= 1)
      ++leading;
    adjustment += 6 - int32_t(leading);
    estimate <<= leading - 6;
  }
  if ((estimate & 5) && (estimate & 2))
    estimate += 4;
  return (0x3F800000u + uint32_t(adjustment) * 0x00800000u) | ((estimate >> 2) & 0x7FFFFF);
}

struct RsqrtTables {
  uint32_t base[32], slope[32], round[32];
};
constexpr RsqrtTables MakeRsqrtTables() {
  RsqrtTables t{};
  for (uint32_t seg = 0; seg < 32; ++seg) {
    const uint32_t c = kVRsqrteCoefficients[seg], b = c & 0xFFFF, a = c >> 16;
    // An even biased exponent (x in [0.5, 1) times a power of 4) gives 1/sqrt in (1, sqrt 2]: base b - 1 and the
    // slope as it is; an odd one gives (1/sqrt 2, 1], normalised: twice the base, less 1, and twice the slope.
    t.base[seg] = seg < 16 ? (b - 0x8000) << 8 : (2 * b - 0x8000) << 8;
    t.slope[seg] = seg < 16 ? a : 2 * a;
    for (uint32_t k = 0; k < 8; ++k) {
      const uint32_t input = (seg < 16 ? 0x3F000000u : 0x3F800000u) | ((seg & 15) << 19) | (k << 9);
      const uint32_t plain = (t.base[seg] - ((t.slope[seg] * k) >> 2)) & 0x7FFFFF;
      if ((XeniaVRsqrte(input) & 0x7FFFFF) != plain)
        t.round[seg] |= 1u << k;
    }
  }
  return t;
}
inline constexpr RsqrtTables kRsqrtTables = MakeRsqrtTables();

// One lane of vrsqrtefp (bits in, bits out): the NEON form below, per lane.
inline uint32_t RsqrteLane(uint32_t v) {
  const uint32_t sign = v & 0x80000000u, exp = (v >> 23) & 0xFF, man = v & 0x7FFFFF;
  if (exp == 0xFF)
    return man ? (v | 0x400000u) : (sign ? 0x7FC00000u : 0u);  // NaN quieted; 1/sqrt(+INF) = +0, -INF: NaN
  if (exp == 0)
    return sign | 0x7F800000u;  // +-0 and (flushed) denormals: +-INF
  if (sign)
    return 0x7FC00000u;  // negative: NaN
  const uint32_t seg = ((exp & 1) << 4) | (man >> 19);
  const uint32_t key = (man >> 9) & 0x3FF;
  const uint32_t extra = (kRsqrtTables.round[seg] >> (key & 7)) & 1;
  const uint32_t rman = kRsqrtTables.base[seg] - ((kRsqrtTables.slope[seg] * key) >> 2) - extra;
  return (((380 - exp) >> 1) << 23) | rman;
}

// frsqrte: the result's top 8 mantissa bits (double-precision bits 51..44; the rest 0) by (input exponent is even,
// input mantissa bits 51..49).
inline constexpr uint8_t kFrsqrte[16] = {241, 216, 192, 168, 152, 136, 128, 112, 96, 76, 60, 48, 32, 24, 16, 8};

// vmsum: a float64 sum to single precision as the console's (see the file's head).
inline float DotSum(double s) {
  uint64_t b;
  std::memcpy(&b, &s, 8);
  const uint32_t e = uint32_t(b >> 52) & 0x7FF;
  if (e == 0x7FF)  // from an infinity or NaN input
    return float(s);
  b &= 0xFFFFFFFFE0000000ull;  // the bits below single precision: toward zero
  if (e < 1023 - 126) {        // below 2^-126 (or zero): a zero of its sign
    b &= 0x8000000000000000ull;
  } else if (e > 1023 + 127) {  // 2^128 and up: overflow
    const uint32_t q = 0x7FC00000u;
    float f;
    std::memcpy(&f, &q, 4);
    return f;
  }
  double t;
  std::memcpy(&t, &b, 8);
  return float(t);  // exact
}
}  // namespace vmx_est360

// vrefp: the 360's estimate of 1/x, per lane.
inline simde__m128 vmx_re360_ps(simde__m128 x) {
  uint32_t l[4];
  std::memcpy(l, &x, 16);
  for (uint32_t& e : l)
    e = hw_est::EstimateReciprocal(e);
  simde__m128 out;
  std::memcpy(&out, l, 16);
  return out;
}

// vrsqrtefp: the hardware's estimate of 1/sqrt(x).
inline simde__m128 vmx_rsqrte360_ps(simde__m128 x) {
#if defined(REX_VMX_EST360_NEON)
  using vmx_est360::kRsqrtTables;
  uint32x4_t v;
  std::memcpy(&v, &x, 16);
  const uint32x4_t sign = vandq_u32(v, vdupq_n_u32(0x80000000u));
  const uint32x4_t exp = vandq_u32(vshrq_n_u32(v, 23), vdupq_n_u32(0xFF));
  const uint32x4_t man = vandq_u32(v, vdupq_n_u32(0x7FFFFF));
  // Segment: exponent parity (an odd biased exponent is an even power of 2) and the top 4 mantissa bits.
  const uint32x4_t seg = vorrq_u32(vshlq_n_u32(vandq_u32(exp, vdupq_n_u32(1)), 4), vshrq_n_u32(man, 19));
  uint32x4_t base = vdupq_n_u32(0), slope = vdupq_n_u32(0), round = vdupq_n_u32(0);
#define REX_VMX_RSQRT360_LANE(i)                                                        \
  base = vld1q_lane_u32(&kRsqrtTables.base[vgetq_lane_u32(seg, i)], base, i);           \
  slope = vld1q_lane_u32(&kRsqrtTables.slope[vgetq_lane_u32(seg, i)], slope, i);        \
  round = vld1q_lane_u32(&kRsqrtTables.round[vgetq_lane_u32(seg, i)], round, i);
  REX_VMX_RSQRT360_LANE(0) REX_VMX_RSQRT360_LANE(1) REX_VMX_RSQRT360_LANE(2) REX_VMX_RSQRT360_LANE(3)
#undef REX_VMX_RSQRT360_LANE
  const uint32x4_t key = vandq_u32(vshrq_n_u32(man, 9), vdupq_n_u32(0x3FF));
  const uint32x4_t extra = vandq_u32(
      vshlq_u32(round, vnegq_s32(vreinterpretq_s32_u32(vandq_u32(key, vdupq_n_u32(7))))), vdupq_n_u32(1));
  const uint32x4_t rman = vsubq_u32(vsubq_u32(base, vshrq_n_u32(vmulq_u32(slope, key), 2)), extra);
  // Biased exponent of the result: (380 - e) / 2, rounded down.
  uint32x4_t r = vorrq_u32(vshlq_n_u32(vshrq_n_u32(vsubq_u32(vdupq_n_u32(380), exp), 1), 23), rman);
  r = vbslq_u32(vtstq_u32(v, vdupq_n_u32(0x80000000u)), vdupq_n_u32(0x7FC00000u), r);  // negative: NaN
  r = vbslq_u32(vceqzq_u32(exp), vorrq_u32(sign, vdupq_n_u32(0x7F800000u)), r);       // +-0 / denormal: +-INF
  const uint32x4_t special = vceqq_u32(exp, vdupq_n_u32(0xFF));
  const uint32x4_t inf = vbslq_u32(vtstq_u32(v, vdupq_n_u32(0x80000000u)), vdupq_n_u32(0x7FC00000u), vdupq_n_u32(0));
  const uint32x4_t inf_or_nan = vbslq_u32(vceqzq_u32(man), inf, vorrq_u32(v, vdupq_n_u32(0x400000)));
  r = vbslq_u32(special, inf_or_nan, r);
  simde__m128 out;
  std::memcpy(&out, &r, 16);
  return out;
#else
  uint32_t l[4];
  std::memcpy(l, &x, 16);
  for (uint32_t& e : l)
    e = vmx_est360::RsqrteLane(e);
  simde__m128 out;
  std::memcpy(&out, l, 16);
  return out;
#endif
}

// vexptefp: the 360's estimate of 2^x, per lane.
inline simde__m128 vmx_expte360_ps(simde__m128 x) {
  uint32_t l[4];
  std::memcpy(l, &x, 16);
  for (uint32_t& e : l)
    e = hw_est::EstimatePow2(e);
  simde__m128 out;
  std::memcpy(&out, l, 16);
  return out;
}

// vlogefp: the 360's estimate of log2(x), per lane.
inline simde__m128 vmx_loge360_ps(simde__m128 x) {
  uint32_t l[4];
  std::memcpy(l, &x, 16);
  for (uint32_t& e : l)
    e = hw_est::EstimateLog2(e);
  simde__m128 out;
  std::memcpy(&out, l, 16);
  return out;
}

// fres: the 360's single-precision estimate of 1/x (in the FPU: denormal results kept).
inline double fres360(double x) { return hw_est::EstimateReciprocalSingle(x); }

// frsqrte: the hardware's estimate of 1/sqrt(x) (double precision, 8 significant mantissa bits).
inline double frsqrte360(double x) {
  uint64_t b;
  std::memcpy(&b, &x, 8);
  const uint64_t sign = b & 0x8000000000000000ull;
  int64_t exp = int64_t((b >> 52) & 0x7FF);
  uint64_t man = b & 0xFFFFFFFFFFFFFull;
  uint64_t r;
  if (exp == 0x7FF) {
    r = man ? (b | 0x0008000000000000ull) : (sign ? 0x7FF8000000000000ull : 0);  // NaN quieted; +INF: +0; -INF: NaN
  } else if (exp == 0 && man == 0) {
    r = sign | 0x7FF0000000000000ull;  // +-0: +-INF
  } else if (sign) {
    r = 0x7FF8000000000000ull;  // negative: NaN
  } else {
    if (exp == 0) {  // denormal: normalized first (the FPU keeps denormals)
      const int shift = __builtin_clzll(man) - 11;
      man = (man << shift) & 0xFFFFFFFFFFFFFull;
      exp = 1 - shift;
    }
    // Biased exponent of the result: 0x5FE - floor((e + 1) / 2).
    const int64_t rexp = 0x5FE - ((exp + 1) >> 1);
    const uint32_t idx = uint32_t(((exp & 1) ^ 1) << 3) | uint32_t((man >> 49) & 7);
    r = uint64_t(rexp) << 52 | uint64_t(vmx_est360::kFrsqrte[idx]) << 44;
  }
  double d;
  std::memcpy(&d, &r, 8);
  return d;
}

// vmsum3fp128 / vmsum4fp128 with the console's rounding (host lanes 3..0 hold x, y, z, w), splat to four lanes.
inline simde__m128 vmx_dot3_360_ps(simde__m128 a, simde__m128 b) {
  alignas(16) float fa[4], fb[4];
  simde_mm_store_ps(fa, a);
  simde_mm_store_ps(fb, b);
  const double s = (double(fa[3]) * fb[3] + double(fa[1]) * fb[1]) + double(fa[2]) * fb[2];
  return simde_mm_set1_ps(vmx_est360::DotSum(s));
}

inline simde__m128 vmx_dot4_360_ps(simde__m128 a, simde__m128 b) {
  alignas(16) float fa[4], fb[4];
  simde_mm_store_ps(fa, a);
  simde_mm_store_ps(fb, b);
  const double s =
      (double(fa[3]) * fb[3] + double(fa[1]) * fb[1]) + (double(fa[2]) * fb[2] + double(fa[0]) * fb[0]);
  return simde_mm_set1_ps(vmx_est360::DotSum(s));
}

}  // namespace rex::ppc
