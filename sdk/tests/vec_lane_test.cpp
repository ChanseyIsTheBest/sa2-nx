// blur-nx (2026-11-11): rex/ppc/vec_lane.h against the generator's indexed lane reads (tools/vec_lane_index.py).
// Every index of random vectors, compared with v.u32[i] / v.u16[i]; exit 1 on a difference. On AArch64 the NEON path is
// also covered at start-up on the device by app/src/cpu/banjo_forms_selfcheck.cpp.
#include <cstdint>
#include <cstdio>
#include <random>

#include <rex/ppc/vec_lane.h>

int main() {
  std::mt19937 rng(20261111);
  uint64_t checks = 0, differences = 0;
  for (int round = 0; round < 100000; ++round) {
    alignas(16) union {
      uint32_t u32[4];
      uint16_t u16[8];
    } v;
    for (uint32_t& x : v.u32) x = uint32_t(rng());
    for (uint32_t i = 0; i < 4; ++i, ++checks)
      if (rex::ppc::VecLaneU32(v.u32, i) != v.u32[i]) ++differences;
    for (uint32_t i = 0; i < 8; ++i, ++checks)
      if (rex::ppc::VecLaneU16(v.u16, i) != v.u16[i]) ++differences;
  }
  std::printf("vec_lane: %llu checks, %llu differences\n", (unsigned long long)checks, (unsigned long long)differences);
  return differences ? 1 : 0;
}
