// blur-nx (2026-11-14): see rex/system/wait_registry.h.
#include <rex/platform.h>
#include <rex/system/wait_registry.h>

#include <chrono>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

namespace rex::system::waitreg {

std::atomic<bool> g_on{false};
std::atomic<bool> g_reads_on{false};

namespace {

constexpr size_t kSlots = 256;
constexpr size_t kSignals = 4096;  // a power of two
constexpr int kProbes = 4;

// One per guest thread that has blocked; seq is a sequence lock (odd while the owner writes the fields).
struct Slot {
  std::atomic<uint32_t> owner{0};  // 1: a live thread has it
  std::atomic<uint32_t> seq{0};
  std::atomic<uint32_t> host_thread{0};
  std::atomic<uint32_t> handle{0};
  std::atomic<uint32_t> object_type{0};
  std::atomic<uint32_t> count{0};
  std::atomic<uint32_t> kind{0};
  std::atomic<int64_t> timeout_ms{0};
  std::atomic<uint64_t> since_ns{0};  // 0: not blocked
  std::atomic<uint32_t> guest[5] = {};  // port 167: WaitGuestCallers
};
Slot g_slots[kSlots];

struct Signal {
  std::atomic<uint32_t> handle{0};
  std::atomic<uint32_t> sets{0};
  std::atomic<uint32_t> resets{0};
  std::atomic<uint64_t> last_set_ns{0};
  std::atomic<uint64_t> last_reset_ns{0};
};
Signal g_signals[kSignals];

std::atomic<uint64_t> g_file_reads{0};
std::atomic<uint64_t> g_file_read_bytes{0};

// The thread's slot, given back when the thread exits.
struct ThreadSlot {
  int index = -1;  // -1: not claimed yet, -2: none free
  ~ThreadSlot() {
    if (index >= 0) {
      Slot& s = g_slots[index];
      const uint32_t q = s.seq.load(std::memory_order_relaxed);
      s.seq.store(q + 1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      s.since_ns.store(0, std::memory_order_relaxed);
      s.seq.store(q + 2, std::memory_order_release);
      s.owner.store(0, std::memory_order_release);
    }
  }
};
thread_local ThreadSlot t_slot;

Slot* MySlot() {
  if (t_slot.index == -1) {
    t_slot.index = -2;
    for (size_t i = 0; i < kSlots; i++) {
      uint32_t expected = 0;
      if (g_slots[i].owner.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        t_slot.index = int(i);
#if REX_PLATFORM_SWITCH
        g_slots[i].host_thread.store(threadGetCurHandle(), std::memory_order_relaxed);
#else
        g_slots[i].host_thread.store(0, std::memory_order_relaxed);
#endif
        break;
      }
    }
  }
  return t_slot.index >= 0 ? &g_slots[t_slot.index] : nullptr;
}

Signal& SignalOf(uint32_t handle) {
  const size_t home = size_t((handle * 0x9E3779B1u) >> 20) & (kSignals - 1);
  for (int i = 0; i < kProbes; i++) {
    Signal& e = g_signals[(home + size_t(i)) & (kSignals - 1)];
    uint32_t cur = e.handle.load(std::memory_order_relaxed);
    if (cur == handle) return e;
    if (cur == 0 && e.handle.compare_exchange_strong(cur, handle, std::memory_order_relaxed)) return e;
    if (cur == handle) return e;  // another thread claimed it for the same handle
  }
  // Every probe taken by other handles: the home entry starts over for this one (its history is lost).
  Signal& e = g_signals[home];
  e.handle.store(handle, std::memory_order_relaxed);
  e.sets.store(0, std::memory_order_relaxed);
  e.resets.store(0, std::memory_order_relaxed);
  e.last_set_ns.store(0, std::memory_order_relaxed);
  e.last_reset_ns.store(0, std::memory_order_relaxed);
  return e;
}

}  // namespace

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void BeginSlow(uint32_t handle, uint32_t object_type, uint32_t count, uint32_t kind, int64_t timeout_ms) {
  Slot* s = MySlot();
  if (s == nullptr) return;
  const uint32_t q = s->seq.load(std::memory_order_relaxed);
  s->seq.store(q + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s->handle.store(handle, std::memory_order_relaxed);
  s->object_type.store(object_type, std::memory_order_relaxed);
  s->count.store(count, std::memory_order_relaxed);
  s->kind.store(kind, std::memory_order_relaxed);
  s->timeout_ms.store(timeout_ms, std::memory_order_relaxed);
  uint32_t guest[5] = {};
  WaitGuestCallers(guest);  // port 167
  for (int i = 0; i < 5; ++i)
    s->guest[i].store(guest[i], std::memory_order_relaxed);
  const uint64_t now = NowNs();
  s->since_ns.store(now != 0 ? now : 1, std::memory_order_relaxed);
  s->seq.store(q + 2, std::memory_order_release);
}

void EndSlow() {
  if (t_slot.index < 0) return;
  Slot& s = g_slots[t_slot.index];
  if (s.since_ns.load(std::memory_order_relaxed) == 0) return;
  const uint32_t q = s.seq.load(std::memory_order_relaxed);
  s.seq.store(q + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.since_ns.store(0, std::memory_order_relaxed);
  s.seq.store(q + 2, std::memory_order_release);
}

void NoteSignalSlow(uint32_t handle, uint32_t kind) {
  Signal& e = SignalOf(handle);
  const uint64_t now = NowNs();
  if (kind == kReset) {
    e.last_reset_ns.store(now, std::memory_order_relaxed);
    e.resets.fetch_add(1, std::memory_order_relaxed);
  } else {
    e.last_set_ns.store(now, std::memory_order_relaxed);
    e.sets.fetch_add(1, std::memory_order_relaxed);
  }
}

void NoteFileReadSlow(uint64_t bytes) {
  g_file_reads.fetch_add(1, std::memory_order_relaxed);
  g_file_read_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

size_t CopyWaits(WaitView* out, size_t max) {
  size_t n = 0;
  for (size_t i = 0; i < kSlots && n < max; i++) {
    Slot& s = g_slots[i];
    if (s.owner.load(std::memory_order_acquire) == 0) continue;
    for (int attempt = 0; attempt < 3; attempt++) {
      const uint32_t q1 = s.seq.load(std::memory_order_acquire);
      if (q1 & 1) continue;
      WaitView v;
      v.host_thread = s.host_thread.load(std::memory_order_relaxed);
      v.handle = s.handle.load(std::memory_order_relaxed);
      v.object_type = s.object_type.load(std::memory_order_relaxed);
      v.count = s.count.load(std::memory_order_relaxed);
      v.kind = s.kind.load(std::memory_order_relaxed);
      v.timeout_ms = s.timeout_ms.load(std::memory_order_relaxed);
      v.since_ns = s.since_ns.load(std::memory_order_relaxed);
      for (int k = 0; k < 5; ++k)
        v.guest[k] = s.guest[k].load(std::memory_order_relaxed);  // port 167
      std::atomic_thread_fence(std::memory_order_acquire);
      if (s.seq.load(std::memory_order_relaxed) != q1) continue;
      if (v.since_ns != 0) out[n++] = v;
      break;
    }
  }
  return n;
}

SignalView FindSignal(uint32_t handle) {
  SignalView v;
  if (handle == 0) return v;
  const size_t home = size_t((handle * 0x9E3779B1u) >> 20) & (kSignals - 1);
  for (int i = 0; i < kProbes; i++) {
    Signal& e = g_signals[(home + size_t(i)) & (kSignals - 1)];
    if (e.handle.load(std::memory_order_relaxed) != handle) continue;
    v.seen = true;
    v.sets = e.sets.load(std::memory_order_relaxed);
    v.resets = e.resets.load(std::memory_order_relaxed);
    v.last_set_ns = e.last_set_ns.load(std::memory_order_relaxed);
    v.last_reset_ns = e.last_reset_ns.load(std::memory_order_relaxed);
    break;
  }
  return v;
}

uint64_t FileReads() { return g_file_reads.load(std::memory_order_relaxed); }
uint64_t FileReadBytes() { return g_file_read_bytes.load(std::memory_order_relaxed); }

}  // namespace rex::system::waitreg
