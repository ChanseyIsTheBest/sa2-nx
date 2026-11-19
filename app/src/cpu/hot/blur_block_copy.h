// blur-nx (2026-11-11, CPU batch 2: docs/vector-store-ideas.md VS-07; the user: "do all the exact ones"): the hand
// natives' block copies inline. sub_822EA3C0's native copies 1 KB and sub_8230A4D8's n blocks of 1 KB with
// std::memcpy, which GCC leaves to the C library at that size: devkitA64's memcpy moves 64 bytes a step through eight
// 8-byte register pairs (ldp/stp x), behind a size dispatch. In the console race of the LTO+PGO build memcpy was 8.8 %
// of a core, mostly from these two on job workers 3 and 4. Here: 64 bytes a step through four 16-byte registers, no
// call, no dispatch. The same bytes (the natives never pass overlapping ranges: they decline them first). A length that
// is not a multiple of 64 goes to memcpy. BLUR_BLOCK_COPY_LIBC (compile time): memcpy always.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace blur_copy {

[[gnu::always_inline]] inline void Copy64s(void* dst, const void* src, size_t len) {
#if defined(__aarch64__) && !defined(BLUR_BLOCK_COPY_LIBC)
  if ((len & 63) == 0) {
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < len; i += 64) {
      const uint8x16_t a = vld1q_u8(s + i), b = vld1q_u8(s + i + 16), c = vld1q_u8(s + i + 32), e = vld1q_u8(s + i + 48);
      vst1q_u8(d + i, a);
      vst1q_u8(d + i + 16, b);
      vst1q_u8(d + i + 32, c);
      vst1q_u8(d + i + 48, e);
    }
    return;
  }
#endif
  std::memcpy(dst, src, len);
}

}  // namespace blur_copy
