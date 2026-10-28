// sa2-nx (2026-10-28): hand-written sub_8213EC28, an ordered tree's (MSVC std::map's) lower-bound search by a 32-bit
// key, branchless. Written against its localized copy (app/generated/hot/n_8213EC28.h), which stays its reference.
//
// Optimisations: the copy walks the tree with a branch per node on the key compare (left or right child), which the
// A57 mispredicts about half the time in a search. Here each step reads both children (they and the key sit in the
// node's first 16 bytes, one line) and selects the next node and the candidate with conditional selects; the loop's
// only branch is its exit (the next node's nil flag). The node's loads are the copy's plus the other child: reads of
// the same node, which the copy reads too, so nothing that could fault and nothing written.
//
// Audit (against n_8213EC28.h, statement by statement):
//   head r10 = [r4+4]; root r11 = [head+4]; r9 = root->isnil (byte +33).
//   isnil == 0: r9 = key [r5]; loop { r8 = [node+12]; if (r8 < key unsigned) node = [node+8]; else { cand = node;
//     node = [node+0]; } r8 = [node+33] byte } while (r8 == 0). The selects below make the same cand and node.
//   Then r11 = [r4+4] (head, read again as the copy does); [r1-16] = cand (stack, no card, as REX_STORE_D_U32).
//   cand != head: r9 = [r5]; r8 = [cand+12]; if !(r9 < r8): [r3] = cand (with card), return.
//   Otherwise: [r1-12] = head (stack); [r3] = head (with card), return.
//   Registers the copy writes back: r8, r9, r10 (r1, r3, r4, r5 unchanged); r11 and cr6 are its locals. This keeps r8,
//   r9 and r10 exactly as the copy leaves them on every path (root nil: r9 = the nil byte, r8 untouched).
//   Memory: [r1-16] always, [r1-12] only on the not-found path, [r3] always, in the copy's order. The guard compares
//   every register (Cmp::kAll) and both ranges (sa2_hot_guest.cpp).
#pragma once

#include "../sa2_hot_common.h"

namespace sa2::hot::n_map_lower_bound {

inline uint8_t Ld8(const uint8_t* base, uint32_t a) { return *Raw(base, a); }
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Raw(base, a), &v, 4);
}

inline void Native(PPCContext& ctx, uint8_t* base) {
  const uint32_t r1 = ctx.r1.u32, r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  const uint32_t head = Ld32(base, r4 + 4);
  uint32_t cand = head;
  uint32_t node = Ld32(base, head + 4);
  uint32_t r9 = Ld8(base, node + 33);
  uint32_t r8 = ctx.r8.u32;
  bool r8_set = false;
  if (r9 == 0) {
    const uint32_t key = Ld32(base, r5);
    r9 = key;
    uint32_t nil;
    do {
      const uint32_t nkey = Ld32(base, node + 12);
      const uint32_t left = Ld32(base, node + 0);
      const uint32_t right = Ld32(base, node + 8);
      const bool go_right = nkey < key;
      cand = go_right ? cand : node;
      node = go_right ? right : left;
      nil = Ld8(base, node + 33);
    } while (nil == 0);
    r8 = nil;
    r8_set = true;
  }
  const uint32_t head2 = Ld32(base, r4 + 4);
  St32(base, r1 - 16, cand);
  if (cand != head2) {
    r9 = Ld32(base, r5);
    r8 = Ld32(base, cand + 12);
    r8_set = true;
    if (!(r9 < r8)) {
      St32(base, r3, cand);
      Card(base, r3, 4);
      ctx.r10.u64 = cand;
      ctx.r9.u64 = r9;
      ctx.r8.u64 = r8;
      return;
    }
  }
  St32(base, r1 - 12, head2);
  St32(base, r3, head2);
  Card(base, r3, 4);
  ctx.r10.u64 = cand;
  ctx.r9.u64 = r9;
  if (r8_set) ctx.r8.u64 = r8;
}

}  // namespace sa2::hot::n_map_lower_bound
