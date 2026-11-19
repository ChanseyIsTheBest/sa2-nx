// banjo-nx (2026-10-20, from sa2-nx): whether two 64-byte lines differ, for the device chunks of the delta chain (backend/queued.cpp)
// and the pushed shared constants' lines (render/draw.cpp). GCC left the eight 64-bit words of the plain version scalar
// (about 30 instructions a line: 9 % of the game thread in the second console run); four 128-bit loads a side, XORed,
// ORed and one horizontal max are about 10 (banjo_renut_neon_compare).
#pragma once

#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace banjo::renut_gpu {

inline bool Line64DiffersPlain(const uint8_t *a, const uint8_t *b) {
  uint64_t diff = 0;
  for (uint32_t k = 0; k < 64; k += 8) {
    uint64_t x, y;
    std::memcpy(&x, a + k, 8);
    std::memcpy(&y, b + k, 8);
    diff |= x ^ y;
  }
  return diff != 0;
}

inline bool Line64Differs(const uint8_t *a, const uint8_t *b, bool neon) {
#if defined(__aarch64__)
  if (neon) {
    const uint8x16x4_t x = vld1q_u8_x4(a);
    const uint8x16x4_t y = vld1q_u8_x4(b);
    const uint8x16_t d = vorrq_u8(vorrq_u8(veorq_u8(x.val[0], y.val[0]), veorq_u8(x.val[1], y.val[1])),
                                  vorrq_u8(veorq_u8(x.val[2], y.val[2]), veorq_u8(x.val[3], y.val[3])));
    return vmaxvq_u32(vreinterpretq_u32_u8(d)) != 0;
  }
#else
  (void)neon;
#endif
  return Line64DiffersPlain(a, b);
}

// banjo-nx (2026-10-23, render-thread round): which of 'count' (at most 32) 64-byte lines differ, bit i = line i, with
// no branch per line: four lines' XORs folded by pairwise maxima into one vector of the four lines' maxima, and that
// into four bits. A branch per line (Line64Differs in a loop) mispredicted on the pushed shared constants, whose
// changed lines vary from draw to draw: 7 % of the console's render thread in the city (2026-10-23 profile).
inline uint32_t Lines64DifferMask(const uint8_t *a, const uint8_t *b, uint32_t count) {
  uint32_t mask = 0;
  uint32_t i = 0;
#if defined(__aarch64__)
  const uint32x4_t bits = {1u, 2u, 4u, 8u};
  auto line = [](const uint8_t *x, const uint8_t *y) {
    const uint8x16x4_t p = vld1q_u8_x4(x);
    const uint8x16x4_t q = vld1q_u8_x4(y);
    return vreinterpretq_u32_u8(vorrq_u8(vorrq_u8(veorq_u8(p.val[0], q.val[0]), veorq_u8(p.val[1], q.val[1])),
                                         vorrq_u8(veorq_u8(p.val[2], q.val[2]), veorq_u8(p.val[3], q.val[3]))));
  };
  const auto group = [&](uint32_t g) {  // lines g..g+3's bits, at bit g
    const uint32x4_t l0 = line(a + g * 64, b + g * 64), l1 = line(a + g * 64 + 64, b + g * 64 + 64);
    const uint32x4_t l2 = line(a + g * 64 + 128, b + g * 64 + 128), l3 = line(a + g * 64 + 192, b + g * 64 + 192);
    const uint32x4_t m = vpmaxq_u32(vpmaxq_u32(l0, l1), vpmaxq_u32(l2, l3));  // lane k: line g + k's maximum
    return vaddvq_u32(vandq_u32(vtstq_u32(m, m), bits)) << g;
  };
  for (; i + 4 <= count; i += 4)
    mask |= group(i);
  // bangaio-nx (2026-10-29): 1-3 lines left of four or more: the last four lines again (the overlap's bits are the
  // same), not a reduction and a branch per line.
  if (i < count && count >= 4) {
    mask |= group(count - 4);
    i = count;
  }
  for (; i < count; ++i)
    mask |= uint32_t(Line64Differs(a + i * 64, b + i * 64, true)) << i;
#else
  for (; i < count; ++i)
    mask |= uint32_t(Line64DiffersPlain(a + i * 64, b + i * 64)) << i;
#endif
  return mask;
}

}  // namespace banjo::renut_gpu
