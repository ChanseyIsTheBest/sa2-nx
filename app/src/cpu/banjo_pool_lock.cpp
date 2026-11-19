// banjo-nx - startup crash "guest read of 0x24 on thread F8000048": a race in the game's resource
// pool, made reachable by the PC/Switch timing.
//
// THE GAME
//   Two global pools, [0x82E198D4] and [0x82E198D8], are created lazily and reset by:
//     sub_82333388(which)  getter: if pool0 == 0, sub_823333F0(0x100000, 1); return pool[which]
//     sub_823333F0(size,n) reset: destroy pool0 (sub_82333668 frees every block with
//                          meInternalFree), store 0; destroy pool1, store 0; create both again.
//   The main thread resets them on display/movie transitions (sub_82392B60, sub_82392E80,
//   sub_8239C318, sub_82289C30). The loader thread (F8000048: sub_822F6988 ... sub_829FF5B0 ->
//   sub_82299800) calls the getter. Nothing makes the two exclusive.
//
// WHAT HAPPENS (traced: tools in banjo-nx-cpu/app-diag/src/cpu/banjo_heap_diag.cpp)
//   main:   reset ENTER (pools 40B97000 40B99CD0) - destroys pool0 - [0x82E198D4] = 0 - destroying pool1...
//   loader: getter sees pool0 == 0 -> reset ENTER -> pool0 == 0, skip -> pool1 != 0 -> destroys pool1 too
//   Every block of pool1 is freed twice. The second meInternalFree cannot place the pointer
//   (RtlSizeHeap -> -1 on all heaps), its heap lookup sub_8220F0B0 returns -1, it reads the heap
//   handle through the empty entry -1 (r31 = 0): "read of guest 0x00000024". Logged twice:
//   "ALREADY FREED (#224288 of 224290) by 82333698 thread 6 (main)" then the loader's free.
//   When the timing is a little different the race does not crash but leaks a pool (both threads
//   create one and the last store wins).
//
// THE FIX
//   The reset and the getter's check-and-create run under one host recursive mutex: the getter
//   waits for a reset in progress and then sees the new pool. Nothing else changes (same guest
//   code, same order). Lock order: this mutex -> the allocator's lock (taken inside the reset);
//   no caller of the getter holds a pool or allocator lock when it calls it.
//   Off with banjo_pool_lock = false (A/B).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <mutex>

// banjo-nx (2026-10-29): off by default on title update 3. The update guards these pools with a lock of its own
// (0x82E198E4: the getter and the reset take it; reNut upstream's game/level_pools.cpp), which fixes this race; this
// host mutex taken around the reset, which then takes the game's lock, would be the other order of a thread that holds
// the game's lock and calls the getter (a deadlock). Kept for A/B on the base.
REXCVAR_DEFINE_BOOL(banjo_pool_lock, false, "Banjo/Fixes",
                    "Serialize the game's resource-pool reset (sub_823333F0) and its lazy getter "
                    "(sub_82333388): fixes the startup crash 'read of guest 0x24' (double free)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
std::recursive_mutex& PoolMutex() {
  static std::recursive_mutex m;
  return m;
}
}  // namespace

REX_EXTERN(__imp__sub_823333F0);
REX_HOOK_RAW(sub_823333F0) {
  if (!REXCVAR_GET(banjo_pool_lock)) {
    __imp__sub_823333F0(ctx, base);
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(PoolMutex());
  __imp__sub_823333F0(ctx, base);
}

REX_EXTERN(__imp__sub_82333388);
REX_HOOK_RAW(sub_82333388) {
  if (!REXCVAR_GET(banjo_pool_lock)) {
    __imp__sub_82333388(ctx, base);
    return;
  }
  std::unique_lock<std::recursive_mutex> lock(PoolMutex(), std::try_to_lock);
  if (!lock.owns_lock()) {
    // Another thread is inside a pool reset: before this fix, this is where the getter could see
    // pool0 == 0 half-way through and destroy pool1 a second time.
    REXLOG_INFO("[banjo_pool_lock] pool getter (lr {:08X}) waited for a pool reset in progress",
                uint32_t(ctx.lr));
    lock.lock();
  }
  __imp__sub_82333388(ctx, base);
}
