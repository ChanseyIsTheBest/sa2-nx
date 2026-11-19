// Mass Effect - the game's D3D wait for the GPU without spinning (native renderer).
//
// WHY (Switch stack profile)
//   sub_821FEBE0 is the D3D check "has the GPU read pointer moved since the last iteration":
//   32 db16cyc delays, then it compares the word the GPU writes the
//   read pointer back to ([[device+10768]]) with the value seen last time ([r3+8]), and after 5000 ticks
//   without change declares the GPU hung. Its callers loop on it. On the console it was ~22 % of the busy
//   game thread (plus ~6 % in the caller), on a 3-core machine where the ring thread needs that core.
//
// WHAT IT DOES
//   If the read pointer is unchanged, it waits at most masseffect_wait_ring_max_us for the ring thread
//   to write it back again (NotifyRingProgress), then calls the original, which decides as before. The game
//   sees the same values, only later. With bit 0x04 of [device+10813] (GPU already declared hung) or
//   without the native renderer, it does not wait.

#include "me_native_system.h"
#include "cpu/banjo_enhancements.h"
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

REXCVAR_DEFINE_BOOL(masseffect_wait_blocking_ring, false, "Mass Effect",
                    "Native renderer: the game's D3D sleeps while waiting for the ring thread to advance "
                    "instead of spinning in sub_821FEBE0")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(masseffect_notify_no_lock, false, "Mass Effect",
                    "Ring thread: NotifyRingProgress takes the progress mutex only when the game's D3D wait is "
                    "registered as sleeping (sequentially consistent handshake on both sides) instead of on every call "
                    "(one per fence write and per ring segment). Same wake-ups")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(masseffect_wait_ring_max_us, 2000, "Mass Effect",
                     "Native renderer: longest sleep per iteration of the game's D3D GPU wait, in microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

std::mutex g_progress_mutex;
std::condition_variable g_progress_cv;
std::atomic<uint32_t> g_progress{0};
std::atomic<int> g_waiting{0};

}  // namespace

// The ring thread bumps the counter after writing the read pointer back, and checks for waiters under the
// lock (an unlocked Dekker handshake lost wake-ups on ARM).
void NotifyRingProgress() {
  static const bool no_lock = REXCVAR_GET(masseffect_notify_no_lock);
  if (no_lock) {
    // Dekker handshake, sequentially consistent on both sides: the waiter registers in g_waiting and then checks
    // g_progress (hook below); this side bumps g_progress and then reads g_waiting. One of the two sees the other's
    // write. If a waiter is registered, the lock/unlock below waits for it to be inside the condition variable (it holds
    // the mutex from registering until wait_for releases it), so the notify cannot be lost.
    g_progress.fetch_add(1, std::memory_order_seq_cst);
    if (g_waiting.load(std::memory_order_seq_cst) <= 0) return;
    { std::lock_guard<std::mutex> lock(g_progress_mutex); }
    g_progress_cv.notify_all();
    return;
  }
  g_progress.fetch_add(1, std::memory_order_acq_rel);
  bool notify;
  {
    std::lock_guard<std::mutex> lock(g_progress_mutex);
    notify = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (notify) g_progress_cv.notify_all();
}

}  // namespace me::native

// banjo-nx: Nuts & Bolts' D3D GPU waits. sub_82634740 is the same D3D check as Mass Effect's sub_821FEBE0: the
// caller passes a wait record (r3: +0 device, +8 the read pointer seen last, +12 when it last changed, +20 the timebase
// when the wait began), the check compares the read pointer write-back word [[device+10896]] with +8, gives up after
// 5000 ms without progress (hung bit 0x02 of [device+10941]) and returns 1 = keep waiting. Six game loops call it
// (ring space sub_8225D370, Swap sub_822554F8, fences sub_821FC6E8 / sub_821DF880 / sub_8264A3D8 / sub_8264AE90); on
// the console three threads spun in them at ~55 % each while the ring thread compiled a pipeline.
//
// The hook sleeps, before running the original check, until the ring thread makes progress (NotifyRingProgress: the
// read pointer write-back and the fence writes) or masseffect_wait_ring_max_us passes, but only when the read pointer
// has not moved since the previous check of the same wait. The first check of each wait never sleeps: Swap's loop
// checks before testing its own condition, which may already hold. The game then checks exactly as before and sees
// the same values, only later.
REXCVAR_DEFINE_BOOL(banjo_wait_blocking_ring, true, "Banjo",
                    "Native renderer: the game's D3D GPU waits (sub_82634740 and its six loops) sleep until the ring "
                    "thread advances instead of spinning. Same values seen by the game, less CPU")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace me::native {
namespace {

bool BlockingRingWait() {
  static const bool blocking =
      banjo::EnhancementActive(banjo::EnhancementGroup::kCpu, REXCVAR_GET(banjo_wait_blocking_ring)) && Enabled();
  return blocking;
}

// Big-endian guest word. TranslateVirtual applies each heap's host offset (the write-back word is in a physical view,
// shifted by 0x1000 on Windows above 0xE0000000).
uint32_t LoadGuest32(uint32_t address) {
  uint32_t value;
  std::memcpy(&value, rex::system::kernel_state()->memory()->TranslateVirtual(address), sizeof(value));
  return __builtin_bswap32(value);
}

uint8_t LoadGuest8(uint32_t address) {
  return *rex::system::kernel_state()->memory()->TranslateVirtual(address);
}

}  // namespace

// Optimization_Hook (renut_hooks.toml, inside sub_8225D370's loop, just before its sub_82634740 check): the waiting is
// done by the hook below; without it, the yield it always did.
void WaitRingProgress() {
  if (!BlockingRingWait()) {
    std::this_thread::yield();
  }
}

}  // namespace me::native

// katamari-nx (2026-10-30): N&B's ring-wait function; not hooked in Beautiful Katamari (the address is another function
// here). KATAMARI_BANJO_ADDRESS_HOOKS stays 0 until the game's own wait is found.
#if KATAMARI_BANJO_ADDRESS_HOOKS
REX_EXTERN(__imp__sub_82634740);
REX_HOOK_RAW(sub_82634740) {
  BANJO_RENUT_YIELD_SHARED(sub_82634740)  // Renut's renderer chosen (banjo_renderer = "renut")
  using namespace me::native;
  if (BlockingRingWait()) {
    const uint32_t record = ctx.r3.u32;
    const uint32_t began = LoadGuest32(record + 20);
    thread_local uint32_t last_record = 0, last_began = 0;
    const bool repeat = record == last_record && began == last_began;
    last_record = record;
    last_began = began;
    const uint32_t device = LoadGuest32(record);
    if (repeat && device && !(LoadGuest8(device + 10941) & 0x02)) {
      static const auto max_wait = std::chrono::microseconds(REXCVAR_GET(masseffect_wait_ring_max_us));
      // The counter first: a write-back after this load bumps it, so the wake-up cannot be lost.
      const uint32_t before = g_progress.load(std::memory_order_seq_cst);
      const uint32_t word = LoadGuest32(device + 10896);
      if (word && LoadGuest32(word) == LoadGuest32(record + 8)) {
        std::unique_lock<std::mutex> lock(g_progress_mutex);
        g_waiting.fetch_add(1, std::memory_order_seq_cst);
        g_progress_cv.wait_for(lock, max_wait,
                               [&] { return g_progress.load(std::memory_order_seq_cst) != before; });
        g_waiting.fetch_sub(1, std::memory_order_seq_cst);
      }
    }
  }
  __imp__sub_82634740(ctx, base);
}
#endif  // KATAMARI_BANJO_ADDRESS_HOOKS

// banjo-nx: Mass Effect's other hooks on its D3D busy-waits (sub_822CE0C0 render-thread handoff, sub_82811750
// occlusion poll) are not ported: Banjo's equivalents have not been identified.
