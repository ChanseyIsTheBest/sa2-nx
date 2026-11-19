/**
 * @file        ppc/intrinsics.h
 * @brief       SIMD intrinsic helpers and vector lookup tables for PPC AltiVec emulation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Based on XenonRecomp/UnleashedRecomp SIMD patterns and simde
 */

#pragma once

#include <climits>
#include <cmath>
#include <cstring>

#include <simde/x86/avx.h>
#include <simde/x86/avx2.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

#include <rex/types.h>

namespace rex::ppc {

//=============================================================================
// Vector Load/Store Mask Tables
//=============================================================================
// These tables are used for lvlx/lvrx (load vector left/right) and
// stvlx/stvrx (store vector left/right) instructions.

inline uint8_t VectorMaskL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};

inline uint8_t VectorMaskR[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

inline uint8_t VectorShiftTableL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
};

inline uint8_t VectorShiftTableR[] = {
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

//=============================================================================
// SIMD Helper Functions
//=============================================================================

// rexglue ports (sasrt-nx, 2026-10-12): on AArch64 the helpers below are the one NEON instruction that does what the
// VMX instruction does, instead of the x86 emulation compiled through simde (vperm: two pshufb, each an AND and a TBL
// there, a shift and a blend; the float conversions: x86's "integer indefinite" result patched up with compares and
// blends, on top of simde's own emulation of that result). Same results for every input:
// sdk/tests/vmx_helpers_neon_test.cpp compares vperm, vctsxs and vctuxs with the x86 form through simde's model of
// the NEON instruction; sdk/tests/vmx_small_helpers_test.cpp compares the small ones (sa2-nx, 2026-10-12: saturating
// add, the signed averages, the unsigned compares, vcfux, the per-lane shifts) with a scalar statement of the NEON
// instruction, and the shifts with simde's model too. REX_VMX_HELPERS_X86_FORM keeps the x86 forms.
// vcfux as UCVTF rounds in the host's current mode, the x86 form to nearest: the same while the mode is to nearest,
// which is the only mode the vector unit of the 360 has and the mode vector code runs in.
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(REX_VMX_HELPERS_X86_FORM)
#define REX_VMX_HELPERS_NEON 1
#endif

// Unsigned 32-bit saturating add
inline simde__m128i simde_mm_adds_epu32(simde__m128i a, simde__m128i b) {
#if defined(REX_VMX_HELPERS_NEON)
  return simde__m128i_from_neon_u32(vqaddq_u32(simde__m128i_to_neon_u32(a), simde__m128i_to_neon_u32(b)));  // UQADD
#else
  return simde_mm_add_epi32(
      a, simde_mm_min_epu32(simde_mm_xor_si128(a, simde_mm_cmpeq_epi32(a, a)), b));
#endif
}

// Signed 8-bit average (rounds towards zero)
inline simde__m128i simde_mm_avg_epi8(simde__m128i a, simde__m128i b) {
#if defined(REX_VMX_HELPERS_NEON)
  return simde__m128i_from_neon_i8(vrhaddq_s8(simde__m128i_to_neon_i8(a), simde__m128i_to_neon_i8(b)));  // SRHADD
#else
  simde__m128i c = simde_mm_set1_epi8(char(128));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu8(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
#endif
}

// Signed 16-bit average
inline simde__m128i simde_mm_avg_epi16(simde__m128i a, simde__m128i b) {
#if defined(REX_VMX_HELPERS_NEON)
  return simde__m128i_from_neon_i16(vrhaddq_s16(simde__m128i_to_neon_i16(a), simde__m128i_to_neon_i16(b)));
#else
  simde__m128i c = simde_mm_set1_epi16(short(32768));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu16(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
#endif
}

// Signed 32-bit average
inline simde__m128i simde_mm_avg_epi32(simde__m128i a, simde__m128i b) {
#if defined(REX_VMX_HELPERS_NEON)
  return simde__m128i_from_neon_i32(vrhaddq_s32(simde__m128i_to_neon_i32(a), simde__m128i_to_neon_i32(b)));
#else
  simde__m128i sum = simde_mm_add_epi32(simde_mm_srai_epi32(a, 1), simde_mm_srai_epi32(b, 1));
  return simde_mm_add_epi32(sum,
                            simde_mm_and_si128(simde_mm_or_si128(a, b), simde_mm_set1_epi32(1)));
#endif
}

// Convert unsigned 32-bit integers to floats
inline simde__m128 simde_mm_cvtepu32_ps_(simde__m128i src1) {
#if defined(REX_VMX_HELPERS_NEON)
  return simde__m128_from_neon_f32(vcvtq_f32_u32(simde__m128i_to_neon_u32(src1)));  // UCVTF
#else
  simde__m128i xmm1 = simde_mm_add_epi32(src1, simde_mm_set1_epi32(127));
  simde__m128i xmm0 = simde_mm_slli_epi32(src1, 31 - 8);
  xmm0 = simde_mm_srli_epi32(xmm0, 31);
  xmm0 = simde_mm_add_epi32(xmm0, xmm1);
  xmm0 = simde_mm_srai_epi32(xmm0, 8);
  xmm0 = simde_mm_add_epi32(xmm0, simde_mm_set1_epi32(0x4F800000));
  simde__m128 xmm2 = simde_mm_cvtepi32_ps(src1);
  return simde_mm_blendv_ps(xmm2, simde_mm_castsi128_ps(xmm0), simde_mm_castsi128_ps(src1));
#endif
}

// Permute bytes from two vectors based on control vector
inline simde__m128i simde_mm_perm_epi8_(simde__m128i a, simde__m128i b, simde__m128i c) {
#if defined(REX_VMX_HELPERS_NEON)
  // Result byte i = (bit 4 of c[i] ? b : a)[15 - (c[i] & 15)]: with a and b as one 32-byte table that is entry
  // (c[i] & 0x1F) ^ 0x0F (the XOR is the 15 - x of the low four bits and leaves bit 4).
  const uint8x16x2_t table = {{simde__m128i_to_neon_u8(a), simde__m128i_to_neon_u8(b)}};
  const uint8x16_t index =
      veorq_u8(vandq_u8(simde__m128i_to_neon_u8(c), vdupq_n_u8(0x1F)), vdupq_n_u8(0x0F));
  return simde__m128i_from_neon_u8(vqtbl2q_u8(table, index));
#else
  simde__m128i d = simde_mm_set1_epi8(0xF);
  simde__m128i e = simde_mm_sub_epi8(d, simde_mm_and_si128(c, d));
  return simde_mm_blendv_epi8(simde_mm_shuffle_epi8(a, e), simde_mm_shuffle_epi8(b, e),
                              simde_mm_slli_epi32(c, 3));
#endif
}

// vperm with both sources the same vector (from sasrt-nx, 2026-10-14; sa2-nx's idea 8): byte i = a[15 - (c[i] & 15)],
// bit 4 of c[i] choosing between two equal halves. One one-register table lookup (TBL with one table register and
// the index (c & 0x0F) ^ 0x0F) instead of a two-register one, which needs a register pair. Exact: with a == b the
// two-source form above takes the same byte from either half. tools/vperm_one_source.py writes it (REX_VPERM1);
// sdk/tests/vmx_helpers_neon_test.cpp checks both forms against the two-source one.
inline simde__m128i simde_mm_perm1_epi8_(simde__m128i a, simde__m128i c) {
#if defined(REX_VMX_HELPERS_NEON)
  const uint8x16_t index =
      veorq_u8(vandq_u8(simde__m128i_to_neon_u8(c), vdupq_n_u8(0x0F)), vdupq_n_u8(0x0F));
  return simde__m128i_from_neon_u8(vqtbl1q_u8(simde__m128i_to_neon_u8(a), index));
#else
  simde__m128i d = simde_mm_set1_epi8(0xF);
  return simde_mm_shuffle_epi8(a, simde_mm_sub_epi8(d, simde_mm_and_si128(c, d)));
#endif
}

// Unsigned 8-bit compare greater than
inline simde__m128i simde_mm_cmpgt_epu8(simde__m128i a, simde__m128i b) {
#if defined(REX_VMX_HELPERS_NEON)
  return simde__m128i_from_neon_u8(vcgtq_u8(simde__m128i_to_neon_u8(a), simde__m128i_to_neon_u8(b)));  // CMHI
#else
  simde__m128i c = simde_mm_set1_epi8(char(128));
  return simde_mm_cmpgt_epi8(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
#endif
}

// Unsigned 16-bit compare greater than
inline simde__m128i simde_mm_cmpgt_epu16(simde__m128i a, simde__m128i b) {
#if defined(REX_VMX_HELPERS_NEON)
  return simde__m128i_from_neon_u16(vcgtq_u16(simde__m128i_to_neon_u16(a), simde__m128i_to_neon_u16(b)));
#else
  simde__m128i c = simde_mm_set1_epi16(short(32768));
  return simde_mm_cmpgt_epi16(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
#endif
}

// Vector Convert To Signed Fixed-Point Word Saturate
inline simde__m128i simde_mm_vctsxs(simde__m128 src1) {
#if defined(REX_VMX_HELPERS_NEON)
  // FCVTZS: truncation, saturated at both ends, NaN -> 0: what the x86 form below builds.
  return simde__m128i_from_neon_i32(vcvtq_s32_f32(simde__m128_to_neon_f32(src1)));
#else
  simde__m128 xmm2 = simde_mm_cmpunord_ps(src1, src1);
  simde__m128i xmm0 = simde_mm_cvttps_epi32(src1);
  simde__m128i xmm1 = simde_mm_cmpeq_epi32(xmm0, simde_mm_set1_epi32(INT_MIN));
  xmm1 = simde_mm_andnot_si128(simde_mm_castps_si128(src1), xmm1);
  simde__m128 dest = simde_mm_blendv_ps(simde_mm_castsi128_ps(xmm0),
                                        simde_mm_castsi128_ps(simde_mm_set1_epi32(INT_MAX)),
                                        simde_mm_castsi128_ps(xmm1));
  return simde_mm_andnot_si128(simde_mm_castps_si128(xmm2), simde_mm_castps_si128(dest));
#endif
}

// Vector Convert To Unsigned Fixed-Point Word Saturate
// Convert float to unsigned int with saturation to [0, UINT_MAX]
// NaN -> 0, negative -> 0, > UINT_MAX -> UINT_MAX
inline simde__m128i simde_mm_vctuxs(simde__m128 src1) {
#if defined(REX_VMX_HELPERS_NEON)
  // FCVTZU: truncation, negative -> 0, saturated at UINT_MAX, NaN -> 0.
  return simde__m128i_from_neon_u32(vcvtq_u32_f32(simde__m128_to_neon_f32(src1)));
#else
  simde__m128 nan_mask = simde_mm_cmpunord_ps(src1, src1);
  simde__m128 neg_mask = simde_mm_cmplt_ps(src1, simde_mm_setzero_ps());
  simde__m128 max_val = simde_mm_set1_ps(4294967295.0f);  // UINT_MAX as float
  simde__m128 overflow_mask = simde_mm_cmpge_ps(src1, max_val);

  // Clamp to [0, UINT_MAX]
  simde__m128 clamped = simde_mm_max_ps(src1, simde_mm_setzero_ps());
  clamped = simde_mm_min_ps(clamped, max_val);

  // Convert to signed int first (will handle values up to INT_MAX correctly)
  // For values > INT_MAX, we need special handling
  simde__m128 half_range = simde_mm_set1_ps(2147483648.0f);  // 2^31
  simde__m128 high_bit_mask = simde_mm_cmpge_ps(clamped, half_range);

  // For values >= 2^31, subtract 2^31 before conversion and add it back after
  simde__m128 adjusted = simde_mm_sub_ps(clamped, simde_mm_and_ps(high_bit_mask, half_range));
  simde__m128i low_bits = simde_mm_cvttps_epi32(adjusted);
  simde__m128i high_bit = simde_mm_and_si128(simde_mm_castps_si128(high_bit_mask),
                                             simde_mm_set1_epi32(int(0x80000000u)));
  simde__m128i result = simde_mm_or_si128(low_bits, high_bit);

  // Apply saturation: NaN -> 0, overflow -> UINT_MAX
  result = simde_mm_andnot_si128(simde_mm_castps_si128(nan_mask), result);
  result = simde_mm_andnot_si128(simde_mm_castps_si128(neg_mask), result);
  result = simde_mm_or_si128(
      simde_mm_andnot_si128(simde_mm_castps_si128(overflow_mask), result),
      simde_mm_and_si128(simde_mm_castps_si128(overflow_mask), simde_mm_set1_epi32(-1)));

  return result;
#endif
}

#if defined(REX_VSHIFT_PER_BYTE)
// renut-ultimate (2026-11-16, port 144; tw2de_nx's of 2026-11-01; CMake TW2DE_VSHIFT_PER_BYTE, off by default at the
// user's word): vsl / vsr shift the 128-bit vector left / right by bits, each byte of the result by the count in the
// low 3 bits of the same byte of b, taking the bits it needs from the neighbouring byte: vsl: r[i] = (a[i] << s[i]) |
// (a[i+1] >> (8 - s[i])), vsr: r[i] = (a[i] >> s[i]) | (a[i-1] << (8 - s[i])) (PowerPC byte order, a[16] = a[-1] = 0).
// That is what the hardware does (Xenia's instruction tests, recorded on it: 453 of 475 vsl cases with counts that
// differ between bytes failed when one count was used for all). The ISA asks for the same count in every byte; then
// this is the plain 128-bit shift, as the forms below. In the host layout PowerPC byte i is byte 15 - i, so the
// neighbour a[i+1] is the host byte below. On AArch64 five NEON instructions each (the vsl below goes through memory).
inline simde__m128i simde_mm_vsl(simde__m128i a, simde__m128i b) {
  const simde__m128i prev = simde_mm_slli_si128(a, 1);  // host byte h: a[h - 1]
#if defined(__aarch64__) || defined(_M_ARM64)
  const int8x16_t s = vreinterpretq_s8_u8(vandq_u8(simde__m128i_to_neon_u8(b), vdupq_n_u8(7)));
  const uint8x16_t r = vorrq_u8(vshlq_u8(simde__m128i_to_neon_u8(a), s),
                                vshlq_u8(simde__m128i_to_neon_u8(prev), vsubq_s8(s, vdupq_n_s8(8))));
  return simde__m128i_from_neon_u8(r);
#else
  alignas(16) uint8_t va[16], vp[16], vb[16], vr[16];
  simde_mm_store_si128((simde__m128i*)va, a);
  simde_mm_store_si128((simde__m128i*)vp, prev);
  simde_mm_store_si128((simde__m128i*)vb, b);
  for (int h = 0; h < 16; ++h) {
    const int s = vb[h] & 7;
    vr[h] = uint8_t((va[h] << s) | (s ? (vp[h] >> (8 - s)) : 0));
  }
  return simde_mm_load_si128((simde__m128i*)vr);
#endif
}

inline simde__m128i simde_mm_vsr(simde__m128i a, simde__m128i b) {
  const simde__m128i next = simde_mm_srli_si128(a, 1);  // host byte h: a[h + 1]
#if defined(__aarch64__) || defined(_M_ARM64)
  const int8x16_t s = vreinterpretq_s8_u8(vandq_u8(simde__m128i_to_neon_u8(b), vdupq_n_u8(7)));
  const uint8x16_t r = vorrq_u8(vshlq_u8(simde__m128i_to_neon_u8(a), vnegq_s8(s)),
                                vshlq_u8(simde__m128i_to_neon_u8(next), vsubq_s8(vdupq_n_s8(8), s)));
  return simde__m128i_from_neon_u8(r);
#else
  alignas(16) uint8_t va[16], vn[16], vb[16], vr[16];
  simde_mm_store_si128((simde__m128i*)va, a);
  simde_mm_store_si128((simde__m128i*)vn, next);
  simde_mm_store_si128((simde__m128i*)vb, b);
  for (int h = 0; h < 16; ++h) {
    const int s = vb[h] & 7;
    vr[h] = uint8_t((va[h] >> s) | (s ? (vn[h] << (8 - s)) : 0));
  }
  return simde_mm_load_si128((simde__m128i*)vr);
#endif
}
#else  // !REX_VSHIFT_PER_BYTE: one count for the whole vector, as before
// Vector Shift Right
inline simde__m128i simde_mm_vsr(simde__m128i a, simde__m128i b) {
  b = simde_mm_srli_epi64(simde_mm_slli_epi64(b, 61), 61);
  return simde_mm_castps_si128(simde_mm_insert_ps(
      simde_mm_castsi128_ps(simde_mm_srl_epi64(a, b)),
      simde_mm_castsi128_ps(simde_mm_srl_epi64(simde_mm_srli_si128(a, 4), b)), 0x10));
}

// Vector Shift Left - shift entire 128-bit vector left by bits in low 3 bits of b
inline simde__m128i simde_mm_vsl(simde__m128i a, simde__m128i b) {
  int shift = simde_mm_extract_epi8(b, 15) & 0x7;  // Get low 3 bits from byte 15 (BE: byte 0)
  if (shift == 0)
    return a;

#if defined(__x86_64__) || defined(_M_X64)
  // Split into high and low 64-bit parts
  simde__m128i low_shifted = simde_mm_slli_epi64(a, shift);
  simde__m128i high_carry = simde_mm_srli_epi64(a, 64 - shift);
  // Shift the carry from low qword to high qword position
  high_carry = simde_mm_slli_si128(high_carry, 8);
  return simde_mm_or_si128(low_shifted, high_carry);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using vld1/vst1 for conversion
  uint64_t vals[2];
  uint64_t res[2] = {0, 0};

  // Store simde__m128i to memory
  simde_mm_store_si128((simde__m128i*)vals, a);

  // Load as NEON vector
  uint64x2_t va = vld1q_u64(vals);

  // vshlq_u64 accepts variable shift per lane
  int64x2_t shift_vector = vdupq_n_s64(shift);
  uint64x2_t low_shifted = vshlq_u64(va, shift_vector);

  // NEON vshl uses negative counts for right shifts.
  int64x2_t rshift_vector = vdupq_n_s64(shift - 64);
  uint64x2_t high_carry = vshlq_u64(va, rshift_vector);

  // Combine results
  uint64x2_t result_vec = vdupq_n_u64(0);
  result_vec = vsetq_lane_u64(vgetq_lane_u64(low_shifted, 0), result_vec, 0);
  result_vec =
      vsetq_lane_u64(vgetq_lane_u64(low_shifted, 1) | vgetq_lane_u64(high_carry, 0), result_vec, 1);

  // Store back to memory and reload as simde__m128i
  vst1q_u64(res, result_vec);
  return simde_mm_load_si128((simde__m128i*)res);
#else
#error "Unsupported architecture for simde_mm_vsl (only x86_64 and ARM64 supported)"
#endif
}
#endif  // REX_VSHIFT_PER_BYTE

// Vector Shift Left by Octet - shift entire vector left by bytes in bits [121:124] of vB
// In PPC big-endian byte 15 is at LSB position, which in x86 LE is at index 0
// Bits 121:124 within the byte are extracted as (byte >> 3) & 0xF
// PPC left shift = shift towards MSB (lower PPC addresses) = shift towards higher x86 addresses
inline simde__m128i simde_mm_vslo(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();

#if defined(__x86_64__) || defined(_M_X64)
  alignas(16) uint8_t src[16], dst[16];
  simde_mm_store_si128((simde__m128i*)src, a);
  memset(dst, 0, sizeof(dst));
  memcpy(dst + shift_bytes, src, 16 - shift_bytes);
  return simde_mm_load_si128((simde__m128i*)dst);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using memory for conversion
  uint8_t src[16];
  uint8_t dst[16] = {0};

  simde_mm_store_si128((simde__m128i*)src, a);
  memcpy(dst + shift_bytes, src, 16 - shift_bytes);

  return simde_mm_load_si128((simde__m128i*)dst);
#else
#error "Unsupported architecture for simde_mm_vslo (only x86_64 and ARM64 supported)"
#endif
}

// Vector Shift Right by Octet - shift entire vector right by bytes in bits [121:124] of vB
// In PPC big-endian byte 15 is at LSB position, which in x86 LE is at index 0
// Bits 121:124 within the byte are extracted as (byte >> 3) & 0xF
// PPC right shift = shift towards LSB (higher PPC addresses) = shift towards lower x86 addresses
inline simde__m128i simde_mm_vsro(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();

#if defined(__x86_64__) || defined(_M_X64)
  alignas(16) uint8_t src[16], dst[16];
  simde_mm_store_si128((simde__m128i*)src, a);
  memset(dst, 0, sizeof(dst));
  memcpy(dst, src + shift_bytes, 16 - shift_bytes);
  return simde_mm_load_si128((simde__m128i*)dst);
#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 NEON implementation using memory for conversion
  uint8_t src[16];
  uint8_t dst[16] = {0};

  simde_mm_store_si128((simde__m128i*)src, a);
  memcpy(dst, src + shift_bytes, 16 - shift_bytes);

  return simde_mm_load_si128((simde__m128i*)dst);
#else
#error "Unsupported architecture for simde_mm_vsro (only x86_64 and ARM64 supported)"
#endif
}

// Variable 16-bit shift left: widen to 32-bit, shift, narrow back
inline simde__m128i simde_mm_sllv_epi16(simde__m128i a, simde__m128i count) {
#if defined(REX_VMX_HELPERS_NEON)
  // USHL reads its count as the lane's low byte, signed: clamped to 16 it gives 0 for every count of 16 or more,
  // as the widened x86 form does.
  const uint16x8_t n = vminq_u16(simde__m128i_to_neon_u16(count), vdupq_n_u16(16));
  return simde__m128i_from_neon_u16(vshlq_u16(simde__m128i_to_neon_u16(a), vreinterpretq_s16_u16(n)));
#else
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi32(a_hi, s_hi);
  simde__m128i mask16 = simde_mm_set1_epi32(0xFFFF);
  r_lo = simde_mm_and_si128(r_lo, mask16);
  r_hi = simde_mm_and_si128(r_hi, mask16);
  return simde_mm_packus_epi32(r_lo, r_hi);
#endif
}

// Variable 16-bit logical right shift: widen to 32-bit, shift, narrow back
inline simde__m128i simde_mm_srlv_epi16(simde__m128i a, simde__m128i count) {
#if defined(REX_VMX_HELPERS_NEON)
  const uint16x8_t n = vminq_u16(simde__m128i_to_neon_u16(count), vdupq_n_u16(16));
  return simde__m128i_from_neon_u16(
      vshlq_u16(simde__m128i_to_neon_u16(a), vnegq_s16(vreinterpretq_s16_u16(n))));  // USHL by -n: a right shift
#else
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srlv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srlv_epi32(a_hi, s_hi);
  return simde_mm_packus_epi32(r_lo, r_hi);
#endif
}

// Variable 16-bit arithmetic right shift: sign-extend to 32-bit, shift, narrow back
inline simde__m128i simde_mm_srav_epi16(simde__m128i a, simde__m128i count) {
#if defined(REX_VMX_HELPERS_NEON)
  // A count of 15 already gives the sign fill, which is what every larger count gives too.
  const uint16x8_t n = vminq_u16(simde__m128i_to_neon_u16(count), vdupq_n_u16(15));
  return simde__m128i_from_neon_i16(
      vshlq_s16(simde__m128i_to_neon_i16(a), vnegq_s16(vreinterpretq_s16_u16(n))));  // SSHL by -n
#else
  simde__m128i zero = simde_mm_setzero_si128();
  // Sign-extend a: duplicate each 16-bit lane, then arithmetic shift right by 16
  simde__m128i a_lo = simde_mm_srai_epi32(simde_mm_unpacklo_epi16(a, a), 16);
  simde__m128i a_hi = simde_mm_srai_epi32(simde_mm_unpackhi_epi16(a, a), 16);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srav_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srav_epi32(a_hi, s_hi);
  return simde_mm_packs_epi32(r_lo, r_hi);
#endif
}

// Variable 8-bit shift left: widen to 16-bit, shift, narrow back
inline simde__m128i simde_mm_sllv_epi8(simde__m128i a, simde__m128i count) {
#if defined(REX_VMX_HELPERS_NEON)
  const uint8x16_t n = vminq_u8(simde__m128i_to_neon_u8(count), vdupq_n_u8(8));
  return simde__m128i_from_neon_u8(vshlq_u8(simde__m128i_to_neon_u8(a), vreinterpretq_s8_u8(n)));
#else
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi8(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi8(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi8(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi8(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi16(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi16(a_hi, s_hi);
  simde__m128i mask8 = simde_mm_set1_epi16(0xFF);
  r_lo = simde_mm_and_si128(r_lo, mask8);
  r_hi = simde_mm_and_si128(r_hi, mask8);
  return simde_mm_packus_epi16(r_lo, r_hi);
#endif
}

//=============================================================================
// VMX float results as the Xbox 360 gives them (futari-nx, 2026-10-26)
//=============================================================================
// The code generator wrote these instructions as x86 SSE through simde, which emulates x86 on AArch64 too: the PC
// and the Switch agreed with each other and not with the 360. Each has its codegen key (rex/codegen/config.h,
// vmx_*). The vector unit always runs with denormals flushed (the code generator's flush mode): AArch64's FZ flushes
// the inputs and results of FMAX / FMIN / FMLA; on x86 DAZ / FTZ cover the SSE arithmetic but neither bitwise
// operations nor a library std::fma, so the x86 forms flush those by hand. Both forms give the same bits.

// A NaN quieted (its quiet bit set, the payload and sign kept), as the vector unit returns one.
inline simde__m128 vmx_quiet_ps(simde__m128 v) {
  return simde_mm_or_ps(v, simde_mm_castsi128_ps(simde_mm_set1_epi32(0x00400000)));
}

// A denormal (and a zero) as the zero of its sign; everything else as it is. By the exponent bits, so it does not
// depend on DAZ.
inline simde__m128 vmx_flush_ps(simde__m128 v) {
  const simde__m128i bits = simde_mm_castps_si128(v);
  const simde__m128i zero_exponent =
      simde_mm_cmpeq_epi32(simde_mm_and_si128(bits, simde_mm_set1_epi32(0x7F800000)), simde_mm_setzero_si128());
  const simde__m128i sign = simde_mm_and_si128(bits, simde_mm_set1_epi32(int(0x80000000)));
  return simde_mm_castsi128_ps(simde_mm_blendv_epi8(bits, sign, zero_exponent));
}

// One float's NaN quieted, and the quiet test of AArch64's NaN priority.
inline float vmx_quiet_f32(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  bits |= 0x00400000;
  std::memcpy(&v, &bits, 4);
  return v;
}

inline bool vmx_is_snan_f32(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  return (bits & 0x7F800000) == 0x7F800000 && (bits & 0x007FFFFF) != 0 && !(bits & 0x00400000);
}

// vmaxfp / vminfp (vmx_exact_minmax): a NaN operand gives that NaN quieted, vA's before vB's; max(+0, -0) = +0,
// min(+0, -0) = -0. x86's maxps / minps return the second operand for a NaN or for two zeros (and simde builds
// exactly that on AArch64: a compare and a select).
inline simde__m128 vmx_max_ps(simde__m128 a, simde__m128 b) {
#if defined(REX_VMX_HELPERS_NEON)
  // FMAX gives vB's NaN quieted when only vB is one, and the zeros as AltiVec does; a NaN in vA comes first.
  const float32x4_t na = simde__m128_to_neon_f32(a);
  const float32x4_t r = vmaxq_f32(na, simde__m128_to_neon_f32(b));
  const uint32x4_t a_nan = vmvnq_u32(vceqq_f32(na, na));
  const float32x4_t qa = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(na), vdupq_n_u32(0x00400000)));
  return simde__m128_from_neon_f32(vbslq_f32(a_nan, qa, r));
#else
  a = vmx_flush_ps(a);
  b = vmx_flush_ps(b);
  simde__m128 r = simde_mm_max_ps(a, b);
  r = simde_mm_blendv_ps(r, simde_mm_and_ps(a, b), simde_mm_cmpeq_ps(a, b));  // two zeros: +0 unless both are -0
  r = simde_mm_blendv_ps(r, vmx_quiet_ps(b), simde_mm_cmpunord_ps(b, b));
  return simde_mm_blendv_ps(r, vmx_quiet_ps(a), simde_mm_cmpunord_ps(a, a));
#endif
}

inline simde__m128 vmx_min_ps(simde__m128 a, simde__m128 b) {
#if defined(REX_VMX_HELPERS_NEON)
  const float32x4_t na = simde__m128_to_neon_f32(a);
  const float32x4_t r = vminq_f32(na, simde__m128_to_neon_f32(b));
  const uint32x4_t a_nan = vmvnq_u32(vceqq_f32(na, na));
  const float32x4_t qa = vreinterpretq_f32_u32(vorrq_u32(vreinterpretq_u32_f32(na), vdupq_n_u32(0x00400000)));
  return simde__m128_from_neon_f32(vbslq_f32(a_nan, qa, r));
#else
  a = vmx_flush_ps(a);
  b = vmx_flush_ps(b);
  simde__m128 r = simde_mm_min_ps(a, b);
  r = simde_mm_blendv_ps(r, simde_mm_or_ps(a, b), simde_mm_cmpeq_ps(a, b));  // two zeros: -0 if either is -0
  r = simde_mm_blendv_ps(r, vmx_quiet_ps(b), simde_mm_cmpunord_ps(b, b));
  return simde_mm_blendv_ps(r, vmx_quiet_ps(a), simde_mm_cmpunord_ps(a, a));
#endif
}

// vmaddfp / vmaddcfp128 (vmx_fused_madd): a * b + c with one rounding, as AltiVec's multiply-add is.
inline simde__m128 vmx_madd_ps(simde__m128 a, simde__m128 b, simde__m128 c) {
#if defined(REX_VMX_HELPERS_NEON)
  return simde__m128_from_neon_f32(vfmaq_f32(simde__m128_to_neon_f32(c), simde__m128_to_neon_f32(a),
                                             simde__m128_to_neon_f32(b)));  // FMLA
#else
  // No FMA instruction in the PC build's x86-64-v2: each lane through std::fma (exact, a library call), the flush of
  // the inputs and the result by hand, and NaNs as FMLA gives them (the PC build and the Switch agree bit for bit):
  // a signalling NaN before a quiet one, the addend before the factors, quieted; 0 x inf with no NaN operand is the
  // default NaN 0x7FC00000 (as on the 360), where x86 arithmetic would make 0xFFC00000.
  alignas(16) float x[4], y[4], z[4], r[4];
  simde_mm_store_ps(x, vmx_flush_ps(a));
  simde_mm_store_ps(y, vmx_flush_ps(b));
  simde_mm_store_ps(z, vmx_flush_ps(c));
  for (int i = 0; i < 4; ++i) {
    if (std::isnan(x[i]) || std::isnan(y[i]) || std::isnan(z[i])) {
      const float order[3] = {z[i], x[i], y[i]};
      float pick = 0.0f;
      bool found = false;
      for (float v : order) {
        if (!found && vmx_is_snan_f32(v)) {
          pick = v;
          found = true;
        }
      }
      for (float v : order) {
        if (!found && std::isnan(v)) {
          pick = v;
          found = true;
        }
      }
      r[i] = vmx_quiet_f32(pick);
      continue;
    }
    r[i] = std::fma(x[i], y[i], z[i]);
    if (std::isnan(r[i])) {
      const uint32_t default_nan = 0x7FC00000;
      std::memcpy(&r[i], &default_nan, 4);
    }
  }
  return vmx_flush_ps(simde_mm_load_ps(r));
#endif
}

// vexptefp (vmx_expte_range): the estimate of 2^x the code generator computes, `estimate`, with the 360's results
// outside its range: 0 below -126 (the denormal results flushed; -inf included), +inf from 128 (+inf included), a NaN
// quieted. Inside the range the estimate is kept.
inline simde__m128 vmx_expte_range_ps(simde__m128 x, simde__m128 estimate) {
  simde__m128 r = simde_mm_blendv_ps(estimate, simde_mm_setzero_ps(), simde_mm_cmplt_ps(x, simde_mm_set1_ps(-126.0f)));
  r = simde_mm_blendv_ps(r, simde_mm_castsi128_ps(simde_mm_set1_epi32(0x7F800000)),
                         simde_mm_cmpge_ps(x, simde_mm_set1_ps(128.0f)));
  return simde_mm_blendv_ps(r, vmx_quiet_ps(x), simde_mm_cmpunord_ps(x, x));
}

}  // namespace rex::ppc

//=============================================================================
// Global Aliases for Generated Code
//=============================================================================
// Vector mask tables accessible from global scope for generated code
using rex::ppc::VectorMaskL;
using rex::ppc::VectorMaskR;
using rex::ppc::VectorShiftTableL;
using rex::ppc::VectorShiftTableR;
