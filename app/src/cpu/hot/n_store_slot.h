// sa2-nx (2026-10-28): hand-written sub_82141700, a store of r4 into one of seven words of an object by a selector
// (r5 = 0, 4, ..., 24). Written against its localized copy (app/generated/hot/n_82141700.h), its reference.
//
// Optimisations: the copy is a 25-way switch (the guest's jump table: an indirect branch and its table load) around
// one store; here an offset table and a mask test.
//
// Audit (against n_82141700.h):
//   r5 > 24 (unsigned): the copy's switch traps (out of range): the copy runs (and traps).
//   r5 = 0 -> [r3+1580], 4 -> [r3+1584], 8 -> [r3+1588], 12 -> [r3+1592], 16 -> [r3+1572], 20 -> [r3+1568],
//   24 -> [r3+1576] = r4 (word, with its card); any other r5 up to 24: no store.
//   Registers: the copy writes back r3, r4, r5 unchanged (ctr, r0, r12 are its locals): none change. Its jump-table
//   load is a read of the game's code (no effect). The guard compares every register (Cmp::kAll) and [r3+1568, +28).
#pragma once

#include "../sa2_hot_common.h"
#include "../../../generated/hot/n_82141700.h"

namespace sa2::hot::n_store_slot {

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t sel = ctx.r5.u32;
  if (sel > 24) [[unlikely]] {
    ::sa2::hot::n_82141700::Native(ctx, base);
    return;
  }
  if (sel & 3) return;
  static constexpr uint16_t kOffset[7] = {1580, 1584, 1588, 1592, 1572, 1568, 1576};
  const uint32_t a = ctx.r3.u32 + kOffset[sel >> 2];
  const uint32_t v = __builtin_bswap32(ctx.r4.u32);
  std::memcpy(Raw(base, a), &v, 4);
  Card(base, a, 4);
}

}  // namespace sa2::hot::n_store_slot
