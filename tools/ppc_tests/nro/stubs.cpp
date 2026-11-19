// The few runtime functions the generated test code calls (mtmsrd / mfmsr: the global lock; mftb: the guest clock).
#include <rex/thread/mutex.h>
#include <rex/chrono/clock.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace rex::thread {
// Single-threaded tests: a lock count, so that a test's mtmsrd without the matching one (they test the register, not
// the lock) does not unlock what is not held.
static thread_local int g_held = 0;
void GlobalCriticalMutex::lock() { ++g_held; }
void GlobalCriticalMutex::unlock() { if (g_held) --g_held; }
bool GlobalCriticalMutex::try_lock() { ++g_held; return true; }
GlobalCriticalMutex& global_critical_region::mutex() {
  static GlobalCriticalMutex m;
  return m;
}
}  // namespace rex::thread

namespace rex::chrono {
uint64_t Clock::QueryGuestTickCount() {
  using namespace std::chrono;
  return uint64_t(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count()) / 20;  // 50 MHz
}
}  // namespace rex::chrono

extern "C" void __assert_func(const char* file, int line, const char* fn, const char* expr) {
  std::fprintf(stderr, "assert %s:%d %s: %s\n", file, line, fn, expr);
  std::abort();
}

// Logging (ppc_trap's messages): no logger, so nothing is logged.
#include <rex/logging/api.h>
namespace rex {
LogCategoryId RegisterLogCategory(const char*) { return LogCategoryId(0); }
spdlog::logger* GetLoggerRaw(LogCategoryId) { return nullptr; }
}  // namespace rex

#if defined(__linux__) && defined(__aarch64__)
// -mtp=soft (the Switch flags): the thread pointer through a call; libnx has it on the console.
extern "C" __attribute__((naked)) void* __aarch64_read_tp() {
  __asm__("mrs x0, tpidr_el0\n\tret");
}
#endif

// renut-ultimate (port 147): the spin-wait hints with spin_hint_yield on (rex/ppc/context.h): nothing to yield to here.
extern "C" void rex_spin_hint(void) {}
