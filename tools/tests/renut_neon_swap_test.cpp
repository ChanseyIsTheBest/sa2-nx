// banjo-nx (2026-10-17): banjo_renut_neon_swap's vector byte reverses against Renut's SwapDword, on the PC through
// simde. Build: clang++ -std=c++20 -O2 -I sdk/thirdparty/simde tools/tests/renut_neon_swap_test.cpp (0 mismatches).
#define SIMDE_ENABLE_NATIVE_ALIASES
#include <simde/arm/neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
using u32 = uint32_t; using u8 = uint8_t;
u32 SwapDword(u32 value, u32 endian) {
  switch (endian) {
  case 1: return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
  case 2: return (value << 24) | ((value << 8) & 0x00FF0000u) | ((value >> 8) & 0x0000FF00u) | (value >> 24);
  case 3: return (value << 16) | (value >> 16);
  default: return value;
  }
}
template <u32 kEndian> inline uint8x16_t SwapVector(uint8x16_t v) {
  if constexpr (kEndian == 1) return vrev16q_u8(v);
  else if constexpr (kEndian == 2) return vrev32q_u8(v);
  else return vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
}
template <u32 E> bool Check(const u8 *src) {
  u8 out[16]; vst1q_u8(out, SwapVector<E>(vld1q_u8(src)));
  for (int w = 0; w < 4; ++w) { u32 a, b; std::memcpy(&a, src + 4 * w, 4); std::memcpy(&b, out + 4 * w, 4);
    if (SwapDword(a, E) != b) return false; }
  return true;
}
int main() {
  std::mt19937 rng(7); u8 buf[16]; int bad = 0;
  for (int t = 0; t < 200000; ++t) { for (auto &b : buf) b = u8(rng());
    bad += !Check<1>(buf) + !Check<2>(buf) + !Check<3>(buf); }
  std::printf("neon swap vs SwapDword: %d mismatches in 600000 vectors\n", bad);
  return bad != 0;
}
