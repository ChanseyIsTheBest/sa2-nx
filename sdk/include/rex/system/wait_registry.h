#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// blur-nx (2026-11-14): what the Switch stall watchdog (app/src/profiler/stall_watch_switch.cpp) prints about guest
// waits, for the boot black screen whose frames keep coming while the loader threads wait on events that never
// wake them (a lost wakeup or a missing set: the dumps tell which).
//
// Every blocking guest wait (XObject::Wait / WaitMultiple / SignalAndWait with a timeout other than 0) marks its
// thread's slot with the object's handle, type and the time it began; the slot is cleared when the wait returns.
// Every set, pulse and reset of an event and every release of a semaphore or mutant is noted against its handle
// (the last time and a count). So a dump can say, for each blocked thread, what it waits on, for how long, and whether
// that object was set after the wait began (the thread missed it, or another waiter took it) or not since (nobody set
// it). NtReadFile's calls are counted too: the watchdog's "quiet" trigger dumps when no file has been read for a while.
//
// Off (one relaxed load per call) until the watchdog turns it on (g_on, g_reads_on). Diagnostics only: what a dump reads may be a moment
// stale or, for a reused handle, another object's; nothing here changes what the game does.
namespace rex::system::waitreg {

extern std::atomic<bool> g_on;        // the waits and the signals
extern std::atomic<bool> g_reads_on;  // the file reads (the quiet trigger needs only these)

enum WaitKind : uint32_t { kWaitOne = 1, kWaitAny = 2, kWaitAll = 3, kSignalAndWait = 4 };
enum SignalKind : uint32_t { kSet = 1, kPulse = 2, kReset = 3, kRelease = 4 };

void BeginSlow(uint32_t handle, uint32_t object_type, uint32_t count, uint32_t kind, int64_t timeout_ms);
void EndSlow();
void NoteSignalSlow(uint32_t handle, uint32_t kind);
void NoteFileReadSlow(uint64_t bytes);

// timeout_ms < 0: none (an untimed wait). Waits with a timeout of 0 (polls) are not marked.
inline void Begin(uint32_t handle, uint32_t object_type, uint32_t count, uint32_t kind, int64_t timeout_ms) {
  if (g_on.load(std::memory_order_relaxed) && timeout_ms != 0)
    BeginSlow(handle, object_type, count, kind, timeout_ms);
}
inline void End() {
  if (g_on.load(std::memory_order_relaxed)) EndSlow();
}
inline void NoteSignal(uint32_t handle, uint32_t kind) {
  if (g_on.load(std::memory_order_relaxed) && handle != 0) NoteSignalSlow(handle, kind);
}
// An object's set or release, by its first handle (none: not noted).
template <typename Object>
inline void NoteSignalOf(Object* object, uint32_t kind) {
  if (g_on.load(std::memory_order_relaxed)) {
    auto& handles = object->handles();
    if (!handles.empty()) NoteSignalSlow(uint32_t(handles[0]), kind);
  }
}
inline void NoteFileRead(uint64_t bytes) {
  if (g_reads_on.load(std::memory_order_relaxed)) NoteFileReadSlow(bytes);
}

// A monotonic clock in nanoseconds (the times below are on it).
uint64_t NowNs();

// Port 167 (from fnxf_xiii, its notes of 2026-10-22 and 10-25): the waiting guest thread's LR (the return address of
// the wait's caller) and the four return addresses above it on its guest stack's back chain, bounded by its KTHREAD
// stack (xobject.cpp). The wait's caller is often a wrapper (WaitForSingleObject): the callers above it say who waits.
void WaitGuestCallers(uint32_t out[5]);

struct WaitView {
  uint32_t host_thread;  // the host thread's handle (Switch: libnx's; 0 elsewhere)
  uint32_t handle;       // the (first) object's handle
  uint32_t object_type;  // XObject::Type
  uint32_t count;        // objects waited on
  uint32_t kind;         // WaitKind
  int64_t timeout_ms;    // < 0: none
  uint64_t since_ns;
  uint32_t guest[5];     // port 167: the guest LR and four callers above it (0: unknown)
};
// The waits blocked right now (at most max), in slot order.
size_t CopyWaits(WaitView* out, size_t max);

struct SignalView {
  bool seen = false;
  uint64_t last_set_ns = 0;    // a set, pulse or release
  uint64_t last_reset_ns = 0;
  uint32_t sets = 0;
  uint32_t resets = 0;
};
SignalView FindSignal(uint32_t handle);

uint64_t FileReads();
uint64_t FileReadBytes();

}  // namespace rex::system::waitreg
