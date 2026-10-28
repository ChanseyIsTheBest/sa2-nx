// sa2-nx (2026-10-28): verify mode of the hand-written natives' fast loops (hot/n_*.h). A hand-written native made
// from a localized copy replaces a loop of it by a fast loop that applies under conditions it tests, and keeps the
// copy's loop for the rest. In verify mode (sa2_cpu_verify) every fast loop that runs checks itself: the guest bytes
// it writes and the registers either loop writes are saved, the fast loop runs, its result is kept aside and the
// inputs put back, the copy's loop runs on them, and the two results are compared (the copy's stands). A difference
// counts in the [verify] line and logs "[hot] DIFFERENCE".
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#include <rex/logging.h>

#include "../sa2_enhancements.h"
#include "../sa2_hot_common.h"

namespace sa2::hot::fast_loop {

struct Check {
  static constexpr int kRanges = 4, kRegs = 24;
  uint32_t at[kRanges] = {}, len[kRanges] = {};
  int ranges = 0;
  std::vector<uint8_t> before[kRanges], fast[kRanges];
  uint64_t in[kRegs] = {}, out[kRegs] = {};
  void Add(uint32_t a, uint32_t n) {
    at[ranges] = a;
    len[ranges] = n;
    ++ranges;
  }
};

// The fast loops' conditions: guest ranges [a, a + an) and [b, b + bn) (64-bit ends: no wrap) apart, and one that
// neither wraps at 4 GB nor crosses the hosts' physical-range offset (one host range).
inline bool Apart(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) { return a + an <= b || b + bn <= a; }
inline bool Flat(uint64_t a, uint64_t n) {
  return n != 0 && a + n <= 0x100000000ull && PhysOff(uint32_t(a)) == PhysOff(uint32_t(a + n - 1));
}

inline void Save(const uint8_t* base, uint32_t a, uint32_t len, std::vector<uint8_t>& out) {
  out.resize(len);
  for (uint32_t k = 0; k < len; ++k) out[k] = *Raw(base, a + k);
}

inline void Restore(uint8_t* base, uint32_t a, const std::vector<uint8_t>& in) {
  for (uint32_t k = 0; k < in.size(); ++k) *Raw(base, a + k) = in[k];
}

// Before the fast loop: the bytes it may write.
inline void Begin(Check& c, const uint8_t* base) {
  for (int k = 0; k < c.ranges; ++k) Save(base, c.at[k], c.len[k], c.before[k]);
}

// After the fast loop: its bytes kept aside, the inputs' put back (the registers by the caller).
inline void Swap(Check& c, uint8_t* base) {
  for (int k = 0; k < c.ranges; ++k) {
    Save(base, c.at[k], c.len[k], c.fast[k]);
    Restore(base, c.at[k], c.before[k]);
  }
}

// After the copy's loop: the first `compared` registers (`want`: the copy's) and the bytes against the fast loop's.
inline void Finish(Check*& c, const uint8_t* base, const uint64_t* want, int compared, const char* what,
                   std::atomic<uint32_t>& checks) {
  bool same = std::equal(want, want + compared, c->out);
  std::vector<uint8_t> now;
  for (int k = 0; k < c->ranges; ++k) {
    Save(base, c->at[k], c->len[k], now);
    same = same && now == c->fast[k];
  }
  ::sa2::enh::VerifyCheck();
  const uint32_t n = checks.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == 1 || n == 4096 || n == 262144) REXLOG_INFO("[hot] {}: {} checks against the copy's loop", what, n);
  if (!same) {
    ::sa2::enh::VerifyDifference();
    REXLOG_ERROR("[hot] DIFFERENCE: {} differs from the copy's loop (first range {:08X}, {} bytes); the copy's result "
                 "stands", what, c->at[0], c->len[0]);
  }
  delete c;
  c = nullptr;
}

}  // namespace sa2::hot::fast_loop
