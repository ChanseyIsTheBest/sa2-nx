/**
 * @file        switch_perf.cpp
 * @brief       Performance measurement on the console, without a debugger
 *
 * The first boot that reached the title screen ran at 5-10 fps, with two cores at 97% and the
 * GPU at 99.7% according to the Horizon OC monitor. That does not tell what to optimize: it takes
 * knowing which thread spends the CPU and in which function. Horizon has no perf and no debugger
 * on a retail console, so it is measured from inside. Every 10 s a report is appended to
 * <NRO folder>/logs/rex/rex_profile.log:
 *
 *  - Game FPS: guest presents (IssueSwap).
 *  - Faults per second by type: each one costs an exception, or two with the resume through the
 *    kernel.
 *  - Exact CPU of each thread: the kernel keeps each thread's CPU time (svcGetInfo with
 *    InfoType_ThreadTickCount, 13.0.0+), with its priority, its preferred core and its core mask.
 *  - Where it is spent: sampling at 1 kHz. A high-priority thread pauses a busy thread
 *    (svcSetThreadActivity), reads its pc, its lr and its stack through the x29 chain
 *    (svcGetThreadContext3) and resumes it. The stacks also tell what a thread stopped in the
 *    kernel is waiting for. Addresses come out as image+0x...; they are translated with addr2line
 *    on the ELF.
 *
 * Threads: by wrapping libnx's threadCreate and threadClose (--wrap in rexglue_switch.cmake). Every
 * thread goes through there, pthread ones included, so Mesa's and the SDK's show up too. The name
 * comes from pthread_setname_np (switch_libc_supplement.c).
 *
 * libnx calls: also with --wrap, the calls that cost IPC or walk memory are counted, along with the
 * time the calling thread spends inside: fences (zero-timeout queries and waits), submission
 * kickoffs, new NvMaps (with and without CPU cache), GPU addresses and mappings, armDCacheClean, the
 * window queue and svcSleepThread. Every fence query is at least one ioctl even when the GPU has
 * already finished (see docs/platform-notes.md), so the count per second is needed before changing
 * anything.
 *
 * Careful with the pause: while a thread is paused nothing is done that could take a lock (no
 * malloc, no stdio): if the paused thread held it, this one would wait forever. Between pause and
 * resume there are only system calls. If pausing fails (for example because the SDK had already
 * suspended the thread) it is left alone: resuming it would break that suspension.
 *
 * It goes in the executable (rexglue_switch_startup): the SDK calls RexSwitchPerfCount from
 * libraries, and the wrappers have to be defined before libnx.a enters the link.
 */

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>  // blur-nx (2026-11-16): strtol (switch_profile_stacks_ms)
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <switch.h>

#include "rex/ui/switch_apm.h"

// banjo-nx (2026-10-27): the system core (core/threading_switch.cpp; its header clashes with switch.h).
extern "C" int RexSwitchSystemCore(void);
extern "C" int RexSwitchApplyNamedPriority(unsigned handle, const char* name);  // blur-nx: switch_thread_priorities
extern "C" int RexSwitchSystemCoreMode(void);
extern "C" unsigned RexSwitchAppCoreMask(void);
extern "C" bool RexSwitchCoreIdleSample(int core, unsigned long long* idle_ticks, unsigned long long* at_ticks);
#include "rex/ui/switch_saltynx.h"
#include "rex/ui/switch_sysclk.h"

extern "C" {
void _start(void);
/* rexglue log folder, ending in '/' (switch_crash_hooks.c). */
const char* RexSwitchLogDir(void);
/*
 * From the guest memory core (guest_memory_switch.cpp): how much backing is
 * committed and how much is mapped in total, counting the 360 mirrors.
 */
size_t RexGmCommittedBytes(void);
size_t RexGmMappedBytes(void);
/* 0 = untested, 1 = permissions (watched pages readable), 2 = unmapping. */
int RexGmProtectionMode(void);
Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid);
Result __real_threadClose(Thread* t);
Result __real_nvFenceWait(NvFence* f, s32 timeout_us);
Result __real_nvGpuChannelKickoff(NvGpuChannel* c);
Result __real_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable);
Result __real_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova);
Result __real_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset);
void __real_armDCacheClean(void* addr, size_t size);
Result __real_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence);
Result __real_nwindowDequeueBuffer(NWindow* nw, s32* out_slot, NvMultiFence* out_fence);  // port 145: async present
Result __real_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence);
void __real_svcSleepThread(s64 nano);
}

// banjo-nx (as futari-nx): declared here rather than through rex/cvar.h (this file is built without fmt's include path).
namespace rex::cvar {
std::string GetFlagByName(std::string_view name);
}  // namespace rex::cvar

namespace {

constexpr size_t kMaxThreads = 160;
constexpr u64 kSampleIntervalNs = 1000000ULL;  // 1 ms
constexpr u64 kSecondNs = 1000000000ULL;      // tick for the console overlays

/* internal resolution published to the overlays. The game sets it when choosing the video mode. */
std::atomic<uint32_t> g_resolution{(1280u << 16) | 720u};
constexpr u64 kPassiveIntervalNs = 100000000ULL;  // 100 ms, without pausing threads
// Next to the rexglue logs, in <NRO folder>/logs/rex/.
std::string StackFlagPath() {
  return std::string(RexSwitchLogDir()) + "stacks_profile.flag";
}
constexpr u64 kReportSeconds = 10;
/*
 * Let the game start first. This only delays stack sampling and the first report, which read the
 * SD and pause threads. The console overlays (FPS and resolution) are published from second zero:
 * see the warm-up loop in ProfilerMain.
 */
constexpr u64 kStartDelayNs = 8000000000ULL;
constexpr size_t kMaxSamples = 1 << 15;
constexpr size_t kFrames = 10;
// 0-32 renderer/audio counters (see RexSwitchPerfAdd); 40-55 host-runtime overhead (include/rex/sys_counters.h).
constexpr unsigned kCounterCount = 64;
}  // namespace
extern "C" const char* RexAppBuildStamp(void);  // port 193: switch_crash_hooks.c (weak), app/src/build_stamp.cpp
namespace {
// banjo-nx (2026-11-01, the public build; port 230): whether rex_profile.log is written (logging on, or stack
// sampling).
bool g_write_reports = true;

// In <NRO folder>/logs/rex/ (switch_crash_hooks.c computes it at startup).
std::string ReportPath() {
  return std::string(RexSwitchLogDir()) + "rex_profile.log";
}

struct Slot {
  std::atomic<u32> handle{0};
  char name[32]{};
  // Only the profiler thread touches these:
  u32 seen = 0;
  u64 last_ticks = 0;
  bool busy = true;
  double cpu = 0.0;
  // sasrt-nx (2026-11-09; port 58, the guest threads' 16 MB host stacks: ~500 MB of heap in SASRT): the deepest the
  // sampled sp has been below the top of its stack mapping (svcQueryMemory of sp), since the slot's thread registered.
  std::atomic<u64> stack_deepest{0};
  // blur-nx (2026-11-13, M-44 of Blur's docs/cpu-moonshots-50-1113.md; the census of #34 in docs/cpu-near-exact-50-1113.md):
  // the thread's svcSleepThread calls by kind, written by the thread itself (its slot found once, a thread_local), read
  // by the report: yields (0, -1, -2), short sleeps (up to 1 ms) and the ticks spent inside them, longer sleeps.
  std::atomic<u64> yields{0}, sleeps{0}, sleep_ticks{0}, long_sleeps{0};
  u64 last_yields = 0, last_sleeps = 0, last_sleep_ticks = 0, last_long = 0;  // the profiler thread's
  double yields_s = 0.0, sleeps_s = 0.0, sleep_ms = 0.0, long_s = 0.0;
};
Slot g_slots[kMaxThreads];
std::atomic<u64> g_stack_deepest_all{0};  // port 58: the deepest of any thread so far
// Exclusive core for the hottest guest thread (the game's main thread): -1 = off. See ApplyExclusiveCore.
std::atomic<int> g_exclusive_core{-1};
std::atomic<bool> g_light_core{false};
std::atomic<bool> g_core_render{false};

struct Sample {
  u64 pc;
  u64 lr;
  u64 frames[kFrames];  // return addresses along the x29 chain
  u16 slot;
  u64 tick;  // when it was taken (armGetSystemTick), to separate those from long frames
};

/*
 * Long frame windows. The renderer calls RexSwitchPerfHitch with the interval of every frame over
 * 45 ms; the report separates the samples taken inside those intervals and says what each thread was
 * doing right then (section "during the hitches"). Only works with stacks_profile.flag.
 */
constexpr size_t kMaxHitches = 256;
struct WindowHitch {
  u64 start;
  u64 end;
};
WindowHitch g_hitches[kMaxHitches];
std::atomic<u32> g_num_hitches{0};
Sample g_samples[kMaxSamples];
size_t g_sample_count = 0;

// blur-nx (2026-11-13, cycle 3, M-39's kind: Blur's docs/cpu-moonshots-50-1113.md): the counters in kCounterShards sets
// on lines of their own, a thread's set picked by its TLS slot (tpidrro_el0, one register read; libnx's slots are 0x200
// bytes apart). They were one array every thread added to: the kernel's REX_SYS_COUNT sites alone ~20,000 times a
// second in Blur's races (ObjectTable lookups 11,700, signals with nobody blocked 8,000), each add an exclusive
// load/store loop on a line other cores had just written. Two threads that share a set still count right (atomic
// adds); CounterRead sums the sets. The max counters (RexSwitchPerfMax, 23) are set 0's alone (nothing adds to them).
constexpr unsigned kCounterShards = 8;
struct alignas(64) CounterShard {
  std::atomic<u64> c[kCounterCount];
};
CounterShard g_counter_shards[kCounterShards];

inline std::atomic<u64>& CounterOf(unsigned id) {
  const uintptr_t tls = reinterpret_cast<uintptr_t>(armGetTls());
  return g_counter_shards[(tls >> 9) % kCounterShards].c[id];
}

u64 CounterRead(unsigned id) {
  u64 sum = 0;
  for (const CounterShard& shard : g_counter_shards) {
    sum += shard.c[id].load(std::memory_order_relaxed);
  }
  return sum;
}

/*
 * Wrapped libnx calls (see the file header): how many, how much time inside and, where it makes
 * sense, how many bytes. They are constant-initialized, so they are valid even if someone calls
 * before the constructors.
 */
enum : unsigned {
  kFenceQuery,     // nvFenceWait with wait 0
  kFenceWait,       // nvFenceWait with wait
  kKickoff,           // nvGpuChannelKickoff
  kNvMapCached,     // nvMapCreate with CPU cache
  kNvMapNoCache,     // nvMapCreate without CPU cache
  kReserveAddress,  // nvAddressSpaceAllocFixed
  kMappingGpu,          // nvioctlNvhostAsGpu_MapBufferEx
  kCacheClean,        // armDCacheClean
  kQueueBuffer,       // nwindowQueueBuffer
  kDequeueBuffer,     // bqDequeueBuffer
  kSleep0,            // svcSleepThread(0)
  kSleepYield,        // svcSleepThread(-1 o -2)
  kSleepShort,        // up to 1 ms
  kSleepLong,        // over 1 ms
  kCallsCount,
};
struct Calls {
  std::atomic<u64> n{0};
  std::atomic<u64> ticks{0};
  std::atomic<u64> bytes{0};
};
// blur-nx (2026-11-13, cycle 3, M-39's kind: Blur's docs/cpu-moonshots-50-1113.md): the counters in kCallShards sets, a
// thread's set picked by its TLS slot (tpidrro_el0, one register read; libnx's slots are 0x200 bytes apart), each set on
// lines of its own. They were one array every thread added to: svcSleepThread alone ~4,500 times a second from the job
// workers' yields (each add an exclusive load/store loop on a line the other cores had just written). Two threads that
// share a set still count right (the adds are atomic); the report sums the sets.
constexpr unsigned kCallShards = 8;
struct alignas(64) CallShard {
  Calls calls[kCallsCount];
};
CallShard g_call_shards[kCallShards];

inline Calls& CallsOf(unsigned id) {
  const uintptr_t tls = reinterpret_cast<uintptr_t>(armGetTls());
  return g_call_shards[(tls >> 9) % kCallShards].calls[id];
}

// blur-nx (2026-11-17, the Renut audit's #29): the wrapped libnx calls' counters (the "libnx per second" line), off
// with the title's switch_perf_count_calls = false (read by ProfilerMain after its warm-up; counted until then): a
// shipping config that reads no reports skips a clock read and two atomics per kick, mapping, cache clean and yield.
std::atomic<bool> g_count_calls{true};

inline void Note(unsigned id, u64 from, u64 bytes = 0) {
  if (!g_count_calls.load(std::memory_order_relaxed)) return;
  const u64 ticks = armGetSystemTick() - from;
  Calls& l = CallsOf(id);
  l.n.fetch_add(1, std::memory_order_relaxed);
  l.ticks.fetch_add(ticks, std::memory_order_relaxed);
  if (bytes) {
    l.bytes.fetch_add(bytes, std::memory_order_relaxed);
  }
}

/*
 * Automatic A/B tests. Each report (10 s) removes one part of the GPU work: those commands (draw or
 * dispatch) stop being recorded, but everything else stays the same, so the state does not break.
 * The image looks wrong while the mode lasts; that is expected. If with "nothing" it is just as
 * slow, the cost is not in the work but in something fixed (submissions, driver).
 */
struct Mode {
  u32 mask;
  const char* name;
};
constexpr Mode kModes[] = {
    {0, "normal"},
    {1, "no draws"},
    {2, "no transfers"},
    {4, "no resolves"},
    {8, "no texture loads"},
    {16, "no presentation effect"},
    {32, "no tile dispatch"},
    {63, "nothing"},
};
constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

/*
 * A/B tests, started by hand
 *
 * They used to be enabled with this constant and rotate on their own from startup. That is no good:
 * the game takes a while to reach a race, and on the way it spent its time in modes with drawing
 * turned off. Once they were left on by mistake and the game did not even reach the title screen.
 *
 * Now the player starts the rotation with L+R+ZL from inside the race, which is the only place where
 * the measurement is useful. From then on it removes one part of the GPU work per report, goes once
 * through the eight modes and returns to "normal".
 *
 * How to read it. If with "nothing" the game is just as slow, the cost is not in the GPU work but
 * in something fixed: submissions, the driver, or the guest itself. If FPS shoots up, the mode where
 * it rises most points to the culprit.
 */
std::atomic<bool> g_ab_active{false};
std::atomic<u32> g_skip_mask{0};

extern "C" void RexSwitchPerfToggleAb(void) {
  const bool new_value = !g_ab_active.load(std::memory_order_relaxed);
  g_ab_active.store(new_value, std::memory_order_relaxed);
  if (!new_value) {
    g_skip_mask.store(0, std::memory_order_relaxed);
  }
}

/*
 * Horizon limits how much memory a process may map (LimitableResource_Memory). Our
 * guest memory model maps each chunk several times (the shadow and once per 360
 * view), so that limit is the real ceiling, not free memory. When it runs out,
 * even committing 4 KB fails with 2001-0103 (resource exhausted).
 */
void LimitOfMapping(u64* used_mb, u64* cap_mb, u64* process_mb, u64* total_mb) {
  *used_mb = *cap_mb = *process_mb = *total_mb = 0;
  u64 v = 0;
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *process_mb = v >> 20;
  }
  if (R_SUCCEEDED(svcGetInfo(&v, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
    *total_mb = v >> 20;
  }
  Handle reslimit = INVALID_HANDLE;
  if (R_FAILED(svcGetInfo(&v, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0))) {
    return;
  }
  reslimit = static_cast<Handle>(v);
  s64 actual = 0, cap = 0;
  if (R_SUCCEEDED(svcGetResourceLimitCurrentValue(&actual, reslimit, LimitableResource_Memory))) {
    *used_mb = static_cast<u64>(actual) >> 20;
  }
  if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&cap, reslimit, LimitableResource_Memory))) {
    *cap_mb = static_cast<u64>(cap) >> 20;
  }
  svcCloseHandle(reslimit);
}

u64 g_base = 0, g_text_lo = 0, g_text_hi = 0;
Thread g_thread;

void Register(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = 0;
    if (s.handle.compare_exchange_strong(expected, h)) {
      s.name[0] = 0;
      s.stack_deepest.store(0, std::memory_order_relaxed);  // port 58
      s.last_yields = s.yields.load(std::memory_order_relaxed);  // blur-nx (M-44): a reused slot's counts go on
      s.last_sleeps = s.sleeps.load(std::memory_order_relaxed);
      s.last_sleep_ticks = s.sleep_ticks.load(std::memory_order_relaxed);
      s.last_long = s.long_sleeps.load(std::memory_order_relaxed);
      return;
    }
  }
}

void Unregister(Handle h) {
  if (!h) {
    return;
  }
  for (auto& s : g_slots) {
    u32 expected = h;
    if (s.handle.compare_exchange_strong(expected, 0)) {
      return;
    }
  }
}

bool InText(u64 addr) { return addr >= g_text_lo && addr < g_text_hi; }

void PrintAddr(FILE* f, u64 addr) {
  if (InText(addr)) {
    std::fprintf(f, "image+0x%" PRIx64, addr - g_base);
  } else {
    std::fprintf(f, "0x%016" PRIx64, addr);
  }
}

/*
 * Is the pc right after a system call? Then the thread is waiting in the kernel,
 * not using CPU.
 */
bool AfterSvc(u64 pc) {
  if (!InText(pc) || pc < g_text_lo + 4) {
    return false;
  }
  // A thread blocked in the kernel reports either the svc itself or the instruction after it as its pc
  // (on this firmware the svc itself: every thread showed 0 % in the kernel with only the pc - 4 check).
  auto is_svc = [](u32 insn) { return (insn & 0xFFE0001Fu) == 0xD4000001u; };
  return is_svc(*reinterpret_cast<const u32*>(pc - 4)) ||
         (InText(pc + 4) && is_svc(*reinterpret_cast<const u32*>(pc)));
}

struct Count {
  u64 addr;
  u32 n;
};

std::vector<Count> Histogram(std::vector<u64>& addrs) {
  std::sort(addrs.begin(), addrs.end());
  std::vector<Count> out;
  for (u64 a : addrs) {
    if (out.empty() || out.back().addr != a) {
      out.push_back({a, 1});
    } else {
      ++out.back().n;
    }
  }
  std::sort(out.begin(), out.end(), [](const Count& x, const Count& y) { return x.n > y.n; });
  return out;
}

/*
 * Exclusive core (RexSwitchPerfExclusiveCore): once the hottest guest thread ("XThread...", the game's main
 * thread in gameplay) uses more than 60 % of a core, it is pinned to core K alone and every other registered
 * thread loses core K from its mask (threads pinned to K alone are left alone). Re-applied at every report
 * so that threads created later move off K too. Same work, different placement: the main thread no longer
 * shares its core with the ring thread and the C20 worker, which run at a higher priority (0x2C < 0x3B).
 */
void ApplyExclusiveCore(FILE* f, const std::vector<size_t>& order) {
  static u32 pinned = 0;
  const int k = g_exclusive_core.load(std::memory_order_relaxed);
  if (k < 0 || k > 2) return;
  const u64 bit = 1ull << k;
  if (!pinned) {
    for (size_t i : order) {
      const Slot& s = g_slots[i];
      if (std::strncmp(s.name, "XThread", 7) != 0) continue;
      if (s.cpu < 60.0) break;
      if (R_SUCCEEDED(svcSetThreadCoreMask(s.handle.load(), k, bit))) {
        pinned = s.handle.load();
        std::fprintf(f, "-- exclusive core %d for \"%s\" (CPU %.1f%%)\n", k, s.name, s.cpu);
      }
      break;
    }
    if (!pinned) return;
  }
  // 20 + K: also raise the second-hottest guest thread (UE3's render thread, the main thread's critical
  // path since C45) above the ring thread and the C20 worker (0x2C), which preempted it on cores 0-1.
  static u32 raised = 0;
  if (g_core_render.load(std::memory_order_relaxed) && !raised) {
    for (size_t i : order) {
      const Slot& s = g_slots[i];
      if (s.handle.load() == pinned || std::strncmp(s.name, "XThread", 7) != 0) continue;
      if (s.cpu < 35.0) break;
      if (R_SUCCEEDED(svcSetThreadPriority(s.handle.load(), 0x2B))) {
        raised = s.handle.load();
        std::fprintf(f, "-- priority 0x2B for \"%s\" (CPU %.1f%%)\n", s.name, s.cpu);
      }
      break;
    }
  }
  const bool light = g_light_core.load(std::memory_order_relaxed);
  for (size_t i = 0; i < kMaxThreads; ++i) {
    const u32 h = g_slots[i].handle.load();
    if (!h || h == pinned) continue;
    s32 preferred = -1;
    u64 mask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&preferred, &mask, h))) continue;
    // Light guest threads (audio, streaming: under 35 % of a core) may use the main thread's spare time on K,
    // which leaves cores 0-1 to the ring and render threads. Same priority as the main thread (0x3B): they
    // only run on K while it waits.
    if (light && std::strncmp(g_slots[i].name, "XThread", 7) == 0 && g_slots[i].cpu < 35.0) {
      if (mask == 0x6 || mask == 0x5 || mask == 0x3) svcSetThreadCoreMask(h, -3, mask | bit);
      continue;
    }
    if (!(mask & bit) || mask == bit) continue;
    const u64 new_entry = mask & ~bit;
    if (preferred == k || (preferred >= 0 && !(new_entry & (1ull << preferred)))) preferred = -1;
    svcSetThreadCoreMask(h, preferred, new_entry);
  }
}

void Report(u64 elapsed_ticks, u64 tick_freq, u64 counters_last[kCounterCount],
            const char* mode) {
  const double seconds = double(elapsed_ticks) / double(tick_freq);

  u64 counters_now[kCounterCount];
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_now[i] = CounterRead(i);
  }

  // CPU of each thread over the interval, as % of one core.
  std::vector<size_t> order;
  for (size_t i = 0; i < kMaxThreads; ++i) {
    Slot& s = g_slots[i];
    const u32 h = s.handle.load();
    if (!h) {
      s.seen = 0;
      continue;
    }
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX))) {
      continue;
    }
    if (s.seen != h) {  // new thread: no interval yet
      s.seen = h;
      s.last_ticks = ticks;
      s.busy = true;
      s.cpu = 0.0;
      continue;
    }
    s.cpu = double(ticks - s.last_ticks) * 100.0 / double(elapsed_ticks);
    s.last_ticks = ticks;
    {  // blur-nx (2026-11-13, M-44): the thread's sleeps and yields in the interval
      const double seconds = double(elapsed_ticks) / double(armGetSystemTickFreq());
      const u64 y = s.yields.load(std::memory_order_relaxed), n = s.sleeps.load(std::memory_order_relaxed);
      const u64 t = s.sleep_ticks.load(std::memory_order_relaxed), l = s.long_sleeps.load(std::memory_order_relaxed);
      s.yields_s = double(y - s.last_yields) / seconds;
      s.sleeps_s = double(n - s.last_sleeps) / seconds;
      s.sleep_ms = n != s.last_sleeps ? double(t - s.last_sleep_ticks) * 1000.0 / double(armGetSystemTickFreq()) /
                                            double(n - s.last_sleeps)
                                      : 0.0;
      s.long_s = double(l - s.last_long) / seconds;
      s.last_yields = y;
      s.last_sleeps = n;
      s.last_sleep_ticks = t;
      s.last_long = l;
    }
    /*
     * All threads are sampled, including those that use no CPU. In a hang caused by
     * a wait, the thread that matters is exactly that one: the stopped one.
     */
    s.busy = true;
    order.push_back(i);
  }
  std::sort(order.begin(), order.end(),
            [](size_t a, size_t b) { return g_slots[a].cpu > g_slots[b].cpu; });

  FILE* f = g_write_reports ? std::fopen(ReportPath().c_str(), "a") : nullptr;  // banjo-nx (port 230)
  if (!f) {
    g_sample_count = 0;
    return;
  }

  ApplyExclusiveCore(f, order);

  u64 lim_used = 0, lim_cap = 0, proc_used = 0, proc_total = 0;
  LimitOfMapping(&lim_used, &lim_cap, &proc_used, &proc_total);

  double total_cpu = 0.0;
  for (size_t i : order) {
    total_cpu += g_slots[i].cpu;
  }
  std::fprintf(f,
               "==== %.1f s | mode: %s | game %.1f fps | CPU total %.0f%% (400%% = 4 cores) | faults/s: "
               "emulated read %.0f, SDK handler %.0f, emulated retry %.0f, SEH %.0f, "
               "physical committed %.0f, views %.0f | guest %zu/%zu MB (backing/mapped) | "
               "mapping limit %llu/%llu MB, process %llu/%llu MB | "
               "samples %zu | watching: %s | samplers: %.0f new/s, %.0f stalls/s\n",
               seconds, mode, double(counters_now[0] - counters_last[0]) / seconds, total_cpu,
               double(counters_now[1] - counters_last[1]) / seconds,
               double(counters_now[2] - counters_last[2]) / seconds,
               double(counters_now[3] - counters_last[3]) / seconds,
               double(counters_now[4] - counters_last[4]) / seconds,
               double(counters_now[17] - counters_last[17]) / seconds,
               double(counters_now[18] - counters_last[18]) / seconds,
               RexGmCommittedBytes() >> 20, RexGmMappedBytes() >> 20,
               (u64)lim_used, (u64)lim_cap, (u64)proc_used, (u64)proc_total, g_sample_count,
                (RexGmProtectionMode() == 1   ? "permissions (readable pages)"
                 : RexGmProtectionMode() == 2 ? "unmapping (every read faults)"
                                              : "untested"),
                double(counters_now[20] - counters_last[20]) / seconds,
                double(counters_now[19] - counters_last[19]) / seconds);
  // Work the game sends to the GPU, per presented frame. One screen is
  // 1280x720 = 921,600 pixels.
  const auto delta = [&](unsigned id) { return double(counters_now[id] - counters_last[id]); };
  const double frames = delta(0);
  // Mix clipping before conversion to 16 bits (21 samples above 1.0, 22 buffers with a peak of 0.98 or
  // more, 23 maximum peak of the interval in ten-thousandths) and the console mode, for the crackling
  // in handheld mode.
  const u64 audio_peak = g_counter_shards[0].c[23].exchange(0, std::memory_order_relaxed);  // a max: set 0's
  std::fprintf(f,
               "     audio: %.0f client blocks mixed, %.0f requests without data, "
               "%.0f/%.0f audout buffers with non-zero PCM, %.0f saturated samples (above 1.0 before clipping), "
               "%.0f buffers with a peak of 0.98 or more, maximum peak %.3f | audout played %.0f samples (%.0f Hz), "
               "%.0f wakes with every buffer released | console in %s mode\n",
               delta(24), delta(25), delta(26), delta(27), delta(21), delta(22), double(audio_peak) / 10000.0,
               delta(57), delta(57) / seconds, delta(58),
               rex::ui::switch_saltynx::BaseMode(appletGetOperationMode() == AppletOperationMode_Console)
                   ? "docked"
                   : "handheld");
  // Reverse-NX state as is, without interpretation. A whole test session was wasted because its
  // overlay said "Docked" while "Controlled by system" was Yes, and in that case its mode does not
  // rule: it only mirrors the console's. This shows at a glance which of the two is happening.
  {
    const auto reverse = rex::ui::switch_saltynx::ReverseState();
    const bool real = appletGetOperationMode() == AppletOperationMode_Console;
    if (!reverse.has) {
      std::fprintf(f, "     Reverse-NX: no block (console %s, it rules)\n",
                   real ? "docked" : "in hand");
    } else {
      std::fprintf(f,
                   "     Reverse-NX: says %s, rules %s (Controlled by system %s), the game has asked: %s; "
                   "real console %s -> obeying %s\n",
                   reverse.in_base ? "docked" : "handheld", reverse.by_default ? "the console" : "Reverse-NX",
                   reverse.by_default ? "Yes" : "No", reverse.plugin_active ? "yes" : "no",
                   real ? "docked" : "in hand",
                   rex::ui::switch_saltynx::BaseMode(real) ? "docked" : "handheld");
    }
  }
  // Real console clocks (clkrst, 8.0.0+) and the cores the process may use. It tells whether the
  // session ran overclocked and with which CPU limit it was measured: performance tests run without
  // overclock.
  {
    static bool started = false;
    static bool has_clkrst = false;
    static ClkrstSession sessions[3]{};
    static u64 cores_mask = 0;
    if (!started) {
      started = true;
      if (R_FAILED(svcGetInfo(&cores_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        cores_mask = 0;
      }
      if (R_SUCCEEDED(clkrstInitialize())) {
        static const PcvModuleId modules[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
        has_clkrst = true;
        for (unsigned i = 0; i < 3; ++i) {
          if (R_FAILED(clkrstOpenSession(&sessions[i], modules[i], 3))) {
            has_clkrst = false;
          }
        }
      }
    }
    u32 hz[3] = {0, 0, 0};
    if (has_clkrst) {
      for (unsigned i = 0; i < 3; ++i) {
        clkrstGetClockRate(&sessions[i], &hz[i]);
      }
    }
    unsigned cores = 0;
    for (unsigned i = 0; i < 64; ++i) {
      cores += unsigned((cores_mask >> i) & 1);
    }
    // SoC and board temperature (ts service, sessions on [10.0.0+]). The system lowers clocks around
    // 70 degrees and Erista gets there before Mariko: without this, "it is slow" cannot be told apart
    // from "it is hot". On the OLED model the board sensor reads wrong, because it sits next to the
    // charging IC.
    static bool ts_started = false;
    static bool has_ts = false;
    static TsSession ts_soc{}, ts_board{};
    if (!ts_started) {
      ts_started = true;
      if (R_SUCCEEDED(tsInitialize())) {
        has_ts = R_SUCCEEDED(tsOpenSession(&ts_soc, TsDeviceCode_LocationExternal)) &&
                 R_SUCCEEDED(tsOpenSession(&ts_board, TsDeviceCode_LocationInternal));
      }
    }
    float degrees_soc = 0.0f, degrees_board = 0.0f;
    if (has_ts) {
      tsSessionGetTemperature(&ts_soc, &degrees_soc);
      tsSessionGetTemperature(&ts_board, &degrees_board);
    }
    std::fprintf(f,
                 "     clocks: CPU %.1f MHz, GPU %.1f MHz, memory %.1f MHz%s | process cores %u "
                 "(mask 0x%llX) | SoC %.1f C, board %.1f C%s\n",
                 double(hz[0]) / 1.0e6, double(hz[1]) / 1.0e6, double(hz[2]) / 1.0e6,
                 has_clkrst ? "" : " (clkrst not available)", cores,
                 (unsigned long long)cores_mask, double(degrees_soc), double(degrees_board),
                 has_ts ? "" : " (ts not available)");
    // banjo-nx (2026-10-27): the system core (4-core mode, threading_switch.h): how much of it nobody used over the
    // interval (sampled by the audio output thread, which runs there), and the mode.
    const int system_core = RexSwitchSystemCore();
    if (system_core >= 0) {
      static unsigned long long last_idle = 0, last_at = 0;
      unsigned long long idle = 0, at = 0;
      if (RexSwitchCoreIdleSample(system_core, &idle, &at)) {
        if (last_at != 0 && at > last_at) {
          std::fprintf(f, "     system core %d: idle %.1f%% (banjo_system_core %d; game threads on 0x%X)\n",
                       system_core, 100.0 * double(idle - last_idle) / double(at - last_at),
                       RexSwitchSystemCoreMode(), RexSwitchAppCoreMask());
        }
        last_idle = idle;
        last_at = at;
      }
    }
  }
  // libnx calls: per second, with the ms per second the calling threads spend inside.
  {
    static u64 last[kCallsCount][3];
    double n[kCallsCount], ms[kCallsCount], mb[kCallsCount];
    for (unsigned i = 0; i < kCallsCount; ++i) {
      u64 vn = 0, vt = 0, vb = 0;  // blur-nx (2026-11-13): the sets summed (CallShard)
      for (const CallShard& shard : g_call_shards) {
        vn += shard.calls[i].n.load(std::memory_order_relaxed);
        vt += shard.calls[i].ticks.load(std::memory_order_relaxed);
        vb += shard.calls[i].bytes.load(std::memory_order_relaxed);
      }
      n[i] = double(vn - last[i][0]) / seconds;
      ms[i] = double(vt - last[i][1]) * 1000.0 / double(tick_freq) / seconds;
      mb[i] = double(vb - last[i][2]) / 1048576.0 / seconds;
      last[i][0] = vn;
      last[i][1] = vt;
      last[i][2] = vb;
    }
    std::fprintf(f,
                 "     libnx per second (calls and ms inside): fences queried %.0f (%.1f ms), waited %.0f "
                 "(%.1f ms) | kickoff %.0f (%.1f ms) | NvMap new %.1f cached (%.1f MB) and %.1f uncached "
                 "(%.1f MB), %.1f ms | GPU addresses %.1f (%.1f ms), mappings %.1f (%.1f MB, %.1f ms) | "
                 "armDCacheClean %.0f (%.1f MB, %.1f ms) | window queue %.1f (%.1f ms), dequeue buffer %.1f "
                 "(%.1f ms) | svcSleepThread: 0 %.0f, yield %.0f, up to 1 ms %.0f (%.1f ms), more %.0f\n",
                 n[kFenceQuery], ms[kFenceQuery], n[kFenceWait], ms[kFenceWait], n[kKickoff],
                 ms[kKickoff], n[kNvMapCached], mb[kNvMapCached], n[kNvMapNoCache], mb[kNvMapNoCache],
                 ms[kNvMapCached] + ms[kNvMapNoCache], n[kReserveAddress], ms[kReserveAddress],
                 n[kMappingGpu], mb[kMappingGpu], ms[kMappingGpu], n[kCacheClean], mb[kCacheClean], ms[kCacheClean],
                 n[kQueueBuffer], ms[kQueueBuffer], n[kDequeueBuffer], ms[kDequeueBuffer], n[kSleep0],
                 n[kSleepYield], n[kSleepShort], ms[kSleepShort], n[kSleepLong]);
  }
  // Host-runtime overhead (rex/sys_counters.h): wake-ups spent polling and lock stalls, per second.
  std::fprintf(f,
               "     system per second: alertable waits: %.0f poll-slice wakes, %.0f woken by APC hint | "
               "WaitMultiple: %.0f parked, %.0f woken by signal, %.0f safety re-polls, %.0f legacy 1 ms polls | "
               "timestamp timer ticks %.0f | UpdateGuestClock %.0f calls, %.0f contended | ObjectTable lookups "
               "%.0f, %.0f contended | VolumeChangeMask %.0f | vblank thread wakes %.0f | native audio DSP calls "
               "%.0f, %.0f mismatches | critical-section host waits %.0f | signals with nobody blocked (no wake "
               "syscall, thread_signal_skip_idle) %.0f\n",
               delta(40) / seconds, delta(41) / seconds, delta(42) / seconds, delta(43) / seconds,
               delta(44) / seconds, delta(45) / seconds, delta(46) / seconds, delta(47) / seconds,
               delta(48) / seconds, delta(49) / seconds, delta(50) / seconds, delta(51) / seconds,
               delta(52) / seconds, delta(53) / seconds, delta(54) / seconds, delta(55) / seconds,
               delta(56) / seconds);
  if (frames > 0) {
    std::fprintf(f,
                 "     per frame: %.0f draws | scissor %.1f screens | %.1f submits | "
                 "%.1f resolves (%.2f screens) | transfers: %.1f calls, %.1f render "
                 "targets, %.1f draws | %.1f textures loaded | %.2f MB of shared "
                 "memory uploaded | pipelines created in the interval: %.0f\n",
                 delta(5) / frames, delta(6) / frames / 921600.0, delta(7) / frames,
                 delta(9) / frames, delta(10) / frames / 921600.0, delta(11) / frames,
                 delta(12) / frames, delta(13) / frames, delta(14) / frames,
                 delta(15) / frames / 1048576.0, delta(16));
    std::fprintf(f,
                 "     draws by surface width: 1600+ %.0f | 1280-1599 %.0f | "
                 "640-1279 %.0f | 256-639 %.0f | under 256 %.0f\n",
                 delta(28) / frames, delta(29) / frames, delta(30) / frames, delta(31) / frames,
                 delta(32) / frames);
  }

  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = counters_now[i];
  }

  std::vector<Sample> samples(g_samples, g_samples + g_sample_count);
  g_sample_count = 0;
  std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) { return a.slot < b.slot; });

  // What each thread was doing during the long frames of this interval.
  {
    const u32 nt = std::min<u32>(g_num_hitches.exchange(0), kMaxHitches);
    if (nt && !samples.empty()) {
      std::vector<WindowHitch> v(g_hitches, g_hitches + nt);
      std::sort(v.begin(), v.end(), [](const WindowHitch& a, const WindowHitch& b) { return a.start < b.start; });
      double ms_total = 0.0;
      for (const WindowHitch& w : v) {
        ms_total += double(w.end - w.start) * 1000.0 / double(tick_freq);
      }
      const auto inside = [&v](u64 t) {
        auto it = std::upper_bound(v.begin(), v.end(), t, [](u64 x, const WindowHitch& w) { return x < w.start; });
        return it != v.begin() && t <= (it - 1)->end;
      };
      std::fprintf(f, "\n== during the hitches: %u frames over 45 ms (%.0f ms in total) ==\n", nt, ms_total);
      for (size_t i : order) {
        std::vector<u64> pcs;
        std::vector<const Sample*> all;
        size_t inside_svc = 0;
        for (const Sample& m : samples) {
          if (m.slot != i || !inside(m.tick)) {
            continue;
          }
          all.push_back(&m);
          if (AfterSvc(m.pc)) {
            ++inside_svc;
          } else {
            pcs.push_back(m.pc);
          }
        }
        if (all.size() < 5) {
          continue;
        }
        const Slot& s = g_slots[i];
        std::fprintf(f, "-- thread \"%s\": %zu samples during the hitches, %.0f%% waiting in the kernel\n",
                     s.name[0] ? s.name : "?", all.size(), double(inside_svc) * 100.0 / double(all.size()));
        const auto h = Histogram(pcs);
        for (size_t k = 0; k < h.size() && k < 25; ++k) {
          std::fprintf(f, "   %5.1f%%  pc ", double(h[k].n) * 100.0 / double(all.size()));
          PrintAddr(f, h[k].addr);
          std::fputc('\n', f);
        }
        std::sort(all.begin(), all.end(), [](const Sample* a, const Sample* b) {
          return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
        });
        std::vector<std::pair<const Sample*, u32>> stack_groups;
        for (const Sample* m : all) {
          if (stack_groups.empty() || std::memcmp(stack_groups.back().first->frames, m->frames, sizeof(m->frames)) != 0) {
            stack_groups.push_back({m, 1});
          } else {
            ++stack_groups.back().second;
          }
        }
        std::sort(stack_groups.begin(), stack_groups.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t k = 0; k < stack_groups.size() && k < 10; ++k) {
          std::fprintf(f, "   %5.1f%%  stack", double(stack_groups[k].second) * 100.0 / double(all.size()));
          for (size_t j = 0; j < kFrames && stack_groups[k].first->frames[j]; ++j) {
            std::fputs(j == 0 ? " " : " <- ", f);
            PrintAddr(f, stack_groups[k].first->frames[j]);
          }
          std::fputc('\n', f);
        }
      }
    }
  }

  for (size_t i : order) {
    const Slot& s = g_slots[i];
    /*
     * Threads that use no CPU are listed too, with less detail: in a hang their
     * stacks are needed to know what they are waiting for.
     */
    const bool active = s.cpu >= 1.0;
    u64 tid = 0;
    svcGetThreadId(&tid, s.handle.load());
    // Horizon priority, preferred core and allowed cores: who can take the CPU from whom.
    s32 priority = -1;
    s32 preferred = -1;
    u64 mask = 0;
    svcGetThreadPriority(&priority, s.handle.load());
    svcGetThreadCoreMask(&preferred, &mask, s.handle.load());
    std::fprintf(f,
                 "\n-- thread %" PRIu64 " \"%s\": CPU %.1f%% | priority 0x%X, preferred core %d, "
                 "mask 0x%llX | stack %llu KB deepest sampled (all threads %llu KB) | per second: yields %.0f, "
                 "sleeps up to 1 ms %.0f (%.2f ms each), longer sleeps %.1f\n",
                 tid, s.name[0] ? s.name : "?", s.cpu, static_cast<unsigned>(priority),
                 static_cast<int>(preferred), static_cast<unsigned long long>(mask),
                 static_cast<unsigned long long>(s.stack_deepest.load(std::memory_order_relaxed) >> 10),
                 static_cast<unsigned long long>(g_stack_deepest_all.load(std::memory_order_relaxed) >> 10),
                 s.yields_s, s.sleeps_s, s.sleep_ms, s.long_s);

    auto lo = std::lower_bound(samples.begin(), samples.end(), i,
                               [](const Sample& a, size_t v) { return a.slot < v; });
    auto hi = std::upper_bound(samples.begin(), samples.end(), i,
                               [](size_t v, const Sample& a) { return v < a.slot; });
    const size_t n = size_t(hi - lo);
    if (n == 0) {
      std::fprintf(f, "   no samples\n");
      continue;
    }
    std::vector<u64> pcs, lrs;
    size_t in_svc = 0;
    for (auto it = lo; it != hi; ++it) {
      if (AfterSvc(it->pc)) {
        ++in_svc;
        continue;
      }
      pcs.push_back(it->pc);
      lrs.push_back(it->lr);
    }
    std::fprintf(f, "   %zu samples, %.0f%% waiting in the kernel\n", n,
                 double(in_svc) * 100.0 / double(n));
    const auto pc_hist = Histogram(pcs);
    const size_t busy = pcs.size();
    // banjo-nx (as futari-nx): every pc of an active thread with its raw count, so tools/switch_hot_functions.py can
    // add a function's addresses up into its self time (the top 80 split hot functions and dropped their tail).
    for (size_t k = 0; k < pc_hist.size() && k < (active ? 100000u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  pc ", double(pc_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, pc_hist[k].addr);
      std::fprintf(f, " x%u\n", unsigned(pc_hist[k].n));
    }
    const auto lr_hist = Histogram(lrs);
    for (size_t k = 0; k < lr_hist.size() && k < (active ? 40u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  lr ", double(lr_hist[k].n) * 100.0 / double(busy));
      PrintAddr(f, lr_hist[k].addr);
      std::fputc('\n', f);
    }
    // banjo-nx (2026-10-28): who calls the C library's copies (a leaf keeps its caller in lr): the samples whose pc is
    // in memcpy, memmove or memset (their first 0x800 bytes), by lr, as "lrleaf image+0x... xN" lines.
    if (active) {
      static const u64 kLeaf[3] = {reinterpret_cast<u64>(&std::memcpy), reinterpret_cast<u64>(&std::memmove),
                                   reinterpret_cast<u64>(&std::memset)};
      std::vector<u64> leaf_lrs;
      for (auto it = lo; it != hi; ++it) {
        for (const u64 start : kLeaf) {
          if (it->pc - start < 0x800) {
            leaf_lrs.push_back(it->lr);
            break;
          }
        }
      }
      const auto leaf_hist = Histogram(leaf_lrs);
      for (size_t k = 0; k < leaf_hist.size() && k < 30u; ++k) {
        std::fprintf(f, "   %5.1f%%  lrleaf ", double(leaf_hist[k].n) * 100.0 / double(busy));
        PrintAddr(f, leaf_hist[k].addr);
        std::fprintf(f, " x%u\n", unsigned(leaf_hist[k].n));
      }
    }

    // Full stacks, waits included: they say what the thread is waiting for.
    std::vector<const Sample*> by_stack;
    for (auto it = lo; it != hi; ++it) {
      by_stack.push_back(&*it);
    }
    std::sort(by_stack.begin(), by_stack.end(), [](const Sample* a, const Sample* b) {
      return std::memcmp(a->frames, b->frames, sizeof(a->frames)) < 0;
    });
    struct StackCount {
      const Sample* s;
      u32 n;
    };
    std::vector<StackCount> stacks;
    for (const Sample* s : by_stack) {
      if (stacks.empty() ||
          std::memcmp(stacks.back().s->frames, s->frames, sizeof(s->frames)) != 0) {
        stacks.push_back({s, 1});
      } else {
        ++stacks.back().n;
      }
    }
    std::sort(stacks.begin(), stacks.end(),
              [](const StackCount& a, const StackCount& b) { return a.n > b.n; });
    for (size_t k = 0; k < stacks.size() && k < (active ? 30u : 3u); ++k) {
      std::fprintf(f, "   %5.1f%%  stack", double(stacks[k].n) * 100.0 / double(n));
      for (size_t j = 0; j < kFrames && stacks[k].s->frames[j]; ++j) {
        std::fputs(j == 0 ? " " : " <- ", f);
        PrintAddr(f, stacks[k].s->frames[j]);
      }
      std::fputc('\n', f);
    }
    // Kernel waits (svc) grouped by their first callers only: full stacks of waits are scattered across many
    // distinct stacks and never reach the list above, so a contended lock had no visible owner.
    if (active && in_svc) {
      struct Wait {
        u64 frames[5];
        u32 n;
      };
      std::vector<Wait> waits;
      for (auto it = lo; it != hi; ++it) {
        if (!AfterSvc(it->pc)) continue;
        Wait e{};
        for (size_t j = 0; j < 5 && j < kFrames; ++j) e.frames[j] = it->frames[j];
        auto same = std::find_if(waits.begin(), waits.end(), [&](const Wait& o) {
          return std::memcmp(o.frames, e.frames, sizeof(e.frames)) == 0;
        });
        if (same == waits.end()) {
          e.n = 1;
          waits.push_back(e);
        } else {
          ++same->n;
        }
      }
      std::sort(waits.begin(), waits.end(), [](const Wait& a, const Wait& b) { return a.n > b.n; });
      for (size_t k = 0; k < waits.size() && k < 12; ++k) {
        std::fprintf(f, "   %5.1f%%  wait", double(waits[k].n) * 100.0 / double(n));
        for (size_t j = 0; j < 5 && waits[k].frames[j]; ++j) {
          std::fputs(j == 0 ? " " : " <- ", f);
          PrintAddr(f, waits[k].frames[j]);
        }
        std::fputc('\n', f);
      }
    }
  }
  std::fputc('\n', f);
  std::fclose(f);
}

/*
 * The one-second tick for the console overlays, separate from the profile loop.
 *
 * Two loops use it: the warm-up one (from second zero) and the normal one (from kStartDelayNs on).
 * When it lived only in the normal loop there was neither FPS nor resolution until 8 seconds had
 * passed: they appeared with the EA logo instead of with the first splash screen, which comes much
 * earlier.
 *
 * It is cheap: two counter reads and a few bytes in shared memory. Update also re-attaches the
 * block if it is missing, so the overlay can be opened at any time.
 */
struct TicOverlay {
  u64 tic = 0;
  u64 presented = 0;
  double media[10] = {};
  size_t pos = 0;

  void Boot(u64 now) {
    tic = now;
    presented = CounterRead(0);
    // Publish what is known right away: the resolution is valid from the first instant (g_resolution
    // starts at 1280x720 and the game corrects it when choosing the video mode), so the overlay's RES
    // row can show up even before the first frame.
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Update(0.0, 0.0, res >> 16, res & 0xFFFF, presented);
  }

  // Once per second. `with_clocks` turns off the Reverse-NX and clock sysmodule part, which is better
  // left alone during warm-up.
  void Step(u64 now, u64 freq, bool with_clocks) {
    if (now - tic < freq) {
      return;
    }
    const double seconds = double(now - tic) / double(freq);
    const u64 presented_now = CounterRead(0);
    const double fps = seconds > 0.0 ? double(presented_now - presented) / seconds : 0.0;
    media[pos] = fps;
    pos = (pos + 1) % 10;
    double sum = 0.0;
    size_t how_many = 0;
    for (double v : media) {
      if (v > 0.0) {
        sum += v;
        ++how_many;
      }
    }
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Update(fps, how_many ? sum / double(how_many) : fps, res >> 16, res & 0xFFFF,
                                        presented_now);
    // Reverse-NX is also told that the game keeps asking for the mode (its overlay requires it to show
    // the controls) and, if the system rules, the real mode is mirrored.
    // And if requested, the clock sysmodule is asked for its docked clocks.
    if (with_clocks) {
      const bool docked_real = appletGetOperationMode() == AppletOperationMode_Console;
      const bool docked = rex::ui::switch_saltynx::BaseMode(docked_real);
      rex::ui::switch_sysclk::FollowMode(docked, docked_real);
    }
    /*
     * The memory clock is watched here too. It is outside the if above on purpose: the
     * system raises the EMC right after we request the GPU profile, that is, during
     * warm-up, which is when with_clocks is false. It is one clkrst read per second for
     * the first 20 and one every 30 after that; when we have not changed anything it
     * does not even do that.
     */
    RexSwitchApmWatch();
    presented = presented_now;
    tic = now;
  }
};

// sa2-nx, second CPU round (2026-10-12): what one access of FPCR costs on this CPU. The recompiled code switches the
// host's float flush mode with `msr fpcr`, and every choice between "a test in each conversion" and "a switch" (the
// generator's fp_moves_modeless, fp_mode_lookahead and fp_copy_pairs) hangs on that cost, which had never been
// measured on the A57. Four loops of the same shape, the best of five runs each: empty, a read, a write of the
// value already there, a write that toggles FZ (what a real switch does). Run once, for the report's header.
// renut-ultimate (2026-11-13, from sasrt-nx's handoff of 2026-11-13): also FPSR's read and clear, and one exclusive
// increment of a cache-resident word, relaxed (ldxr/stxr) and acquire/release (ldaxr/stlxr): what a guest lwarx/stwcx.
// pair costs in each form (sasrt-nx's A57, handheld: 17.6 and 26.4 ns).
struct FpcrCost {
  double read_ns, write_same_ns, write_toggle_ns;
  double fpsr_read_ns = 0, fpsr_clear_ns = 0;
  double llsc_relaxed_ns = 0, llsc_acqrel_ns = 0;
};
FpcrCost MeasureFpcrCost(u64 tick_freq) {
  constexpr int kCount = 200000;
  u64 saved;
  asm volatile("mrs %0, fpcr" : "=r"(saved));
  const auto loop = [&](int kind) {
    u64 best = ~u64(0);
    for (int run = 0; run < 5; ++run) {
      u64 v = saved;
      const u64 t0 = armGetSystemTick();
      for (int i = 0; i < kCount; ++i) {
        if (kind == 1) {
          asm volatile("mrs %0, fpcr" : "=r"(v));
        } else if (kind == 2) {
          asm volatile("msr fpcr, %0" : : "r"(v));
        } else if (kind == 3) {
          v ^= u64(1) << 24;  // FZ
          asm volatile("msr fpcr, %0" : : "r"(v));
        } else if (kind == 4) {
          asm volatile("mrs %0, fpsr" : "=r"(v));
        } else if (kind == 5) {
          asm volatile("msr fpsr, %0" : : "r"(u64(0)));
        } else if (kind == 6 || kind == 7) {  // an exclusive increment, as a guest lwarx/stwcx.
          static u32 word = 0;
          u32 t, fail;
          if (kind == 6)
            asm volatile("1: ldxr %w0, [%2]\n add %w0, %w0, #1\n stxr %w1, %w0, [%2]\n cbnz %w1, 1b"
                         : "=&r"(t), "=&r"(fail) : "r"(&word) : "memory");
          else
            asm volatile("1: ldaxr %w0, [%2]\n add %w0, %w0, #1\n stlxr %w1, %w0, [%2]\n cbnz %w1, 1b"
                         : "=&r"(t), "=&r"(fail) : "r"(&word) : "memory");
        } else {
          asm volatile("" : "+r"(v));
        }
      }
      const u64 t = armGetSystemTick() - t0;
      if (t < best) best = t;
      asm volatile("msr fpcr, %0" : : "r"(saved));
    }
    return double(best) * 1e9 / double(tick_freq) / double(kCount);
  };
  const double empty = loop(0);
  u64 fpsr_saved;
  asm volatile("mrs %0, fpsr" : "=r"(fpsr_saved));
  FpcrCost out{loop(1) - empty, loop(2) - empty, loop(3) - empty};
  out.fpsr_read_ns = loop(4) - empty;
  out.fpsr_clear_ns = loop(5) - empty;
  out.llsc_relaxed_ns = loop(6) - empty;
  out.llsc_acqrel_ns = loop(7) - empty;
  asm volatile("msr fpsr, %0" : : "r"(fpsr_saved));
  return out;
}

void ProfilerMain(void*) {
  const u64 tick_freq = armGetSystemTickFreq();

  /*
   * The overlay first, without waiting for kStartDelayNs. Those 8 seconds are for stack sampling and
   * the first report (which read the SD and pause threads), not for this: publishing FPS and
   * resolution is a few bytes in shared memory, and it has to show already at the first splash screen.
   * During warm-up the tick keeps running every second, so attaching the block is also retried if
   * SaltyNX is slow.
   */
  TicOverlay overlay;
  const u64 start = armGetSystemTick();
  overlay.Boot(start);
  const u64 warmup = (kStartDelayNs / 1000000ULL) * tick_freq / 1000ULL;
  while (armGetSystemTick() - start < warmup) {
    __real_svcSleepThread(kPassiveIntervalNs);
    overlay.Step(armGetSystemTick(), tick_freq, false);
  }

  // blur-nx (2026-11-17, audit #29): switch_perf_count_calls (the title's cvar; true when it has none).
  {
    const std::string value = rex::cvar::GetFlagByName("switch_perf_count_calls");
    if (value == "false" || value == "0") g_count_calls.store(false, std::memory_order_relaxed);
  }
  // Invasive sampling is only enabled explicitly, for diagnostics.
  // The normal mode keeps FPS, CPU and counters, without stopping the game.
  bool sample_stacks = false;
  if (FILE* flag = std::fopen(StackFlagPath().c_str(), "r")) {
    sample_stacks = true;
    std::fclose(flag);
  }
  // banjo-nx (as futari-nx): also from the config, so a profiling config works without copying the flag file to
  // the SD card (the title defines the cvar switch_profile_stacks).
  {
    const std::string value = rex::cvar::GetFlagByName("switch_profile_stacks");
    if (value == "true" || value == "1") sample_stacks = true;
  }
  // blur-nx (2026-11-16, the 15 ms batch): the interval between two stack samples, from the title's cvar
  // switch_profile_stacks_ms (1 when it has none). Each sample pauses one game thread (svcSetThreadActivity), reads its
  // context and stack and resumes it, from a thread above every game thread: at 1 ms that is ~1,000 pauses and ~5,000
  // system calls a second, which no report counts (the sampler leaves itself out).
  u64 sample_interval_ns = kSampleIntervalNs;
  {
    const std::string value = rex::cvar::GetFlagByName("switch_profile_stacks_ms");
    const long ms = value.empty() ? 0 : std::strtol(value.c_str(), nullptr, 10);
    if (ms >= 1 && ms <= 100) sample_interval_ns = u64(ms) * 1000000ULL;
  }

  u64 counters_last[kCounterCount]{};
  for (unsigned i = 0; i < kCounterCount; ++i) {
    counters_last[i] = CounterRead(i);
  }
  // banjo-nx (2026-11-01, the public build; port 230): rex_profile.log only with logging on (log_level not "off"),
  // or with stack sampling; port 193's copy of the last one only then too.
  g_write_reports = rex::cvar::GetFlagByName("log_level") != "off" || sample_stacks;
  // Port 193 (TFU-nx's): the last launch's report kept as rex_profile.prev.log, and the build stamp first.
  if (g_write_reports) {
    const std::string prev = std::string(RexSwitchLogDir()) + "rex_profile.prev.log";
    std::remove(prev.c_str());
    if (std::rename(ReportPath().c_str(), prev.c_str()) != 0) std::remove(ReportPath().c_str());
  }
  if (FILE* f = g_write_reports ? std::fopen(ReportPath().c_str(), "w") : nullptr) {
    std::fprintf(f, "%s\n", RexAppBuildStamp());
    std::fprintf(f, "image 0x%016" PRIx64 ", code up to 0x%016" PRIx64 "\n\n", g_base,
                 g_text_hi);
    std::fprintf(f, "Stack sampling: %s (%u ms)\n", sample_stacks ? "active" : "disabled",
                 unsigned(sample_interval_ns / 1000000ULL));
    const FpcrCost fpcr = MeasureFpcrCost(tick_freq);
    std::fprintf(f,
                 "FPCR on this CPU (per access, loop overhead taken off): read %.1f ns, write of the same value "
                 "%.1f ns, write that toggles the flush mode %.1f ns (one cycle is 0.98 ns at 1020 MHz); FPSR: read "
                 "%.1f ns, write of zero %.1f ns; an exclusive increment: relaxed (ldxr/stxr) %.1f ns, acquire/release "
                 "(ldaxr/stlxr) %.1f ns\n",
                 fpcr.read_ns, fpcr.write_same_ns, fpcr.write_toggle_ns, fpcr.fpsr_read_ns, fpcr.fpsr_clear_ns,
                 fpcr.llsc_relaxed_ns, fpcr.llsc_acqrel_ns);
    std::fclose(f);
  }

  // The console overlays read from SaltyNX's shared memory. That is already running from second
  // zero (see the warm-up loop above); only the normal loop continues here.

  // A short first report just to set the starting CPU time.
  u64 report_start = armGetSystemTick();
  Report(1, tick_freq, counters_last, "startup");
  report_start = armGetSystemTick();

  size_t rr = 0;
  size_t mode_index = 0;
  while (true) {
    __real_svcSleepThread(sample_stacks ? sample_interval_ns : kPassiveIntervalNs);

    for (size_t tries = 0; sample_stacks && tries < kMaxThreads; ++tries) {
      const size_t idx = rr;
      rr = (rr + 1) % kMaxThreads;
      Slot& s = g_slots[idx];
      const u32 h = s.handle.load();
      if (!h || !s.busy) {
        continue;
      }
      if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
        break;
      }
      ThreadContext ctx;
      const Result rc = svcGetThreadContext3(&ctx, h);
      if (R_SUCCEEDED(rc)) {  // sasrt-nx (2026-11-09; port 58): the stack's depth at this sample (top: the mapping's end)
        MemoryInfo dmi;
        u32 dpi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&dmi, &dpi, ctx.sp)) && (dmi.perm & Perm_R)) {
          const u64 top = dmi.addr + dmi.size;
          const u64 depth = top > ctx.sp ? top - ctx.sp : 0;
          if (depth > s.stack_deepest.load(std::memory_order_relaxed)) {
            s.stack_deepest.store(depth, std::memory_order_relaxed);
            if (depth > g_stack_deepest_all.load(std::memory_order_relaxed))
              g_stack_deepest_all.store(depth, std::memory_order_relaxed);
          }
        }
      }
      if (R_SUCCEEDED(rc) && g_sample_count < kMaxSamples) {
        Sample& out = g_samples[g_sample_count++];
        out = {};
        out.pc = ctx.pc.x;
        out.lr = ctx.lr;
        out.slot = static_cast<u16>(idx);
        out.tick = armGetSystemTick();
        // The stack, with the thread still paused: it is our own memory and
        // there are no locks. Only frames within the region of its sp.
        MemoryInfo smi;
        u32 spi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&smi, &spi, ctx.sp)) && (smi.perm & Perm_R)) {
          const u64 lo = smi.addr, hi = smi.addr + smi.size;
          u64 fp = ctx.fp;
          // banjo-nx (2026-10-20, SASRT's report): x29 can hold anything (recompiled code uses it as a general
          // register); fp + 16 wrapped past 2^64 for fp near it and passed, and the walk read 0xFFFFFFFFFFFFFFF8.
          for (size_t k = 0; k < kFrames && fp >= lo && fp < hi && hi - fp >= 16 && (fp & 7) == 0; ++k) {
            out.frames[k] = reinterpret_cast<const u64*>(fp)[1];
            const u64 next = reinterpret_cast<const u64*>(fp)[0];
            if (next <= fp) {
              break;
            }
            fp = next;
          }
        }
      }
      svcSetThreadActivity(h, ThreadActivity_Runnable);
      break;
    }

    const u64 now = armGetSystemTick();
    // Once per second, FPS and resolution for the overlay (the profile report runs every 10 s).
    overlay.Step(now, tick_freq, true);
    if (now - report_start >= kReportSeconds * tick_freq) {
      // sasrt-nx (2026-11-11, port 71): the report sorts and groups up to 32k stack samples and writes ~800 lines to
      // the SD, and it ran here at 0x2A, above every game thread, on a core the game's threads use. Only the 1 ms
      // samples need 0x2A (to be on time): the report is written at the game's lowest priority (0x3B), from the system
      // core when the process has one (RexSwitchSystemCore, four-core mode), then the sampler goes back to its priority
      // and core. Very likely this removed a ~100 ms render stall in every 10 s window: sasrt-nx's fixed-resolution
      // races at the handheld clocks, stack sampling off, had a ~105 ms worst frame in 5 of 7 windows before the move and
      // 17-28 ms in every window after (different builds and drivers, not a same-build A/B). Its earlier long frames
      // every 10 s with screenshots were the test bot's own (sys-botbase's pixelPeek stalls the game ~250 ms a capture).
      s32 report_core = -2;
      u64 report_mask = 0;
      const int system_core = RexSwitchSystemCore();
      const bool moved = system_core >= 0 &&
                         R_SUCCEEDED(svcGetThreadCoreMask(&report_core, &report_mask, CUR_THREAD_HANDLE)) &&
                         R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, system_core, 1ull << system_core));
      svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
      Report(now - report_start, tick_freq, counters_last, kModes[mode_index].name);
      if (moved) svcSetThreadCoreMask(CUR_THREAD_HANDLE, report_core, report_mask);
      svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2A);
      if (g_ab_active.load(std::memory_order_relaxed)) {
        mode_index = (mode_index + 1) % kModeCount;
        g_skip_mask.store(kModes[mode_index].mask, std::memory_order_relaxed);
        if (mode_index == 0) {
          // Full cycle: it turns itself off and the game looks right again.
          g_ab_active.store(false, std::memory_order_relaxed);
        }
      }
      report_start = armGetSystemTick();
    }
  }
}

/* Priority 102: after switch_crash_hooks.c, before the SDK. */
__attribute__((constructor(102))) void StartProfiler() {
  g_base = reinterpret_cast<u64>(&_start);
  MemoryInfo mi;
  u32 page_info = 0;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &page_info, g_base))) {
    g_text_lo = mi.addr;
    g_text_hi = mi.addr + mi.size;
  }
  Register(envGetMainThreadHandle());
  std::strncpy(g_slots[0].name, "main (UI)", sizeof(g_slots[0].name) - 1);
  // 0x2A: above everything in the game (audio runs at 0x2B), so the samples
  // are taken on time. It sleeps almost all the time.
  if (R_SUCCEEDED(__real_threadCreate(&g_thread, ProfilerMain, nullptr, nullptr, 0x10000, 0x2A,
                                      -2))) {
    threadStart(&g_thread);
  }
}

}  // namespace

extern "C" {

/*
 * banjo-nx: optional per-thread hooks for an app-side profiler (app/src/profiler, ported from
 * UnleashedRecomp-NX). Weak: titles that do not define them pay one null check per thread event.
 */
void RexSwitchAppThreadCreated(u32 handle) __attribute__((weak));
void RexSwitchAppThreadNamed(u32 handle, const char* name) __attribute__((weak));
void RexSwitchAppThreadClosed(u32 handle) __attribute__((weak));

Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                           size_t stack_sz, int prio, int cpuid) {
  const Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
  if (R_SUCCEEDED(rc) && t) {
    Register(t->handle);
    if (RexSwitchAppThreadCreated) {
      RexSwitchAppThreadCreated(t->handle);
    }
  }
  return rc;
}

Result __wrap_threadClose(Thread* t) {
  if (t) {
    Unregister(t->handle);
    if (RexSwitchAppThreadClosed) {
      RexSwitchAppThreadClosed(t->handle);
    }
  }
  return __real_threadClose(t);
}

/* libnx calls that cost IPC or walk memory. See CallShard. */
Result __wrap_nvFenceWait(NvFence* f, s32 timeout_us) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvFenceWait(f, timeout_us);
  Note(timeout_us == 0 ? kFenceQuery : kFenceWait, from);
  return rc;
}

// Port 167 (from fnxf_xiii): plume_deko3d_commands.cpp, the channel's report of a failed kickoff (weak: a build
// without plume's deko3d back end has none).
extern "C" __attribute__((weak)) void RexSwitchGpuKickoffFailed(NvGpuChannel* channel, uint32_t rc);

Result __wrap_nvGpuChannelKickoff(NvGpuChannel* c) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvGpuChannelKickoff(c);
  Note(kKickoff, from);
  if (R_FAILED(rc) && RexSwitchGpuKickoffFailed)
    RexSwitchGpuKickoffFailed(c, rc);
  return rc;
}

Result __wrap_nvMapCreate(NvMap* m, void* cpu_addr, u32 size, u32 align, NvKind kind, bool is_cpu_cacheable) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvMapCreate(m, cpu_addr, size, align, kind, is_cpu_cacheable);
  Note(is_cpu_cacheable ? kNvMapCached : kNvMapNoCache, from, size);
  return rc;
}

Result __wrap_nvAddressSpaceAllocFixed(NvAddressSpace* a, bool sparse, u64 size, iova_t iova) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvAddressSpaceAllocFixed(a, sparse, size, iova);
  Note(kReserveAddress, from, size);
  return rc;
}

Result __wrap_nvioctlNvhostAsGpu_MapBufferEx(u32 fd, u32 flags, u32 kind, u32 nvmap_handle, u32 page_size,
                                             u64 buffer_offset, u64 mapping_size, u64 input_offset, u64* offset) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_nvioctlNvhostAsGpu_MapBufferEx(fd, flags, kind, nvmap_handle, page_size, buffer_offset,
                                                          mapping_size, input_offset, offset);
  Note(kMappingGpu, from, mapping_size);
  return rc;
}

void __wrap_armDCacheClean(void* addr, size_t size) {
  const u64 from = armGetSystemTick();
  __real_armDCacheClean(addr, size);
  Note(kCacheClean, from, size);
}

/*
 * The presentation interval (masseffect_interval_swap).
 *
 * It is reapplied on every present, not when the chain is created, on purpose: the WSI sets it to 1
 * when creating the swapchain, and the chain is recreated when switching from docked to handheld.
 * With 0 nothing is touched and the swapchain mode rules (IMMEDIATE -> interval 0).
 *
 * libnx only accepts interval 0 if the NWindow has three or more buffers; the Horizon WSI always
 * configures exactly 3, so the condition holds. 2 is always accepted.
 *
 * Warning: it is 0 and must stay that way. Measured: with the current frame time, setting 2 sends
 * 22 % of the frames to 66.7 ms. It only makes sense with a median frame time below 31 ms.
 */
std::atomic<unsigned> g_interval_swap{0};

/*
 * renut-ultimate (2026-11-16, port 145; tw2de_nx's tw2de_async_present of 2026-11-13, ON at the user's word: "port the
 * speed ups as on by default"; the key banjo_async_present, app/src/renut_engine/gpu/banjo_renut_tuning.cpp): the
 * window's two binder calls off the presenting thread.
 *
 * nwindowQueueBuffer and the bqDequeueBuffer inside nwindowDequeueBuffer are binder transactions answered by the
 * compositor's process: tw2de_nx measured 1.40 and 1.33 ms a call in its heaviest 10 s, 2.7 ms a frame in which the
 * renderer's feed thread only waits. With the key on, a helper thread makes them: the queue call returns at once, the
 * helper queues the buffer and right away dequeues the next one, which the next nwindowDequeueBuffer call takes
 * (waiting for the helper if it is not there yet). The same calls on the window in the same order (queue N, dequeue,
 * queue N+1, ...), so the same buffers at the same vblanks; only the caller no longer waits for the answers. A dequeue
 * that finds no free buffer blocks the helper as it blocked the caller, and the caller then waits at its next dequeue:
 * the pacing at the frame cap is the same. RexSwitchPerfPresentQuiesce: before a swapchain lets go of the window
 * (libnx refuses to release its buffers while one is dequeued), the buffer dequeued ahead is cancelled (plume).
 */
struct AsyncPresent {
  Mutex mutex = 0;
  CondVar work = 0, done = 0;
  Thread thread{};
  std::atomic<int> state{0};   // 0 not asked yet, 1 on (the helper runs), -1 off
  bool queue_pending = false;  // a queue call the helper has not finished (the dequeue after it included)
  NWindow* nw = nullptr;
  s32 queue_slot = -1;
  bool queue_has_fence = false;
  NvMultiFence queue_fence{};
  bool held = false;           // the dequeue made ahead, not yet taken by a caller
  Result held_rc = 0;
  s32 held_slot = -1;
  NvMultiFence held_fence{};
};
static AsyncPresent g_async;

static void AsyncPresentMain(void*) {
  AsyncPresent& a = g_async;
  mutexLock(&a.mutex);
  for (;;) {
    while (!a.queue_pending) condvarWait(&a.work, &a.mutex);
    NWindow* const nw = a.nw;
    const s32 slot = a.queue_slot;
    const bool has_fence = a.queue_has_fence;
    const NvMultiFence fence = a.queue_fence;
    mutexUnlock(&a.mutex);
    const u64 from = armGetSystemTick();
    __real_nwindowQueueBuffer(nw, slot, has_fence ? &fence : nullptr);
    Note(kQueueBuffer, from);
    s32 next = -1;
    NvMultiFence next_fence{};
    const Result rc = __real_nwindowDequeueBuffer(nw, &next, &next_fence);
    mutexLock(&a.mutex);
    a.held = true;
    a.held_rc = rc;
    a.held_slot = next;
    a.held_fence = next_fence;
    a.queue_pending = false;
    condvarWakeAll(&a.done);
  }
}

// Whether the helper makes the calls (decided at the first queue call: the config is read by then).
static bool AsyncPresentOn() {
  AsyncPresent& a = g_async;
  int state = a.state.load(std::memory_order_acquire);
  if (state == 0) [[unlikely]] {
    const std::string value = rex::cvar::GetFlagByName("banjo_async_present");
    state = -1;
    if (value == "true" || value == "1") {
      // Cores 0 and 2 (not the feed thread's core 1), at the top priority the process may use: it runs for
      // microseconds between its waits.
      if (R_SUCCEEDED(threadCreate(&a.thread, AsyncPresentMain, nullptr, nullptr, 0x10000, 0x2C, 2))) {
        svcSetThreadCoreMask(a.thread.handle, 2, 0x5);
        if (R_SUCCEEDED(threadStart(&a.thread))) state = 1;
      }
    }
    a.state.store(state, std::memory_order_release);
  }
  return state > 0;
}

// The queue call handed to the helper. False: not handed (the caller makes it).
static bool AsyncPresentQueue(NWindow* nw, s32 slot, const NvMultiFence* fence) {
  AsyncPresent& a = g_async;
  mutexLock(&a.mutex);
  while (a.queue_pending) condvarWait(&a.done, &a.mutex);
  if (a.held) {
    // A buffer dequeued ahead that no caller took: this slot did not come from it. Not expected (a caller dequeues
    // before it queues); the caller makes this call itself.
    mutexUnlock(&a.mutex);
    return false;
  }
  a.nw = nw;
  a.queue_slot = slot;
  a.queue_has_fence = fence != nullptr;
  if (fence) a.queue_fence = *fence;
  a.queue_pending = true;
  condvarWakeOne(&a.work);
  mutexUnlock(&a.mutex);
  return true;
}

Result __wrap_nwindowDequeueBuffer(NWindow* nw, s32* out_slot, NvMultiFence* out_fence) {
  AsyncPresent& a = g_async;
  if (a.state.load(std::memory_order_acquire) <= 0) return __real_nwindowDequeueBuffer(nw, out_slot, out_fence);
  mutexLock(&a.mutex);
  while (a.queue_pending) condvarWait(&a.done, &a.mutex);
  if (a.held && a.nw == nw) {
    a.held = false;
    const Result rc = a.held_rc;
    if (R_SUCCEEDED(rc)) {
      if (out_slot) *out_slot = a.held_slot;
      if (out_fence) *out_fence = a.held_fence;
    }
    mutexUnlock(&a.mutex);
    return rc;
  }
  mutexUnlock(&a.mutex);
  return __real_nwindowDequeueBuffer(nw, out_slot, out_fence);
}

void RexSwitchPerfPresentQuiesce(NWindow* nw) {
  AsyncPresent& a = g_async;
  if (a.state.load(std::memory_order_acquire) <= 0) return;
  mutexLock(&a.mutex);
  while (a.queue_pending) condvarWait(&a.done, &a.mutex);
  if (a.held && a.nw == nw) {
    a.held = false;
    if (R_SUCCEEDED(a.held_rc)) nwindowCancelBuffer(nw, a.held_slot, &a.held_fence);
  }
  mutexUnlock(&a.mutex);
}

Result __wrap_nwindowQueueBuffer(NWindow* nw, s32 slot, const NvMultiFence* fence) {
  const u64 from = armGetSystemTick();
  const unsigned request = g_interval_swap.load(std::memory_order_relaxed);
  if (request != 0 && nw != nullptr && nw->swap_interval != request) {
    nwindowSetSwapInterval(nw, request);
  }
  // Port 145 (banjo_async_present): handed to the helper when it runs.
  if (AsyncPresentOn() && AsyncPresentQueue(nw, slot, fence))
    return 0;
  const Result rc = __real_nwindowQueueBuffer(nw, slot, fence);
  Note(kQueueBuffer, from);
  return rc;
}

Result __wrap_bqDequeueBuffer(Binder* b, bool async, u32 width, u32 height, s32 format, u32 usage, s32* buf,
                              NvMultiFence* fence) {
  const u64 from = armGetSystemTick();
  const Result rc = __real_bqDequeueBuffer(b, async, width, height, format, usage, buf, fence);
  Note(kDequeueBuffer, from);
  return rc;
}

// blur-nx (2026-11-13, M-44): the calling thread's slot, found once (none: not registered, e.g. the main thread).
Slot* CurrentSlot() {
  thread_local size_t index = SIZE_MAX;
  if (index == SIZE_MAX) [[unlikely]] {
    const u32 h = threadGetCurHandle();
    index = kMaxThreads;
    for (size_t i = 0; i < kMaxThreads; ++i) {
      if (g_slots[i].handle.load(std::memory_order_relaxed) == h) {
        index = i;
        break;
      }
    }
  }
  return index < kMaxThreads ? &g_slots[index] : nullptr;
}

void AddOwn(std::atomic<u64>& counter, u64 value) {  // one writer: a load and a store
  counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

void __wrap_svcSleepThread(s64 nano) {
  if (!g_count_calls.load(std::memory_order_relaxed)) {  // blur-nx (2026-11-17, audit #29)
    __real_svcSleepThread(nano);
    return;
  }
  const u64 from = armGetSystemTick();
  __real_svcSleepThread(nano);
  Note(nano == 0 ? kSleep0 : nano < 0 ? kSleepYield : nano <= 1000000 ? kSleepShort : kSleepLong, from);
  if (Slot* slot = CurrentSlot()) {  // blur-nx (2026-11-13, M-44): the thread's own counters
    if (nano <= 0) {
      AddOwn(slot->yields, 1);
    } else if (nano <= 1000000) {
      AddOwn(slot->sleeps, 1);
      AddOwn(slot->sleep_ticks, armGetSystemTick() - from);
    } else {
      AddOwn(slot->long_sleeps, 1);
    }
  }
}

/*
 * Counters: 0 = guest present, 1 = read emulated through the shadow,
 * 2 = fault resolved by an SDK handler, 3 = retry emulated through the
 * shadow, 4 = SEH. 28..32 = draws per surface width from IssueDraw
 * (1600 or more, 1280-1599, 640-1279, 256-639, under 256).
 */
// A long frame, in armGetSystemTick ticks (see WindowHitch).
void RexSwitchPerfHitch(u64 start, u64 end) {
  const u32 i = g_num_hitches.fetch_add(1);
  if (i < kMaxHitches) {
    g_hitches[i] = {start, end};
  }
}

void RexSwitchPerfExclusiveCore(int core) {
  // 10 + K: core K, light guest threads allowed on it too.
  // 10 + K: light guest threads allowed on K; 20 + K: the render thread raised above the ring thread.
  // Both may be combined: 30 + K.
  if (core < 0) return;
  g_core_render.store(core >= 20, std::memory_order_relaxed);
  g_light_core.store((core % 20) >= 10, std::memory_order_relaxed);
  g_exclusive_core.store(core % 10, std::memory_order_relaxed);
}

void RexSwitchPerfResolution(unsigned w, unsigned h) {
  g_resolution.store(((w & 0xFFFF) << 16) | (h & 0xFFFF), std::memory_order_relaxed);
}

// See g_interval_swap, next to the nwindowQueueBuffer wrapper. 0 = touch nothing (the normal case).
void RexSwitchPerfIntervalSwap(unsigned vblanks) {
  g_interval_swap.store(vblanks, std::memory_order_relaxed);
}

void RexSwitchPerfCount(unsigned id) {
  if (id < kCounterCount) {
    CounterOf(id).fetch_add(1, std::memory_order_relaxed);
  }
  if (id == 0) {
    // Counter 0 is presents. The console overlay clears the "alive" mark and the resolution mark and
    // only waits 100 ms, so it has to be answered on every frame, not once per second. It is writing a
    // few bytes to shared memory.
    const uint32_t res = g_resolution.load(std::memory_order_relaxed);
    rex::ui::switch_saltynx::Beat(res >> 16, res & 0xFFFF);
  }
}

/*
 * 5 draws, 6 scissor area of the draws (pixels), 7 queue submissions,
 * 9 resolves, 10 resolve area, 11 render target transfer calls,
 * 12 render targets transferred, 13 transfer draws, 14 textures loaded,
 * 15 shared memory bytes uploaded, 16 pipelines created, 17 physical memory
 * chunks committed on touch, 18 chunks mapped into a view on touch,
 * 21 clipped audio samples, 22 audio buffers with a peak of 0.98 or more.
 */
void RexSwitchPerfAdd(unsigned id, u64 value) {
  if (id < kCounterCount) {
    CounterOf(id).fetch_add(value, std::memory_order_relaxed);
  }
}

/* max counter (23 = audio mix peak in ten-thousandths; the report resets it to 0). */
void RexSwitchPerfMax(unsigned id, u64 value) {
  if (id >= kCounterCount) {
    return;
  }
  std::atomic<u64>& max = g_counter_shards[0].c[id];  // blur-nx (2026-11-13): set 0's (CounterShard)
  u64 actual = max.load(std::memory_order_relaxed);
  while (value > actual && !max.compare_exchange_weak(actual, value, std::memory_order_relaxed)) {
  }
}

/*
 * Bits: 1 draws, 2 transfers, 4 resolves, 8 texture loads,
 * 16 presentation effect, 32 tiled dispatch.
 */
bool RexSwitchPerfSkip(unsigned bit) {
  return (g_skip_mask.load(std::memory_order_relaxed) & bit) != 0;
}

void RexSwitchPerfSetThreadName(u32 handle, const char* name) {
  if (!handle || !name) {
    return;
  }
  // blur-nx (2026-11-12, CPU batch 3): a named thread's priority from switch_thread_priorities (threading_switch.cpp).
  RexSwitchApplyNamedPriority(handle, name);
  if (RexSwitchAppThreadNamed) {
    RexSwitchAppThreadNamed(handle, name);
  }
  for (auto& s : g_slots) {
    if (s.handle.load() == handle) {
      char tmp[sizeof(s.name)]{};
      std::strncpy(tmp, name, sizeof(tmp) - 1);
      std::memcpy(s.name, tmp, sizeof(tmp));
      return;
    }
  }
}

}  // extern "C"
