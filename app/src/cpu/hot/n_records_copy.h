// sa2-nx (2026-10-28): hand-written sub_82963590, a copy of r5 records of 24 bytes from r4 to r3+4 followed by the
// count at [r3]. Written against its localized copy (app/generated/hot/n_82963590.h), its reference and fallback.
//
// Optimisations: the copy moves a word at a time (a load, a byte swap, a store, a byte swap and a card mark each, 6 a
// record); words copied unchanged are bytes copied unchanged, so here one memcpy and the cards of the range.
//
// Audit (against n_82963590.h):
//   r8 = r4 first, also on the early returns. r4 == 0 or r5 <= 0 (signed 32-bit): return (only r8 changed).
//   Otherwise n = r5 (signed 32-bit, > 0) records: the copy's words go forward one at a time from [r4] to [r3+4], so
//   with overlapping ranges ([r4, r4+24n) and [r3+4, r3+4+24n)) its result is not a memcpy's: the copy runs. It also
//   runs when a range crosses the hosts' physical-range offset (sa2_hot_common.h PhysOff; never on the Switch) or wraps
//   at 4 GB, where the host addresses are not contiguous.
//   Then [r3] = r5 (word, card).
//   Registers as the copy leaves them (64-bit arithmetic where it uses it): r6 = r5 - n (its counter), r7 = r3 + 4 +
//   24n, r8 = r4 + 24n, r9 = the last word read ([r4 + 24n - 4], zero-extended), r10 = the last store's address:
//   upper half of r3 + 24n - 4 (as r3 + 4 + 24(n-1) - 4 in 64 bits), lower half low32(r3 + 24n). ctr, xer, cr0, cr6,
//   r11 are its locals. The guard compares every register (Cmp::kAll) and [r3, r3 + 4 + 24n).
#pragma once

#include "../sa2_hot_common.h"
#include "../../../generated/hot/n_82963590.h"

namespace sa2::hot::n_records_copy {

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64;
  if (uint32_t(r4) == 0 || int32_t(uint32_t(r5)) <= 0) {
    ctx.r8.u64 = r4;
    return;
  }
  const uint32_t n = uint32_t(r5);
  const uint64_t bytes = 24ull * n;
  const uint64_t src = uint32_t(r4), dst = uint64_t(uint32_t(r3)) + 4;
  const bool wraps = src + bytes > 0x100000000ull || dst + bytes > 0x100000000ull;
  const bool overlap = src < dst + bytes && dst < src + bytes;
  const bool phys = PhysOff(uint32_t(src)) != PhysOff(uint32_t(src + bytes - 1)) ||
                    PhysOff(uint32_t(dst)) != PhysOff(uint32_t(dst + bytes - 1));
  if (wraps || overlap || phys) {
    ::sa2::hot::n_82963590::Native(ctx, base);
    return;
  }
  std::memcpy(Raw(base, uint32_t(dst)), Raw(base, uint32_t(src)), size_t(bytes));
  Card(base, uint32_t(dst), uint32_t(bytes));
  // The last word read, before the count's store ([r3] may be that word: the copy reads all before it stores [r3]).
  const uint32_t last_word = Ld32(base, uint32_t(src + bytes - 4));
  const uint32_t count = __builtin_bswap32(uint32_t(r5));
  std::memcpy(Raw(base, uint32_t(r3)), &count, 4);
  Card(base, uint32_t(r3), 4);
  ctx.r6.u64 = r5 - n;
  ctx.r7.u64 = r3 + 4 + bytes;
  ctx.r8.u64 = r4 + bytes;
  ctx.r9.u64 = last_word;
  const uint64_t last_record = r3 + 4 + 24ull * (n - 1) - 4;  // the copy's l_r10 before the record's six stores
  ctx.r10.u64 = (last_record & 0xFFFFFFFF00000000ull) | uint32_t(uint32_t(last_record) + 24u);
}

}  // namespace sa2::hot::n_records_copy
