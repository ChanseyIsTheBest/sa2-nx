/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/platform.h>
#include <rex/sys_counters.h>

#if !defined(__SWITCH__)
// The runtime census (sys_counters.h): off the Switch, what REX_SYS_COUNT / REX_SYS_CENSUS count into.
namespace rex::syscount {
static std::atomic<uint64_t> g_census[kCensusCount]{};

namespace {
struct ExportEntry {
  const char* name;
  std::atomic<uint64_t> count{0};
};
std::mutex g_export_mutex;
std::deque<ExportEntry>& ExportEntries() {
  static std::deque<ExportEntry> entries;  // a deque: the counters never move
  return entries;
}
}  // namespace

std::atomic<uint64_t>* ExportCounter(const char* name) {
  std::lock_guard<std::mutex> lock(g_export_mutex);
  ExportEntry& entry = ExportEntries().emplace_back();
  entry.name = name;
  return &entry.count;
}

size_t ExportCensus(const char** names, uint64_t* counts, size_t capacity) {
  std::vector<std::pair<uint64_t, const char*>> all;
  {
    std::lock_guard<std::mutex> lock(g_export_mutex);
    for (const ExportEntry& entry : ExportEntries()) {
      const uint64_t n = entry.count.load(std::memory_order_relaxed);
      if (n) all.emplace_back(n, entry.name);
    }
  }
  std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  const size_t n = std::min(capacity, all.size());
  for (size_t i = 0; i < n; ++i) {
    names[i] = all[i].second;
    counts[i] = all[i].first;
  }
  return n;
}
void Census(unsigned id, uint64_t n) {
  if (id < kCensusCount)
    g_census[id].store(g_census[id].load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
uint64_t CensusRead(unsigned id) {
  return id < kCensusCount ? g_census[id].load(std::memory_order_relaxed) : 0;
}

namespace {
struct Site {
  std::atomic<uint64_t> address{0}, count{0};
};
constexpr size_t kSites = size_t(1) << 16;
Site g_sites[kSites];
std::atomic<bool> g_sites_on{false};
}  // namespace

void CensusSitesEnable(bool on) {
  g_sites_on.store(on, std::memory_order_relaxed);
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
void CensusSite(unsigned id) {
  Census(id, 1);
  if (!g_sites_on.load(std::memory_order_relaxed)) return;
#if defined(__clang__) || defined(__GNUC__)
  const uint64_t a = reinterpret_cast<uint64_t>(__builtin_return_address(0));
#else
  const uint64_t a = 1;
#endif
  size_t i = size_t((a * 0x9E3779B97F4A7C15ull) >> 48);
  for (size_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSites - 1)) {
    uint64_t have = g_sites[i].address.load(std::memory_order_relaxed);
    if (have == 0 && g_sites[i].address.compare_exchange_strong(have, a, std::memory_order_relaxed)) have = a;
    if (have == a) {
      g_sites[i].count.store(g_sites[i].count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      return;
    }
  }
}

size_t CensusSites(uint64_t* addresses, uint64_t* counts, size_t capacity) {
  size_t n = 0;
  for (size_t i = 0; i < kSites; ++i) {
    const uint64_t c = g_sites[i].count.load(std::memory_order_relaxed);
    if (!c) continue;
    const uint64_t a = g_sites[i].address.load(std::memory_order_relaxed);
    // Insertion into the list sorted by count, largest first.
    size_t at = n < capacity ? n : capacity;
    while (at > 0 && counts[at - 1] < c) --at;
    if (at >= capacity) continue;
    const size_t last = n < capacity ? n : capacity - 1;
    for (size_t k = last; k > at; --k) {
      addresses[k] = addresses[k - 1];
      counts[k] = counts[k - 1];
    }
    addresses[at] = a;
    counts[at] = c;
    if (n < capacity) ++n;
  }
  return n;
}

// blur-nx (2026-11-12, CG-13's census): the denormal sites, a table of their own (the same open addressing).
namespace {
constexpr size_t kDenormalSites = size_t(1) << 14;
Site g_denormal_sites[kDenormalSites];

void CountDenormalSite(uint64_t a) {
  size_t i = size_t((a * 0x9E3779B97F4A7C15ull) >> 50);
  for (size_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kDenormalSites - 1)) {
    uint64_t have = g_denormal_sites[i].address.load(std::memory_order_relaxed);
    if (have == 0 && g_denormal_sites[i].address.compare_exchange_strong(have, a, std::memory_order_relaxed)) have = a;
    if (have == a) {
      g_denormal_sites[i].count.store(g_denormal_sites[i].count.load(std::memory_order_relaxed) + 1,
                                      std::memory_order_relaxed);
      return;
    }
  }
}
}  // namespace

void CensusDenormalSite(uint64_t address) {
  CountDenormalSite(address);
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
void CensusFpuRegionEnd(uint32_t csr) {
  Census(kCensusFpuRegions, 1);
  // x86 MXCSR: DE (bit 1) a denormal operand, UE (bit 4) an underflow. Other hosts: nothing to read here.
#if defined(__x86_64__) || defined(_M_X64)
  if (csr & 0x12u) {
    Census(kCensusFpuDenormal, 1);
#if defined(__clang__) || defined(__GNUC__)
    CountDenormalSite(reinterpret_cast<uint64_t>(__builtin_return_address(0)));
#endif
  }
#else
  (void)csr;
#endif
}

size_t CensusDenormalSites(uint64_t* addresses, uint64_t* counts, size_t capacity) {
  size_t n = 0;
  for (size_t i = 0; i < kDenormalSites; ++i) {
    const uint64_t c = g_denormal_sites[i].count.load(std::memory_order_relaxed);
    if (!c) continue;
    const uint64_t a = g_denormal_sites[i].address.load(std::memory_order_relaxed);
    size_t at = n < capacity ? n : capacity;
    while (at > 0 && counts[at - 1] < c) --at;
    if (at >= capacity) continue;
    const size_t last = n < capacity ? n : capacity - 1;
    for (size_t k = last; k > at; --k) {
      addresses[k] = addresses[k - 1];
      counts[k] = counts[k - 1];
    }
    addresses[at] = a;
    counts[at] = c;
    if (n < capacity) ++n;
  }
  return n;
}
}  // namespace rex::syscount
#endif

REXCVAR_DEFINE_BOOL(clock_no_scaling, false, "Clock",
                    "Disable clock scaling (inverted: false = scaling enabled)");

REXCVAR_DEFINE_BOOL(clock_source_raw, false, "Clock", "Use raw clock source without scaling");

REXCVAR_DEFINE_BOOL(clock_nonblocking, true, "Clock",
                    "UpdateGuestClock never blocks on the clock mutex: a caller that finds another thread in the "
                    "middle of an update returns that thread's published count (equivalent to waiting for it "
                    "and reading the same value, without the wait). false = the original blocking path");

// katamari-nx (2026-11-01): Beautiful Katamari's QueryPerformanceCounter (sub_824C0698, one mftb) runs about
// 300,000 times a second in a level, nearly all on the draw thread, and every call try-locked tick_mutex_, stored
// both counts and added to a counter all the cores share: UpdateGuestClock, its mutex and that counter were 8 % of
// the draw thread on the console (rc5). The lock-free count is a fixed base plus the scaled host delta.
REXCVAR_DEFINE_BOOL(clock_lock_free, true, "Clock",
                    "UpdateGuestClock without its mutex: guest ticks = base + (host ticks - base host ticks) * "
                    "ratio, from a base that moves only when the ratio changes (at start). The summed per-call "
                    "deltas of the locked path dropped each call's remainder, which made the guest clock run slow "
                    "by up to a tick every call (~0.3 % at 300,000 calls a second); this count does not drift. "
                    "false = the locked path");

// rr6-nx (2026-11-08; port 48): Ridge Racer 6's drawing thread reads the time base for its timers so often that the
// clock was about 5 % of that thread while racing on the console, and the lock-free path still made four calls a read:
// the host count, the clock_no_scaling accessor, the thread pointer for the per-thread count below, and the base
// through an acquire load of a heap object (most samples right after it). With these settled once
// (RecomputeGuestTickScalar, after the toml), a read is the counter, the base from its own cache line, a multiply and a
// divide by multiplication: the same count.
REXCVAR_DEFINE_BOOL(clock_count_calls, false, "Clock",
                    "Count UpdateGuestClock's calls for the runtime census (banjo_diag_runtime_census's clock line). "
                    "Off, a time-base read takes no thread pointer and no counter (the census shows 0 calls)");

// ds3-nx (2026-10-24, sdk-patches/0013; port 57): the guest never saw its console sleep. While the process is
// suspended (the Switch asleep or at HOME; a PC asleep) no thread reads the clock, so the first read after it carried
// the whole nap in one step, and a game takes that as one enormous frame (Dead Space 2's RenderWare Audio crashed
// within seconds of waking). Here a gap of more than 250 ms in which nothing read the clock is cut out of the guest's
// time. ds3-nx clamped each read's delta, with a shared write per read; this lock-free count (ports 46/48) keeps its
// reads free of shared writes: they compare the host count with a shared mark that moves at most every 5 ms. As in
// ds3-nx, the reads alone keep it fresh: a running game reads the clock far more often than every 250 ms (a stretch
// with no read at all, such as the start before the guest runs, is cut too, which only slows its time). A thread of
// its own to keep the mark fresh stopped the Switch build at start in Ryujinx (port 57's first try).
REXCVAR_DEFINE_BOOL(clock_nap_clamp, true, "Clock",
                    "A gap of more than 250 ms with no clock read at all (the console asleep or at HOME, the PC "
                    "asleep: the process suspended) is cut out of the guest's time, which goes on from where it was. "
                    "false: the guest sees the whole gap at once, as one huge frame");

namespace rex::chrono {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Native guest ticks. Atomic so that a contended reader can take it without the mutex.
std::atomic<uint64_t> last_guest_tick_count_{0};
// Last sampled host tick count.
uint64_t last_host_tick_count_ = Clock::QueryHostTickCount();
// Mutex to ensure last_host_tick_count_ and last_guest_tick_count_ are in sync
std::mutex tick_mutex_;

// The base of the lock-free count (clock_lock_free). RecomputeGuestTickScalar publishes a new one whole and never
// frees the old one, which a reader may still hold (two in all: set_guest_tick_frequency and
// set_guest_time_scalar at start).
struct TickBase {
  uint64_t host;
  uint64_t guest;
  uint64_t num;
  uint64_t den;
  uint64_t max_delta;  // the largest host delta whose product with num fits in 64 bits
  uint64_t recip = 0;  // rr6-nx (2026-11-08): floor(2^64 / den) for den >= 2 (DivideByDen); 0: divide
};
const TickBase initial_tick_base_{last_host_tick_count_, 0, 1, 1, std::numeric_limits<uint64_t>::max()};
std::atomic<const TickBase*> tick_base_{&initial_tick_base_};
// clock_lock_free as RecomputeGuestTickScalar last read it (Runtime::Setup, after the toml): a cvar read is a call.
std::atomic<bool> lock_free_{true};

// rr6-nx (2026-11-08, clock_count_calls): the bases RecomputeGuestTickScalar publishes, each on a cache line of its own
// (the heap object before shared its line with whatever the allocator put next to it); new past these. And the fast
// read's switch: clock_lock_free on, clock_no_scaling and clock_count_calls off, as last read there.
struct alignas(64) AlignedTickBase {
  TickBase base;
};
AlignedTickBase tick_base_slots_[8];
size_t tick_base_slots_used_ = 0;  // under tick_mutex_
alignas(64) std::atomic<bool> fast_read_{false};

// port 57 (clock_nap_clamp): the host count of a recent clock read, on a line of its own. Every read has a host count
// at most one mark step above it, so the guest time any read returned is at most the time at mark + step.
alignas(64) std::atomic<uint64_t> clock_mark_{0};
// Set by RecomputeGuestTickScalar before nap_clamp_ is turned on: 5 ms and 250 ms of host ticks.
uint64_t mark_step_ticks_ = 0;
uint64_t nap_ticks_ = 0;
std::atomic<bool> nap_clamp_{false};

// The host count, inline where the platform's is one register read.
[[gnu::always_inline]] inline uint64_t HostTicksNow() {
#if REX_PLATFORM_SWITCH && defined(__aarch64__)
  uint64_t ticks;
  __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(ticks));  // libnx's armGetSystemTick
  return ticks;
#else
  return Clock::QueryHostTickCount();
#endif
}

// rr6-nx (2026-11-08): floor(x / den) by a multiply. The A57's 64-bit divide (up to about 40 cycles) sat on the
// chain every time-base read waits on (125/48 here: 50 MHz guest, 19.2 MHz host). With r = floor(2^64 / den),
// floor(x * r / 2^64) is floor(x / den) or one less (x < 2^64): one compare of the remainder fixes it. Exact.
[[gnu::always_inline]] inline uint64_t DivideByDen(const TickBase& base, uint64_t x) {
#if defined(__SIZEOF_INT128__)
  if (base.recip) [[likely]] {
    uint64_t q = uint64_t((static_cast<unsigned __int128>(x) * base.recip) >> 64);
    if (x - q * base.den >= base.den) ++q;
    return q;
  }
#endif
  return x / base.den;
}

// floor(2^64 / den) in 64-bit arithmetic: 2^64 = (2^64 - 1) + 1. (A 128-bit divide needs __udivti3, which the PC
// build's clang on Windows does not link.)
inline uint64_t ReciprocalOf(uint64_t den) {
#if defined(__SIZEOF_INT128__)
  if (den < 2) return 0;
  const uint64_t all = std::numeric_limits<uint64_t>::max();
  return all / den + (all % den == den - 1 ? 1 : 0);
#else
  (void)den;
  return 0;
#endif
}

inline uint64_t GuestTicksAt(const TickBase& base, uint64_t host_tick_count) {
  const uint64_t delta = host_tick_count > base.host ? host_tick_count - base.host : 0;
  if (delta <= base.max_delta) [[likely]] {
    return base.guest + DivideByDen(base, delta * base.num);
  }
  // delta = q * den + r: the same floor without the 64-bit overflow (centuries away at the start ratio).
  return base.guest + delta / base.den * base.num + delta % base.den * base.num / base.den;
}

// port 57: a gap found by a read (UpdateGuestClock). Under the lock the gap is checked again (the first
// thread to find it cuts it, the others find the mark moved), then the lock-free base goes on from the guest time at
// mark + step (the latest any read can have returned) at host count now. The locked path's state too.
[[gnu::noinline, gnu::cold]] void CutNap(uint64_t now) {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  const uint64_t mark = clock_mark_.load(std::memory_order_relaxed);
  if (now <= mark + nap_ticks_) return;
  const uint64_t resume = mark + mark_step_ticks_;
  const TickBase* old_base = tick_base_.load(std::memory_order_relaxed);
  TickBase next = *old_base;
  next.guest = GuestTicksAt(*old_base, resume);
  next.host = now;
  TickBase* slot = tick_base_slots_used_ < std::size(tick_base_slots_)
                       ? &tick_base_slots_[tick_base_slots_used_++].base
                       : new TickBase;  // a reader may still hold the old one: never freed (one per nap)
  *slot = next;
  tick_base_.store(slot, std::memory_order_release);
  last_host_tick_count_ = now;
  clock_mark_.store(now, std::memory_order_release);
  static uint32_t naps = 0;
  ++naps;
  REXLOG_INFO("[clock] {} ms with no clock read cut out of the guest's time (the process suspended: asleep "
              "or at HOME; or no reads yet, at the start) ({} so far; clock_nap_clamp)",
              (now - resume) * 1000 / std::max<uint64_t>(1, Clock::QueryHostTickFrequency()), naps);
}

// port 57: a read's check against the mark (acquire: a base published by CutNap before the mark is seen with it).
[[gnu::always_inline]] inline void NoteClockRead(uint64_t now) {
  uint64_t mark = clock_mark_.load(std::memory_order_acquire);
  if (now > mark + mark_step_ticks_) [[unlikely]] {
    if (now - mark > nap_ticks_) {
      CutNap(now);
      return;
    }
    // Only forward: a thread holding an older count must not move it back.
    while (now > mark && !clock_mark_.compare_exchange_weak(mark, now, std::memory_order_relaxed)) {
    }
  }
}

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac = std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  std::lock_guard<std::mutex> lock(tick_mutex_);
  guest_tick_ratio_ = frac;
  lock_free_.store(REXCVAR_GET(clock_lock_free), std::memory_order_relaxed);
  // The lock-free base moves to now, so the count goes on from where the old ratio took it.
  const uint64_t host_now = Clock::QueryHostTickCount();
  const TickBase* old_base = tick_base_.load(std::memory_order_relaxed);
  const TickBase next{host_now, GuestTicksAt(*old_base, host_now), frac.first, frac.second,
                      std::numeric_limits<uint64_t>::max() / frac.first, ReciprocalOf(frac.second)};
  TickBase* slot = tick_base_slots_used_ < std::size(tick_base_slots_)
                       ? &tick_base_slots_[tick_base_slots_used_++].base
                       : new TickBase;
  *slot = next;
  tick_base_.store(slot, std::memory_order_release);
#if REX_CLOCK_RAW_AVAILABLE
  const bool raw = REXCVAR_GET(clock_source_raw);  // the fast read takes the platform's count
#else
  const bool raw = false;
#endif
  fast_read_.store(REXCVAR_GET(clock_lock_free) && !REXCVAR_GET(clock_no_scaling) &&
                       !REXCVAR_GET(clock_count_calls) && !raw,
                   std::memory_order_release);
  // port 57 (clock_nap_clamp): the steps in host ticks, the mark at now, then the switch.
  const uint64_t host_frequency = std::max<uint64_t>(1, Clock::QueryHostTickFrequency());
  mark_step_ticks_ = host_frequency / 200;
  nap_ticks_ = host_frequency / 4;
  clock_mark_.store(host_now, std::memory_order_release);
  const bool clamp = REXCVAR_GET(clock_nap_clamp) && !REXCVAR_GET(clock_no_scaling);
  nap_clamp_.store(clamp, std::memory_order_release);
}

// Update the guest timer for all threads.
// Return a copy of the value so locking is reduced.
uint64_t UpdateGuestClock() {
  if (fast_read_.load(std::memory_order_relaxed)) [[likely]] {  // rr6-nx (2026-11-08, clock_count_calls)
    const uint64_t now = HostTicksNow();
    if (nap_clamp_.load(std::memory_order_relaxed)) [[likely]] {
      NoteClockRead(now);  // port 57: before the base is loaded
    }
    const TickBase& base = *tick_base_.load(std::memory_order_acquire);
    return GuestTicksAt(base, now);
  }
  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (REXCVAR_GET(clock_no_scaling)) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  if (lock_free_.load(std::memory_order_relaxed)) {
    // Counted 64 at a time per thread: the shared counter's add on every call cost as much as the rest.
    thread_local uint32_t calls = 0;
    if (++calls == 64) {
      calls = 0;
      REX_SYS_COUNT(rex::syscount::kClockCall, 64);
    }
    if (nap_clamp_.load(std::memory_order_relaxed)) {
      NoteClockRead(host_tick_count);  // port 57
    }
    return GuestTicksAt(*tick_base_.load(std::memory_order_acquire), host_tick_count);
  }

  REX_SYS_COUNT(rex::syscount::kClockCall, 1);
  std::unique_lock<std::mutex> lock(tick_mutex_, std::defer_lock);
  if (lock.try_lock()) {
    // Translate host tick count to guest tick count.
    uint64_t host_tick_delta =
        host_tick_count > last_host_tick_count_ ? host_tick_count - last_host_tick_count_ : 0;
    last_host_tick_count_ = host_tick_count;
    // ds3-nx (sdk-patches/0013; port 57): the locked path's own form: a delta longer than 250 ms is cut to it.
    if (nap_clamp_.load(std::memory_order_relaxed) && host_tick_delta > nap_ticks_) {
      host_tick_delta = nap_ticks_;
    }
    uint64_t guest_tick_delta =
        host_tick_delta * guest_tick_ratio_.first / guest_tick_ratio_.second;
    const uint64_t guest_tick_count =
        last_guest_tick_count_.load(std::memory_order_relaxed) + guest_tick_delta;
    last_guest_tick_count_.store(guest_tick_count, std::memory_order_release);
    return guest_tick_count;
  } else {
    REX_SYS_COUNT(rex::syscount::kClockBlocked, 1);
    if (REXCVAR_GET(clock_nonblocking)) {
      // Another thread is inside the update (a few instructions): its count is the one it is about to
      // publish and that is what waiting for the lock used to return.
      return last_guest_tick_count_.load(std::memory_order_acquire);
    }
    // Wait until another thread has finished updating the clock.
    lock.lock();
    return last_guest_tick_count_.load(std::memory_order_acquire);
  }
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}

uint64_t Clock::QueryHostTickFrequency() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() {
  return guest_time_scalar_;
}

void Clock::set_guest_time_scalar(double scalar) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() {
  return guest_tick_frequency_;
}

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() {
  return guest_system_time_base_;
}

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t Clock::QueryGuestSystemTime() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                                                  std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms =
      static_cast<uint64_t>((static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time = static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time =
        static_cast<uint64_t>((static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) * guest_time_scalar_);
  uint64_t scaled_usec =
      static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) * guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace rex::chrono
