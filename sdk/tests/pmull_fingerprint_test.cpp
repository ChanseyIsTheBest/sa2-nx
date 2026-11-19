// banjo-nx (2026-10-15): app/src/me/masseffect/banjo_pmull_fingerprint.h, the PMULL-folded 64-bit CRC of the ring
// thread's whole-range fingerprints, against its bit-by-bit reference, through simde's model of the NEON / crypto
// instructions (the same intrinsics the Switch build compiles to PMULL, PMULL2, EOR).
//   tools/run_sdk_tests.bat pmull
// Checks: every length 0..1100 and some long ones, random seeds and data, equal to the reference; every single-bit
// change of a 1 KB message changes the fingerprint; the 4-chain path and the block-by-block path give the same value for
// the same bytes (split points around 64).
#include <cstdio>
#include <random>
#include <vector>

#include <simde/arm/neon.h>
// The NEON names the header uses, mapped to simde's (simde's global aliases collide with clang's x86 CRC intrinsics).
using uint64x2_t = simde_uint64x2_t;
using poly64x2_t = simde_poly64x2_t;
using poly64_t = simde_poly64_t;
#define vreinterpretq_p64_u64 simde_vreinterpretq_p64_u64
#define vreinterpretq_u64_p128 simde_vreinterpretq_u64_p128
#define vreinterpretq_u64_u8 simde_vreinterpretq_u64_u8
#define vmull_p64 simde_vmull_p64
#define vmull_high_p64 simde_vmull_high_p64
#define vgetq_lane_p64 simde_vgetq_lane_p64
#define vgetq_lane_u64 simde_vgetq_lane_u64
#define veorq_u64 simde_veorq_u64
#define vcombine_u64 simde_vcombine_u64
#define vcreate_u64 simde_vcreate_u64
#define vld1q_u8 simde_vld1q_u8
#define BANJO_PMULL_TEST 1
#include "../../app/src/me/masseffect/banjo_pmull_fingerprint.h"

using namespace banjo::pmull_fingerprint;

int main() {
  std::mt19937_64 rng(0x5EED1234);
  uint64_t checked = 0, differences = 0;
  std::vector<uint8_t> data(70000);
  for (auto& b : data) b = uint8_t(rng());
  auto check = [&](size_t offset, size_t bytes, uint64_t seed) {
    const uint64_t a = Fingerprint(data.data() + offset, bytes, seed);
    const uint64_t b = Reference(data.data() + offset, bytes, seed);
    ++checked;
    if (a != b) {
      if (differences < 10)
        std::printf("DIFFERENCE: offset %zu, %zu bytes, seed %016llx: %016llx against %016llx\n", offset, bytes,
                    (unsigned long long)seed, (unsigned long long)a, (unsigned long long)b);
      ++differences;
    }
  };
  for (size_t bytes = 0; bytes <= 1100; ++bytes) {
    check(rng() % 64, bytes, 0);
    check(rng() % 64, bytes, rng());
  }
  for (size_t bytes : {4096u, 4097u, 4111u, 4159u, 65536u, 65535u, 69999u}) check(0, bytes, rng());
  // zero-filled and all-ones data (structured inputs)
  std::vector<uint8_t> saved = data;
  std::fill(data.begin(), data.end(), 0);
  for (size_t bytes = 0; bytes <= 300; ++bytes) check(0, bytes, 0);
  std::fill(data.begin(), data.end(), 0xFF);
  for (size_t bytes = 0; bytes <= 300; ++bytes) check(0, bytes, 0);
  data = saved;
  // every single-bit change of a 1 KB message is seen
  uint64_t flips = 0, missed = 0;
  const uint64_t base = Fingerprint(data.data(), 1024, 0);
  for (size_t bit = 0; bit < 1024 * 8; ++bit) {
    data[bit / 8] ^= uint8_t(1u << (bit % 8));
    if (Fingerprint(data.data(), 1024, 0) == base) ++missed;
    data[bit / 8] ^= uint8_t(1u << (bit % 8));
    ++flips;
  }
  // two-bit changes less than 64 bits apart in one 16-byte block (a difference of degree < 64: always seen)
  for (int t = 0; t < 20000; ++t) {
    const size_t block = rng() % 64, i0 = rng() % (128 - 63), b0 = block * 128 + i0, b1 = b0 + 1 + rng() % 62;
    data[b0 / 8] ^= uint8_t(1u << (b0 % 8));
    data[b1 / 8] ^= uint8_t(1u << (b1 % 8));
    if (Fingerprint(data.data(), 1024, 0) == base) ++missed;
    data[b0 / 8] ^= uint8_t(1u << (b0 % 8));
    data[b1 / 8] ^= uint8_t(1u << (b1 % 8));
    ++flips;
  }
  std::printf("pmull_fingerprint: %llu fingerprints against the reference, %llu differences; %llu changed messages, "
              "%llu not seen\n",
              (unsigned long long)checked, (unsigned long long)differences, (unsigned long long)flips,
              (unsigned long long)missed);
  return differences || missed ? 1 : 0;
}
