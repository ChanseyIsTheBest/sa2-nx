/**
 * blur-nx (2026-11-11, CPU batch 1; the user: "do all the exact ones", "optimise the hell out of CPU"): the helpers
 * behind the forms of tools/vec_cvt_fixed.py (VS-27), tools/vec_shift_whole.py (VX-20, VX-21) and tools/vec_dcbz_zva.py
 * (VS-05), in one header so the generated code (through renut_pch.h's macros) and the app's forms self-check
 * (banjo_forms_selfcheck.cpp, which compares each with its reference form on the device) compile the same code.
 * Every helper is exact against the form the generator writes; the comments say why. Off the AArch64 each one is the
 * reference form itself.
 */
#pragma once

#include <cstdint>
#include <cstring>

#include <simde/x86/sse2.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace rex::ppc {

// --- VS-27: vcfsx / vcfux with a scale 2^-N (1 <= N <= 31) ----------------------------------------------------------
// The reference: convert (round to nearest), then multiply by 2^-N. The multiply only changes the exponent; for a
// nonzero 32-bit integer the result is at least 2^-31 (normal), so it cannot underflow and the flush mode cannot
// matter; zero stays +0. SCVTF / UCVTF #N rounds x / 2^N once: the same value.
inline simde__m128 VcfsxRef(simde__m128i x, int n) {
  return simde_mm_mul_ps(simde_mm_cvtepi32_ps(x), simde_mm_castsi128_ps(simde_mm_set1_epi32(int((127u - unsigned(n)) << 23))));
}
inline simde__m128 VcfuxRef(simde__m128i x, int n) {
  // The unsigned conversion as intrinsics.h's simde_mm_cvtepu32_ps_ (UCVTF on the AArch64): exact rounding.
#if defined(__aarch64__)
  const simde__m128 c = simde__m128_from_neon_f32(vcvtq_f32_u32(simde__m128i_to_neon_u32(x)));
#else
  alignas(16) uint32_t u[4];
  alignas(16) float f[4];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(u), x);
  for (int k = 0; k < 4; ++k) f[k] = float(u[k]);
  const simde__m128 c = simde_mm_load_ps(f);
#endif
  return simde_mm_mul_ps(c, simde_mm_castsi128_ps(simde_mm_set1_epi32(int((127u - unsigned(n)) << 23))));
}
template <int N>
inline simde__m128 VcfsxN(simde__m128i x) {
  static_assert(N >= 1 && N <= 31, "vcfsx scale");
#if defined(__aarch64__) && !defined(REX_VEC_CVT_FIXED_OFF)
  return simde__m128_from_neon_f32(vcvtq_n_f32_s32(simde__m128i_to_neon_i32(x), N));
#else
  return VcfsxRef(x, N);
#endif
}
template <int N>
inline simde__m128 VcfuxN(simde__m128i x) {
  static_assert(N >= 1 && N <= 31, "vcfux scale");
#if defined(__aarch64__) && !defined(REX_VEC_CVT_FIXED_OFF)
  return simde__m128_from_neon_f32(vcvtq_n_f32_u32(simde__m128i_to_neon_u32(x), N));
#else
  return VcfuxRef(x, N);
#endif
}

// --- VX-20: vsrw / vsraw, every lane shifted right by (its count lane & 31) -----------------------------------------
inline simde__m128i VecSrwRef(simde__m128i a, simde__m128i b) {
  alignas(16) uint32_t x[4], y[4];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(x), a);
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(y), b);
  for (int k = 0; k < 4; ++k) x[k] = x[k] >> (y[k] & 0x1F);
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(x));
}
inline simde__m128i VecSrawRef(simde__m128i a, simde__m128i b) {
  alignas(16) int32_t x[4];
  alignas(16) uint32_t y[4];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(x), a);
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(y), b);
  for (int k = 0; k < 4; ++k) x[k] = x[k] >> (y[k] & 0x1F);
  return simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(x));
}
inline simde__m128i VecSrw(simde__m128i a, simde__m128i b) {
#if defined(__aarch64__) && !defined(REX_VEC_SHIFT_SCALAR)
  const int32x4_t n = vnegq_s32(vreinterpretq_s32_u32(vandq_u32(simde__m128i_to_neon_u32(b), vdupq_n_u32(31))));
  return simde__m128i_from_neon_u32(vshlq_u32(simde__m128i_to_neon_u32(a), n));
#else
  return VecSrwRef(a, b);
#endif
}
inline simde__m128i VecSraw(simde__m128i a, simde__m128i b) {
#if defined(__aarch64__) && !defined(REX_VEC_SHIFT_SCALAR)
  const int32x4_t n = vnegq_s32(vreinterpretq_s32_u32(vandq_u32(simde__m128i_to_neon_u32(b), vdupq_n_u32(31))));
  return simde__m128i_from_neon_i32(vshlq_s32(simde__m128i_to_neon_i32(a), n));
#else
  return VecSrawRef(a, b);
#endif
}
// --- VX-21: vslw by a count the generator has already masked to 0..31 ------------------------------------------------
// simde_mm_sllv_epi32's NEON form also compares the count with 32 and clears those lanes: never true after the mask.
inline simde__m128i VecSllv31(simde__m128i a, simde__m128i n) {
#if defined(__aarch64__) && !defined(REX_VEC_SHIFT_SCALAR)
  return simde__m128i_from_neon_u32(vshlq_u32(simde__m128i_to_neon_u32(a), vreinterpretq_s32_u32(simde__m128i_to_neon_u32(n))));
#else
  return simde_mm_sllv_epi32(a, n);
#endif
}

// --- VS-05: a guest dcbzl, 128 zero bytes at a 128-byte aligned host address -----------------------------------------
// DC ZVA zeroes a whole block without reading it first (a plain store of zero makes the A57 read the line in). The block
// is DCZID_EL0's (bits 3:0, log2 of 4-byte words; bit 4 prohibits): 64 bytes on the A57. Any other answer: the memset.
#if defined(__aarch64__) && !defined(REX_DCBZ_MEMSET)
namespace detail {
inline bool ReadZva64Ok() {
  uint64_t dczid;
  __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
  return (dczid & 0x1F) == 4;
}
// blur-nx (2026-11-12, CPU batch 3): read once, by the program's dynamic initialisation (before main, so before any
// guest code), and a plain load at each use. It was a function-local static: every dcbz paid the load-acquire of its
// guard (`ldar` + `tbz`) and the flag's load; in the console race 190 of the 227 samples of the audio mixer's zeroing
// loop (sub_8230AD50: eight dcbz a 1 KB buffer) sat on those two loads, 5 on the `dc zva`s. A dcbz that ran before
// the initialisation would read false and take the memset: the same bytes.
inline const bool g_zva64_ok = ReadZva64Ok();
}  // namespace detail
inline bool Zva64Ok() { return detail::g_zva64_ok; }
#endif
inline void Dcbz128(void* p) {
#if defined(__aarch64__) && !defined(REX_DCBZ_MEMSET)
  if (__builtin_expect(Zva64Ok(), 1)) {
    char* const c = static_cast<char*>(p);
    __asm__("dc zva, %1" : "=m"(*reinterpret_cast<char(*)[64]>(c)) : "r"(c));
    __asm__("dc zva, %1" : "=m"(*reinterpret_cast<char(*)[64]>(c + 64)) : "r"(c + 64));
    return;
  }
#endif
  std::memset(p, 0, 128);
}

}  // namespace rex::ppc
