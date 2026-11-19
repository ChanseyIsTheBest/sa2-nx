/**
 * sasrt-nx (2026-11-10, thread_event_address_wait; the user: "reduce latency between threads ... less time waiting").
 *
 * The blocking part of an event (PosixCondition<Event>, threading_posix.cpp) as an address wait instead of the
 * mutex's condition variable. On Horizon a condition variable's wake gives the woken thread the mutex at once
 * (svcSignalProcessWideKey takes the mutex tag for it) although the thread may not run for a while: with cores 0-2
 * busy a woken guest thread (priority 0x3B) waits for its core. A thread setting the same event meanwhile blocks on
 * the mutex behind a thread that does not run: the handheld race (run P1) had the game's main thread in
 * pthread_mutex_lock at PosixEvent::Set+0x20 (the event's own mutex) for 5.5 % of its samples, ~2.3 ms a frame.
 * Here a blocked thread sleeps on a 32-bit word (svcWaitForAddress, "wait if equal") and takes the event's mutex only
 * once it runs again; the setter changes the word under the mutex and wakes the address after releasing it.
 *
 * The 360's semantics, which the condition variable kept only because the second Set waited for the first waiter:
 * a Set of an auto-reset event that finds a thread blocked releases one blocked thread (a grant) and leaves the event
 * unsignalled; a manual-reset Set releases every thread blocked at that moment (so a Reset right after, or Pulse,
 * cannot strand them) and leaves it signalled. A blocked thread that runs again takes a grant first, then the signal,
 * then an interrupt (an alertable wait's queued callback), then its timeout. blocked and grants are guarded by the
 * owner's mutex; the word (gen) changes only under it, so a thread that read it there and then sleeps "while equal"
 * cannot miss a change. Blocked threads re-check every safety_ns even unwoken (a net; a re-check that finds itself
 * released without a wake is counted).
 *
 * Header only, templated on the arbiter (Horizon's svcWaitForAddress / svcSignalToAddress on the console; the host
 * test tools/test_address_event.cpp runs the same code with WaitOnAddress).
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::thread::detail {

enum class AddressWaitResult { kSignaled, kGranted, kInterrupted, kTimeout };

template <typename Arbiter>
class AddressEventCore {
 public:
  // Under the owner's mutex: a Set. Returns the threads to wake once the mutex is released (0, 1, or -1 for all).
  int32_t SetLocked(bool manual_reset, bool& signal) {
    int32_t wake = 0;
    if (manual_reset) {
      signal = true;
      if (blocked_ > grants_) {
        grants_ = blocked_;
        wake = -1;
      }
    } else if (blocked_ > grants_) {
      ++grants_;  // released directly: the event stays unsignalled
      wake = 1;
    } else {
      signal = true;
    }
    if (wake) gen_.fetch_add(1, std::memory_order_relaxed);
    return wake;
  }

  // Under the owner's mutex: an alertable waiter's callback was queued. Every blocked thread re-checks.
  int32_t InterruptLocked() {
    if (!blocked_) return 0;
    gen_.fetch_add(1, std::memory_order_relaxed);
    return -1;
  }

  void Wake(int32_t count) { Arbiter::Wake(&gen_, count); }

  // With the owner's mutex held (lock), the event found unsignalled, the interrupt not raised and the timeout not
  // over: blocks until released. Returns with the lock held again. kSignaled: the caller consumes the signal (an
  // auto-reset event's post_execution); kGranted: nothing to consume.
  template <typename Lock, typename Clock = std::chrono::steady_clock>
  AddressWaitResult Block(Lock& lock, const bool& signal, const std::atomic<bool>* interrupt, bool infinite,
                          typename Clock::time_point deadline, int64_t safety_ns) {
    ++blocked_;
    for (;;) {
      const uint32_t seen = gen_.load(std::memory_order_relaxed);
      lock.unlock();
      int64_t ns = safety_ns;
      if (!infinite) {
        const auto now = Clock::now();
        const int64_t left =
            now >= deadline ? 0 : std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
        if (left < ns) ns = left;
      }
      const bool woken = Arbiter::WaitIfEqual(&gen_, seen, ns);
      lock.lock();
      if (grants_) {
        --grants_;
        --blocked_;
        if (!woken && gen_.load(std::memory_order_relaxed) == seen) NoteSafety();
        return AddressWaitResult::kGranted;
      }
      if (signal) {
        --blocked_;
        if (!woken && gen_.load(std::memory_order_relaxed) == seen) NoteSafety();
        return AddressWaitResult::kSignaled;
      }
      if (interrupt && interrupt->load(std::memory_order_acquire)) {
        --blocked_;
        return AddressWaitResult::kInterrupted;
      }
      if (!infinite && Clock::now() >= deadline) {
        --blocked_;
        return AddressWaitResult::kTimeout;
      }
    }
  }

  // For tests and the profiler: under the owner's mutex.
  uint32_t blocked_locked() const { return blocked_; }
  uint32_t grants_locked() const { return grants_; }
  static uint64_t safety_releases() { return safety_releases_.load(std::memory_order_relaxed); }

 private:
  static void NoteSafety() { safety_releases_.fetch_add(1, std::memory_order_relaxed); }

  std::atomic<uint32_t> gen_{0};  // the address the blocked threads sleep on; changed under the owner's mutex
  uint32_t blocked_ = 0;          // threads inside Block
  uint32_t grants_ = 0;           // of those, released by a Set and not yet run (always <= blocked_)
  static inline std::atomic<uint64_t> safety_releases_{0};
};

}  // namespace rex::thread::detail
