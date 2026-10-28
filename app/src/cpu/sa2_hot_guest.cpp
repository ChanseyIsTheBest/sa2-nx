// Sonic Adventure 2 - native replacements of hot guest functions (CPU enhancements sa2_cpu_native_*; from
// masseffect-nx's me_hot_guest.cpp).
//
// Every hook replaces one recompiled function by a native version (src/cpu/hot/n_<addr>.h) and checks itself: for the
// first sa2_hot_guard_calls calls of each thread (every call with sa2_cpu_verify) and every sa2_hot_guard_period-th call
// afterwards, the native version runs on a copy of the registers with the guest ranges it writes snapshotted, the
// memory is rolled back, the original (__imp__sub_X) runs for real, and registers, written ranges and the FPU mode are
// compared. A difference logs "[hot] DIFFERENCE ..." and switches that hook off for the rest of the run (the original
// already produced the correct state). A hook also stays off when the loaded game code is not byte for byte the code
// the native was written from.
//
// The profile that chose them: the PC profile of the game thread with the GPU wait asleep (docs/enhancements.md).
// WHY EXACT: the CRT natives reproduce the routines' memory effect and return register (n_827F05C0.h etc. say why);
//   the others are localized copies of the recompiled functions themselves (tools/localize_native.py).
// RISK: a native that differs only on inputs the guard did not see. KEY: each sa2_cpu_native_* (sa2_cpu_enhancements).

// The generated code's macros with plain guest memory, the hooks' macros, the copies' default: shared with the bulk
// copies' shards (sa2_hot_bulk_N.cpp).
#include "sa2_hot_prelude.h"

// sa2-nx (2026-10-20, as banjo-nx): whether the generated code marks the store cards (tools/store_cards.py ran and
// REX_STORE_CARDS_OFF is not set): Renut proves its cached copies by them only then (banjo_renut_store_cards).
#if defined(REX_CARD) && !defined(REX_STORE_CARDS_OFF)
extern "C" const bool g_banjo_store_cards = true;
#else
extern "C" const bool g_banjo_store_cards = false;
#endif

#include <algorithm>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../../generated/hot/n_8213EC28.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_map_lower_bound.h"            // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82146ED8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82146F60.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147AC0.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147B38.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147C08.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147CD8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82147DA8.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8213A4D8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82139DA0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "hot/n_vertex_chunks.h"  // hand-written: sub_8214CF10 and sub_8214D0F8 (the chunk-model vertex loaders)
#include "hot/n_ninja_matrix.h"   // hand-written: njCalcPoint and njMultiMatrix (round 13)
#include "../../generated/hot/n_8214E130.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_vertex24_strips.h"            // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82555CC8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_821BC520.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82549178.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82960CE0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82141420.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82555B70.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82960928.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8295F9C0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82552408.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82960808.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214D6D8.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_state_block.h"                // hand-written, sa2-nx 2026-10-28 (batch 3)
#include "hot/n_state_handler8.h"             // hand-written, sa2-nx 2026-10-28 (batch 3)
#include "hot/n_state_bytes.h"                // hand-written, sa2-nx 2026-10-28 (batch 3)
#include "hot/n_matrix_transpose.h"           // hand-written, sa2-nx 2026-10-28 (batch 3)
#include "hot/n_colour_pack.h"                // hand-written, sa2-nx 2026-10-28 (batch 3)
#include "../../generated/hot/n_8214BBD8.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_key_search.h"                  // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82963658.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82140C98.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82140A90.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8214A208.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8214A050.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_82146E48.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_mtx34_to_44.h"                 // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82146DD8.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_mtx44_to_34.h"                 // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82141700.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_store_slot.h"                  // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82963590.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_records_copy.h"                // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_8214E3F0.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_strip_builder.h"               // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_8214E2B0.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_position_strips.h"             // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_821BA660.h"  // localized copy (tools/localize_native.py)
#include "../../generated/hot/n_8214B6C8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214B520.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8213D1F8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_829601B0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82832B78.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8213D7F0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214ACA8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_828326F8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82833E20.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_828243A0.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_texel_span.h"                  // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_8214F120.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_colour_strips.h"               // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82919200.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_buffer_clear.h"                // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82139750.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_slot_copies.h"                // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82823E08.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_texel_store.h"                // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_8214EF60.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_copied_strips.h"              // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_8214F028.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_strided_strips.h"             // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82147628.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_point_transform.h"            // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_821BA890.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82833938.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_alpha_block.h"                // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82834458.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82816B40.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82139930.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82834538.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_821472E0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82565758.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8213E040.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82833568.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_dxt1_block.h"                  // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_82832298.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_alpha_rescale.h"               // hand-written, sa2-nx 2026-10-28
#include "../../generated/hot/n_8214B320.h"  // localized copy (tools/localize_native.py): the reference of
#include "hot/n_record_store.h"               // hand-written, sa2-nx 2026-10-28 (batch 3)
#include "../../generated/hot/n_82565C60.h"  // localized copy, called by the renderer's hook
#include "../../generated/hot/n_82556A60.h"  // localized copy, called by the renderer's hook
#include "../../generated/hot/n_82555E18.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8251D2A0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82141050.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_821390C8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8217EB38.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82141240.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_825632F8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8251D1A8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82556718.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8254DED0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8254D0E0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8217F048.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8214EAF0.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82227C98.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_8213D5C8.h"  // localized copy, unguarded (tools/localize_native.py)
#include "../../generated/hot/n_82139C90.h"  // localized copy, unguarded (tools/localize_native.py)
#include "hot/n_827EF8B0.h"
#include "hot/n_d3d_constants.h"
#include "hot/n_827EFEA0.h"
#include "hot/n_827F05C0.h"

REXCVAR_DEFINE_BOOL(sa2_cpu_verify, false, "SA2 enhancements",
                    "Verify mode: every call of a native replacement runs both versions and compares them (slow; "
                    "'[verify]' lines report the checks and differences), and the renderer's self-checks check every "
                    "use")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_hot_guard_calls, 256, "SA2 enhancements",
                     "Native replacements: the first N calls of each thread are checked against the recompiled original")
    .range(0, 1000000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_hot_guard_period, 4096, "SA2 enhancements",
                     "Native replacements: after the first N calls every P-th call is checked (rounded to a power of two)")
    .range(1, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace sa2::hot {

SA2_ENHANCEMENT(g_counter_shared, sa2_cpu_hook_counter_shared, sa2::enh::kCpu, true,
                "Native replacements: one relaxed call counter per hook shared by all threads instead of a thread_local "
                "one (on the Switch every thread_local access is a call to __aarch64_read_tp, on every hooked call)");

SA2_ENHANCEMENT(g_counter_ctx, sa2_cpu_hook_counter_ctx, sa2::enh::kCpu, true,
                "Native replacements: each hook's call counter lives in the calling thread's own context "
                "(PPCContext::host_counters) instead of one counter shared by all threads: no store into a cache line "
                "every calling thread pulls to its core, and still no thread_local (sasrt-nx's, 2026-10-12)");

void TakeCtxSlot(std::atomic<uint8_t>& slot) {
  if (slot.load(std::memory_order_relaxed) != kNoSlot) return;
  static std::atomic<uint32_t> next_slot{0};
  const uint32_t s = next_slot.fetch_add(1, std::memory_order_relaxed);
  uint8_t none = kNoSlot;
  slot.compare_exchange_strong(none, s < kCtxSlots ? uint8_t(s) : kSlotsFull, std::memory_order_relaxed);
}

void InitHook(Hook& h, uint8_t* base) {
  // May run on several threads at once for the first call: every writer stores the same values.
  TakeCtxSlot(h.slot);
  bool on = h.key->active;
  if (on && !sa2::enh::GuestCodeMatches(base, h.code_address, h.code_bytes, h.code_hash, h.key->key)) on = false;
  const bool verify = REXCVAR_GET(sa2_cpu_verify);
  const uint32_t guard_calls = verify ? 0xFFFFFFFFu : uint32_t(std::max(0, REXCVAR_GET(sa2_hot_guard_calls)));
  const uint32_t period = uint32_t(std::max(1, REXCVAR_GET(sa2_hot_guard_period)));
  uint32_t p2 = 1;
  while (p2 < period) p2 <<= 1;
  h.guard_calls = guard_calls;
  h.period_mask = p2 - 1;
  if (on && h.state.load(std::memory_order_relaxed) == kUninit) {
    if (verify) {
      REXLOG_INFO("[hot] {} ({}): native replacement ON, verify mode (every call checked)", h.name, h.key->key);
    } else {
      REXLOG_INFO("[hot] {} ({}): native replacement ON (guard: first {} calls, then 1 in {})", h.name, h.key->key,
                  guard_calls, p2);
    }
  }
  h.state.store(on ? kOn : kOff, std::memory_order_release);
}

namespace {

const PPCRegister* Gpr(const PPCContext& c, int n) {
  switch (n) {
#define G(i) case i: return &c.r##i;
    G(0) G(1) G(2) G(3) G(4) G(5) G(6) G(7) G(8) G(9) G(10) G(11) G(12) G(13) G(14) G(15) G(16) G(17) G(18) G(19)
    G(20) G(21) G(22) G(23) G(24) G(25) G(26) G(27) G(28) G(29) G(30) G(31)
#undef G
  }
  return nullptr;
}
const PPCRegister* Fpr(const PPCContext& c, int n) { return &c.f0 + n; }       // f0..f31 are consecutive
const PPCVRegister* Vr(const PPCContext& c, int n) { return &c.v0 + n; }       // v0..v127 are consecutive
const PPCCRRegister* Cr(const PPCContext& c, int n) { return &c.cr0 + n; }     // cr0..cr7 are consecutive

// First difference between the native result (a) and the original (b), or an empty string.
std::string CompareRegs(Cmp cmp, const PPCContext& a, const PPCContext& b) {
  const bool all = CmpAll(cmp);
  const bool nan = CmpNaN(cmp);
  const bool ret_fp = cmp != Cmp::kReturnInt && cmp != Cmp::kReturnIntNaN;  // f1 / v1: a float return value
  for (int i = 0; i < 32; ++i) {
    const bool want = all || i == 1 || i == 3 || i >= 13;
    if (want && Gpr(a, i)->u64 != Gpr(b, i)->u64)
      return fmt::format("r{} native={:#018x} original={:#018x}", i, Gpr(a, i)->u64, Gpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    const bool want = all || (ret_fp && i == 1) || i >= 14;
    const uint64_t x = Fpr(a, i)->u64, y = Fpr(b, i)->u64;
    if (want && x != y && !(nan && IsNaN64(x) && IsNaN64(y)))
      return fmt::format("f{} native={:#018x} original={:#018x}", i, x, y);
  }
  for (int i = 0; i < 128; ++i) {
    const bool want = all || (ret_fp && i == 1) || (i >= 14 && i < 32) || i >= 64;
    bool differ = want && std::memcmp(Vr(a, i)->u8, Vr(b, i)->u8, 16) != 0;
    if (differ && nan) {
      differ = false;
      for (int k = 0; k < 4; ++k) {
        const uint32_t x = Vr(a, i)->u32[k], y = Vr(b, i)->u32[k];
        if (x != y && !(IsNaN32(x) && IsNaN32(y))) differ = true;
      }
    }
    if (differ) {
      const auto* x = Vr(a, i)->u32;
      const auto* y = Vr(b, i)->u32;
      return fmt::format("v{} native={:08x}{:08x}{:08x}{:08x} original={:08x}{:08x}{:08x}{:08x}", i, x[0], x[1], x[2],
                         x[3], y[0], y[1], y[2], y[3]);
    }
  }
  if (all) {
    for (int i = 0; i < 8; ++i)
      if (Cr(a, i)->raw() != Cr(b, i)->raw())
        return fmt::format("cr{} native={:x} original={:x}", i, Cr(a, i)->raw(), Cr(b, i)->raw());
    if (a.ctr.u64 != b.ctr.u64) return fmt::format("ctr native={:#x} original={:#x}", a.ctr.u64, b.ctr.u64);
    if (a.xer.so != b.xer.so || a.xer.ov != b.xer.ov || a.xer.ca != b.xer.ca) return "xer";
    if (a.reserved.u64 != b.reserved.u64) return "reserved";
    if (a.lr != b.lr) return fmt::format("lr native={:#x} original={:#x}", a.lr, b.lr);
  }
  // Only the guest-visible bits (rounding, flush): host sticky exception flags (x86 MXCSR) may differ harmlessly.
  constexpr uint32_t kGuest = rex::ppc::FPSCRRegister::GuestMask;
  if ((a.fpscr.csr & kGuest) != (b.fpscr.csr & kGuest))
    return fmt::format("fpscr.csr native={:#x} original={:#x}", a.fpscr.csr, b.fpscr.csr);
  return {};
}

}  // namespace

void Check(Hook& h, PPCContext& ctx, uint8_t* base) {
  Writes w;
  h.writes(ctx, base, w);
  if (w.overflow) {  // cannot verify: do not trust the native version for this call
    h.orig(ctx, base);
    return;
  }
  size_t total = 0;
  for (int i = 0; i < w.n; ++i) total += w.r[i].len;
  std::vector<uint8_t> before(total), native_out(total);
  size_t off = 0;
  for (int i = 0; i < w.n; ++i) {
    std::memcpy(before.data() + off, Raw(base, w.r[i].addr), w.r[i].len);
    off += w.r[i].len;
  }
  const uint32_t hw_before = ctx.fpscr.getcsr();
  const uint32_t csr_before = ctx.fpscr.csr;
  const PPCContext in = ctx;  // inputs for the report
  PPCContext nat = ctx;
  if (!h.native(nat, base)) {  // declined: the original runs, nothing to compare
    h.orig(ctx, base);
    return;
  }
  const uint32_t hw_native = nat.fpscr.getcsr();
  off = 0;
  for (int i = 0; i < w.n; ++i) {
    std::memcpy(native_out.data() + off, Raw(base, w.r[i].addr), w.r[i].len);
    std::memcpy(Raw(base, w.r[i].addr), before.data() + off, w.r[i].len);  // roll back
    off += w.r[i].len;
  }
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;
  h.orig(ctx, base);  // the call for real
  const uint32_t hw_orig = ctx.fpscr.getcsr();

  std::string diff = CompareRegs(h.cmp, nat, ctx);
  if (diff.empty() && (hw_native & rex::ppc::FPSCRRegister::GuestMask) != (hw_orig & rex::ppc::FPSCRRegister::GuestMask))
    diff = fmt::format("hardware FPCR native={:#x} original={:#x}", hw_native, hw_orig);
  if (diff.empty()) {
    off = 0;
    for (int i = 0; i < w.n && diff.empty(); ++i) {
      const uint8_t* now = Raw(base, w.r[i].addr);
      for (uint32_t k = 0; k < w.r[i].len; ++k) {
        if (now[k] != native_out[off + k]) {
          if (CmpNaN(h.cmp) && k / 4 * 4 + 4 <= w.r[i].len) {  // a float NaN word on both sides is equal
            const uint32_t q = k / 4 * 4;
            uint32_t x, y;
            std::memcpy(&x, native_out.data() + off + q, 4);
            std::memcpy(&y, now + q, 4);
            if (IsNaN32(__builtin_bswap32(x)) && IsNaN32(__builtin_bswap32(y))) {
              k = q + 3;
              continue;
            }
          }
          diff = fmt::format("memory {:#010x} (+{} of range {:#010x}+{}) native={:#04x} original={:#04x}",
                             w.r[i].addr + k, k, w.r[i].addr, w.r[i].len, native_out[off + k], now[k]);
          break;
        }
      }
      off += w.r[i].len;
    }
  }
  const uint64_t n = h.checks.fetch_add(1, std::memory_order_relaxed) + 1;
  sa2::enh::VerifyCheck();
  if (!diff.empty()) {
    h.failed.store(true, std::memory_order_relaxed);
    sa2::enh::VerifyDifference();
    REXLOG_ERROR(
        "[hot] DIFFERENCE {} ({}): {}; inputs r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} f1={:#x} f2={:#x} "
        "f3={:#x}; native replacement switched off for the rest of the run",
        h.name, h.key->key, diff, in.r3.u64, in.r4.u64, in.r5.u64, in.r6.u64, in.r7.u64, in.f1.u64, in.f2.u64,
        in.f3.u64);
  } else if (n == 4096 || n == (1u << 20)) {
    REXLOG_INFO("[hot] {} ({}): {} checks, no difference", h.name, h.key->key, n);
  }
}

}  // namespace sa2::hot

// ---------------------------------------------------------------------------------------------------------------
// The hooks. Writes: the guest ranges each may write (the stack ranges are the frames below r1 the localized copies
// write exactly as the originals do). Cmp: kAll for the leaf localized copies (every register); kCalls for the copies
// that call other functions: the scratch registers a callee leaves depend on whether that callee's own call was a
// checked one (a checked memcpy runs the recompiled original, which leaves r4 advanced; the native one leaves it), so
// they compare what their callers can see: r3/f1, the non-volatile registers, CTR-free state and memory. Their callers
// read no volatile register after the call (tools/call_site_liveness.py).
// The stack range of a function that makes calls must be exactly its own frame (stwu): below it lies its callees'
// dead scratch, which differs legitimately between a native callee and its original (the CRT memcpy saves r3 at
// -8(r1), the native one writes nothing there) whenever only one of the two nested calls of a check is itself checked.
// Round 2 (2026-10-06) found push_matrix (128 for a 112 frame), the rotations (176 for 160) and sub_82140C98 (320 for
// 288) switched off by such false DIFFERENCEs in normal runs; verify mode never shows it (every nested call checked).

namespace {
using sa2::hot::Ld32;
using sa2::hot::Writes;
using PPC = PPCContext;
}  // namespace

// C runtime (profile: memcpy 6.1 % of the busy game thread).
SA2_HOT_HOOK(827F05C0, sa2_cpu_native_memcpy, "Native CRT memcpy (sub_827F05C0): host memcpy/memmove",
             sa2::hot::n_827F05C0, &sa2::hot::n_827F05C0::Writes, sa2::hot::Cmp::kReturn, 1160, 0xEE0EAEB1E0584297ull)
SA2_HOT_HOOK(827EFEA0, sa2_cpu_native_memset, "Native CRT memset (sub_827EFEA0): host memset",
             sa2::hot::n_827EFEA0, &sa2::hot::n_827EFEA0::Writes, sa2::hot::Cmp::kReturn, 160, 0x54D201DC8A21DABAull)
SA2_HOT_HOOK(827EF8B0, sa2_cpu_native_memmove, "Native CRT memmove (sub_827EF8B0): host memmove",
             sa2::hot::n_827EF8B0, &sa2::hot::n_827EF8B0::Writes, sa2::hot::Cmp::kReturn, 192, 0x8CFADCBB872C5938ull)

// Ninja matrix library (profile: njCalcPoint 4.2 %, njMultiMatrix 1.9 %; njTranslate / njRotate* / njPushMatrix are
// called from 1,000-2,700 places each).
// Round 13: both hand-written, writing only what callers can see (src/cpu/hot/n_ninja_matrix.h; the localized copies
// moved every register they touch in and out of the context on every call). kReturnInt: callers read no float
// register after either (tools/call_site_liveness.py); njMultiMatrix's -48(r1) copy (out equal to an input) is the
// callee's dead scratch, so it is not compared. Since 2026-10-28 kReturnIntNaN (a NaN equals a NaN in the result,
// sa2_hot_common.h): njCalcPoint of a garbage matrix in a story stage wrote a NaN of the other sign than the original.
SA2_HOT_HOOK(82146ED8, sa2_cpu_native_calc_point, "Native njCalcPoint (sub_82146ED8), hand-written: the result only",
             sa2::hot::n_calc_point, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r5.u32, 12); },
             sa2::hot::Cmp::kReturnIntNaN, 136, 0x1A5C740F62335800ull)
SA2_HOT_HOOK(82146F60, sa2_cpu_native_multi_matrix, "Native njMultiMatrix (sub_82146F60), hand-written: the result only",
             sa2::hot::n_multi_matrix,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32 && c.r4.u32 && c.r5.u32) w.Add(c.r5.u32, 48);
             },
             sa2::hot::Cmp::kReturnIntNaN, 752, 0x6118F26106826963ull)
SA2_HOT_HOOK(82147AC0, sa2_cpu_native_translate, "Native njTranslate (sub_82147AC0), localized copy",
             sa2::hot::n_82147AC0,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32) w.Add(c.r3.u32 + 12, 36);
             },
             sa2::hot::Cmp::kAll, 120, 0x601D72585018A282ull)
SA2_HOT_HOOK(82147DA8, sa2_cpu_native_push_matrix, "Native njPushMatrix (sub_82147DA8), localized copy",
             sa2::hot::n_82147DA8,
             +[](const PPC& c, const uint8_t* base, Writes& w) {
               w.Add(0x839C07DCu, 4);  // the matrix stack pointer
               w.Add(Ld32(base, 0x839C07DCu) + 48, 48);
               w.Add(c.r1.u32 - 112, 112);  // its own frame (stwu -112), not the callees' dead scratch below it
             },
             sa2::hot::Cmp::kCalls, 112, 0x2A8618613032BDB9ull)
SA2_HOT_HOOK(82147B38, sa2_cpu_native_rotate_x, "Native njRotateX (sub_82147B38), localized copy",
             sa2::hot::n_82147B38,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32) w.Add(c.r3.u32, 48);
               w.Add(c.r1.u32 - 160, 160);  // its own frame (stwu -160), not the callees' dead scratch below it
             },
             sa2::hot::Cmp::kCalls, 208, 0x99AED3F258180A60ull)
SA2_HOT_HOOK(82147C08, sa2_cpu_native_rotate_y, "Native njRotateY (sub_82147C08), localized copy",
             sa2::hot::n_82147C08,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32) w.Add(c.r3.u32, 48);
               w.Add(c.r1.u32 - 160, 160);  // its own frame (stwu -160), not the callees' dead scratch below it
             },
             sa2::hot::Cmp::kCalls, 208, 0x85C214CA04B80D48ull)
SA2_HOT_HOOK(82147CD8, sa2_cpu_native_rotate_z, "Native njRotateZ (sub_82147CD8), localized copy",
             sa2::hot::n_82147CD8,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r3.u32) w.Add(c.r3.u32, 48);
               w.Add(c.r1.u32 - 160, 160);  // its own frame (stwu -160), not the callees' dead scratch below it
             },
             sa2::hot::Cmp::kCalls, 208, 0x3A5DA27104AF59BBull)

// Engine (profile: 3.0 %): an ordered-tree (std::map) lower-bound search by a 32-bit key; result iterator to [r3].
// sa2-nx (2026-10-28): hand-written, branchless (hot/n_map_lower_bound.h; the copy n_8213EC28.h is its reference).
SA2_HOT_HOOK(8213EC28, sa2_cpu_native_map_lower_bound, "Native std::map lower bound (sub_8213EC28), hand-written: "
             "branchless tree walk",
             sa2::hot::n_map_lower_bound,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r3.u32, 4);
               w.Add(c.r1.u32 - 16, 8);
             },
             sa2::hot::Cmp::kAll, 144, 0xA0A2745BFA713291ull)

// Default of the unguarded localized copies (the 15 below, round 3's 20 and round 4's 18). On since round 4: with round
// 3's smart sync a call syncs only what its callee can reach, and all of them passed the PC checks (verify mode and the
// lockstep capture, pixel-identical). Before smart sync a copy that makes calls wrote back and reloaded every localized
// register around each call and was slower on the PC (sasrt-nx). configs/sa2_test_copies_off.toml turns them all off
// for the console A/B.
#ifndef SA2_COPIES_DEFAULT
#define SA2_COPIES_DEFAULT true
#endif

// The 15 hottest game functions after the ones above (PC profile of the game thread, busy samples), localized copies.
// They call other functions, so they run unguarded; SA2_COPIES_DEFAULT (above) decides their default.
SA2_HOT_HOOK_COPY(8213A4D8, sa2_cpu_native_sub_8213A4D8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213A4D8 (4.5 % of the game thread on PC)",
                       sa2::hot::n_8213A4D8, 1696, 0x7DAC45C2DC3B95D2ull)
SA2_HOT_HOOK_COPY(82139DA0, sa2_cpu_native_sub_82139DA0, SA2_COPIES_DEFAULT, "Native localized copy of sub_82139DA0 (3.2 % of the game thread on PC)",
                       sa2::hot::n_82139DA0, 632, 0x06A3E594049A1B65ull)
// sub_8214CF10: round 13's hand-written version (below) replaced the localized copy.
// sa2-nx (2026-10-28): hand-written (hot/n_vertex24_strips.h; the copy n_8214E130.h is its reference, and its
// vertex loop the fallback). Unguarded as the copy was; in verify mode its fast loop checks itself.
SA2_HOT_HOOK_COPY(8214E130, sa2_cpu_native_vertex24_strips, SA2_COPIES_DEFAULT, "Native 24-byte vertex strips (sub_8214E130, 1.3 % of the game thread in gameplay), hand-written: the vertex buffer's base and cursor in registers",
                  sa2::hot::n_vertex24_strips, 384, 0x856253660DDC74D0ull)
SA2_COPY_FOR_HOOK(82555CC8, sa2_cpu_native_sub_82555CC8, SA2_COPIES_DEFAULT, "Native localized copy of sub_82555CC8 (2.4 %, Direct3D of the game thread on PC)",
                       sa2::hot::n_82555CC8, 336, 0x5C91B7E296B6DE66ull)
SA2_HOT_HOOK_COPY(821BC520, sa2_cpu_native_sub_821BC520, SA2_COPIES_DEFAULT, "Native localized copy of sub_821BC520 (2.3 % of the game thread on PC)",
                       sa2::hot::n_821BC520, 920, 0xCB982BEFF627C84Full)
SA2_HOT_HOOK_COPY(82549178, sa2_cpu_native_sub_82549178, SA2_COPIES_DEFAULT, "Native localized copy of sub_82549178 (2.0 %, Direct3D of the game thread on PC)",
                       sa2::hot::n_82549178, 400, 0x3E4CB7FAF32B3E2Bull)
SA2_HOT_HOOK_COPY(82960CE0, sa2_cpu_native_sub_82960CE0, SA2_COPIES_DEFAULT, "Native localized copy of sub_82960CE0 (1.8 % of the game thread on PC)",
                       sa2::hot::n_82960CE0, 200, 0x478F9A3FA2705FDDull)
SA2_HOT_HOOK_COPY(82141420, sa2_cpu_native_sub_82141420, SA2_COPIES_DEFAULT, "Native localized copy of sub_82141420 (1.8 %, VMX matrices of the game thread on PC)",
                       sa2::hot::n_82141420, 496, 0xE90489A245BCA2C4ull)
SA2_COPY_FOR_HOOK(82555B70, sa2_cpu_native_sub_82555B70, SA2_COPIES_DEFAULT, "Native localized copy of sub_82555B70 (1.5 %, Direct3D of the game thread on PC)",
                       sa2::hot::n_82555B70, 344, 0xDF67A0D237233D0Bull)
SA2_HOT_HOOK_COPY(82960928, sa2_cpu_native_sub_82960928, SA2_COPIES_DEFAULT, "Native localized copy of sub_82960928 (1.4 % of the game thread on PC)",
                       sa2::hot::n_82960928, 216, 0x37665FDBD2590B41ull)
SA2_HOT_HOOK_COPY(8295F9C0, sa2_cpu_native_sub_8295F9C0, SA2_COPIES_DEFAULT, "Native localized copy of sub_8295F9C0 (1.4 % of the game thread on PC)",
                       sa2::hot::n_8295F9C0, 360, 0x982E75743E03435Eull)
SA2_HOT_HOOK_COPY(82552408, sa2_cpu_native_sub_82552408, SA2_COPIES_DEFAULT, "Native localized copy of sub_82552408 (1.4 %, Direct3D of the game thread on PC)",
                       sa2::hot::n_82552408, 288, 0xCD2D456D05D4AC56ull)
SA2_HOT_HOOK_COPY(82960808, sa2_cpu_native_sub_82960808, SA2_COPIES_DEFAULT, "Native localized copy of sub_82960808 (1.3 % of the game thread on PC)",
                       sa2::hot::n_82960808, 288, 0x7C79132A3BE8A43Full)
// sa2-nx (2026-10-28, batch 3): hand-written (hot/n_state_block.h; the copy n_8214D6D8.h is its reference): the
// block's cards once. Handler 8 (sub_8214B3A0, hot/n_state_handler8.h) runs this body inline, not through this hook.
SA2_HOT_HOOK_COPY(8214D6D8, sa2_cpu_native_sub_8214D6D8, SA2_COPIES_DEFAULT, "Native render-state handler 8 body (sub_8214D6D8), hand-written: the block's cards once",
                       sa2::hot::n_state_block, 240, 0xA6DE8ACA97828B1Dull)
SA2_HOT_HOOK_COPY(8214B3A0, sa2_cpu_native_state_handler8, SA2_COPIES_DEFAULT, "Native render-state handler 8 (sub_8214B3A0, the game thread's hottest function in the 2P console profile), hand-written: its body inline",
                       sa2::hot::n_state_handler8, 24, 0x805675BDEF589D59ull)
SA2_HOT_HOOK_COPY(8214B168, sa2_cpu_native_state_bytes, SA2_COPIES_DEFAULT, "Native render-state handler 5 (sub_8214B168), hand-written: four byte stores' cards once",
                       sa2::hot::n_state_bytes, 328, 0x7A9EBB8C92253F6Full)

// Round 2: the hottest guardable functions of the CONSOLE profile (cpu_on, 1020 MHz, gameplay under 40 fps,
// 2026-10-06; tools/switch_profile_report.py). Guarded like the first set and on by default. Percentages are of the
// game thread's running samples there.
//
// Direct3D's shader float-constant writers (1.3 % + 1.2 %): hand-written, a copy of 16-byte vectors plus the dirty
// mask (hot/n_d3d_constants.h says why it is exact); they write no register, so the guard compares r3, f1, the
// non-volatile registers and the memory.
SA2_HOT_HOOK(8255D990, sa2_cpu_native_d3d_constants_a, "Native Direct3D constant upload (sub_8255D990, constants 376+): one copy",
             sa2::hot::n_8255D990, &sa2::hot::n_8255D990::Writes, sa2::hot::Cmp::kReturn, 216, 0xA97944CBFFA1143Aull)
SA2_HOT_HOOK(8255D8B8, sa2_cpu_native_d3d_constants_b, "Native Direct3D constant upload (sub_8255D8B8, constants 120+): one copy",
             sa2::hot::n_8255D8B8, &sa2::hot::n_8255D8B8::Writes, sa2::hot::Cmp::kReturn, 216, 0x9C7F63BCBF9EE982ull)
// Localized copies (tools/localize_native.py), leaves or calling only the save/restore helpers: kAll.
// sa2-nx (2026-10-28): hand-written, branchless search loop (hot/n_key_search.h; the copy n_8214BBD8.h is its reference).
SA2_HOT_HOOK(8214BBD8, sa2_cpu_native_key_search, "Native keyframe search (sub_8214BBD8), hand-written: branchless "
             "binary search",
             sa2::hot::n_key_search,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r7.u32, 4);  // the lower neighbour
               w.Add(c.r8.u32, 4);  // the upper neighbour
               w.Add(c.r9.u32, 4);  // the blend factor
               w.Add(c.r1.u32 - 32, 32);
             },
             sa2::hot::Cmp::kAll, 248, 0xBBFF0F4CF33C2132ull)
SA2_HOT_HOOK(82963658, sa2_cpu_native_records_equal, "Native 24-byte record array compare (sub_82963658), localized copy",
             sa2::hot::n_82963658, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r1.u32 - 64, 64); },
             sa2::hot::Cmp::kAll, 224, 0x6B9C3F8175340FC8ull)
SA2_HOT_HOOK(82140A90, sa2_cpu_native_vmx_matrix, "Native VMX 4x4 matrix routine (sub_82140A90), localized copy",
             sa2::hot::n_82140A90,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r1.u32 + 32, 48);       // the caller's parameter area (r5-r10 spilled)
               w.Add(c.r4.u32 & ~15u, 16);     // stvx
               w.Add(c.r3.u32 & ~15u, 64);     // four stvx
             },
             sa2::hot::Cmp::kAllNaN, 520, 0x5D26CDBD1029AE91ull)
SA2_HOT_HOOK(8214A208, sa2_cpu_native_cos, "Native njCos (sub_8214A208), localized copy",
             sa2::hot::n_8214A208, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r1.u32 - 16, 8); },
             sa2::hot::Cmp::kAll, 456, 0xE926619F5281C11Bull)
SA2_HOT_HOOK(8214A050, sa2_cpu_native_sin, "Native njSin (sub_8214A050), localized copy",
             sa2::hot::n_8214A050, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r1.u32 - 16, 8); },
             sa2::hot::Cmp::kAll, 440, 0xEF455A09F926F7DDull)
// sa2-nx (2026-10-28): hand-written (hot/n_mtx34_to_44.h; the copy n_82146E48.h is its reference and overlap fallback).
SA2_HOT_HOOK(82146E48, sa2_cpu_native_matrix_4x4, "Native 3x4 to 4x4 matrix (sub_82146E48), hand-written: words, "
             "one card mark",
             sa2::hot::n_mtx34_to_44,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r4.u32) w.Add(c.r3.u32, 64);
             },
             sa2::hot::Cmp::kAll, 144, 0xC6318134F65E2EDEull)
// sa2-nx (2026-10-28): hand-written (hot/n_mtx44_to_34.h; the copy n_82146DD8.h is its reference and overlap fallback).
SA2_HOT_HOOK(82146DD8, sa2_cpu_native_matrix_3x4, "Native 4x4 to 3x4 matrix (sub_82146DD8), hand-written: words, "
             "one card mark",
             sa2::hot::n_mtx44_to_34,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r4.u32) w.Add(c.r4.u32, 48);
             },
             sa2::hot::Cmp::kAll, 112, 0x5A0112B4F25E6F9Bull)
// sa2-nx (2026-10-28): hand-written (hot/n_store_slot.h; the copy n_82141700.h is its reference and trap fallback).
SA2_HOT_HOOK(82141700, sa2_cpu_native_store_slot, "Native one-of-seven word store (sub_82141700), hand-written: "
             "an offset table",
             sa2::hot::n_store_slot, +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r3.u32 + 1568, 28); },
             sa2::hot::Cmp::kAll, 184, 0x2480CFD0D6204630ull)
// sa2-nx (2026-10-28): hand-written (hot/n_records_copy.h; the copy n_82963590.h is its reference and fallback).
SA2_HOT_HOOK(82963590, sa2_cpu_native_records_copy, "Native 24-byte record copy (sub_82963590), hand-written: one "
             "memcpy",
             sa2::hot::n_records_copy,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               if (c.r4.u32 && c.r5.s32 > 0) {
                 const uint64_t bytes = 4 + 24 * uint64_t(uint32_t(c.r5.s32));
                 if (bytes > Writes::kMaxBytes) w.overflow = true;
                 else w.Add(c.r3.u32, uint32_t(bytes));
               }
             },
             sa2::hot::Cmp::kAll, 80, 0x0C525A39B6BED5B8ull)
// sa2-nx (2026-10-28): hand-written (hot/n_strip_builder.h; the copy n_8214E3F0.h is its reference, and its inner loop
// the fallback). It calls other functions, so unguarded; in verify mode its fast loop checks itself against the copy's.
SA2_HOT_HOOK_COPY(8214E3F0, sa2_cpu_native_strip_builder, SA2_COPIES_DEFAULT, "Native strip builder (sub_8214E3F0, "
                  "6.1 % of the console session), hand-written: the vertex buffer's base and cursor in registers",
                  sa2::hot::n_strip_builder, 432, 0x21848FD34265E6D7ull)
// sa2-nx (2026-10-28): hand-written (hot/n_position_strips.h; the copy n_8214E2B0.h is its reference, and its inner
// loop the fallback). It calls sub_82139C90, so unguarded; in verify mode its fast loop checks itself.
SA2_HOT_HOOK_COPY(8214E2B0, sa2_cpu_native_position_strips, SA2_COPIES_DEFAULT, "Native position strips (sub_8214E2B0, "
                  "1.3 % of the console session), hand-written: the vertex buffer's base and cursor in registers",
                  sa2::hot::n_position_strips, 232, 0x0B7E0B88E1867F99ull)
// Calls memcpy and sub_82140A90 (both hooked: a nested call may itself be a checked one), so kCalls.
SA2_HOT_HOOK(82140C98, sa2_cpu_native_matrix_op, "Native 4x4 matrix routine (sub_82140C98), localized copy",
             sa2::hot::n_82140C98,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r3.u32, 64);              // the result (unaligned stvlx/stvrx)
               w.Add(c.r1.u32 - 288, 288);       // its own frame (the helper's saves and the callees' writes are in it)
             },
             sa2::hot::Cmp::kCallsNaN, 256, 0xADE17C4CA1DC4F4Cull)
// Calls njCalcPoint (hooked) on its own frame. Writes its 112-byte frame (njCalcPoint's output is in it, the saved
// lr/f31 just below the caller's r1) and z to [r5] when r5 is not null. kReturnInt (2026-10-07, was kCalls): it returns
// an int in r3 and its 8 call sites read r3, r8 and r9 after it, never f1 (tools/call_site_liveness.py). Since round 13
// njCalcPoint's native leaves f1 alone where the original leaves a scratch value, so f1 at this function's return
// depended on whether the nested njCalcPoint call was itself a checked one: a DIFFERENCE on f1 alone switched this copy
// off (console "logs11 all on", PC 2026-10-07), with r3, the non-volatile registers and the memory equal.
SA2_HOT_HOOK(821BA660, sa2_cpu_native_visibility, "Native visibility test (sub_821BA660), localized copy",
             sa2::hot::n_821BA660,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r1.u32 - 112, 112);
               if (c.r5.u32) w.Add(c.r5.u32, 4);
             },
             sa2::hot::Cmp::kReturnInt, 280, 0x06E537F621A2EA14ull)

// Round 3: the next 20 hottest game-thread functions (console profile of the round-2 NRO), localized copies with smart
// sync (tools/localize_native.py: each call syncs only what its callee can reach). Unguarded like the 15 above, each
// with its own key; SA2_COPIES_DEFAULT decides whether they ship on (docs/enhancements.md, "Round 3").
SA2_HOT_HOOK_COPY(8214B6C8, sa2_cpu_native_sub_8214B6C8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214B6C8 (object draw walk, calls the render-state, matrix and model functions (11.7 % innermost))",
                  sa2::hot::n_8214B6C8, 440, 0x82BAF7A465166843ull)
SA2_HOT_HOOK_COPY(8214B520, sa2_cpu_native_sub_8214B520, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214B520 (render-state table: applies changed states through their handlers (2.7 % self))",
                  sa2::hot::n_8214B520, 144, 0xE4977AECB394F67Eull)
SA2_HOT_HOOK_COPY(8213D1F8, sa2_cpu_native_sub_8213D1F8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213D1F8 (model draw setup, one indirect call (3.6 % self))",
                  sa2::hot::n_8213D1F8, 640, 0x3A3A2DC475A4348Cull)
SA2_HOT_HOOK_COPY(829601B0, sa2_cpu_native_sub_829601B0, SA2_COPIES_DEFAULT, "Native localized copy of sub_829601B0 (draw submission, calls the Direct3D draw functions (1.9 % self))",
                  sa2::hot::n_829601B0, 280, 0x129F34B1EE627CF2ull)
SA2_HOT_HOOK_COPY(82832B78, sa2_cpu_native_sub_82832B78, SA2_COPIES_DEFAULT, "Native localized copy of sub_82832B78 (float routine of the 0x8283 group (collision or physics math; 13 self + 19.8 inner))",
                  sa2::hot::n_82832B78, 2544, 0xE7E7AD758AB929D6ull)
SA2_HOT_HOOK_COPY(8213D7F0, sa2_cpu_native_sub_8213D7F0, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213D7F0 (matrix setup: matrix copy, transpose and VMX matrix calls)",
                  sa2::hot::n_8213D7F0, 104, 0xCEC87EFDD287C802ull)
SA2_HOT_HOOK_COPY(8214ACA8, sa2_cpu_native_sub_8214ACA8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214ACA8 (object transform and draw (inclusive 52.8 %))",
                  sa2::hot::n_8214ACA8, 360, 0x736AE8C2FF120106ull)
SA2_HOT_HOOK_COPY(828326F8, sa2_cpu_native_sub_828326F8, SA2_COPIES_DEFAULT, "Native localized copy of sub_828326F8 (float routine of the 0x8283 group, only the save/restore helpers)",
                  sa2::hot::n_828326F8, 1152, 0xBF8AEF4A4FAFB058ull)
SA2_HOT_HOOK_COPY(82833E20, sa2_cpu_native_sub_82833E20, SA2_COPIES_DEFAULT, "Native localized copy of sub_82833E20 (float routine of the 0x8283 group, calls 82832B78)",
                  sa2::hot::n_82833E20, 1464, 0xDE0E6D2C7FC69E70ull)
// sa2-nx (2026-10-28): hand-written (hot/n_texel_span.h; the copy n_828243A0.h is its reference, and its two loops the
// fallbacks). Unguarded as the copy was; in verify mode its fast loops check themselves against the copy's.
SA2_HOT_HOOK_COPY(828243A0, sa2_cpu_native_texel_span, SA2_COPIES_DEFAULT, "Native texel span (sub_828243A0, 5.2 % of "
                  "the game thread in gameplay), hand-written: the texel copy and colour key without reloads",
                  sa2::hot::n_texel_span, 744, 0x5AECFD5F6266ABE4ull)
// sa2-nx (2026-10-28): hand-written (hot/n_colour_strips.h; the copy n_8214F120.h is its reference, and its vertex
// loop the fallback). Unguarded; in verify mode its fast loop checks itself.
SA2_HOT_HOOK_COPY(8214F120, sa2_cpu_native_colour_strips, SA2_COPIES_DEFAULT, "Native position and colour strips "
                  "(sub_8214F120), hand-written: the vertex buffer's base and cursor in registers",
                  sa2::hot::n_colour_strips, 240, 0x5F4744D63E88B207ull)
// sa2-nx (2026-10-28): hand-written (hot/n_buffer_clear.h; the copy n_82919200.h is its reference, and its fill loop
// the fallback): the session's top function, off the game thread. Unguarded; in verify mode its fast fill checks itself.
SA2_HOT_HOOK_COPY(82919200, sa2_cpu_native_buffer_clear, SA2_COPIES_DEFAULT, "Native float buffers clear (sub_82919200, "
                  "8.2 % of the console session, off the game thread), hand-written: one fill a buffer",
                  sa2::hot::n_buffer_clear, 144, 0x2F1E5065BC393E90ull)
// sa2-nx (2026-10-28): hand-written (hot/n_slot_copies.h; the copy n_82139750.h is its reference, and its six
// word loops the fallbacks). Unguarded as its bulk copy was; in verify mode its fast copies check themselves.
SA2_HOT_HOOK_COPY(82139750, sa2_cpu_native_slot_copies, SA2_COPIES_DEFAULT, "Native flagged slot copies (sub_82139750), hand-written: one copy a slot",
                  sa2::hot::n_slot_copies, 480, 0xB5FDE7E1028196D5ull)
// sa2-nx (2026-10-28): hand-written (hot/n_texel_store.h; the copy n_82823E08.h is its reference, and its
// scatter loop the fallback). Unguarded as its bulk copy was; in verify mode its fast loop checks itself.
SA2_HOT_HOOK_COPY(82823E08, sa2_cpu_native_texel_store, SA2_COPIES_DEFAULT, "Native texel span store (sub_82823E08, 0.7 % of the game thread in gameplay), hand-written: the block cache held",
                  sa2::hot::n_texel_store, 1432, 0xDD4F5389FEEAC424ull)
// sa2-nx (2026-10-28): hand-written (hot/n_copied_strips.h; the copy n_8214EF60.h is its reference, and its
// vertex loop the fallback). Unguarded as its bulk copy was; in verify mode its fast loop checks itself.
SA2_HOT_HOOK_COPY(8214EF60, sa2_cpu_native_copied_strips, SA2_COPIES_DEFAULT, "Native copied-vertex strips (sub_8214EF60), hand-written: the vertex buffer's base and cursor in registers",
                  sa2::hot::n_copied_strips, 200, 0x72EF078A5DC72F95ull)
// sa2-nx (2026-10-28): hand-written (hot/n_strided_strips.h; the copy n_8214F028.h is its reference, and its
// vertex loop the fallback). Unguarded as its bulk copy was; in verify mode its fast loop checks itself.
SA2_HOT_HOOK_COPY(8214F028, sa2_cpu_native_strided_strips, SA2_COPIES_DEFAULT, "Native strided position strips (sub_8214F028), hand-written: the vertex buffer's base and cursor in registers",
                  sa2::hot::n_strided_strips, 248, 0x966D8AC3700821FBull)
// sa2-nx (2026-10-28): hand-written (hot/n_point_transform.h; the copy n_82147628.h is its reference, and its two
// loops the fallbacks). Unguarded as its bulk copy was; in verify mode its fast loops check themselves.
SA2_HOT_HOOK_COPY(82147628, sa2_cpu_native_point_transform, SA2_COPIES_DEFAULT, "Native point array by a 3x4 matrix (sub_82147628), hand-written: the matrix held",
                  sa2::hot::n_point_transform, 712, 0xE698E73597B2B11Cull)
SA2_HOT_HOOK_COPY(821BA890, sa2_cpu_native_sub_821BA890, SA2_COPIES_DEFAULT, "Native localized copy of sub_821BA890 (object draw wrapper (inclusive 59 %))",
                  sa2::hot::n_821BA890, 128, 0x997B62A9F632926Eull)
// sa2-nx (2026-10-28): hand-written (hot/n_alpha_block.h; the copy n_82833938.h is its reference, and its four
// loops the fallbacks). Unguarded as the copy was; in verify mode its fast loops check themselves.
SA2_HOT_HOOK_COPY(82833938, sa2_cpu_native_alpha_block, SA2_COPIES_DEFAULT, "Native DXT5 block decode (sub_82833938, 2.6 % of the game thread in gameplay), hand-written: palette and texels without reloads",
                  sa2::hot::n_alpha_block, 560, 0x1CFEFBE869CCAA91ull)
SA2_HOT_HOOK_COPY(82834458, sa2_cpu_native_sub_82834458, SA2_COPIES_DEFAULT, "Native localized copy of sub_82834458 (wrapper of 82833E20)",
                  sa2::hot::n_82834458, 128, 0x34037AF73161CA53ull)
SA2_HOT_HOOK_COPY(82816B40, sa2_cpu_native_sub_82816B40, SA2_COPIES_DEFAULT, "Native localized copy of sub_82816B40 (hot game function, indirect calls)",
                  sa2::hot::n_82816B40, 1192, 0xE0E6E9A9BD9196F9ull)
SA2_HOT_HOOK_COPY(82139930, sa2_cpu_native_sub_82139930, SA2_COPIES_DEFAULT, "Native localized copy of sub_82139930 (hot game function, one indirect call)",
                  sa2::hot::n_82139930, 536, 0x70051431EDFAD872ull)
SA2_HOT_HOOK_COPY(82834538, sa2_cpu_native_sub_82834538, SA2_COPIES_DEFAULT, "Native localized copy of sub_82834538 (wrapper of 82832298 and 82833938)",
                  sa2::hot::n_82834538, 96, 0xE384CE4115B7233Full)
SA2_HOT_HOOK_COPY(821472E0, sa2_cpu_native_sub_821472E0, SA2_COPIES_DEFAULT, "Native localized copy of sub_821472E0 (matrix routine: transpose, VMX matrix and memcpy calls)",
                  sa2::hot::n_821472E0, 96, 0x78B2850954A4B571ull)
SA2_COPY_FOR_HOOK(82565758, sa2_cpu_native_sub_82565758, SA2_COPIES_DEFAULT, "Native localized copy of sub_82565758 (Direct3D draw path; called by the D3D API layer's hook)",
                  sa2::hot::n_82565758, 1216, 0x16CE63397DFBCDCBull)
SA2_HOT_HOOK_COPY(8213E040, sa2_cpu_native_sub_8213E040, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213E040 (hot game function, indirect calls)",
                  sa2::hot::n_8213E040, 256, 0x9FC6BCE3A1354E06ull)
// sa2-nx (2026-10-28): hand-written (hot/n_dxt1_block.h; the copy n_82833568.h is its reference, and its texel loop the
// fallback). Unguarded as the copy was; in verify mode its fast loop checks itself against the copy's.
SA2_HOT_HOOK_COPY(82833568, sa2_cpu_native_dxt1_block, SA2_COPIES_DEFAULT, "Native DXT1 block decode (sub_82833568, "
                  "8.9 % of the game thread in gameplay), hand-written: a texel is one 16-byte copy",
                  sa2::hot::n_dxt1_block, 784, 0x36D820C7E337CEF3ull)

// Round 4: 20 more game-thread functions of the console profile (round-2 NRO, cpu_on, 1020 MHz), ranked by their own
// samples: a copy speeds up the function's own code, not its callees (docs/enhancements.md, "Round 4").
//
// Two leaves, guarded like the round-2 copies (every register compared; the guard runs both versions):
//  - sub_82832298 multiplies floats: NaN-tolerant compare (round 2: the sign of a NaN depends on the compilation);
//  - sub_8214B320 saves r30/r31 below r1 and writes one record of a table: (entry - 9) * 20 bytes after 0x83853480,
//    entry = the word at 0x83850768 + ((r3 >> 14) & 0x3FC), the same loads the function makes.
// sa2-nx (2026-10-28): hand-written (hot/n_alpha_rescale.h; the copy n_82832298.h is its reference): selects for the
// per-component branches, a record's cards once. Guarded as the copy was.
SA2_HOT_HOOK(82832298, sa2_cpu_native_alpha_rescale, "Native alpha rescale of 16 texels (sub_82832298, 2.0 % of the "
             "game thread in gameplay), hand-written: selects, a record's cards once",
             sa2::hot::n_alpha_rescale,
             +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r3.u32, 256); },
             sa2::hot::Cmp::kAllNaN, 168, 0x2F043BAEA9B4C012ull)
// sa2-nx (2026-10-28, batch 3): hand-written (hot/n_record_store.h; the copy n_8214B320.h is its reference): the
// record's cards once. Guarded as the copy was. The two leaves after it likewise (batch 3).
SA2_HOT_HOOK(8214B320, sa2_cpu_native_sub_8214B320, "Native render-state handler 0 (sub_8214B320, fills one record of a table at 0x83853480 from three lookups indexed by r3; leaf), hand-written: the record's cards once",
             sa2::hot::n_record_store,
             +[](const PPC& c, const uint8_t* base, Writes& w) {
               w.Add(c.r1.u32 - 16, 16);  // r30 and r31 (no frame of its own)
               const uint32_t entry = Ld32(base, 0x83850768u + ((c.r3.u32 >> 14) & 0x3FCu));
               w.Add(0x83853480u + (entry - 9u) * 20u, 17);  // +0..+16: four words and the byte of r3
             },
             sa2::hot::Cmp::kAll, 128, 0x757EF8F6C1A7A575ull)
SA2_HOT_HOOK(82147340, sa2_cpu_native_matrix_transpose, "Native 3x4 matrix transpose (sub_82147340), hand-written: "
             "the matrix's cards once",
             sa2::hot::n_matrix_transpose,
             +[](const PPC& c, const uint8_t*, Writes& w) { w.Add(c.r4.u32, 48); },
             sa2::hot::Cmp::kAllNaN, 168, 0xEFAD9A3A2B25854Bull)
SA2_HOT_HOOK(8214D9F8, sa2_cpu_native_colour_pack, "Native colour clamp and pack (sub_8214D9F8), hand-written: the "
             "floats' and the bytes' cards once each",
             sa2::hot::n_colour_pack,
             +[](const PPC& c, const uint8_t*, Writes& w) {
               w.Add(c.r3.u32, 16);       // the four clamped floats
               w.Add(c.r4.u32, 4);        // the packed bytes
               w.Add(c.r1.u32 - 32, 24);  // the conversions' words below r1 (no frame of its own)
             },
             sa2::hot::Cmp::kAllNaN, 208, 0x0F7DB10A30C0DAF3ull)
// The Direct3D draw and its shader load are hooked by the renderer (src/me/sa2_me_hooks.cpp, NoteDrawCall /
// NoteShaderObject): the hooks call these copies in place of the originals (sub_82565C60 was the largest function
// of the game thread without a native: 1.7 % self, 7.5 % with what it calls).
SA2_COPY_FOR_HOOK(82565C60, sa2_cpu_native_sub_82565C60, SA2_COPIES_DEFAULT, "Native localized copy of sub_82565C60 (D3DDevice_DrawVertices: the draw's PM4 packets; called by the renderer's hook)",
                  sa2::hot::n_82565C60, 1048, 0x96808E6D756A267Full)
SA2_COPY_FOR_HOOK(82556A60, sa2_cpu_native_sub_82556A60, SA2_COPIES_DEFAULT, "Native localized copy of sub_82556A60 (D3D draw-time shader load; called by the renderer's hook)",
                  sa2::hot::n_82556A60, 2432, 0x1D2AC18C0489EEC1ull)
// Sixteen more that make calls: unguarded, exact by construction, smart sync. (The GPU wait loop is not among them:
// src/cpu/sa2_gpu_wait.cpp hooks it already.)
// Name no other function's address in this file: tools/direct_calls.py and tools/localize_native.py treat every
// 82xxxxxx address the app sources mention as hooked (no direct calls to it, full sync around it in the copies).
SA2_HOT_HOOK_COPY(82555E18, sa2_cpu_native_sub_82555E18, SA2_COPIES_DEFAULT, "Native localized copy of sub_82555E18 (D3D: copies a block of state into the command buffer)",
                  sa2::hot::n_82555E18, 1576, 0x2BC39EB4A3C1AA0Dull)
SA2_HOT_HOOK_COPY(8251D2A0, sa2_cpu_native_sub_8251D2A0, SA2_COPIES_DEFAULT, "Native localized copy of sub_8251D2A0 (float routine with indirect calls)",
                  sa2::hot::n_8251D2A0, 528, 0x5AAFCDEB139DF728ull)
SA2_HOT_HOOK_COPY(82141050, sa2_cpu_native_sub_82141050, SA2_COPIES_DEFAULT, "Native localized copy of sub_82141050 (one direct and one indirect call)",
                  sa2::hot::n_82141050, 128, 0x85B45347A7086026ull)
SA2_HOT_HOOK_COPY(821390C8, sa2_cpu_native_sub_821390C8, SA2_COPIES_DEFAULT, "Native localized copy of sub_821390C8 (a wrapper with one direct call)",
                  sa2::hot::n_821390C8, 88, 0x5C9A9200D8FF4EF1ull)
SA2_HOT_HOOK_COPY(8217EB38, sa2_cpu_native_sub_8217EB38, SA2_COPIES_DEFAULT, "Native localized copy of sub_8217EB38 (one direct and one indirect call)",
                  sa2::hot::n_8217EB38, 160, 0x430CEE9E1B5B4811ull)
SA2_HOT_HOOK_COPY(82141240, sa2_cpu_native_sub_82141240, SA2_COPIES_DEFAULT, "Native localized copy of sub_82141240 (float routine with six indirect calls)",
                  sa2::hot::n_82141240, 480, 0x6627FEACAC5B993Cull)
SA2_HOT_HOOK_COPY(825632F8, sa2_cpu_native_sub_825632F8, SA2_COPIES_DEFAULT, "Native localized copy of sub_825632F8 (D3D: VMX and float state into PM4 packets)",
                  sa2::hot::n_825632F8, 1664, 0xD23721070E2EAA54ull)
SA2_HOT_HOOK_COPY(8251D1A8, sa2_cpu_native_sub_8251D1A8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8251D1A8 (object transform: njTranslate, njRotateX/Y/Z and sub_8251D2A0)",
                  sa2::hot::n_8251D1A8, 248, 0x3DA2C02A2AD65BFDull)
SA2_HOT_HOOK_COPY(82556718, sa2_cpu_native_sub_82556718, SA2_COPIES_DEFAULT, "Native localized copy of sub_82556718 (D3D: memcpy and four D3D helpers)",
                  sa2::hot::n_82556718, 344, 0x875D371E7E7931EFull)
SA2_HOT_HOOK_COPY(8254DED0, sa2_cpu_native_sub_8254DED0, SA2_COPIES_DEFAULT, "Native localized copy of sub_8254DED0 (D3D: two direct calls)",
                  sa2::hot::n_8254DED0, 528, 0xB162AF7A88554AEBull)
SA2_HOT_HOOK_COPY(8254D0E0, sa2_cpu_native_sub_8254D0E0, SA2_COPIES_DEFAULT, "Native localized copy of sub_8254D0E0 (D3D: three direct calls, the GPU wait loop among them)",
                  sa2::hot::n_8254D0E0, 152, 0x036E0642FDE072D7ull)
SA2_HOT_HOOK_COPY(8217F048, sa2_cpu_native_sub_8217F048, SA2_COPIES_DEFAULT, "Native localized copy of sub_8217F048 (calls the 0x8217E family and one indirect call)",
                  sa2::hot::n_8217F048, 776, 0xE00AD515528C6D6Cull)
SA2_HOT_HOOK_COPY(8214EAF0, sa2_cpu_native_sub_8214EAF0, SA2_COPIES_DEFAULT, "Native localized copy of sub_8214EAF0 (float routine, njCalcPoint twice)",
                  sa2::hot::n_8214EAF0, 552, 0x293DD9F7805E8702ull)
SA2_HOT_HOOK_COPY(82227C98, sa2_cpu_native_sub_82227C98, SA2_COPIES_DEFAULT, "Native localized copy of sub_82227C98 (seven direct calls into three modules)",
                  sa2::hot::n_82227C98, 512, 0xA50BABE6ECAB795Dull)
SA2_HOT_HOOK_COPY(8213D5C8, sa2_cpu_native_sub_8213D5C8, SA2_COPIES_DEFAULT, "Native localized copy of sub_8213D5C8 (converts a float and dispatches through one indirect call)",
                  sa2::hot::n_8213D5C8, 96, 0x748F92E966D9F5A0ull)
SA2_HOT_HOOK_COPY(82139C90, sa2_cpu_native_sub_82139C90, SA2_COPIES_DEFAULT, "Native localized copy of sub_82139C90 (three memcpy calls)",
                  sa2::hot::n_82139C90, 272, 0xF1C04D41B8B51FD1ull)

// Round 13 (2026-10-07, the user's request after the 2P console profile): the chunk-model vertex loaders by hand
// (src/cpu/hot/n_vertex_chunks.h). Their njCalcPoint calls were the game thread's largest single cost in the 2P race
// (8.5 %), each one through the hook and a register write-back and reload around it. Lists with skinned chunks (types
// 44 and 35, which call other functions) are declined. Guarded (every call in verify mode), kReturnInt.
SA2_HOT_HOOK(8214CF10, sa2_cpu_native_vertex_normals,
             "Native chunk-model vertex loader with normals (sub_8214CF10): njCalcPoint inline, the matrix read once",
             sa2::hot::n_vn_points_normals, &sa2::hot::n_vn_points_normals::Writes, sa2::hot::Cmp::kReturnInt, 488,
             0x6A9224B55ADC26CEull)
SA2_HOT_HOOK(8214D0F8, sa2_cpu_native_vertex_points,
             "Native chunk-model vertex loader, positions (sub_8214D0F8): njCalcPoint inline, the matrix read once",
             sa2::hot::n_vn_points, &sa2::hot::n_vn_points::Writes, sa2::hot::Cmp::kReturnInt, 400,
             0xEB679F82BD509F40ull)
