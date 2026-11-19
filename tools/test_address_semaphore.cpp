// blur-nx (2026-11-17, the Renut audit's #10, thread_semaphore_address_wait): host stress test of the semaphore's
// address wait (PosixCondition<Semaphore> in sdk/src/core/threading_posix.cpp with the key on), on
// sdk/src/core/address_event_core.h, with Windows' WaitOnAddress standing in for svcWaitForAddress. The Semaphore
// below mirrors that class's Release, Wait and post_execution.
//
//   C:\devkitPro\msys2\clang64\bin\clang++.exe -O2 -std=c++20 tools/test_address_semaphore.cpp -o out/test_address_semaphore.exe -lsynchronization
//
// Tests: ping-pong (lost wake-ups), K blocked waiters released by one Release(K), many producers and consumers with
// conservation (units acquired == units released, every consumer's last wait ends), timeouts mixed in, and the
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

class Semaphore {
 public:
  Semaphore(uint32_t initial, uint32_t maximum) : count_(initial), maximum_(maximum), has_count_(initial > 0) {}

  bool Release(uint32_t n) {
    int32_t wake = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (n > maximum_ - count_) return false;
      for (uint32_t i = 0; i < n; ++i) {
        bool to_count = false;
        wake += address_.SetLocked(false, to_count);
        if (to_count) ++count_;
      }
      has_count_ = count_ > 0;
    }
    if (wake) address_.Wake(wake);
    return true;
  }

  // true: a unit taken.
  bool Wait(milliseconds timeout) {
    const bool infinite = timeout == milliseconds::max();
    const auto deadline = infinite ? steady_clock::time_point::max() : steady_clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    if (count_ > 0) {
      Take();
      return true;
    }
    if (!infinite && steady_clock::now() >= deadline) return false;
    switch (address_.Block(lock, has_count_, nullptr, infinite, deadline, 100'000'000)) {
      case AddressWaitResult::kSignaled:
        Take();
        return true;
      case AddressWaitResult::kGranted:
        return true;
      default:
        return false;
    }
  }

  uint32_t count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }
  uint32_t blocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    return address_.blocked_locked();
  }
  uint32_t grants() {
    std::lock_guard<std::mutex> lock(mutex_);
    return address_.grants_locked();
  }

 private:
  void Take() {
    --count_;
    has_count_ = count_ > 0;
  }
  std::mutex mutex_;
  AddressEventCore<WinArbiter> address_;
  uint32_t count_;
  const uint32_t maximum_;
  bool has_count_;
};

int g_failures = 0;
void Check(bool ok, const char* what) {
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Two threads hand one unit back and forth through two semaphores: a lost wake-up hangs (caught by the timeout).
void PingPong() {
  Semaphore a(0, 1), b(0, 1);
  constexpr int kRounds = 200000;
  std::atomic<bool> stuck{false};
  std::thread t([&] {
    for (int i = 0; i < kRounds; ++i) {
      if (!a.Wait(milliseconds(5000))) {
        stuck = true;
        return;
      }
      b.Release(1);
    }
  });
  for (int i = 0; i < kRounds && !stuck; ++i) {
    a.Release(1);
    if (!b.Wait(milliseconds(5000))) {
      stuck = true;
      break;
    }
  }
  t.join();
  Check(!stuck && a.count() == 0 && b.count() == 0 && a.blocked() == 0 && b.blocked() == 0, "ping-pong 200,000 rounds");
}

// K threads blocked, one Release(K): every one wakes with a unit, the count stays 0.
void ReleaseMany() {
  constexpr int kWaiters = 8;
  Semaphore s(0, 1000);
  std::atomic<int> got{0};
  std::vector<std::thread> ts;
  for (int i = 0; i < kWaiters; ++i)
    ts.emplace_back([&] {
      if (s.Wait(milliseconds(5000))) ++got;
    });
  while (s.blocked() != uint32_t(kWaiters)) std::this_thread::sleep_for(milliseconds(1));
  s.Release(kWaiters);
  for (auto& t : ts) t.join();
  Check(got == kWaiters && s.count() == 0 && s.blocked() == 0 && s.grants() == 0, "8 blocked, one Release(8)");
}

// Producers release random amounts, consumers take with short timeouts; every unit released is taken exactly once.
void Mixed() {
  constexpr int kProducers = 4, kConsumers = 6, kPerProducer = 50000;
  Semaphore s(0, 1u << 30);
  std::atomic<int64_t> released{0}, taken{0};
  std::atomic<bool> done{false};
  std::vector<std::thread> ts;
  for (int p = 0; p < kProducers; ++p)
    ts.emplace_back([&, p] {
      std::mt19937 rng(p + 1);
      int left = kPerProducer;
      while (left > 0) {
        const int n = std::min(left, int(rng() % 4) + 1);
        if (s.Release(uint32_t(n))) {
          released += n;
          left -= n;
        }
        if (rng() % 16 == 0) std::this_thread::yield();
      }
    });
  for (int c = 0; c < kConsumers; ++c)
    ts.emplace_back([&, c] {
      std::mt19937 rng(100 + c);
      while (!done.load() || s.count() > 0) {
        if (s.Wait(milliseconds(rng() % 3))) ++taken;
      }
    });
  for (int p = 0; p < kProducers; ++p) ts[p].join();
  // Every unit released: wait until the consumers have taken them all, then stop them.
  const auto until = steady_clock::now() + seconds(20);
  while (taken.load() < released.load() && steady_clock::now() < until) std::this_thread::sleep_for(milliseconds(1));
  done = true;
  for (size_t i = kProducers; i < ts.size(); ++i) ts[i].join();
  Check(taken.load() == released.load() && released.load() == int64_t(kProducers) * kPerProducer && s.count() == 0 &&
            s.blocked() == 0 && s.grants() == 0,
        "4 producers, 6 consumers with timeouts: every unit taken once");
}

int main() {
  PingPong();
  ReleaseMany();
  Mixed();
  std::printf("%s (%d failed)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
  return g_failures ? 1 : 0;
}
