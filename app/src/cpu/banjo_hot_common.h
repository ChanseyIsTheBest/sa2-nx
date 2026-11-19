// banjo-nx - native replacements of hot guest functions: shared definitions.
//
// The mechanism is sa2-nx's (app/src/cpu/sa2_hot_common.h, GPL-3.0, itself from masseffect-nx's me_hot_common.h): a
// "hot hook" replaces one recompiled guest function (sub_X) by a native version with the same observable behaviour,
// under its own key, checked against the recompiled original by a guard (banjo_hot_guest.cpp).
//
// Banjo's natives are "localized copies" written by tools/localize_native.py into app/generated/hot/n_<id>.h: the
// recompiled function's own statements with its context registers in C++ locals. What is different from sa2-nx: the
// guard learns the guest memory a call writes from a WRITE LOG instead of a hand-written list per function. Every copy
// exists a second time with each guest store recorded first (address, length, the bytes it replaces), and so do the
// original statements; the two policies below select the form at compile time.
//
// What "exact" means here: every guest byte the original writes, every register it leaves in the context (all of
// them are compared: a copy writes back exactly the registers its original changes) and the host's flush mode.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/platform.h>
#include <rex/system/xmemory.h>  // banjo-nx (2026-10-19): rex::memory::MarkStoreCardsAt
#include <rex/ppc/context.h>

namespace banjo::hand {
// banjo_hot_count: one call of a hand-written native (hot/banjo_hand_natives.h) with its size (banjo_hot_guest.cpp).
void Count(uint32_t which, uint32_t n);
}  // namespace banjo::hand

namespace banjo::hot {

// Registers the guard leaves out of its compare for one hook: those a hand-written native in the hook's copy leaves
// as it found them where the original changes them (tools/localize_native.py writes one per header, all zero for a
// copy without such a native). Bit n = register n; vr[0] = v0..v63, vr[1] = v64..v127; cr bit n = crn.
struct RegSkip {
  uint32_t gpr;
  uint32_t fpr;
  uint64_t vr[2];
  uint8_t cr;
  bool ctr;
  bool xer;
};

// Guest memory with the generated code's addressing (the +0x1000 physical-range offset of the Windows host).
inline uint32_t PhysOff(uint32_t a) {
#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
  return a >= 0xE0000000u ? 0x1000u : 0u;
#else
  (void)a;
  return 0u;
#endif
}
inline uint8_t* Raw(uint8_t* base, uint32_t a) { return base + a + PhysOff(a); }

// A stretch of guest memory.
struct Region {
  uint32_t addr;
  uint32_t len;
};

// A piece of guest code a copy was made from (tools/localize_native.py writes the list and its hash).
struct CodeRange {
  uint32_t address;
  uint32_t bytes;
};

// The guest stores of one logged run, in order: where, how many bytes (1..16) and what was there.
struct WriteLog {
  struct Entry {
    uint32_t addr;
    uint32_t len;
    uint8_t old[16];
  };
  static constexpr uint32_t kMaxEntries = 1u << 22;  // 96 MB: a call that stores more is not compared
  Entry* entries = nullptr;
  uint32_t count = 0;
  uint32_t capacity = 0;
  uint8_t* base = nullptr;
  bool stopped = false;   // the run reached a call it cannot repeat, or the log is full: it wrote nothing after that
  bool overflow = false;  // ... the second case

  void Reset(uint8_t* guest_base) {
    base = guest_base;
    count = 0;
    stopped = overflow = false;
  }
  bool Grow();  // banjo_hot_guest.cpp
  // Records the bytes a store is about to replace. false: the store must not happen (the run is over).
  bool Note(uint32_t addr, uint32_t len) {
    if (stopped) [[unlikely]] return false;
    while (len) {
      const uint32_t n = len > 16 ? 16 : len;
      if (count == capacity && !Grow()) [[unlikely]] {
        stopped = overflow = true;
        return false;
      }
      Entry& e = entries[count++];
      e.addr = addr;
      e.len = n;
      std::memcpy(e.old, Raw(base, addr), n);
      addr += n;
      len -= n;
    }
    return true;
  }
  // Puts back what the run replaced, newest store first.
  void RollBack() {
    for (uint32_t i = count; i-- > 0;) std::memcpy(Raw(base, entries[i].addr), entries[i].old, entries[i].len);
  }
};

// banjo-nx (2026-10-19): a range a hand-written native writes (outside the stack) marks its store cards, as the guest
// code's stores would (rex/system/xmemory.h; Renut proves its cached vertex and index copies by them).
// blur-nx (2026-11-13, M-22 of docs/cpu-moonshots-50-1113.md): a range of one or two pages (nearly every native store)
// marks as the copies' own stores do (banjo_hot_prelude.h's rex_card_mark_, VS-11/VS-12): only the pages whose card
// Renut reads (the views it checks and the page in front of each), the card byte written only when it is not zero yet.
// MarkStoreCardsAt's page loop became a memset call (the copies held at least 136 card-range `bl memset`s, about
// 0.5 ms a frame, mostly in the job-queue and audio copies the hand natives are inlined into). Exact for every card
// Renut reads (the other cards are never read); a longer range marks as before. BLUR_CARD_RUN_ALL_PAGES: as before.
#if !defined(BLUR_CARD_RUN_ALL_PAGES)
[[gnu::always_inline]] inline void CardPage(uint8_t* base, uint32_t page) {
  if (page - 0x9FFFFu <= 0x60000u || page - 0x7EFFFu <= 0x1000u) {
    uint8_t* const card = base + page;
    if (*card) *card = 0;
  }
}
[[gnu::always_inline]] inline void Card(uint8_t* base, uint32_t addr, uint32_t len) {
  if (!len) return;
  const uint64_t end = uint64_t(addr) + len - 1;
  const uint32_t first = addr >> 12;
  const uint32_t last = end > 0xFFFFFFFFull ? 0xFFFFFu : uint32_t(end >> 12);
  if (last - first <= 1) {
    CardPage(base, first);
    if (last != first) CardPage(base, last);
    return;
  }
  rex::memory::MarkStoreCardsAt(base, addr, len);
}
#else
[[gnu::always_inline]] inline void Card(uint8_t* base, uint32_t addr, uint32_t len) {
  rex::memory::MarkStoreCardsAt(base, addr, len);
}
#endif

// The copy that runs: nothing is recorded, nothing stops it. Every member folds to a constant.
struct Fast {
  [[gnu::always_inline]] constexpr bool Note(uint32_t, uint32_t) const { return true; }
  [[gnu::always_inline]] constexpr bool Stopped() const { return false; }
  [[gnu::always_inline]] constexpr bool Open() const { return false; }
  [[gnu::always_inline]] constexpr void Hit(uint32_t) const {}
  [[gnu::always_inline]] constexpr void Hand(uint32_t, uint32_t) const {}
};

// The guard's form: every store recorded first; a call outside the copy ends the run before it is made.
struct Logged {
  WriteLog& log;
  bool Note(uint32_t addr, uint32_t len) { return log.Note(addr, len); }
  bool Stopped() const { return log.stopped; }
  bool Open() {
    log.stopped = true;
    return true;
  }
  [[gnu::always_inline]] constexpr void Hit(uint32_t) const {}
  [[gnu::always_inline]] constexpr void Hand(uint32_t, uint32_t) const {}
};

// The measuring form (banjo_hot_count): the copy as it runs, counting how often each straight run of statements
// ("block") is entered. tools/hot_arm_compare.py multiplies the counts by the AArch64 instructions of each block, of
// the copy and of the original: the nearest thing to a console measurement that a PC run can give.
struct Counting {
  std::atomic<uint64_t>* hits;
  [[gnu::always_inline]] constexpr bool Note(uint32_t, uint32_t) const { return true; }
  [[gnu::always_inline]] constexpr bool Stopped() const { return false; }
  [[gnu::always_inline]] constexpr bool Open() const { return false; }
  void Hit(uint32_t block) { hits[block].fetch_add(1, std::memory_order_relaxed); }
  void Hand(uint32_t which, uint32_t n) { ::banjo::hand::Count(which, n); }
};

}  // namespace banjo::hot

// The stores of the copies (tools/localize_native.py renames REX_STORE_* to these): recorded, then made.
#define BANJO_HOT_STORE_U8(x, y) (s.Note((u32)(x), 1) ? (void)(REX_STORE_U8(x, y)) : (void)0)
#define BANJO_HOT_STORE_U16(x, y) (s.Note((u32)(x), 2) ? (void)(REX_STORE_U16(x, y)) : (void)0)
#define BANJO_HOT_STORE_U32(x, y) (s.Note((u32)(x), 4) ? (void)(REX_STORE_U32(x, y)) : (void)0)
#define BANJO_HOT_STORE_U64(x, y) (s.Note((u32)(x), 8) ? (void)(REX_STORE_U64(x, y)) : (void)0)
#define BANJO_HOT_STORE_D_U8(r, d, y) (s.Note((u32)(r) + (u32)(d), 1) ? (void)(REX_STORE_D_U8(r, d, y)) : (void)0)
#define BANJO_HOT_STORE_D_U16(r, d, y) (s.Note((u32)(r) + (u32)(d), 2) ? (void)(REX_STORE_D_U16(r, d, y)) : (void)0)
#define BANJO_HOT_STORE_D_U32(r, d, y) (s.Note((u32)(r) + (u32)(d), 4) ? (void)(REX_STORE_D_U32(r, d, y)) : (void)0)
#define BANJO_HOT_STORE_D_U64(r, d, y) (s.Note((u32)(r) + (u32)(d), 8) ? (void)(REX_STORE_D_U64(r, d, y)) : (void)0)
// banjo-nx (2026-10-19): the stores outside the stack, which tools/store_cards.py renames to these: they mark their
// store cards (REX_CARD of the generated pch), as the generated code's do.
#define BANJO_HOT_STORE_U8_C(x, y) (s.Note((u32)(x), 1) ? (void)(REX_STORE_U8(x, y), REX_CARD(x)) : (void)0)
#define BANJO_HOT_STORE_U16_C(x, y) (s.Note((u32)(x), 2) ? (void)(REX_STORE_U16(x, y), REX_CARD(x)) : (void)0)
#define BANJO_HOT_STORE_U32_C(x, y) (s.Note((u32)(x), 4) ? (void)(REX_STORE_U32(x, y), REX_CARD(x)) : (void)0)
#define BANJO_HOT_STORE_U64_C(x, y) (s.Note((u32)(x), 8) ? (void)(REX_STORE_U64(x, y), REX_CARD(x)) : (void)0)
#define BANJO_HOT_STORE_D_U8_C(r, d, y) \
  (s.Note((u32)(r) + (u32)(d), 1) ? (void)(REX_STORE_D_U8(r, d, y), REX_CARD((u32)(r) + (u32)(d))) : (void)0)
#define BANJO_HOT_STORE_D_U16_C(r, d, y) \
  (s.Note((u32)(r) + (u32)(d), 2) ? (void)(REX_STORE_D_U16(r, d, y), REX_CARD((u32)(r) + (u32)(d))) : (void)0)
#define BANJO_HOT_STORE_D_U32_C(r, d, y) \
  (s.Note((u32)(r) + (u32)(d), 4) ? (void)(REX_STORE_D_U32(r, d, y), REX_CARD((u32)(r) + (u32)(d))) : (void)0)
#define BANJO_HOT_STORE_D_U64_C(r, d, y) \
  (s.Note((u32)(r) + (u32)(d), 8) ? (void)(REX_STORE_D_U64(r, d, y), REX_CARD((u32)(r) + (u32)(d))) : (void)0)
// blur-nx (2026-11-13, M-23): tools/float_loads.py's single stores from the vector unit, as the 32-bit ones.
#define BANJO_HOT_STORE_F32ANY(x, v) (s.Note((u32)(x), 4) ? (void)(REX_STORE_F32ANY(x, v)) : (void)0)
#define BANJO_HOT_STORE_D_F32ANY(r, d, v) \
  (s.Note((u32)(r) + (u32)(d), 4) ? (void)(REX_STORE_D_F32ANY(r, d, v)) : (void)0)
#define BANJO_HOT_STORE_F32ANY_C(x, v) (s.Note((u32)(x), 4) ? (void)(REX_STORE_F32ANY(x, v), REX_CARD(x)) : (void)0)
#define BANJO_HOT_STORE_D_F32ANY_C(r, d, v) \
  (s.Note((u32)(r) + (u32)(d), 4) ? (void)(REX_STORE_D_F32ANY(r, d, v), REX_CARD((u32)(r) + (u32)(d))) : (void)0)
// sasrt-nx (2026-11-11, port 74; its idea 52): a prefetch of the guest node a pointer-chasing loop just loaded
// (tools/localize_native.py, PREFETCH). A hint: it cannot fault. CMake SASRT_HOT_PREFETCH=OFF: nothing.
#ifndef REX_PREFETCH_GUEST
#if defined(SASRT_HOT_PREFETCH_OFF)
#define REX_PREFETCH_GUEST(a) ((void)0)
#else
#define REX_PREFETCH_GUEST(a) __builtin_prefetch(REX_RAW_ADDR((u32)(a)))
#endif
#endif
// sasrt-nx (2026-11-11, port 74; its idea 39): tools/localize_native.py's store-card runs; nothing unless
// banjo_hot_prelude.h (the bulk shards, CMake SASRT_HOT_CARD_RUNS, copies written with the runs) defines them.
#ifndef REX_CARD_RUN_DECL
#define REX_CARD_RUN_DECL
#endif
#ifndef REX_CARD_FLUSH
#define REX_CARD_FLUSH() ((void)0)
#endif
