/**
 * @file    gpu/backend/render_thread.cpp
 */
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/stage_census.h"  // blur-nx (2026-11-17, the Renut audit's census)
#include "renut_engine/gpu/host_batch.h"  // blur-nx (2026-11-18, the Renut host batch)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <fmt/format.h>
#include <string>
#include <rex/frame_latency.h>
#include <vector>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/video.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "renut_engine/display_sync.h"  // sa2-nx (2026-10-28): LateInputGuardUs

#include <rex/cvar.h>

// banjo-nx (2026-10-20, from sa2-nx): see g_done_waiters and Run.
// rr6-nx (2026-11-03, from sasrt-nx, which found this on its console): the game running ahead of the render thread can
// rewrite data before the render thread reads it. Ryujinx rarely shows it; a slow GPU shows it constantly.
REXCVAR_DEFINE_INT32(rr6_frames_ahead, 1, "Banjo/Enhancements",
                     "How many frames the game may run ahead of Renut's render thread. 0: the game waits at its Swap "
                     "until the render thread has done the frame. 1: one frame ahead. 2 (blur-nx #37, near-exact): two "
                     "frames ahead (the game may rewrite data the render thread has yet to read)")
    .range(0, 1)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(rr6_diag_render_delay_ms, 0, "Banjo/Enhancements",
                     "Diagnostic: each frame's work on the render thread waits this long first, so the PC lags behind "
                     "the game as a console does")
    .range(0, 500)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_done_notify_waiters, true, "Banjo/Enhancements",
                    "Renut's render thread signals its work done (frames, fences, drains) only when a thread waits for "
                    "it: a signal is a kernel call on the Switch even with nobody waiting, and it made one after every "
                    "batch of work. Off: after every batch, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_render_address_wait, true, "Banjo/Renut",
                    "blur-nx (the Renut audit's #11, exact; the Switch): with the lock-free ring, the render thread sleeps "
                    "on an address (svcWaitForAddress) and a push wakes it with svcSignalToAddress, without g_mutex on "
                    "either side (Blur's Game Master: 0.88 % in notify_one waking it). false: g_work_cv, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(blur_renut_done_lockfree, true, "Banjo/Renut",
                    "blur-nx (the Renut audit's #12, exact): the lock-free ring's batch end counts its items and reads "
                    "the done waiters as atomics, taking g_mutex only to wake one (with banjo_renut_done_notify_waiters). "
                    "false: g_mutex at every batch end, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_prefetch_next, true, "Banjo/Enhancements",
                    "Renut's render thread asks for the next draw's capture (written on the game thread's core) to be "
                    "brought into its cache while it does the draw before it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-11-20, render-thread batch, experiment #58): see RunRing.
REXCVAR_DEFINE_INT32(banjo_renut_ring_spin_us, 0, "Banjo/Renut",
                     "Experiment: microseconds Renut's render thread looks for new work before it sleeps on an empty "
                     "ring (0: sleeps at once)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_renut_wake_batch, 16, "Banjo/Enhancements",
                     "Renut's render thread, when it has caught up and sleeps, is woken by the game's draws only once "
                     "this many are queued (each wake is a kernel call on both threads; the fourth console run woke it "
                     "for almost every draw). Every other item (frames, fences, resources), every wait for it and a "
                     "2 ms timer still wake it at once. 1 or less: every draw wakes it, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_renut_batch_clock, true, "Banjo/Enhancements",
                    "Renut's render thread reads the clock once per batch of work instead of once per item (about "
                    "1,800 items a frame in City Escape); its busy time per frame is the same sum")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// dishonored-nx (sa2-nx's of 2026-10-28; port 105): the render queue as a lock-free ring.
REXCVAR_DEFINE_BOOL(dishonored_renut_lockfree_queue, true, "Banjo/Enhancements",
                    "From dishonored-nx (sa2-nx's): Renut's render thread takes its work from a lock-free ring (one "
                    "compare-and-swap a push) instead of a vector under the queue's mutex, which every draw of the game "
                    "thread took and the render thread took back (two lock round trips a draw on two cores, and the "
                    "wake decision under the lock). Same order, same wakes. Off: the mutex and the vector")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__SWITCH__)
// blur-nx (2026-11-17, the Renut audit's #11): the SDK's address-wait helpers (threading_switch.cpp).
extern "C" bool RexSwitchWaitIfEqual(volatile uint32_t* word, uint32_t value, int64_t ns);
extern "C" void RexSwitchWakeAddress(volatile uint32_t* word, int32_t count);
#endif

namespace renut::gpu {
void SettleExportsForFence();  // port 170: render/draw.cpp (banjo_fence_waits_exports)
}

namespace renut::gpu::render_thread {

namespace {

using Clock = std::chrono::steady_clock;

struct Item {
  std::function<void()> work;
  bool locked = true;  // run with State().mutex held
  const void *prefetch[2] = {};  // banjo-nx (2026-10-20, from sa2-nx, banjo_renut_prefetch_next): fetched while the item before runs
};

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_prefetch_next): the next item's data into the cache (its first lines).
inline void PrefetchItem(const Item &item) {
  if (const auto *a = static_cast<const char *>(item.prefetch[0])) {
    __builtin_prefetch(a);
    __builtin_prefetch(a + 64);
    // sasrt-nx (2026-11-12, batch C/D, item C8; port 93): prefetch[0] is a draw's capture (QueuePrefetched's only
    // caller, QueueCapture): its call's second line (with its binding) and its stack headers' count (blur-nx's, port 76),
    // which the lambda reads first. (sasrt-nx's +320 and +384, its recorded surfaces' headers, are the stack headers'
    // own here, read only when the count is not 0: not fetched.)
    __builtin_prefetch(a + 128);
    __builtin_prefetch(a + 256);
  }
  if (const auto *b = static_cast<const char *>(item.prefetch[1])) {
    __builtin_prefetch(b);
    __builtin_prefetch(b + 64);
    __builtin_prefetch(b + 128);
    __builtin_prefetch(b + 192);
  }
}

std::mutex g_mutex;
std::condition_variable g_work_cv;  // work queued, or stopping
// banjo-nx (banjo_renut_queue_notify_idle): the render thread is in g_work_cv.wait (under g_mutex). A push notifies only
// then: on the Switch every notify is a kernel call, about 1,500 a frame from the game threads, most of them while the
// render thread is busy and not waiting. The render thread reads the queue under g_mutex before it sleeps, so a push
// it has not seen is never missed.
bool g_render_waiting = false;
std::condition_variable g_done_cv;  // work done: frames, fences, drains; or stopped
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_done_notify_waiters): threads in g_done_cv's waits (under g_mutex). A done signal goes
// only when there is one: a waiter checks its condition under g_mutex before it counts itself and sleeps, and the
// state it waits for changes under g_mutex too, so no signal it needs is skipped.
// blur-nx (2026-11-17, the Renut audit's #12): atomics (sequentially consistent), so the ring's batch end can count
// its items and read the waiters without g_mutex (blur_renut_done_lockfree, RunRing); every other access is still
// under g_mutex as before.
std::atomic<int> g_done_waiters{0};
bool DoneNotifyAll() {
  static const bool on = REXCVAR_GET(banjo_renut_done_notify_waiters);
  return !on;
}
// blur-nx (2026-11-17, the Renut audit's #11, blur_renut_render_address_wait; the Switch): the ring's sleep on an
// address instead of g_work_cv. The render thread reads g_wake_word, looks at the ring once more and sleeps while the
// word is unchanged (svcWaitForAddress); a push that publishes, then reads g_waiting true, adds to the word and wakes
// the address (svcSignalToAddress): no g_mutex on either side, and the woken thread does not take a mutex first (a
// condition variable's wake on Horizon hands it the mutex). Either the push's add comes before the render thread's read
// of the word (then its look at the ring, after that read, sees the item) or after it (then the wait sees the word
// changed, or is woken). g_stop_flag: g_stopping for the sleeper, which holds no g_mutex.
std::atomic<uint32_t> g_wake_word{0};
std::atomic<bool> g_stop_flag{false};
// blur-nx (2026-11-17, audit #11): blur_renut_render_address_wait (the Switch only).
bool AddressWakeOn() {
#if defined(__SWITCH__)
  static const bool on = REXCVAR_GET(blur_renut_render_address_wait);
  return on;
#else
  return false;
#endif
}
// A push's wake of the sleeping render thread (g_waiting read true): the address, or g_work_cv (taking g_mutex first,
// so the notify cannot come between the render thread's last look and its wait; Locked: the caller holds g_mutex).
void WakeRenderAddress() {
#if defined(__SWITCH__)
  g_wake_word.fetch_add(1, std::memory_order_seq_cst);
  RexSwitchWakeAddress(reinterpret_cast<volatile uint32_t *>(&g_wake_word), 1);
#endif
}
// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_wake_batch): the draws queued before a sleeping render thread is woken (1: each).
size_t WakeBatch() {
  static const size_t n = size_t(std::max(1, int(REXCVAR_GET(banjo_renut_wake_batch))));
  return n;
}
std::vector<Item> g_queue;

// dishonored-nx (sa2-nx's of 2026-10-28, dishonored_renut_lockfree_queue; port 105): the queue as a bounded ring of
// kRingSlots items (D. Vyukov's bounded MPMC queue, with the render thread its one consumer). A push claims the next
// position with a compare-and-swap, moves its item into the slot and publishes it by the slot's sequence (release); the
// render thread takes the positions in order (acquire), runs each item in its slot and gives the slot back for the push
// kRingSlots positions later. The order is the order of the claims, as it was the order of the lock. sa2-nx's 2P console
// profile had the queue's mutex and vector at 1.4 % of the game thread and its share of the locks (2.5 %) besides, with
// the render thread on the same lock's cache line at every batch. A full ring (kRingSlots items queued, more than the
// game thread can be ahead by with one frame queued) makes a pusher wait for the render thread; the render thread itself
// runs its own item at once instead (logged: never expected).
// Latched by Start before the thread is made and before g_started's release, which every Push reads first (acquire): a
// plain load per push.
bool g_lockfree = false;
bool LockfreeOn() {
  return g_lockfree;
}
// The same for the two PushRing reads at every push: QueueNotifyIdleOn and WakeBatch, latched by Start.
bool g_notify_idle = true;
size_t g_wake_batch = 16;
// sa2-nx (2026-10-28, the console's first run of the ring): with 32,768 slots (2 MB) every push's slot was a line from
// memory (a slot came round again nine frames later, long out of the caches): 30 of PushRing's 86 samples were the load
// of its sequence. A push now prefetches the slot kRingAhead positions on for writing, a few draws before it needs it,
// and the ring is 16,384 slots (1 MB): still more than two frames of the two-player race's items (about 3,500 a frame),
// so that a pusher never waits on a full ring while it may hold a lock the render thread's item needs.
constexpr uint64_t kRingSlots = uint64_t(1) << 14;
constexpr uint64_t kRingAhead = 8;
struct alignas(64) RingSlot {
  std::atomic<uint64_t> seq{0};
  Item item;
};
RingSlot *g_ring = nullptr;                      // made by Start, before the thread runs
alignas(64) std::atomic<uint64_t> g_enqueue{0};  // the next position a push claims (the items queued)
alignas(64) std::atomic<uint64_t> g_dequeue{0};  // the next position the render thread takes (its own)
std::atomic<bool> g_waiting{false};              // the render thread is going to sleep, or asleep, in g_work_cv
std::atomic<bool> g_accepting{false};            // started and not stopping: pushes go to the ring
// The slot at 'pos' holds a published item.
inline bool RingReady(uint64_t pos) {
  return g_ring[pos & (kRingSlots - 1)].seq.load(std::memory_order_acquire) == pos + 1;
}
// The same as the sleeping side's last look: sequentially consistent, so that it is ordered after its store of
// g_waiting (an ldar after an stlr on AArch64, which needs no barrier between them; a push publishes the same way).
inline bool RingReadySc(uint64_t pos) {
  return g_ring[pos & (kRingSlots - 1)].seq.load(std::memory_order_seq_cst) == pos + 1;
}
std::once_flag g_start_once;
// bangaio-nx (2026-10-29, v6): set once Start's call_once has run (relaxed: Push then takes g_mutex, under which Start
// set g_running). Push checks it first: std::call_once was a pthread_once call at every queued draw on the game thread
// (0.6 % of it in the v4 console storm).
std::atomic<bool> g_started{false};
std::thread::id g_thread_id;
bool g_running = false;   // the thread takes work
bool g_stopping = false;  // Stop: finish what is queued, then end
uint64_t g_items_queued = 0;
std::atomic<uint64_t> g_items_done{0};  // blur-nx (2026-11-17, audit #12): see g_done_waiters
uint64_t g_frames_queued = 0;
uint64_t g_frames_done = 0;
uint64_t g_fences_done = 0;  // fence completions written, plus callbacks reached
uint64_t g_callbacks_reached = 0;
uint64_t g_callbacks_run = 0;
// Fence completions waiting for their callbacks, in order.
struct PendingFence {
  u32 completion_va;
  u32 fence;
  uint64_t after_callback;
};
std::vector<PendingFence> g_pending_fences;

// Requires g_mutex.
void WriteFence(u32 completion_va, u32 fence) {
  if (auto *word = GuestPtr<be_u32>(completion_va))
    *word = fence;
  ++g_fences_done;
}

// Render thread only: time spent running work since the last frame ended, and
// when the running item started; of that time, how much it waited (WaitScope).
Clock::duration g_busy{};
Clock::time_point g_item_start{};
Clock::duration g_waited{};
thread_local bool t_render_thread = false;

void Execute(Item &item) {
  ::renut::gpu::census::Scope census_scope(::renut::gpu::census::kRunItem);  // blur-nx (2026-11-17)
  if (item.locked) {
    std::lock_guard lock(State().mutex);
    item.work();
  } else {
    item.work();
  }
}

// dishonored-nx (sa2-nx's of 2026-10-28, dishonored_renut_lockfree_queue; port 105): Run with the ring. The items are
// taken in order as long as they are published, at most kRingRun before the done bookkeeping (Drain's count, the done
// waiters); with none ready the thread says it is going to sleep (g_waiting), looks once more, and sleeps in g_work_cv
// under g_mutex: a push that read g_waiting false after publishing (both sides store, then load, sequentially
// consistent: no store-load reordering) is seen by that last look, and one that read it true takes g_mutex before it
// notifies, so it cannot notify between the look and the wait.
constexpr size_t kRingRun = 256;
// banjo-nx (2026-11-20, render-thread batch, experiment #58): with the ring empty, the render thread looks again for up
// to this many microseconds (a YIELD between looks) before it sleeps: a push in that time is taken without the kernel's
// sleep and wake (svcWaitForAddress / svcSignalToAddress, tens of microseconds each way). 0: sleep at once, as before.
// It spends the render thread's core while it looks; measure it (tools/ab_halves.py) before keeping it.
void RunRing() {
  static const bool batch_clock = REXCVAR_GET(banjo_renut_batch_clock);
  static const bool prefetch = REXCVAR_GET(banjo_renut_prefetch_next);
  static const int spin_us = REXCVAR_GET(banjo_renut_ring_spin_us);
  uint64_t pos = g_dequeue.load(std::memory_order_relaxed);
  for (;;) {
    if (spin_us > 0 && !RingReady(pos)) {
      const Clock::time_point until = Clock::now() + std::chrono::microseconds(spin_us);
      while (!RingReady(pos) && Clock::now() < until) {
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
      }
    }
    if (!RingReady(pos)) {
      g_waiting.store(true, std::memory_order_seq_cst);
      bool stopping = false;
      if (AddressWakeOn()) {
        // blur-nx (2026-11-17, audit #11): the word read before the last look (see g_wake_word). With a wake batch,
        // draws queued without a wake are taken within 2 ms, as with g_work_cv; otherwise a 100 ms net.
        const int64_t slice_ns = WakeBatch() > 1 ? 2'000'000 : 100'000'000;
        for (;;) {
          const uint32_t word = g_wake_word.load(std::memory_order_seq_cst);
          if (RingReadySc(pos) || g_stop_flag.load(std::memory_order_acquire))
            break;
#if defined(__SWITCH__)
          RexSwitchWaitIfEqual(reinterpret_cast<volatile uint32_t *>(&g_wake_word), word, slice_ns);
#else
          (void)word;
          (void)slice_ns;
#endif
        }
        stopping = g_stop_flag.load(std::memory_order_acquire);
      } else if (!RingReadySc(pos)) {
        std::unique_lock lock(g_mutex);
        g_render_waiting = true;
        if (WakeBatch() > 1) {
          // banjo-nx (from sa2-nx) (banjo_renut_wake_batch): draws queued without a wake are taken within 2 ms at the latest.
          while (!RingReady(pos) && !g_stopping)
            g_work_cv.wait_for(lock, std::chrono::milliseconds(2));
        } else {
          g_work_cv.wait(lock, [pos] { return RingReady(pos) || g_stopping; });
        }
        g_render_waiting = false;
        stopping = g_stopping;
      }
      g_waiting.store(false, std::memory_order_relaxed);
      if (!RingReady(pos)) {
        if (stopping)
          break;
        continue;
      }
    }
    const Clock::time_point start = Clock::now();
    if (batch_clock)
      g_item_start = start;
    size_t n = 0;
    Clock::time_point item_start = start;
    do {
      RingSlot &slot = g_ring[pos & (kRingSlots - 1)];
      if (prefetch && RingReady(pos + 1))
        PrefetchItem(g_ring[(pos + 1) & (kRingSlots - 1)].item);
      if (!batch_clock)
        g_item_start = item_start;
      Execute(slot.item);
      slot.item.work = nullptr;  // the closure's captures go now, as the batch's cleared vector freed them
      slot.seq.store(pos + kRingSlots, std::memory_order_release);
      ++pos;
      g_dequeue.store(pos, std::memory_order_relaxed);
      ++n;
      if (!batch_clock) {
        const Clock::time_point end = Clock::now();
        g_busy += end - item_start;
        item_start = end;
      }
    } while (n < kRingRun && RingReady(pos));
    if (batch_clock)
      g_busy += Clock::now() - start;
    bool wake;
    // blur-nx (2026-11-17, the Renut audit's #12, blur_renut_done_lockfree): the count added and the waiters read
    // without g_mutex (both sequentially consistent). A waiter counts itself under g_mutex before it tests its
    // condition: either this load sees it (and the wake takes g_mutex once, so the waiter is asleep in g_done_cv or
    // has not yet tested), or its test comes after this add and sees the new count.
    static const bool lockfree = REXCVAR_GET(blur_renut_done_lockfree);
    if (lockfree && !DoneNotifyAll()) {
      g_items_done.fetch_add(n, std::memory_order_seq_cst);
      wake = g_done_waiters.load(std::memory_order_seq_cst) > 0;
      if (wake) {
        std::lock_guard lock(g_mutex);
      }
    } else {
      std::lock_guard lock(g_mutex);
      g_items_done += n;
      wake = g_done_waiters > 0 || DoneNotifyAll();
    }
    if (wake)
      g_done_cv.notify_all();
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  g_accepting.store(false, std::memory_order_release);
  g_done_cv.notify_all();
}

void Run() {
  {
    // banjo-nx: the thread may be a plain pthread on the Switch (Start), so it records its own id.
    std::lock_guard lock(g_mutex);
    g_thread_id = std::this_thread::get_id();
  }
#if defined(_WIN32)
  SetThreadDescription(GetCurrentThread(), L"renut render");
#endif
  banjo::renut_gpu::OnRenderThreadStart();  // banjo-nx: its core and priority on the Switch
  t_render_thread = true;
  if (LockfreeOn()) {  // dishonored-nx (port 105, dishonored_renut_lockfree_queue)
    RunRing();
    return;
  }
  std::vector<Item> batch;
  for (;;) {
    {
      std::unique_lock lock(g_mutex);
      g_render_waiting = true;
      if (WakeBatch() > 1) {
        // banjo-nx (from sa2-nx) (banjo_renut_wake_batch): draws queued without a wake are taken within 2 ms at the latest.
        while (g_queue.empty() && !g_stopping)
          g_work_cv.wait_for(lock, std::chrono::milliseconds(2));
      } else {
        g_work_cv.wait(lock, [] { return !g_queue.empty() || g_stopping; });
      }
      g_render_waiting = false;
      if (g_queue.empty())
        break;
      batch.swap(g_queue);
    }
    // banjo-nx (2026-10-19, render-thread proposal 19): one clock read per item (the end of one is the start of the
    // next). Its other half, one State().mutex hold across consecutive locked items, hung the emulator run at the
    // first loading screen (a game thread's hook waits on the lock while the render thread's next item waits on it):
    // each item takes the lock on its own, as before.
    Clock::time_point start = Clock::now();
    static const bool batch_clock = REXCVAR_GET(banjo_renut_batch_clock);
    if (batch_clock) {
      // banjo-nx (2026-10-20, from sa2-nx): the batch's start for every item (QueueFrame's item counts its frame up to itself from
      // there and leaves the rest of the batch to the next frame), one clock read at the end.
      g_item_start = start;
      static const bool prefetch = REXCVAR_GET(banjo_renut_prefetch_next);
      const size_t n = batch.size();
      for (size_t i = 0; i < n; ++i) {
        if (prefetch && i + 1 < n)
          PrefetchItem(batch[i + 1]);
        Execute(batch[i]);
      }
      g_busy += Clock::now() - start;
    } else {
      for (Item &item : batch) {
        g_item_start = start;
        Execute(item);
        const Clock::time_point end = Clock::now();
        g_busy += end - start;
        start = end;
      }
    }
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_items_done += batch.size();
      wake = g_done_waiters > 0 || DoneNotifyAll();
    }
    if (wake)
      g_done_cv.notify_all();
    batch.clear();
  }
  {
    std::lock_guard lock(g_mutex);
    g_running = false;
  }
  g_done_cv.notify_all();
}

void Start() {
  std::call_once(g_start_once, [] {
    InstallLateInput();  // banjo-nx (2026-10-19, banjo_late_input): the delay function (it reads the key per frame)
    std::lock_guard lock(g_mutex);
    g_running = true;
    // dishonored-nx (port 105, dishonored_renut_lockfree_queue): latched before the thread is made and before
    // g_started's release; the ring, each slot free for its position.
    g_lockfree = REXCVAR_GET(dishonored_renut_lockfree_queue);
    g_notify_idle = banjo::renut_gpu::QueueNotifyIdleOn();
    g_wake_batch = WakeBatch();
    if (LockfreeOn()) {
      g_ring = new RingSlot[kRingSlots];
      for (uint64_t i = 0; i < kRingSlots; ++i)
        g_ring[i].seq.store(i, std::memory_order_relaxed);
      g_accepting.store(true, std::memory_order_release);
    }
    // Never joined or destroyed: the renderer's objects are left at exit too
    // (Video::BeginShutdown), and a joinable std::thread destroyed at exit
    // would terminate the process.
    // banjo-nx: a 1 MB stack on the Switch (the conversions and the driver run deep on this thread), never joined
    // (banjo_renut_tuning.h); Run records its id.
    banjo::renut_gpu::StartLongLivedThread(Run, 1024 * 1024);
    RGPU_INFO("Render thread started");
  });
  // dishonored-nx (port 105): release, read with acquire by Push: the ring's latch and the ring itself are seen with it
  // (a push into the ring takes no g_mutex, which made relaxed enough before).
  g_started.store(true, std::memory_order_release);
}

// banjo-nx (2026-10-20, from sa2-nx, banjo_renut_wake_batch): before a thread waits for the render thread, the draws queued without a
// wake are handed over. Requires g_mutex.
void WakeForWaitLocked() {
  if (LockfreeOn()) {  // dishonored-nx (port 105, dishonored_renut_lockfree_queue)
    if (AddressWakeOn()) {  // blur-nx (2026-11-17, audit #11): the sleeper holds no g_mutex; g_waiting says it sleeps
      if (g_waiting.load(std::memory_order_seq_cst) && g_ring && RingReady(g_dequeue.load(std::memory_order_relaxed)))
        WakeRenderAddress();
      return;
    }
    if (g_render_waiting && g_ring && RingReady(g_dequeue.load(std::memory_order_relaxed)))
      g_work_cv.notify_one();
    return;
  }
  if (g_render_waiting && !g_queue.empty())
    g_work_cv.notify_one();
}

// vivapinata-nx (2026-11-11, vivapinata_tiles_once): SetBeforePushHook's function.
std::atomic<void (*)()> g_before_push{nullptr};

[[gnu::always_inline]] inline void BeforePush() {
  if (void (*hook)() = g_before_push.load(std::memory_order_relaxed); hook && !t_render_thread) [[unlikely]]
    hook();
}

// dishonored-nx (sa2-nx's of 2026-10-28, dishonored_renut_lockfree_queue; port 105): Push with the ring; false when the
// item was not queued (the ring is full and this is the render thread, which cannot wait for itself). The wake decision
// is Push's: a non-draw item, or WakeBatch draws not yet taken, wakes a render thread that is going to sleep (RunRing
// says why no wake is lost).
bool PushRing(Item &&item, bool draw) {
  uint64_t pos = g_enqueue.load(std::memory_order_relaxed);
  RingSlot *slot;
  for (;;) {
    slot = &g_ring[pos & (kRingSlots - 1)];
    const uint64_t seq = slot->seq.load(std::memory_order_acquire);
    const int64_t diff = int64_t(seq - pos);
    if (diff == 0) {
      if (g_enqueue.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
        break;
    } else if (diff < 0) {
      // Full: the render thread is kRingSlots items behind.
      if (t_render_thread) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
          RGPU_WARN("Render thread queue full ({} items): the render thread's own item ran at once "
                    "(dishonored_renut_lockfree_queue)", kRingSlots);
        return false;
      }
      if (g_waiting.load(std::memory_order_seq_cst)) {  // (a signal is a kernel call on the Switch: only to a sleeper)
        if (AddressWakeOn()) {
          WakeRenderAddress();  // blur-nx (2026-11-17, audit #11)
        } else {
          {
            std::lock_guard lock(g_mutex);
          }
          g_work_cv.notify_one();
        }
      }
      std::this_thread::yield();
      pos = g_enqueue.load(std::memory_order_relaxed);
    } else {
      pos = g_enqueue.load(std::memory_order_relaxed);
    }
  }
  // The slot kRingAhead positions on, into this core's cache for writing while this one is filled (its line was last
  // the render thread's).
  __builtin_prefetch(&g_ring[(pos + kRingAhead) & (kRingSlots - 1)], 1);
  slot->item = std::move(item);
  // Published, then g_waiting read: a sequentially consistent store and load (stlr, ldar: no dmb), RunRing's order.
  slot->seq.store(pos + 1, std::memory_order_seq_cst);
  // blur-nx (2026-11-18, the Renut host batch's #28, blur_renut_wake_batch_all / A/B 238): with a wake batch (the render
  // thread then looks every 2 ms by itself), any item may wait for the batch as draws do: command buffer runs and
  // resolves woke it at once, each a kernel call on the pushing thread (the Game Master's), with the render thread idle
  // half the time.
  const bool as_draw = draw || (g_wake_batch > 1 && ::renut::gpu::host_batch::On(::renut::gpu::host_batch::kWakeBatchAll));
  const bool wake = (g_waiting.load(std::memory_order_seq_cst) &&
                     (!as_draw || pos + 1 - g_dequeue.load(std::memory_order_relaxed) >= g_wake_batch)) ||
                    !g_notify_idle;
  if (wake) {
    const bool hb_census = ::renut::gpu::host_batch::CensusOn();  // blur-nx (2026-11-18, the census's #28)
    const uint64_t t0 = hb_census ? ::renut::gpu::host_batch::Ticks() : 0;
    if (AddressWakeOn()) {
      WakeRenderAddress();  // blur-nx (2026-11-17, audit #11)
    } else {
      {
        std::lock_guard lock(g_mutex);
      }
      g_work_cv.notify_one();
    }
    if (hb_census) {
      ::renut::gpu::host_batch::Add(draw ? ::renut::gpu::host_batch::kWakesDraw : ::renut::gpu::host_batch::kWakesOther);
      ::renut::gpu::host_batch::Add(::renut::gpu::host_batch::kWakeTicks, ::renut::gpu::host_batch::Ticks() - t0);
    }
  }
  return true;
}

void Push(std::function<void()> work, bool locked, const void *prefetch0 = nullptr,
          const void *prefetch1 = nullptr, bool draw = false) {
  BeforePush();  // vivapinata-nx (2026-11-11): a held tile run goes in first
  if (!g_started.load(std::memory_order_acquire)) [[unlikely]]
    Start();  // bangaio-nx (2026-10-29, v6): only until it has run (g_started; acquire since port 105's ring)
  if (LockfreeOn()) {  // dishonored-nx (port 105, dishonored_renut_lockfree_queue)
    Item item{std::move(work), locked, {prefetch0, prefetch1}};
    if (g_accepting.load(std::memory_order_acquire) && PushRing(std::move(item), draw))
      return;
    // Stopping, or the render thread's own item in a full ring: run here, as a push while stopping did.
    Execute(item);
    return;
  }
  {
    std::unique_lock lock(g_mutex);
    // (Work queued by the render thread's own work runs after it.)
    if (g_running && !g_stopping) {
      g_queue.push_back({std::move(work), locked, {prefetch0, prefetch1}});
      ++g_items_queued;
      // banjo-nx (from sa2-nx) (banjo_renut_wake_batch): a draw wakes a sleeping render thread once enough are queued.
      const bool wake = (g_render_waiting && (!draw || g_queue.size() >= WakeBatch())) ||
                        !banjo::renut_gpu::QueueNotifyIdleOn();
      lock.unlock();
      if (wake)
        g_work_cv.notify_one();
      return;
    }
  }
  // Shutting down: nothing takes work any more, so run it here.
  Item item{std::move(work), locked, {}};
  Execute(item);
}

} // namespace

void SetBeforePushHook(void (*hook)()) {
  g_before_push.store(hook, std::memory_order_relaxed);
}

void Queue(std::function<void()> work) {
  Push(std::move(work), true);
}

void QueueUnlocked(std::function<void()> work) {
  Push(std::move(work), false);
}

void QueuePrefetched(const void *prefetch0, const void *prefetch1, std::function<void()> work) {
  Push(std::move(work), true, prefetch0, prefetch1, true);  // banjo-nx (from sa2-nx): the game's draws (banjo_renut_wake_batch)
}

void QueueFenceCompletion(u32 completion_va, u32 fence, uint64_t after_callback) {
  QueueUnlocked([completion_va, fence, after_callback] {
    ::renut::gpu::SettleExportsForFence();  // port 170 (banjo_fence_waits_exports; nothing when off)
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      // Fences complete in order: behind one still waiting, this one waits too.
      if (g_pending_fences.empty() && after_callback <= g_callbacks_run)
        WriteFence(completion_va, fence);
      else
        g_pending_fences.push_back({completion_va, fence, after_callback});
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
}

void QueueCallbackMarker(uint64_t seq) {
  QueueUnlocked([seq] {
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      g_callbacks_reached = std::max(g_callbacks_reached, seq);
      ++g_fences_done;  // wakes WaitForFenceProgress: there is a callback to run
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
}

uint64_t CallbacksReached() {
  std::lock_guard lock(g_mutex);
  return g_callbacks_reached;
}

void CallbacksRun(uint64_t seq) {
  bool wake;
  {
    std::lock_guard lock(g_mutex);
    g_callbacks_run = std::max(g_callbacks_run, seq);
    size_t written = 0;
    for (; written < g_pending_fences.size(); ++written) {
      const PendingFence &p = g_pending_fences[written];
      if (p.after_callback > g_callbacks_run)
        break;
      WriteFence(p.completion_va, p.fence);
    }
    g_pending_fences.erase(g_pending_fences.begin(), g_pending_fences.begin() + written);
    wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
  }
  if (wake)
    g_done_cv.notify_all();
}

bool WaitForFenceProgress(std::chrono::milliseconds timeout) {
  BeforePush();  // vivapinata-nx (2026-11-11): nothing waits on work still held back
  std::unique_lock lock(g_mutex);
  if (!g_running || g_stopping)
    return false;
  const uint64_t seen = g_fences_done;
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait_for(lock, timeout,
                     [seen] { return g_fences_done != seen || !g_running || g_stopping; });
  --g_done_waiters;
  return g_running && !g_stopping;
}

// banjo-nx (2026-10-19, input latency, banjo_late_input): the game reads its controller at the start of a frame, and
// when the render thread is the slower of the two its frame then waits: the Swap below waits for the frame before to be
// done. The slack of a frame is that wait plus the delay its read already had: how much later the read could have been
// without the frame reaching the render thread any later. The next frame's first read is delayed by the least slack of
// the last kSlackFrames frames less kLateInputMarginUs (rex::latency::SetFrameReadDelayFunction): a frame whose game
// work runs long eats into the margin, and a run of them shrinks the delay; with the game thread or the frame cap the
// limit there is no wait, and the delay goes to 0.
namespace {
constexpr uint32_t kSlackFrames = 16;
constexpr uint64_t kLateInputMarginUs = 2000;
// banjo-nx (2026-10-19, latency batch 3): the most the read is delayed: 80 % of the game's frame interval (averaged over
// the frames at QueueFrame), at most kLateInputCeilingUs. A fixed 30 ms held it back where the GPU is the limit: the
// console's 18 fps scenes (package 35) delayed 30.0 ms with more slack than that.
constexpr uint64_t kLateInputCeilingUs = 60000;
std::atomic<uint64_t> g_frame_interval_us{0};
uint64_t g_last_queue_us = 0;
uint64_t g_slack_us[kSlackFrames] = {};
uint32_t g_slack_next = 0;
uint32_t g_slack_count = 0;
std::atomic<uint32_t> g_applied_delay_us{0};
std::atomic<uint64_t> g_delay_total_us{0}, g_delay_frames{0};

// banjo-nx (2026-10-20, package 36s): banjo_late_input_gpu (36l-36r), a second slack from the GPU queue's wait, is gone:
// on the console it gained nothing at the 30 fps cap and, in scenes just able to hold 30, its delay ratcheted up to
// 29 ms and cost about 6 fps (36q). docs/deko3d-renderer.md section 25.

uint32_t LateInputDelay() {
  if (!banjo::renut_gpu::LateInputOn() || g_slack_count < kSlackFrames) {
    g_applied_delay_us.store(0, std::memory_order_relaxed);
    return 0;
  }
  uint64_t least = UINT64_MAX;
  for (uint64_t s : g_slack_us)
    least = std::min(least, s);
  const uint64_t interval = g_frame_interval_us.load(std::memory_order_relaxed);
  const uint64_t ceiling = std::min(kLateInputCeilingUs, interval * 4 / 5);
  // sa2-nx (2026-10-28, sa2_renut_queue_drain, after ketsui-nx's display guard): with double buffering, 1 ms more margin
  // at each image late at its refresh (0.25 ms back after 180 in time), so a delayed read cannot push the frames back
  // into the late queue; 0 otherwise.
  const uint64_t margin = kLateInputMarginUs + banjo::display_sync::LateInputGuardUs();
  const uint64_t delay = least > margin ? std::min(least - margin, ceiling) : 0;
  g_applied_delay_us.store(uint32_t(delay), std::memory_order_relaxed);
  g_delay_total_us.fetch_add(delay, std::memory_order_relaxed);
  g_delay_frames.fetch_add(1, std::memory_order_relaxed);
  return uint32_t(delay);
}
}  // namespace

void InstallLateInput() {
  rex::latency::SetFrameReadDelayFunction(&LateInputDelay);
}

std::string LateInputReport() {
  const uint64_t frames = g_delay_frames.exchange(0, std::memory_order_relaxed);
  const uint64_t total = g_delay_total_us.exchange(0, std::memory_order_relaxed);
  return frames ? fmt::format("late input: the first controller read of a frame delayed {:.1f} ms on average ({} "
                              "frames)",
                              double(total) / 1000.0 / double(frames), frames)
                : std::string("late input: no delay");
}

uint64_t QueueFrame(std::function<void()> work) {
  {
    std::lock_guard lock(g_mutex);
    ++g_frames_queued;
  }
  QueueUnlocked([work = std::move(work)] {
    if (static const int32_t delay = REXCVAR_GET(rr6_diag_render_delay_ms); delay > 0)  // rr6-nx
      std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    work();
    // The frame's work includes this item's (the present); Run adds the whole
    // item afterwards, so what is left of it counts to the next frame.
    const Clock::duration so_far = Clock::now() - g_item_start;
    const double busy_ms =
        std::chrono::duration<double, std::milli>(g_busy + so_far - g_waited).count();
    g_busy = -so_far;
    g_waited = {};
    frame_stats::OnRenderThreadFrame(busy_ms);
    bool wake;
    {
      std::lock_guard lock(g_mutex);
      ++g_frames_done;
      wake = g_done_waiters > 0 || DoneNotifyAll();  // banjo-nx (from sa2-nx)
    }
    if (wake)
      g_done_cv.notify_all();
  });
  const auto waiting = std::chrono::steady_clock::now();
  std::unique_lock lock(g_mutex);
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait(lock, [] {
    static const uint64_t ahead = std::min<uint64_t>(uint64_t(REXCVAR_GET(rr6_frames_ahead)), kMaxFramesAhead);  // rr6-nx
    return !g_running || g_stopping || g_frames_queued - g_frames_done <= ahead;
  });
  --g_done_waiters;
  const uint64_t done = g_frames_done;
  lock.unlock();
  const auto waited = std::chrono::steady_clock::now() - waiting;
  frame_stats::OnGameThreadWait(std::chrono::duration<double, std::milli>(waited).count(), 1);  // blur-nx: #37
  // banjo-nx (2026-10-19, banjo_late_input): this frame's slack (the game thread only: one Swap at a time).
  {
    // The game's frame interval (the game thread only), averaged over about 16 frames.
    const uint64_t at = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (g_last_queue_us && at > g_last_queue_us) {
      const uint64_t gap = std::min<uint64_t>(at - g_last_queue_us, 1'000'000);
      const uint64_t avg = g_frame_interval_us.load(std::memory_order_relaxed);
      g_frame_interval_us.store(avg ? avg + (int64_t(gap) - int64_t(avg)) / 16 : gap, std::memory_order_relaxed);
    }
    g_last_queue_us = at;
  }
  g_slack_us[g_slack_next] =
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(waited).count()) +
      g_applied_delay_us.exchange(0, std::memory_order_relaxed);
  g_slack_next = (g_slack_next + 1) % kSlackFrames;
  g_slack_count = std::min(g_slack_count + 1, kSlackFrames);
  return done;
}

uint64_t FramesDone() {
  std::lock_guard lock(g_mutex);
  return g_frames_done;
}

void Drain() {
  BeforePush();  // vivapinata-nx (2026-11-11): nothing waits on work still held back
  std::unique_lock lock(g_mutex);
  if (!g_running || std::this_thread::get_id() == g_thread_id)
    return;
  // dishonored-nx (port 105, dishonored_renut_lockfree_queue): with the ring, the items queued are its claimed positions.
  const uint64_t target = LockfreeOn() ? g_enqueue.load(std::memory_order_acquire) : g_items_queued;
  WakeForWaitLocked();  // banjo-nx (from sa2-nx) (banjo_renut_wake_batch)
  ++g_done_waiters;  // banjo-nx (from sa2-nx) (banjo_renut_done_notify_waiters)
  g_done_cv.wait(lock, [target] { return !g_running || g_items_done >= target; });
  --g_done_waiters;
}

WaitScope::WaitScope() : start_(Clock::now()), counted_(t_render_thread) {}

WaitScope::~WaitScope() {
  if (counted_)
    g_waited += Clock::now() - start_;
}

void Stop() {
  {
    std::lock_guard lock(g_mutex);
    if (!g_running || g_stopping)
      return;
    g_stopping = true;
    g_stop_flag.store(true, std::memory_order_release);  // blur-nx (2026-11-17, audit #11)
  }
  g_accepting.store(false, std::memory_order_release);  // dishonored-nx (port 105): later pushes run inline
  if (AddressWakeOn())
    WakeRenderAddress();  // blur-nx (2026-11-17, audit #11)
  g_work_cv.notify_all();
  g_done_cv.notify_all();
}

} // namespace renut::gpu::render_thread
