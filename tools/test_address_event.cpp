// sasrt-nx (2026-11-10, thread_event_address_wait): host stress test of sdk/src/core/address_event_core.h, the
// events' address wait on the Switch, with Windows' WaitOnAddress standing in for svcWaitForAddress. The Event below
// mirrors PosixCondition<Event> in threading_posix.cpp (its Wait, Signal, Reset, InterruptWaiters with the key on).
//
//   C:\devkitPro\msys2\clang64\bin\clang++.exe -O2 -std=c++20 tools/test_address_event.cpp -o out/test_address_event.exe -lsynchronization
//
// Tests: ping-pong (lost wake-ups), K blocked waiters released by K back-to-back Sets (the 360's one release a Set),
// a manual-reset Set then Reset at once (every blocked thread released), interrupts (woken, not the 100 ms net), and
// a random mix of Sets, Resets, timeouts and interrupts with the conservation check (successes <= Sets) and the
// counters back at 0. Exit code 0 when every test passed.
#include <windows.h>
#include <synchapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "../sdk/src/core/address_event_core.h"

using namespace std::chrono;
using rex::thread::detail::AddressEventCore;
using rex::thread::detail::AddressWaitResult;

struct WinArbiter {
  static bool WaitIfEqual(std::atomic<uint32_t>* word, uint32_t value, int64_t ns) {
    DWORD ms = ns < 0 ? INFINITE : DWORD((ns + 999999) / 1000000);
    uint32_t v = value;
    if (WaitOnAddress(reinterpret_cast<volatile void*>(word), &v, sizeof(v), ms)) return true;
    return GetLastError() != ERROR_TIMEOUT;
  }
  static void Wake(std::atomic<uint32_t>* word, int32_t count) {
    if (count == 1)
      WakeByAddressSingle(reinterpret_cast<void*>(word));
    else
      WakeByAddressAll(reinterpret_cast<void*>(word));
  }
};

enum class R { kSuccess, kTimeout };

class Event {
 public:
  explicit Event(bool manual) : manual_(manual) {}
  void Set() {
    int32_t wake;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      wake = core_.SetLocked(manual_, signal_);
      Check();
    }
    if (wake) core_.Wake(wake);
  }
  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_ = false;
  }
  void Interrupt() {
    int32_t wake;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      wake = core_.InterruptLocked();
    }
    if (wake) core_.Wake(wake);
  }
  // timeout_ms < 0: infinite
  R Wait(int timeout_ms, const std::atomic<bool>* interrupt = nullptr) {
    const bool infinite = timeout_ms < 0;
    const auto deadline = infinite ? steady_clock::time_point::max() : steady_clock::now() + milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mutex_);
    if (signal_) {
      if (!manual_) signal_ = false;
      return R::kSuccess;
    }
    if (interrupt && interrupt->load(std::memory_order_acquire)) return R::kTimeout;
    if (!infinite && steady_clock::now() >= deadline) return R::kTimeout;
    const auto r = core_.Block(lock, signal_, interrupt, infinite, deadline, 100'000'000);
    Check();
    if (r == AddressWaitResult::kSignaled) {
      if (!manual_) signal_ = false;
      return R::kSuccess;
    }
    return r == AddressWaitResult::kGranted ? R::kSuccess : R::kTimeout;
  }
  uint32_t blocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.blocked_locked();
  }
  uint32_t grants() {
    std::lock_guard<std::mutex> lock(mutex_);
    return core_.grants_locked();
  }
  static std::atomic<int> violations;

 private:
  void Check() {
    if (core_.grants_locked() > core_.blocked_locked()) violations.fetch_add(1);
  }
  std::mutex mutex_;
  bool signal_ = false;
  const bool manual_;
  AddressEventCore<WinArbiter> core_;
};
std::atomic<int> Event::violations{0};

static bool WaitUntil(const std::atomic<bool>& done, int seconds) {
  const auto end = steady_clock::now() + std::chrono::seconds(seconds);
  while (!done.load()) {
    if (steady_clock::now() > end) return false;
    std::this_thread::sleep_for(milliseconds(5));
  }
  return true;
}

static bool PingPong() {
  Event e1(false), e2(false);
  constexpr int N = 200000;
  std::atomic<bool> done{false};
  std::thread b([&] {
    for (int i = 0; i < N; ++i) {
      e1.Wait(-1);
      e2.Set();
    }
  });
  std::thread a([&] {
    for (int i = 0; i < N; ++i) {
      e1.Set();
      e2.Wait(-1);
    }
    done = true;
  });
  const bool ok = WaitUntil(done, 120);
  if (!ok) {
    std::printf("ping-pong: HUNG\n");
    std::_Exit(2);
  }
  a.join();
  b.join();
  std::printf("ping-pong: %d round trips, ok\n", N);
  return true;
}

static bool MultiRelease(bool manual) {
  constexpr int K = 4, Rounds = 20000;
  Event job(manual);
  std::atomic<int> released{0}, round{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> waiters;
  for (int k = 0; k < K; ++k)
    waiters.emplace_back([&] {
      int seen = 0;
      while (true) {
        job.Wait(-1);
        if (stop.load()) return;
        released.fetch_add(1);
        while (round.load() == seen && !stop.load()) std::this_thread::yield();  // one release a round each
        seen = round.load();
      }
    });
  bool ok = true;
  for (int r = 0; r < Rounds && ok; ++r) {
    const auto end = steady_clock::now() + seconds(5);
    while (job.blocked() != K) {
      if (steady_clock::now() > end) {
        std::printf("%s: round %d: only %u of %d blocked\n", manual ? "manual Set+Reset" : "auto K Sets", r,
                    job.blocked(), K);
        ok = false;
        break;
      }
      std::this_thread::yield();
    }
    if (!ok) break;
    if (manual) {
      job.Set();
      job.Reset();  // at once: every thread blocked at the Set is released anyway
    } else {
      for (int k = 0; k < K; ++k) job.Set();  // back to back: one release each (none stranded)
    }
    while (released.load() != (r + 1) * K) {
      if (steady_clock::now() > end) {
        std::printf("%s: round %d: %d released of %d (stranded)\n", manual ? "manual Set+Reset" : "auto K Sets", r,
                    released.load() - r * K, K);
        ok = false;
        break;
      }
      std::this_thread::yield();
    }
    round.fetch_add(1);
  }
  stop = true;
  for (int k = 0; k < K * 4; ++k) job.Set();
  for (auto& t : waiters) t.join();
  std::printf("%s: %d rounds of %d waiters, released %d, %s\n", manual ? "manual Set+Reset" : "auto K Sets", Rounds,
              K, released.load(), ok ? "ok" : "FAILED");
  return ok;
}

static bool Interrupts() {
  Event e(false);
  std::atomic<bool> flag{false};
  int slow = 0;
  double worst = 0;
  for (int i = 0; i < 1000; ++i) {
    flag = false;
    std::atomic<bool> back{false};
    steady_clock::time_point raised;
    std::thread w([&] {
      const R r = e.Wait(-1, &flag);
      if (r != R::kTimeout) std::printf("interrupt: woke with success?\n");
      back = true;
    });
    while (e.blocked() != 1) std::this_thread::yield();
    raised = steady_clock::now();
    flag = true;
    e.Interrupt();
    while (!back.load()) std::this_thread::yield();
    const double ms = duration<double, std::milli>(steady_clock::now() - raised).count();
    worst = ms > worst ? ms : worst;
    if (ms > 50) ++slow;
    w.join();
  }
  std::printf("interrupts: 1000, worst %.2f ms, %d over 50 ms (the 100 ms net): %s\n", worst, slow, slow ? "FAILED" : "ok");
  return slow == 0;
}

static bool RandomMix(bool manual, int C, int P, bool sleeps) {
  Event e(manual);
  std::atomic<long long> sets{0}, successes{0}, timeouts{0}, interrupts{0};
  std::atomic<bool> stop_producers{false}, stop{false};
  std::vector<std::atomic<bool>> flags(C);
  for (auto& f : flags) f = false;
  std::vector<std::thread> threads;
  for (int c = 0; c < C; ++c)
    threads.emplace_back([&, c] {
      std::mt19937 rng(c * 7919 + 1);
      while (!stop.load()) {
        const int pick = rng() % 4;
        const int t = pick == 0 ? -1 : pick == 1 ? 0 : int(rng() % 3);
        const R r = e.Wait(t, &flags[c]);
        if (r == R::kSuccess)
          successes.fetch_add(1);
        else if (flags[c].exchange(false))
          interrupts.fetch_add(1);
        else
          timeouts.fetch_add(1);
      }
    });
  for (int p = 0; p < P; ++p)
    threads.emplace_back([&, p] {
      std::mt19937 rng(p * 104729 + 3);
      while (!stop_producers.load()) {
        const int a = rng() % 100;
        if (a < 70) {
          e.Set();
          sets.fetch_add(1);
        } else if (a < 80) {
          e.Reset();
        } else if (a < 85) {
          flags[rng() % C] = true;
          e.Interrupt();
        } else if (sleeps) {
          std::this_thread::sleep_for(microseconds(rng() % 200));
        }
        if (rng() % 8 == 0) std::this_thread::yield();
      }
    });
  std::this_thread::sleep_for(seconds(10));
  stop_producers = true;
  for (int p = 0; p < P; ++p) threads[C + p].join();
  stop = true;
  const auto end = steady_clock::now() + seconds(5);
  long long extra = 0;
  while (e.blocked() != 0 && steady_clock::now() < end) {
    e.Set();  // lets the consumers out
    ++extra;
    std::this_thread::sleep_for(milliseconds(1));
  }
  for (int c = 0; c < C; ++c) threads[c].join();
  const bool counters = e.blocked() == 0 && e.grants() == 0;
  const bool conserve = manual || successes.load() <= sets.load() + extra;
  std::printf("random mix (%s, %d consumers, %d producers%s): %lld Sets (+%lld to drain), %lld successes, %lld timeouts, %lld interrupts; counters %s, "
              "successes <= Sets %s\n",
              manual ? "manual" : "auto", C, P, sleeps ? ", sleeps" : "", sets.load(), extra, successes.load(), timeouts.load(), interrupts.load(), counters ? "0" : "NOT 0",
              conserve ? "yes" : "NO");
  return counters && conserve;
}

int main() {
  bool ok = true;
  ok &= PingPong();
  ok &= MultiRelease(false);
  ok &= MultiRelease(true);
  ok &= Interrupts();
  ok &= RandomMix(false, 6, 3, true);
  ok &= RandomMix(false, 16, 4, false);
  ok &= RandomMix(true, 16, 4, false);
  const uint64_t safety = AddressEventCore<WinArbiter>::safety_releases();
  std::printf("grants > blocked seen: %d; released at a safety re-check without a wake: %llu\n",
              Event::violations.load(), (unsigned long long)safety);
  ok &= Event::violations.load() == 0;
  std::printf("%s\n", ok ? "ALL OK" : "FAILED");
  return ok ? 0 : 1;
}
