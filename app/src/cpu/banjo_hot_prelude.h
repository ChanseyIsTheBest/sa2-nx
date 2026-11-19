// banjo-nx (2026-10-22): what each shard of the bulk copies (banjo_hot_shard_<k>.cpp) includes before its copies,
// as banjo_hot_guest.cpp does for the hand-picked list: the generated code's macros (a copy is generated statements),
// the hook machinery (banjo_hot_guest.h) and the hand-written natives a copy asks first (hot/hand.txt).
// The method is bejeweled3-nx's (bulk localization, its docs/optimising-functions.md).
#pragma once

#include "renut_pch.h"

#include "cpu/banjo_hot_guest.h"
#include "cpu/hot/banjo_hand_natives.h"
// sasrt-nx (2026-11-11, port 74; its idea 39): store-card runs in the bulk copies (tools/localize_native.py,
// CARD_RUNS). A copy's function keeps the page of its run of stores (rex_card_run; 0xFFFFF: none pending) and marks a
// page's card when the run moves to another page; REX_CARD_FLUSH marks the pending one (before calls, fences and
// returns, and at the end) and clears it. A compare-and-swap marks the pending page and its own at once. With nothing
// pending a flush compiles to nothing (sasrt-nx's batch 2 audit, finding 1: a function that stores nothing marks
// nothing). Only for copies written with the runs (hot/banjo_hot_options.h: finding 2). The hand-written natives above
// keep marking at each store (their REX_CARD was expanded before this point).
#if __has_include("hot/banjo_hot_options.h")
#include "hot/banjo_hot_options.h"
#endif
#if defined(SASRT_HOT_CARD_RUNS) && defined(REX_CARD_RUNS_GENERATED) && !defined(REX_STORE_CARDS_OFF)
#undef REX_CARD
#undef REX_CARD_RUN_DECL
#undef REX_CARD_FLUSH
#undef REX_GUEST_CAS_C
#if !defined(BLUR_CARD_RUN_ALL_PAGES)
// blur-nx (2026-11-11, CPU batch 1: docs/vector-store-ideas.md VS-11 and VS-12; the user: "do all the exact ones"). The
// run's page is marked as before (when the run moves on, and at a flush: after its stores), but
// - only a page whose card Renut reads: the views it checks (draw.cpp's PhysicalOf: 0xA0000000 and up, and
//   0x7F000000-0x7FFFFFFF) and the page in front of each (Renut counts a page as written when the page before it is:
//   an unaligned store into a view's first page marks the page in front), so 0x9FFFF-0xFFFFF and 0x7EFFF-0x7FFFF;
//   the cards of other pages are never read (Blur's audio and object data, the heap's virtual views);
// - the card byte only written when it is not zero yet: a zero card is the result either way, and the card lines
//   (64 cards each) stay shared between the cores instead of moving at every page change of every thread;
// - "no run" is 0x100000, outside the 20-bit pages: port 74's 0xFFFFF was also the top page of the 0xE0000000 view,
//   whose run was then never marked.
// BLUR_CARD_RUN_ALL_PAGES (compile time): port 74's macros as they were.
[[gnu::always_inline]] static inline void rex_card_mark_(uint8_t* base, u32 page) {
  // blur-nx (2026-11-13, #38 / #66's census, PC only): 26 the copies' flushes with a run pending (the 16:05 census
  // counted every flush, an empty one too), 33 of them on a page Renut reads, 34 of those that wrote the card.
  REX_NX_CENSUS_IF(page != 0x100000u, 26);
  if (page - 0x9FFFFu <= 0x60000u || page - 0x7EFFFu <= 0x1000u) {
    u8* const card = (u8*)(base + page);
    REX_NX_CENSUS(33);
    REX_NX_CENSUS_IF(*card != 0, 34);
    if (*card) *card = 0;
  }
}
#define REX_CARD_RUN_DECL [[maybe_unused]] u32 rex_card_run = 0x100000u;
#define REX_CARD(ea)                                                                                      \
  ((void)((((u32)(ea)) >> 12) != rex_card_run                                                            \
              ? (rex_card_mark_(base, rex_card_run), rex_card_run = ((u32)(ea)) >> 12, 0)                 \
              : 0))
#define REX_CARD_FLUSH() ((void)(rex_card_mark_(base, rex_card_run), rex_card_run = 0x100000u))
#else
#define REX_CARD_RUN_DECL [[maybe_unused]] u32 rex_card_run = 0xFFFFFu;
#define REX_CARD(ea)                                                                                      \
  ((void)((((u32)(ea)) >> 12) != rex_card_run                                                            \
              ? ((rex_card_run != 0xFFFFFu ? (void)(*(u8*)(base + rex_card_run) = 0) : (void)0),          \
                 rex_card_run = ((u32)(ea)) >> 12, 0)                                                     \
              : 0))
#define REX_CARD_FLUSH() \
  ((void)(rex_card_run != 0xFFFFFu ? (*(u8*)(base + rex_card_run) = 0, rex_card_run = 0xFFFFFu, 0) : 0))
#endif
#define REX_GUEST_CAS_C(ea, p, e, d) \
  (REX_CARD_FLUSH(), (void)(*(u8*)(base + (((u32)(ea)) >> 12)) = 0), REX_GUEST_CAS(p, e, d))
#endif
