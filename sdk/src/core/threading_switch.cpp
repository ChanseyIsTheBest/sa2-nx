/**
 * @file        rex/core/threading_switch.cpp
 * @brief       Thread shim for Horizon: affinity, priority and suspension
 *
 * See threading_switch.h for why this layer exists and for the priority
 * scheme, which is what decides whether the game stutters or not.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include "threading_switch.h"

/*
 * switch.h is included here, privately. This file uses none of the rex::thread
 * types, so libnx polluting the global namespace (Thread, Timer, Event,
 * Handle...) does no harm. The header above does not include it, precisely so
 * that threading_posix.cpp does not suffer from it.
 */
#include <switch.h>

#include <errno.h>
#include <malloc.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>   // banjo-nx (2026-10-27): the system core
#include <cstring>  // banjo-nx (2026-10-20, from sa2-nx): RexSwitchSetThreadMaskOf
#include <mutex>
#include <string>        // blur-nx (2026-11-12): switch_thread_priorities
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>  // banjo-nx (2026-10-27): banjo_system_core
#include <sys/stat.h>   // banjo-nx (2026-10-27): mkdir (RexSwitchEnsureForwarderNpdm)
#include <cstdio>
#include <cstdlib>
#include <string>

// banjo-nx (2026-10-27, the user's request; NetherSX2_nx's 4-core mode, MIT, NaGaa95). See threading_switch.h.
REXCVAR_DEFINE_INT32(banjo_system_core, 1, "Banjo/Threads",
                     "When the process is granted all four cores (a forwarder whose NPDM allows CPU 0-3, as "
                     "NetherSX2_nx's): 0 = leave core 3 to the system, 1 = audio, the XMA decoder, the XAudio mixer "
                     "thread and shader compiles each pinned to core 3, every other thread on cores 0-2 "
                     "(NetherSX2_nx's layout), 2 = as 1 and the other threads allowed on core 3 too. No effect with "
                     "the usual three cores")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-27, after the first four-core console run: audio stutters with core 3 at 1-2 % idle; the system
// takes about half of it, our tenants took 41-55 %): the XMA Decoder off core 3 by default (it decodes ahead into
// buffers), and the XAudio mixer above the game threads' priority there.
REXCVAR_DEFINE_BOOL(banjo_system_core_xma, false, "Banjo/Threads",
                    "Four-core mode: the XMA Decoder thread on the system core too (it took 5-13 % of it in the city; "
                    "off: it runs with the game threads on cores 0-2)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(banjo_system_core_mixer_priority, 0x2D, "Banjo/Threads",
                     "Four-core mode: the XAudio mixer threads' priority on the system core (0x2D: below the audio "
                     "output (0x2A) and the Audio Worker (0x2B), above anything at the game threads' 0x3B; -1 keeps the "
                     "guest priority)")
    .range(-1, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// banjo-nx (2026-10-27, the user's request): the forwarder whose NPDM Banjo writes (RexSwitchEnsureForwarderNpdm).
REXCVAR_DEFINE_STRING(banjo_forwarder_program_id, "05D6EA1333487000", "Banjo/Threads",
                      "Program id (16 hex digits) of the forwarder that launches Banjo. Running under that id, Banjo "
                      "writes sdmc:/atmosphere/contents/<id>/exefs/main.npdm (an NPDM granting CPU 0-3, Atmosphere's "
                      "ExeFS file replacement) when it is missing or different; the next launch of the forwarder gets "
                      "all four cores (banjo_system_core). Under any other id (a game's, through title takeover) "
                      "nothing is written. Empty: never")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-18, the user: "make 4 core mode an option in the settings exposed to users just make it on by default
// and a warning that it is highly reccomended to be left on"): off, the forwarder's NPDM this program wrote is removed
// (only that file, byte for byte ours), so the forwarder's next launch has the three cores a homebrew launch gets.
REXCVAR_DEFINE_BOOL(four_core_mode, true, "Banjo/Threads",
                    "Four cores (CPU 0-3) when started from the forwarder banjo_forwarder_program_id names: its NPDM is "
                    "written at start (the forwarder's next launch gets them). Highly recommended. false: that NPDM "
                    "is removed (if it is the one written here), three cores from the forwarder's next launch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-12, CPU batch 3; codegen-ideas-3 CG-01): see RexSwitchApplyNamedPriority.
REXCVAR_DEFINE_STRING(switch_thread_priorities, "", "Threading",
                      "Host priorities for named threads, set when a thread takes its name: a comma-separated list "
                      "of NAME_PREFIX=PRIORITY (0x1C-0x3B; lower runs first; the game's threads are at 0x3B, the audio "
                      "threads at 0x2A-0x2D). Example: \"Game Master=0x3A,JQ Worker 0=0x3A\". Empty: no change")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-12, CPU batch 3; codegen-ideas-3 CG-02): see RexSwitchApplyNamedPriority.
REXCVAR_DEFINE_STRING(switch_thread_cores, "", "Threading",
                      "Host core masks for named threads, set when a thread takes its name: a comma-separated list of "
                      "NAME_PREFIX=MASK (bits 0-3 = cores 0-3, within the process's cores; the lowest core preferred). "
                      "Example: \"JQ Worker 3=0xC\". Empty: no change")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// blur-nx (2026-11-13, #60 of Blur's docs/cpu-near-exact-50-1113.md; timing only): see RexSwitchApplyNamedPriority.
REXCVAR_DEFINE_STRING(switch_yield_backoff_exempt, "", "Threading",
                      "Named threads that never take switch_yield_backoff's sleep (a comma-separated list of name "
                      "prefixes, set when a thread takes its name): a frame-chain thread polling for a job keeps "
                      "yielding instead of sleeping (~1 ms on Horizon). Example: \"Game Master,JQ Worker 0\". Empty: none")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_system_core_test, false, "Banjo/Debug",
                    "Test of banjo_system_core's layout without a four-core launch: the process's highest core (2 with "
                    "the usual three) is treated as the system core, so the moves, the hooks and the mask wrap run in "
                    "an emulator. Slower on a console (the game keeps two cores)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

/*
 * Registry pthread_t -> Horizon Handle.
 *
 * libnx exposes no way to get the handle of another thread, and the SDK
 * changes the priority and affinity of other threads, so it has to be tracked
 * by hand.
 *
 * Not a hot path: it is touched when threads are created and destroyed, never
 * per frame. A plain mutex is more than enough.
 */
struct Register {
  std::mutex m;
  std::unordered_map<uintptr_t, RexSwitchHandle> map;
};

Register& register_value() {
  static Register r;
  return r;
}

}  // namespace

extern "C" {
/* libnx: the heap size asked for when there is no loader heap override (runtime_switch.cpp sets it). */
extern size_t __nx_heap_size;
}

extern "C" {

void RexSwitchRegisterCurrentThread(void) {
  auto& r = register_value();
  std::lock_guard<std::mutex> lock(r.m);
  r.map[reinterpret_cast<uintptr_t>(pthread_self())] = threadGetCurHandle();
}

void RexSwitchUnregisterCurrentThread(void) {
  auto& r = register_value();
  std::lock_guard<std::mutex> lock(r.m);
  r.map.erase(reinterpret_cast<uintptr_t>(pthread_self()));
}

RexSwitchHandle RexSwitchHandleFor(pthread_t thread) {
  if (pthread_equal(thread, pthread_self()))
    return threadGetCurHandle();

  auto& r = register_value();
  std::lock_guard<std::mutex> lock(r.m);
  auto it = r.map.find(reinterpret_cast<uintptr_t>(thread));
  return it == r.map.end() ? REX_SWITCH_INVALID_HANDLE : it->second;
}

/* --- affinity ---------------------------------------------------------- */

int pthread_getaffinity_np(pthread_t thread, size_t size, cpu_set_t* set) {
  if (set == nullptr || size < sizeof(cpu_set_t))
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  s32 preferred = 0;
  u64 mask = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferred, &mask, h)))
    return EINVAL;

  set->bits = mask;
  return 0;
}

int pthread_setaffinity_np(pthread_t thread, size_t size, const cpu_set_t* set) {
  if (set == nullptr || size < sizeof(cpu_set_t))
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  /*
   * Horizon also wants a preferred core. The lowest one in the mask is
   * taken; -1 would mean "whatever it already had", which is not what the
   * POSIX API being imitated asks for.
   */
  s32 preferred = -1;
  for (int i = 0; i < 4; i++) {
    if (set->bits & (1ull << i)) { preferred = i; break; }
  }
  if (preferred < 0)
    return EINVAL;  /* empty mask: POSIX does not allow it either */

  /*
   * Only 4 cores exist. Asking for more is not a caller error (the SDK
   * passes 64-bit masks), so it is trimmed silently.
   */
  const u32 mask = static_cast<u32>(set->bits & 0xF);

  if (R_FAILED(svcSetThreadCoreMask(h, preferred, mask)))
    return EPERM;
  return 0;
}

/*
 * --- priority ------------------------------------------------------------
 *
 * sched_param::sched_priority is passed straight through as the Horizon
 * priority, without inverting the scale. This is deliberate: callers using the
 * REX_SWITCH_PRIO_* constants expect Horizon numbers, where lower means higher
 * priority. Inverting it here to look like POSIX would only turn the priority
 * scheme upside down, which is exactly the bug that causes stutter and audio
 * dropouts.
 */

int pthread_getschedparam(pthread_t thread, int* policy, struct sched_param* param) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  s32 prio = 0;
  if (R_FAILED(svcGetThreadPriority(&prio, h)))
    return EINVAL;

  if (policy) *policy = SCHED_FIFO;
  if (param) param->sched_priority = prio;
  return 0;
}

int pthread_setschedparam(pthread_t thread, int policy, const struct sched_param* param) {
  (void)policy;
  if (param == nullptr)
    return EINVAL;

  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;

  if (R_FAILED(svcSetThreadPriority(h, static_cast<u32>(param->sched_priority))))
    return EPERM;
  return 0;
}

int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy) {
  (void)attr;
  (void)policy;
  /* Horizon has a single policy. Accepting and doing nothing is correct. */
  return 0;
}

/* --- robust mutexes: they do not exist --------------------------------- */

int pthread_mutexattr_setrobust(pthread_mutexattr_t* attr, int robustness) {
  (void)attr;
  /*
   * PTHREAD_MUTEX_STALLED is accepted (the real behavior), and ROBUST is
   * accepted without providing it: the SDK uses it as a safeguard against a
   * thread dying while holding the mutex, which should not happen in the guest.
   */
  if (robustness != PTHREAD_MUTEX_STALLED && robustness != PTHREAD_MUTEX_ROBUST)
    return EINVAL;
  return 0;
}

int pthread_mutex_consistent(pthread_mutex_t* mutex) {
  (void)mutex;
  return 0;
}

int pthread_mutexattr_getrobust(const pthread_mutexattr_t* attr, int* robustness) {
  (void)attr;
  if (robustness) *robustness = PTHREAD_MUTEX_STALLED;
  return 0;
}

/*
 * --- suspension ----------------------------------------------------------
 *
 * The SDK suspends threads by sending them a signal. Horizon does not deliver
 * signals, so that cannot work however much shimming is added: the system
 * mechanism has to be used.
 *
 * A difference in semantics worth knowing: svcSetThreadActivity stops the
 * thread wherever it is, not at a safe point chosen by the thread itself. For
 * what the SDK does with this (suspending guest threads, which is what the
 * original console did) that is correct. For any use that needs the thread to
 * stop at a specific place, it is not.
 */

int RexSwitchPauseThread(pthread_t thread) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;
  if (pthread_equal(thread, pthread_self()))
    return EDEADLK;  /* pausing oneself cannot be undone */
  return R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused)) ? 0 : EPERM;
}

int RexSwitchResumeThread(pthread_t thread) {
  RexSwitchHandle h = RexSwitchHandleFor(thread);
  if (h == REX_SWITCH_INVALID_HANDLE)
    return ESRCH;
  return R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Runnable)) ? 0 : EPERM;
}

/* --- priority of the current thread ------------------------------------ */

uint32_t RexSwitchCurrentThreadId(void) {
  /* The current thread's handle serves as a unique id, stable while it lives. */
  return static_cast<uint32_t>(threadGetCurHandle());
}

/*
 * threadGetSelf() only returns something for threads that libnx created with threadCreate. Ours
 * come from pthread_create (rex::thread::Thread::Create -> PosixThread), so if the TLS does not carry
 * libnx's ThreadVars, this used to be a silent no-op: the priority was not applied and the log said
 * it was, because it only printed the cvar value.
 *
 * CUR_THREAD_HANDLE (0xFFFF8000) is the pseudo-handle that the Horizon kernel always understands as
 * "the calling thread", wherever it came from. The result is returned so that it can be checked in
 * the log instead of assumed.
 */
bool RexSwitchSetCurrentThreadPriorityOk(int priority) {
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  if (R_SUCCEEDED(svcSetThreadPriority(h, static_cast<u32>(priority)))) {
    return true;
  }
  if (h != CUR_THREAD_HANDLE) {
    return R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority)));
  }
  return false;
}

void RexSwitchSetCurrentThreadPriority(int priority) {
  (void)RexSwitchSetCurrentThreadPriorityOk(priority);
}

/*
 * Preferred core of the current thread.
 *
 * All our threads run with "preferred core -1", which on Horizon means the process default core:
 * the same one for all of them. With the ring at 94.7 % of a core and the game's main thread at
 * 88.2 %, both prefer the same place and the kernel has to keep moving them. Each migration throws
 * away the thread's L1 and part of its L2, and that does not show in the mean: it shows in the
 * variance, which is exactly what the player notices.
 *
 * The mask is not touched. The thread's existing mask is passed, so this only says "start here",
 * not "you may only run here": if the core is busy, the kernel can still move the thread. That is
 * why it cannot cause starvation, which is what makes real affinity pinning dangerous.
 *
 * Returns false if the kernel does not accept it (for example if the core is not in the process
 * mask: that is the "Too few processor cores" error seen when trying to use the fourth core).
 */
bool RexSwitchSetCurrentThreadCore(int core) {
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  s32 preferred = 0;
  u64 mask = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferred, &mask, h))) {
    if (h == CUR_THREAD_HANDLE ||
        R_FAILED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE))) {
      return false;
    }
    h = CUR_THREAD_HANDLE;
  }
  if (core >= 0 && (mask & (u64(1) << core)) == 0) {
    return false;  // that core is not this process's: do not force it
  }
  return R_SUCCEEDED(svcSetThreadCoreMask(h, core, mask));
}

/*
 * The real pin, with the mask reduced to a single core.
 *
 * Measured: a preferred core is not enough. Horizon accepts it and moves the thread anyway (0.24
 * migrations per loop, wherever it is set). This takes the other cores away from the thread.
 *
 * Different from RexSwitchSetCurrentThreadCore and more dangerous: if that core saturates, the
 * thread does not get to run. That is why it sits behind a cvar that is off by default and is
 * checked in the log. See docs/platform-notes.md (Threads).
 */
// banjo-nx (2026-10-20, from sa2-nx): the core layout's mask setters (RexSetGuestThreadCoreLayout, xthread.cpp).
bool RexSwitchSetThreadMaskOf(void* native_handle, int preferred, unsigned long long mask) {
  pthread_t thread;
  static_assert(sizeof(thread) == sizeof(native_handle));
  std::memcpy(&thread, &native_handle, sizeof(thread));
  const RexSwitchHandle h = RexSwitchHandleFor(thread);
  const u64 m = u64(mask) & 0x7;
  if (h == REX_SWITCH_INVALID_HANDLE || m == 0 || preferred < 0 || !(m & (u64(1) << preferred))) return false;
  return R_SUCCEEDED(svcSetThreadCoreMask(h, preferred, m));
}

bool RexSwitchSetCurrentThreadMask(int preferred, unsigned long long mask) {
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  s32 current_preferred = 0;
  u64 current = 0;
  if (R_FAILED(svcGetThreadCoreMask(&current_preferred, &current, h))) {
    if (h == CUR_THREAD_HANDLE ||
        R_FAILED(svcGetThreadCoreMask(&current_preferred, &current, CUR_THREAD_HANDLE))) {
      return false;
    }
    h = CUR_THREAD_HANDLE;
  }
  const u64 m = u64(mask) & 0x7;  // the process's three cores
  if (m == 0 || preferred < 0 || !(m & (u64(1) << preferred))) {
    return false;
  }
  return R_SUCCEEDED(svcSetThreadCoreMask(h, preferred, m));
}

/*
 * The real pin, with the mask reduced to a single core.
 *
 * Measured: a preferred core is not enough. Horizon accepts it and moves the thread anyway (0.24
 * migrations per loop, wherever it is set). This takes the other cores away from the thread.
 *
 * Different from RexSwitchSetCurrentThreadCore and more dangerous: if that core saturates, the
 * thread does not get to run. That is why it sits behind a cvar that is off by default and is
 * checked in the log. See docs/platform-notes.md (Threads).
 */
bool RexSwitchPinCurrentThreadToCore(int core) {
  if (core < 0) {
    return false;
  }
  // banjo-nx (2026-10-27): the system core is not in a thread's mask (__wrap_svcSetThreadCoreMask takes it out), so
  // the check below would refuse it; when the process has it, the pin goes there.
  if (core == RexSwitchSystemCore()) {
    return RexSwitchMoveCurrentThreadToSystemCore(-1);
  }
  Thread* yo = threadGetSelf();
  Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  s32 preferred = 0;
  u64 mask = 0;
  if (R_FAILED(svcGetThreadCoreMask(&preferred, &mask, h))) {
    if (h == CUR_THREAD_HANDLE ||
        R_FAILED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE))) {
      return false;
    }
    h = CUR_THREAD_HANDLE;
  }
  const u64 that_only = u64(1) << core;
  if ((mask & that_only) == 0) {
    return false;  // that core is not this process's
  }
  return R_SUCCEEDED(svcSetThreadCoreMask(h, core, that_only));
}

bool RexSwitchUnpinCurrentThread(int core) {
  // banjo-nx (2026-10-27): the app cores (the process's without the system core: RexSwitchAppCoreMask).
  const u64 process_mask = RexSwitchAppCoreMask();
  if (process_mask == 0) {
    return false;
  }
  Thread* yo = threadGetSelf();
  const Handle h = (yo != nullptr) ? yo->handle : CUR_THREAD_HANDLE;
  // -1 is the kernel's "don't care" (what an unpinned thread shows); -2 would take the process's ideal core AND reduce
  // the mask to it.
  const s32 preferred = (core >= 0 && (process_mask & (u64(1) << core)) != 0) ? s32(core) : s32(-1);
  if (R_SUCCEEDED(svcSetThreadCoreMask(h, preferred, process_mask))) {
    return true;
  }
  return h != CUR_THREAD_HANDLE && R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferred, process_mask));
}

/*
 * Which core the calling thread is running on right now. This is what allows counting migrations:
 * it is read on every loop iteration and counted each time it changes. There is no syscall to ask
 * it about another thread, so each thread has to look at itself.
 */
int RexSwitchCurrentCore(void) { return int(svcGetCurrentProcessorNumber()); }

void RexSwitchDescribeThreadFailure(size_t stack_size, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  // 1. libnx allocates stack + TLS + reent as one block with memalign(0x1000, ...); 64 KB covers TLS and reent.
  const size_t block = ((stack_size + 0xFFF) & ~size_t(0xFFF)) + 0x10000;
  void* probe = memalign(0x1000, block);
  const bool heap_ok = probe != nullptr;
  free(probe);
  const struct mallinfo mi = mallinfo();
  // With a loader (hbloader) the heap is the loader's, whatever __nx_heap_size says.
  const size_t heap_total = envHasHeapOverride() ? size_t(envGetHeapOverrideSize()) : __nx_heap_size;
  // 2. Room for the stack's mirror in the stack region (then svcMapMemory).
  virtmemLock();
  void* const mirror = virtmemFindStack(block, 0x4000);
  virtmemUnlock();
  // 3. A kernel thread slot (svcCreateThread).
  s64 threads_now = -1;
  s64 threads_cap = -1;
  u64 limit = 0;
  if (R_SUCCEEDED(svcGetInfo(&limit, InfoType_ResourceLimit, CUR_PROCESS_HANDLE, 0)) && limit != 0) {
    svcGetResourceLimitCurrentValue(&threads_now, Handle(limit), LimitableResource_Threads);
    svcGetResourceLimitLimitValue(&threads_cap, Handle(limit), LimitableResource_Threads);
    svcCloseHandle(Handle(limit));
  }
  snprintf(out, out_len,
           "heap %s a %zu KB block (heap %zu MB, arena %zu MB, in use %zu MB, free %zu MB in %zu chunks); "
           "stack region %s; kernel threads %lld of %lld",
           heap_ok ? "has" : "has NO", block >> 10, heap_total >> 20, size_t(mi.arena) >> 20,
           size_t(mi.uordblks) >> 20, size_t(mi.fordblks) >> 20, size_t(mi.ordblks),
           mirror != nullptr ? "has room" : "is FULL", (long long)threads_now, (long long)threads_cap);
}

/*
 * --- the system core (banjo-nx, 2026-10-27; NetherSX2_nx's 4-core mode, MIT, NaGaa95) ------------------------
 *
 * NetherSX2_nx (source/pthr.c, core_init_once): with four cores granted the top one is kept for background
 * threads, each pinned there alone (pthr_pin_bg_core: svcSetThreadCoreMask(CUR_THREAD_HANDLE, bg, 1 << bg)),
 * and the heavy threads keep the first three; with three or fewer, nothing changes. Its forwarder widens the main
 * thread to the whole grant before the NRO starts (launcher/fwd/main.c, restoreMainThreadAffinity), and libnx's
 * pthread_create gives every new thread the whole grant too (newlib.o, __syscall_thread_create:
 * svcSetThreadCoreMask(handle, -1, InfoType_CoreMask)). The game's threads would then land on core 3 at random,
 * sharing it with the system's threads and with ours; __wrap_svcSetThreadCoreMask (--wrap, like threadCreate's)
 * takes core 3 out of every mask of more than one core, unless banjo_system_core is 2.
 */
namespace {
std::atomic<int> g_sys_state{0};  // 0: not read yet, 1: read
std::atomic<unsigned> g_process_mask{0x7};
std::atomic<int> g_system_core{-1};     // 3 when granted and banjo_system_core >= 1
std::atomic<int> g_system_mode{0};
std::atomic<unsigned> g_strip_mask{0};  // bits taken out of multi-core masks
std::mutex g_sys_mutex;

void ReadSystemCore(bool with_cvar) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0) {
    mask = 0x7;  // NetherSX2_nx's fallback: an application's usual three cores
  }
  mask &= 0xF;
  // The top core, when four are granted (NetherSX2_nx: core_count >= 4 -> bg_core = the last of the list); with
  // banjo_system_core_test, the highest of any two or more.
  const bool test = with_cvar && REXCVAR_GET(banjo_system_core_test) && __builtin_popcountll(mask) >= 2;
  const int top = (__builtin_popcountll(mask) >= 4 || test) ? 63 - __builtin_clzll(mask) : -1;
  // Before the cvars are parsed (a thread made before main): the default layout.
  const int mode = top < 0 ? 0 : (with_cvar ? int(REXCVAR_GET(banjo_system_core)) : 1);
  g_process_mask.store(unsigned(mask), std::memory_order_relaxed);
  g_system_mode.store(mode, std::memory_order_relaxed);
  g_system_core.store(mode >= 1 ? top : -1, std::memory_order_relaxed);
  g_strip_mask.store((top >= 0 && mode <= 1) ? (1u << top) : 0u, std::memory_order_relaxed);
  g_sys_state.store(1, std::memory_order_release);
}

inline void EnsureSystemCore() {
  if (g_sys_state.load(std::memory_order_acquire) == 0) {
    std::lock_guard<std::mutex> lock(g_sys_mutex);
    if (g_sys_state.load(std::memory_order_relaxed) == 0) ReadSystemCore(false);
  }
}

#include "forwarder_npdm.inc"  // kForwarderNpdm (tools/forwarder)
std::string g_npdm_status = "not checked";

// RexSwitchSampleCoreIdle's last sample of each core (the report takes the difference between two of its reads).
struct IdleSample {
  std::atomic<u64> idle{0}, at{0};
};
IdleSample g_idle[4];
}  // namespace

Result __real_svcSetThreadCoreMask(Handle handle, s32 preferred_core, u32 affinity_mask);
Result __wrap_svcSetThreadCoreMask(Handle handle, s32 preferred_core, u32 affinity_mask) {
  EnsureSystemCore();
  const u32 strip = g_strip_mask.load(std::memory_order_relaxed);
  // A mask of more than one core loses the system core (an app core stays); a request for the system core alone
  // (our background threads) goes through.
  if (strip != 0 && (affinity_mask & strip) != 0 && (affinity_mask & ~strip) != 0) {
    affinity_mask &= ~strip;
    if (preferred_core >= 0 && (strip & (1u << preferred_core)) != 0) preferred_core = -1;
  }
  return __real_svcSetThreadCoreMask(handle, preferred_core, affinity_mask);
}

int RexSwitchInitSystemCore(void) {
  {
    std::lock_guard<std::mutex> lock(g_sys_mutex);
    ReadSystemCore(true);
  }
  // The main thread: NetherSX2_nx's forwarder hands it over with the whole grant (restoreMainThreadAffinity).
  const u32 strip = g_strip_mask.load(std::memory_order_relaxed);
  if (strip != 0) {
    s32 preferred = 0;
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE)) && (mask & strip) != 0 &&
        (mask & ~u64(strip)) != 0) {
      svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, u32(mask));  // (the wrap takes the system core out)
    }
  }
  return g_system_core.load(std::memory_order_relaxed);
}

unsigned RexSwitchProcessCoreMask(void) {
  EnsureSystemCore();
  return g_process_mask.load(std::memory_order_relaxed);
}

int RexSwitchSystemCore(void) {
  EnsureSystemCore();
  return g_system_core.load(std::memory_order_relaxed);
}

int RexSwitchSystemCoreMode(void) {
  EnsureSystemCore();
  return g_system_mode.load(std::memory_order_relaxed);
}

unsigned RexSwitchAppCoreMask(void) {
  EnsureSystemCore();
  return g_process_mask.load(std::memory_order_relaxed) & ~g_strip_mask.load(std::memory_order_relaxed);
}

bool RexSwitchMoveCurrentThreadToSystemCore(int priority) {
  const int core = RexSwitchSystemCore();
  if (core < 0) return false;
  if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core))) return false;
  if (priority >= 0) (void)RexSwitchSetCurrentThreadPriorityOk(priority);
  return true;
}

void RexSwitchSampleCoreIdle(void) {
  // banjo-nx (2026-10-27, ARM pass): the 100 ms test first, on the system counter (no system call); the core is asked
  // only when a sample is due (the caller loops every 5 ms).
  static std::atomic<u64> last_try{0};
  const u64 now = armGetSystemTick();
  if (now - last_try.load(std::memory_order_relaxed) < armNsToTicks(100000000ull)) return;  // 100 ms
  last_try.store(now, std::memory_order_relaxed);
  const int core = int(svcGetCurrentProcessorNumber());
  if (core < 0 || core >= 4) return;
  IdleSample& s = g_idle[core];
  u64 idle = 0;
  // Only the calling thread's own core can be asked (no handle; sub-type: that core).
  if (R_FAILED(svcGetInfo(&idle, InfoType_IdleTickCount, INVALID_HANDLE, u64(core)))) return;
  s.idle.store(idle, std::memory_order_relaxed);
  s.at.store(now, std::memory_order_release);
}

// renut-nx (2026-11-01, the public build; blur-nx took it from banjo-nx on 2026-11-11, the user's request to work the
// forwarder's id out from the NRO): whether 'program' is the id Sphaira's "Install Forwarder" gives a forwarder of this
// NRO (sphaira source/owo.cpp, forwarder_path_hash and the tid functions): the first 8 bytes of the SHA-256 of the NRO
// path written twice (the forwarder's arguments default to the path) as 05xxxxxxxxxxx000, or, in its native-game mode,
// 01xxxxxxxxxxx000 with bit 12 clear; the path with "sdmc:" and without (its older installs). Checked against banjo-nx's
// user's forwarder: "sdmc:/switch/banjo-nx/banjo-nx.nro" gives 05D6EA1333487000; "sdmc:/switch/blur-nx/blur-nx.nro"
// gives 05A5D8E1BAA3E000. A retail game's id (title takeover) never matches, so its NPDM is never replaced.
extern "C" int __system_argc;      // libnx: the NRO's path in argv[0] (as filesystem_posix.cpp)
extern "C" char** __system_argv;
static bool SphairaForwarderOfThisNro(u64 program) {
  if (__system_argc <= 0 || !__system_argv || !__system_argv[0] || !__system_argv[0][0]) return false;
  const std::string path = __system_argv[0];
  const std::string rooted = path.rfind("sdmc:", 0) == 0 ? path.substr(5) : path;
  for (const std::string& p : {std::string("sdmc:") + rooted, rooted}) {
    const std::string hashed = p + p;
    u64 digest[4] = {};
    sha256CalculateHash(digest, hashed.data(), hashed.size());
    const u64 h = digest[0];
    if (program == (0x0500000000000000ull | (h & 0x00FFFFFFFFFFF000ull)) ||
        program == (0x0100000000000000ull | (h & 0x00FFFFFFFFFFE000ull)))
      return true;
  }
  return false;
}

static int ForwarderNpdm(bool four_cores);
int RexSwitchEnsureForwarderNpdm(void) { return ForwarderNpdm(REXCVAR_GET(four_core_mode)); }
// blur-nx (2026-11-18): the game menu's four-core option, applied when the menu closes (the next launch takes it).
int RexSwitchApplyFourCoreMode(int on) { return ForwarderNpdm(on != 0); }

static int ForwarderNpdm(bool four_cores) {
  const std::string want_text = REXCVAR_GET(banjo_forwarder_program_id);
  if (want_text.empty()) {
    g_npdm_status = "off (banjo_forwarder_program_id empty)";
    return 0;
  }
  const u64 want = std::strtoull(want_text.c_str(), nullptr, 16);
  u64 program = 0;
  // renut-nx (2026-11-01, the public build): also a forwarder Sphaira made for this NRO (SphairaForwarderOfThisNro):
  // its id depends on the NRO's path, so a forwarder of the NRO in another folder works as well.
  if (R_FAILED(svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0)) || want == 0 ||
      (program != want && !SphairaForwarderOfThisNro(program))) {
    char text[160];
    std::snprintf(text, sizeof(text), "not written: running as %016llX, not the forwarder %016llX",
                  (unsigned long long)program, (unsigned long long)want);
    g_npdm_status = text;
    return 0;
  }
  // The NPDM with this program id in ACI0 and in ACID's range.
  unsigned char npdm[sizeof(kForwarderNpdm)];
  std::memcpy(npdm, kForwarderNpdm, sizeof(npdm));
  std::memcpy(npdm + kForwarderNpdmAci0 + 0x10, &program, 8);
  std::memcpy(npdm + kForwarderNpdmAcid + 0x210, &program, 8);
  std::memcpy(npdm + kForwarderNpdmAcid + 0x218, &program, 8);
  char dir[96], path[128];
  std::snprintf(dir, sizeof(dir), "sdmc:/atmosphere/contents/%016llX/exefs", (unsigned long long)program);
  std::snprintf(path, sizeof(path), "%s/main.npdm", dir);
  // Already there, the same bytes?
  bool ours = false;
  if (FILE* f = std::fopen(path, "rb")) {
    unsigned char have[sizeof(npdm) + 1];
    const size_t n = std::fread(have, 1, sizeof(have), f);
    std::fclose(f);
    ours = n == sizeof(npdm) && std::memcmp(have, npdm, sizeof(npdm)) == 0;
    if (ours && four_cores) {
      g_npdm_status = std::string("in place: ") + path;
      return 1;
    }
  }
  // blur-nx (2026-11-18, four_core_mode off): this program's NPDM removed; anything else there is left alone.
  if (!four_cores) {
    if (!ours) {
      g_npdm_status = std::string("off (four_core_mode false; none of ours at ") + path + ")";
      return 0;
    }
    const bool removed = std::remove(path) == 0;
    g_npdm_status = std::string(removed ? "removed (four_core_mode false; three cores from the forwarder's next launch): "
                                        : "could NOT be removed: ") + path;
    return removed ? 3 : -1;
  }
  char part[96];
  for (const char* sub : {"sdmc:/atmosphere", "sdmc:/atmosphere/contents"}) mkdir(sub, 0777);
  std::snprintf(part, sizeof(part), "sdmc:/atmosphere/contents/%016llX", (unsigned long long)program);
  mkdir(part, 0777);
  mkdir(dir, 0777);
  FILE* f = std::fopen(path, "wb");
  const bool ok = f && std::fwrite(npdm, 1, sizeof(npdm), f) == sizeof(npdm);
  if (f) std::fclose(f);
  g_npdm_status = std::string(ok ? "written (launch the forwarder again for four cores): " : "could NOT be written: ") + path;
  return ok ? 2 : -1;
}

bool RexSwitchSystemCoreXma(void) { return RexSwitchSystemCore() >= 0 && REXCVAR_GET(banjo_system_core_xma); }
int RexSwitchSystemCoreMixerPriority(void) { return REXCVAR_GET(banjo_system_core_mixer_priority); }

void RexSwitchSystemCoreDescribe(char* out, size_t out_len) {
  if (!out || !out_len) return;
  std::snprintf(out, out_len,
                "process cores 0x%X, system core %d (banjo_system_core %d; game threads on 0x%X); forwarder NPDM %s",
                RexSwitchProcessCoreMask(), RexSwitchSystemCore(), RexSwitchSystemCoreMode(), RexSwitchAppCoreMask(),
                g_npdm_status.c_str());
}

bool RexSwitchCoreIdleSample(int core, unsigned long long* idle_ticks, unsigned long long* at_ticks) {
  if (core < 0 || core >= 4) return false;
  const IdleSample& s = g_idle[core];
  const u64 at = s.at.load(std::memory_order_acquire);
  if (at == 0) return false;
  *idle_ticks = s.idle.load(std::memory_order_relaxed);
  *at_ticks = at;
  return true;
}

// sasrt-nx (2026-11-10; port 65, thread_event_address_wait): threading_posix.cpp's events wait on an address
// (address_event_core.h); these keep libnx's names out of that file.
bool RexSwitchWaitIfEqual(volatile uint32_t* word, uint32_t value, int64_t ns) {
  const Result rc = svcWaitForAddress((void*)word, ArbitrationType_WaitIfEqual, (s64)(s32)value, ns);
  return R_VALUE(rc) != KERNELRESULT(TimedOut);
}
void RexSwitchWakeAddress(volatile uint32_t* word, int32_t count) {
  svcSignalToAddress((void*)word, SignalType_Signal, 0, count);
}

}  // extern "C"

/*
 * blur-nx (2026-11-12, CPU batch 3; codegen-ideas-3 CG-01): switch_thread_priorities. Every guest thread runs at one
 * priority (0x3B: ignore_thread_priorities), and Horizon time-slices a core's threads of that priority in 10 ms
 * slices, so a thread on the frame's critical path that wakes up behind a long audio job waits for the slice to end.
 * This table gives named threads a priority of their own when they take their name (RexSwitchPerfSetThreadName: a
 * thread naming itself, which is how the guest's threads and the SDK's are named). A thread above 0x3B is not
 * time-sliced with its peers: it runs until it waits. The guest's polling waits sleep 100 us after 16 quick yields
 * (MaybeYield's switch_yield_backoff), so a raised thread that spins on a job a lower one holds still lets it run.
 */
// The same for cores (codegen-ideas-3 CG-02): switch_thread_cores gives a named thread a core mask of its own (within the
// process's cores; the lowest core of the mask preferred), through svcSetThreadCoreMask's wrap (banjo_system_core's
// rules still apply: in mode 1 a mask of several cores loses core 3).
namespace {
struct NamedValue {
  std::string prefix;
  long value;
};
std::once_flag g_named_once;
std::vector<NamedValue> g_named_priority, g_named_cores;
std::vector<std::string> g_named_exempt;  // blur-nx (2026-11-13, #60): switch_yield_backoff_exempt's prefixes
std::atomic<uint32_t> g_named_applied{0};

// "PREFIX=VALUE,PREFIX=VALUE": each value read by strtol (0x.. hex); entries outside [lo, hi] are dropped.
void ParseNamed(const std::string& spec, long lo, long hi, std::vector<NamedValue>& out) {
  size_t at = 0;
  while (at < spec.size()) {
    size_t end = spec.find(',', at);
    if (end == std::string::npos) end = spec.size();
    const std::string item = spec.substr(at, end - at);
    at = end + 1;
    const size_t eq = item.rfind('=');
    if (eq == std::string::npos || eq == 0) continue;
    size_t a = 0, b = eq;
    while (a < b && item[a] == ' ') ++a;
    while (b > a && item[b - 1] == ' ') --b;
    char* tail = nullptr;
    const long v = std::strtol(item.c_str() + eq + 1, &tail, 0);
    if (b == a || tail == item.c_str() + eq + 1 || v < lo || v > hi) continue;
    out.push_back({item.substr(a, b - a), v});
  }
}

void ParseNamedTables() {
  // An application's priorities (its NPDM: 0x1C-0x3B for the usual one); the audio threads sit at 0x2A-0x2D.
  ParseNamed(REXCVAR_GET(switch_thread_priorities), 0x1C, 0x3B, g_named_priority);
  ParseNamed(REXCVAR_GET(switch_thread_cores), 0x1, 0xF, g_named_cores);
  // blur-nx (2026-11-13, #60): "PREFIX,PREFIX" (blanks around each trimmed).
  const std::string exempt = REXCVAR_GET(switch_yield_backoff_exempt);
  size_t at = 0;
  while (at < exempt.size()) {
    size_t end = exempt.find(',', at);
    if (end == std::string::npos) end = exempt.size();
    size_t a = at, b = end;
    while (a < b && exempt[a] == ' ') ++a;
    while (b > a && exempt[b - 1] == ' ') --b;
    if (b > a) g_named_exempt.push_back(exempt.substr(a, b - a));
    at = end + 1;
  }
}

const NamedValue* FindNamed(const std::vector<NamedValue>& table, const char* name) {
  for (const NamedValue& n : table)
    if (std::strncmp(name, n.prefix.c_str(), n.prefix.size()) == 0) return &n;
  return nullptr;
}
}  // namespace

extern "C" {

// blur-nx (2026-11-13, #60): the calling thread is exempt from the yield backoff (MaybeYield reads it).
thread_local bool g_rex_yield_backoff_exempt = false;

int RexSwitchApplyNamedPriority(unsigned handle, const char* name) {
  if (!handle || !name) return -1;
  std::call_once(g_named_once, ParseNamedTables);
  // blur-nx (2026-11-13, #60): switch_yield_backoff_exempt, for the thread naming itself.
  if (!g_named_exempt.empty() && handle == threadGetCurHandle()) {
    for (const std::string& prefix : g_named_exempt)
      if (std::strncmp(name, prefix.c_str(), prefix.size()) == 0) g_rex_yield_backoff_exempt = true;
  }
  if (g_named_priority.empty() && g_named_cores.empty()) return -1;
  int result = -1;  // (the exemption above needs no table of these)
  if (const NamedValue* c = FindNamed(g_named_cores, name)) {
    const u32 mask = u32(c->value) & RexSwitchProcessCoreMask();
    if (mask != 0 && R_SUCCEEDED(svcSetThreadCoreMask(handle, __builtin_ctz(mask), mask)))
      g_named_applied.fetch_add(1, std::memory_order_relaxed);
  }
  if (const NamedValue* p = FindNamed(g_named_priority, name)) {
    if (R_SUCCEEDED(svcSetThreadPriority(handle, int(p->value)))) {
      g_named_applied.fetch_add(1, std::memory_order_relaxed);
      result = int(p->value);
    }
  }
  return result;
}

unsigned RexSwitchNamedPrioritiesApplied(void) { return g_named_applied.load(std::memory_order_relaxed); }

}  // extern "C"

#endif  /* REX_PLATFORM_SWITCH */
