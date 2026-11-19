// banjo-nx - native replacements of hot guest functions: the hook in front of each one, with its self-check guard.
//
// From sa2-nx's sa2_hot_guest.h (GPL-3.0; itself from masseffect-nx's me_hot_guest.h), see banjo_hot_common.h.
//
// The cost of the hook is paid on every call of a hot function, so the path to the copy is: one byte (is this hook on),
// one counter in the calling thread's own context (PPCContext::host_counters: no thread_local, which on the Switch is
// a call to __aarch64_read_tp, and no counter shared between threads), one test, the copy. The counter counts down to
// the next guarded call: Check() refills it.
//
// banjo-nx (2026-10-22, bejeweled3-nx's method): with no guard at all (banjo_hot_guard_calls = 0 and
// banjo_hot_guard_period = 0), verify and banjo_hot_count off, a hook's state is kDirect and the path is one byte and
// the copy: no countdown (for the bulk hooks a counter shared by every thread, written at every call).
// sasrt-nx (2026-11-10, port 78; its idea 27): always kDirect in a build without the guard's forms
// (BANJO_HOT_GUARD_FORMS below).
// sasrt-nx (2026-11-11, port 81; machine copies in place): with BANJO_HOT_IN_PLACE (CMake
// SASRT_HOT_IN_PLACE) there is no hook at all: the generated function's symbol is the copy (BANJO_HOT_HOOK_EX below).
#pragma once

#include <atomic>
#include <cstdint>
#include <iterator>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "cpu/banjo_hot_common.h"

// katamari-nx (2026-10-30): the in-run A/B's target in its off half (renut_engine/gpu/banjo_renut_tuning.cpp); target 70
// sends every localized copy's hook to the generated original for that half.
namespace banjo::renut_gpu {
extern std::atomic<int> g_ab_off;
}  // namespace banjo::renut_gpu

namespace banjo::hot {

enum : uint8_t { kUninit = 0, kOff = 1, kOn = 2, kCount = 3, kDirect = 4 };

// PPCContext::host_counters slots for the hooks (tools/localize_native.py numbers them in list order); a hook past
// them shares one counter between threads.
inline constexpr uint32_t kCtxSlots = 60;
// A counter word: the low 20 bits count down to the next guarded call of this thread, the high 12 count the guarded
// calls it has made while that is still below banjo_hot_guard_calls.
inline constexpr uint32_t kCountdownBits = 20;
inline constexpr uint32_t kCountdownMask = (1u << kCountdownBits) - 1;

struct Hook {
  // Fixed (the BANJO_HOT_HOOK line).
  const char* name;  // "sub_8299E2E8"
  const char* key;   // its cvar
  PPCFunc* orig;     // the recompiled function
  void (*native)(PPCContext&, uint8_t*);
  void (*native_logged)(PPCContext&, uint8_t*, Logged&);     // the guard's two forms (nullptr in a build without
  void (*reference_logged)(PPCContext&, uint8_t*, Logged&);  // them: BANJO_HOT_GUARD_FORMS 0, port 78)
  void (*native_counted)(PPCContext&, uint8_t*);  // banjo_hot_count (a measurement)
  std::atomic<uint64_t>* hits;                    // ... its block counters
  uint32_t hit_count;
  const CodeRange* code;  // the guest code the copy was made from
  uint32_t code_ranges;
  uint64_t code_hash;
  bool (*own)();  // reads the key
  uint32_t slot;
  const RegSkip* skip;  // registers the guard does not compare (hand-written natives in the copy)
  bool bulk;            // a hook of the bulk list (localized_bulk.txt): one master key, a summary in the reports
  Hook* next = nullptr;  // every hook, for the reports

  // Run time. Zero until the first call: kUninit sends that call to InitHook.
  std::atomic<uint8_t> state{kUninit};
  uint32_t guard_calls = 0;  // the first N guarded calls of each thread come one after the other ...
  uint32_t period = 1;       // ... afterwards one call in `period`
  bool verify = false;       // banjo_cpu_verify: every call
  std::atomic<uint32_t> shared_counter{0};  // slot >= kCtxSlots
  std::atomic<uint64_t> uses{0};            // calls the copy answered (estimated from the guard's period)
  std::atomic<uint64_t> compared{0};        // guarded calls that ran both versions and compared them
  std::atomic<uint64_t> not_comparable{0};  // guarded calls that reached a call the guard cannot repeat
  std::atomic<uint64_t> differences{0};
  std::atomic<uint64_t> not_repeated{0};    // guarded calls that differed once and compared equal when repeated
  // sasrt-nx (2026-11-11, port 74; its idea 35, banjo_hot_guard_prefix): calls both forms stopped in (an atomic, or a
  // call outside the copy) whose stores before the stop were the same / not.
  std::atomic<uint64_t> prefix_equal{0};
  std::atomic<uint64_t> prefix_differ{0};

  Hook(const char* name_, const char* key_, PPCFunc* orig_, void (*native_)(PPCContext&, uint8_t*),
       void (*native_logged_)(PPCContext&, uint8_t*, Logged&), void (*reference_logged_)(PPCContext&, uint8_t*, Logged&),
       void (*native_counted_)(PPCContext&, uint8_t*), std::atomic<uint64_t>* hits_, uint32_t hit_count_,
       const CodeRange* code_, uint32_t code_ranges_, uint64_t code_hash_, bool (*own_)(), uint32_t slot_,
       const RegSkip* skip_, bool bulk_);
};

// First call of a hook (any thread): its key and the CPU master switch, the guest code check, the guard's settings.
// Returns the state. banjo_hot_guest.cpp.
uint8_t InitHook(Hook& h, uint8_t* base);

// One guarded call. Leaves registers and memory exactly as the recompiled original leaves them.
void Check(Hook& h, PPCContext& ctx, uint8_t* base, uint32_t& counter);

// banjo_hot_count: the copy with its block counters (tools/hot_arm_compare.py reads them).
void Counted(Hook& h, PPCContext& ctx, uint8_t* base);

// The keys, for the "[enhancements]" line (banjo_enhancements.cpp): calls `row(key, own key on)` for each.
void ForEachKey(void (*row)(void* user, const char* key, bool own), void* user);

// The counters of every hook that was reached, once per report of the renderer (10 s) and at most that often.
void Report();

// sasrt-nx (2026-11-11, port 81; machine copies in place): with BANJO_HOT_IN_PLACE a copy has no
// Hook, only this record, read once at start-up (InPlaceNotes in banjo_hot_guest.cpp, from ForEachKey or the first
// Report): the list's choice for the copy and its key, so the log can name a key set against what the build made.
// Nothing on a call reads it. (Declared in both forms; only the in-place one defines records.)
struct InPlaceCopy {
  const char* name;  // "sub_8299E2E8"
  const char* key;   // its cvar (defined, not read)
  bool on;           // the list's choice: false (`off` in the list) keeps the generated original
  bool bulk;         // a copy of the bulk list (localized_bulk.txt)
  bool (*own)();     // reads the key
  InPlaceCopy* next = nullptr;

  InPlaceCopy(const char* name_, const char* key_, bool on_, bool bulk_, bool (*own_)());
};

}  // namespace banjo::hot

// The counting form is a PC measurement (banjo_hot_count): the Switch build does not make it (nor its code).
#if REX_PLATFORM_SWITCH
#define BANJO_HOT_COUNTED(ID) nullptr
#else
#define BANJO_HOT_COUNTED(ID) &n_##ID::NativeCounted
#endif

// sasrt-nx (2026-11-10, port 78; its idea 27): the guard's two forms of every copy (the copy's logged form and the
// original statements' logged form, about two thirds of the copies' code) are made only with BANJO_HOT_GUARD_FORMS
// (CMake SASRT_HOT_GUARD_FORMS: ON by default on the PC, OFF on the Switch; app/CMakeLists.txt). Without them every
// hook calls its copy directly (InitHook), whatever banjo_cpu_verify and banjo_hot_guard_* say, and the log says so.
// Only this macro names them: an inline form nothing takes the address of is never made, nor the o_* statements and
// the f_*<Logged> instances it alone calls (tools/localize_native.py writes them as before).
// sasrt-nx (2026-11-11, port 81; machine copies in place): BANJO_HOT_IN_PLACE (CMake
// SASRT_HOT_IN_PLACE: ON by default on the Switch wherever the guard forms are OFF, app/CMakeLists.txt) puts every copy
// in place of its generated function (BANJO_HOT_HOOK_EX below). No hook means no guard: it implies no guard forms.
#ifndef BANJO_HOT_IN_PLACE
#define BANJO_HOT_IN_PLACE 0
#endif
#ifndef BANJO_HOT_GUARD_FORMS
#if BANJO_HOT_IN_PLACE
#define BANJO_HOT_GUARD_FORMS 0
#else
#define BANJO_HOT_GUARD_FORMS 1
#endif
#endif
// The copy's entry, n_<ID>::Native (tools/localize_native.py writes "BANJO_HOT_NATIVE_SPEC inline void Native"): out of
// line behind a hook (one body, the hook's paths around it), inlinable in place, so the compiler and LTO may put a copy
// into its callers. Each header falls back to the hooked spec.
#ifndef BANJO_HOT_NATIVE_SPEC
#if BANJO_HOT_IN_PLACE
#define BANJO_HOT_NATIVE_SPEC
#else
#define BANJO_HOT_NATIVE_SPEC [[gnu::noinline]]
#endif
#endif
// Headers written before port 81 still say [[gnu::noinline]]: in place their copies stay calls (still no hook). The
// generator marks headers with the spec in banjo_hot_options.h; python tools/post_codegen.py (or a code generation)
// writes them again. Not tools/localize_native.py alone: post_codegen.py takes the tree's late steps (store cards and
// others) out before it and puts them back after; run alone on the finished tree it refuses copies.
#if BANJO_HOT_IN_PLACE && __has_include("hot/banjo_hot_options.h")
#include "hot/banjo_hot_options.h"
#if !defined(BANJO_HOT_NATIVE_SPEC_HEADERS)
#pragma message( \
    "BANJO_HOT_IN_PLACE: app/generated/hot predates port 81 (its copies keep [[gnu::noinline]]): run python tools/post_codegen.py")
#endif
#endif
#if BANJO_HOT_GUARD_FORMS
#define BANJO_HOT_LOGGED(ID) &n_##ID::NativeLogged
#define BANJO_HOT_REFERENCE(ID) &n_##ID::ReferenceLogged
#else
#define BANJO_HOT_LOGGED(ID) nullptr
#define BANJO_HOT_REFERENCE(ID) nullptr
#endif

// sasrt-nx (2026-11-12, banjo_hot_poison_dead; port 121): after an unguarded run of a copy, the registers of its hook's
// skip set (dead after the return by LiveOut: the guard does not compare them) are overwritten with garbage, so a wrong
// LiveOut shows (a crash, NaNs, a wrong picture) instead of hiding behind the skip set the guard takes from the same
// analysis (tools/reg_liveness.py, leaf_dead_stores.py).
namespace banjo::hot {
extern std::atomic<bool> g_poison_dead;
void PoisonDead(PPCContext& ctx, const RegSkip& skip);
}  // namespace banjo::hot
#define BANJO_HOT_POISON(ID)                                                    \
  do {                                                                          \
    if (banjo::hot::g_poison_dead.load(std::memory_order_relaxed)) [[unlikely]] \
      banjo::hot::PoisonDead(ctx, banjo::hot::n_##ID::kSkip);                   \
  } while (0)

// SYM: the generated function's name; ID: the namespace suffix of its header (n_<ID>); KEY: its cvar; SLOT: its
// PPCContext::host_counters slot. Written by tools/localize_native.py (app/generated/hot/banjo_hot_list.inc, and
// banjo_hot_bulk_<k>.inc for the bulk list: BANJO_HOT_HOOK_BULK, its keys in a category of their own).
#define BANJO_HOT_HOOK(SYM, ID, KEY, DEFAULT, SLOT, DESC) \
  BANJO_HOT_HOOK_EX(SYM, ID, KEY, DEFAULT, SLOT, DESC, false, "Banjo/Enhancements")
#define BANJO_HOT_HOOK_BULK(SYM, ID, KEY, DEFAULT, SLOT, DESC) \
  BANJO_HOT_HOOK_EX(SYM, ID, KEY, DEFAULT, SLOT, DESC, true, "Banjo/Bulk copies")
#if BANJO_HOT_IN_PLACE
// sasrt-nx (2026-11-11, port 81; machine copies in place): the generated function's symbol is the copy
// itself (sasrt-nx's SASRT_HOT_IN_PLACE). No state byte, no guard, no run-time key, no in-run A/B 70: a copy its list
// marks off (DEFAULT false) keeps the generated original, every other one runs. The key stays defined (a toml that names
// it still loads; its description says it is not read), and the record (InPlaceCopy) lets the start-up log name a key
// set against the list. The original (__imp__SYM) loses its last reference: --gc-sections and LTO drop it.
#define BANJO_HOT_HOOK_EX(SYM, ID, KEY, DEFAULT, SLOT, DESC, BULK, CATEGORY)                                     \
  REXCVAR_DEFINE_BOOL(KEY, DEFAULT, CATEGORY, DESC " (not read: this build has the copies in place)")            \
      .lifecycle(rex::cvar::Lifecycle::kInitOnly);                                                               \
  REX_EXTERN(__imp__##SYM);                                                                                      \
  namespace banjo::hot {                                                                                         \
  InPlaceCopy g_in_place_##ID{#SYM, #KEY, DEFAULT, BULK, [] { return bool(REXCVAR_GET(KEY)); }};                 \
  }                                                                                                              \
  REX_HOOK_RAW(SYM) {                                                                                            \
    if constexpr (DEFAULT)                                                                                       \
      banjo::hot::n_##ID::Native(ctx, base);                                                                     \
    else                                                                                                         \
      __imp__##SYM(ctx, base);                                                                                   \
  }
#else
#define BANJO_HOT_HOOK_EX(SYM, ID, KEY, DEFAULT, SLOT, DESC, BULK, CATEGORY)                                     \
  REXCVAR_DEFINE_BOOL(KEY, DEFAULT, CATEGORY, DESC).lifecycle(rex::cvar::Lifecycle::kInitOnly);                  \
  REX_EXTERN(__imp__##SYM);                                                                                      \
  namespace banjo::hot {                                                                                         \
  Hook g_hook_##ID{#SYM, #KEY, &__imp__##SYM, &n_##ID::Native, BANJO_HOT_LOGGED(ID), BANJO_HOT_REFERENCE(ID),    \
                   BANJO_HOT_COUNTED(ID), n_##ID::g_hits, uint32_t(std::size(n_##ID::g_hits)), n_##ID::kCode,    \
                   uint32_t(std::size(n_##ID::kCode)), n_##ID::kCodeHash, [] { return bool(REXCVAR_GET(KEY)); }, \
                   SLOT, &n_##ID::kSkip, BULK};                                                                  \
  }                                                                                                              \
  REX_HOOK_RAW(SYM) {                                                                                            \
    if (banjo::renut_gpu::g_ab_off.load(std::memory_order_relaxed) == 70) [[unlikely]] {                         \
      __imp__##SYM(ctx, base);                                                                                   \
      return;                                                                                                    \
    }                                                                                                            \
    banjo::hot::Hook& h = banjo::hot::g_hook_##ID;                                                               \
    uint8_t st = h.state.load(std::memory_order_relaxed);                                                        \
    if (st == banjo::hot::kDirect) [[likely]] {                                                                  \
      banjo::hot::n_##ID::Native(ctx, base);                                                                     \
      BANJO_HOT_POISON(ID);                                                                                      \
      return;                                                                                                    \
    }                                                                                                            \
    if (st != banjo::hot::kOn) [[unlikely]] {                                                                    \
      if (st == banjo::hot::kUninit) st = banjo::hot::InitHook(h, base);                                         \
      if (st == banjo::hot::kDirect) {                                                                           \
        banjo::hot::n_##ID::Native(ctx, base);                                                                   \
        return;                                                                                                  \
      }                                                                                                          \
      if (st == banjo::hot::kCount) {                                                                            \
        banjo::hot::Counted(h, ctx, base);                                                                       \
        return;                                                                                                  \
      }                                                                                                          \
      if (st != banjo::hot::kOn) {                                                                               \
        __imp__##SYM(ctx, base);                                                                                 \
        return;                                                                                                  \
      }                                                                                                          \
    }                                                                                                            \
    if constexpr ((SLOT) < banjo::hot::kCtxSlots) {                                                              \
      uint32_t& counter = ctx.host_counters[SLOT];                                                               \
      if ((counter & banjo::hot::kCountdownMask) == 0) [[unlikely]] {                                            \
        banjo::hot::Check(h, ctx, base, counter);                                                                \
        return;                                                                                                  \
      }                                                                                                          \
      --counter;                                                                                                 \
    } else {                                                                                                     \
      uint32_t counter = h.shared_counter.load(std::memory_order_relaxed);                                       \
      if ((counter & banjo::hot::kCountdownMask) == 0) [[unlikely]] {                                            \
        banjo::hot::Check(h, ctx, base, counter);                                                                \
        h.shared_counter.store(counter, std::memory_order_relaxed);                                              \
        return;                                                                                                  \
      }                                                                                                          \
      h.shared_counter.store(counter - 1, std::memory_order_relaxed);                                            \
    }                                                                                                            \
    banjo::hot::n_##ID::Native(ctx, base);                                                                       \
    BANJO_HOT_POISON(ID);                                                                                        \
  }
#endif  // BANJO_HOT_IN_PLACE
