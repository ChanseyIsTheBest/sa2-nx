#pragma once
/**
 * @file        sys_counters.h
 * @brief       Passive counters of the host runtime's own overhead (wake-ups, lock stalls) shown in the
 *              Switch profiler report line "system". Relaxed atomic adds; compiled out off the Switch.
 *
 * The ids index the profiler's counter array (src/ui/switch_perf.cpp, kCounterCount); the report prints
 * the per-second delta of each. Keep the enum in sync with the report line there.
 *
 * sasrt-nx (2026-10-12): off the Switch the same sites, and the REX_SYS_CENSUS ones, count into
 * rex::syscount::g_census (a plain load and store each: a lost count does not matter), which the app
 * prints when it wants a census of the runtime's calls (how often the guest reads the clock, enters a
 * critical section, sets an event, looks a handle up). On the Switch REX_SYS_CENSUS is nothing: a count
 * per call is the kind of cost the census is there to find.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rex::syscount {

enum : unsigned {
  kAlertSlice = 40,       // alertable wait woke from its poll slice without anything to do
  kAlertWake = 41,        // alertable wait interrupted by a queued user callback / APC hint
  kMultiBlock = 42,       // WaitMultiple parked on its wake point (event-driven)
  kMultiWake = 43,        // ... woken by a signalled handle
  kMultiSafety = 44,      // ... woke by the safety re-poll (a handle changed without telling us)
  kMultiPoll = 45,        // legacy WaitMultiple 1 ms sleep polls (thread_wait_blocking = false)
  kTimestampTick = 46,    // KeTimeStampBundle timer callbacks
  kClockCall = 47,        // UpdateGuestClock calls
  kClockBlocked = 48,     // ... that found the clock mutex taken (clock_lock_free = false only)
  kObjLookup = 49,        // ObjectTable::LookupObject calls
  kObjBlocked = 50,       // ... that found the global critical region taken
  kVolumeMask = 51,       // XAudioGetVoiceCategoryVolumeChangeMask calls
  kVblankTick = 52,       // native vblank thread wake-ups (app)
  kAudioNative = 53,      // native audio DSP replacements run (app)
  kAudioMismatch = 54,    // ... validate mode mismatches (app)
  kCritSecWait = 55,      // RtlEnterCriticalSection fell into a full host wait
  kSignalIdle = 56,       // event / semaphore signals that found nobody blocked: no wake syscall (thread_signal_skip_idle)
  kCount = 57,
  // Census only (REX_SYS_CENSUS: never counted on the Switch).
  kCensusCritSecEnter = 57,   // RtlEnterCriticalSection calls
  kCensusCritSecRecursive = 58,  // ... that already owned the section
  kCensusEventSet = 59,       // XEvent::Set calls
  kCensusSemRelease = 60,     // XSemaphore releases
  kCensusWait = 61,           // XObject waits (single)
  kCensusNativeObject = 62,   // XObject::GetNativeObject calls (a guest dispatcher object looked up)
  kCensusTryEnter = 63,       // RtlTryEnterCriticalSection calls
  kCensusFlushWrite = 64,     // guest code switched the host flush mode (an `msr fpcr` on AArch64)
  kCensusFlushSame = 65,      // ... of those, unconditional writes that left the mode as it was
  kCensusNoteShaderLocked = 66,  // NoteShaderObject calls that took the shared mutex (app)
  kCensusCritSecPcrOwner = 67,   // critical section calls that took the owner id from the PCR (no thread lookup)
  kCensusCritSecOwnerMismatch = 68,  // ... where that id was not the looked-up thread's (PC self-check: must be 0)
  // The SDK's own overhead (sa2-nx, the third survey of 2026-10-12): how often the runtime makes an out-of-line
  // call for something that is one load or nothing.
  kCensusLogCall = 69,        // log statements executed (rex::GetLoggerRaw calls), at any level, written or not
  kCensusLookupHeap = 70,     // Memory::LookupHeap calls (one inside every Memory::TranslateVirtual)
  kCensusRetainRelease = 71,  // XObject::Retain and Release calls
  kCensusKernelState = 72,    // rex::system::kernel_state() calls
  // banjo-nx (2026-10-14): the rounding mode of each guest FPSCR write (mtfsf). tools/fp_sign_ops.py (float negation
  // folds) is exact only while the game never selects round-up or round-down.
  kCensusRoundNearest = 76,   // guest FPSCR writes with RN = round to nearest
  kCensusRoundZero = 77,      // ... RN = toward zero
  kCensusRoundUpDown = 78,    // ... RN = toward +infinity or -infinity
  // blur-nx (2026-11-12, codegen-ideas-3 CG-13's census; PC game code only, REX_DIET_CENSUS_ON): the scalar FPU code
  // that one flush mode for all code would change. An FPU-mode region is the guest code between a switch of the flush
  // mode off and the next switch on; at that switch the host's sticky flags tell whether any operation in it met a
  // denormal operand or gave an underflow (x86 MXCSR DE / UE; a lower bound: x86 raises UE only on an inexact tiny
  // result). The any-mode helpers' cold paths (rex/ppc/diet_ops.h) are counted as before (g_rex_diet_census[3]).
  kCensusFpuRegions = 79,     // FPU-mode regions ended by a switch of the flush mode on
  kCensusFpuDenormal = 80,    // ... of those, with a denormal operand or an underflow inside
  // blur-nx (2026-11-13, #57 / #34 of Blur's docs/cpu-near-exact-50-1113.md): single-object waits with a zero timeout
  // (polls), and of those the ones that timed out (each then yields: MaybeYield).
  kCensusWaitPoll = 81,
  kCensusWaitPollTimeout = 82,
  kCensusWaitSignalled = 83,  // single-object waits of any timeout that succeeded
  kCensusCount = 84,
};

#if !defined(__SWITCH__)
// Out of line: the table lives in the runtime library (a DLL on Windows, whose data is not exported).
void Census(unsigned id, uint64_t n);
uint64_t CensusRead(unsigned id);
// Census by code address: counts id, and (once CensusSitesEnable(true) was called) the address this was called from.
// CensusSites copies the `capacity` most counted addresses (cumulative) and returns how many it wrote.
void CensusSite(unsigned id);
void CensusSitesEnable(bool on);
size_t CensusSites(uint64_t* addresses, uint64_t* counts, size_t capacity);
// blur-nx (2026-11-12, CG-13's census): the code addresses where a scalar result met a denormal, in a table of their
// own (a flush-mode switch site and a denormal site are different questions). CensusDenormalSite counts one at
// `address` (the any-mode helpers' cold paths pass their return address); CensusFpuRegionEnd is called by the switch
// of the flush mode on with the host's status word, counts the region and, when its flags show a denormal operand or
// an underflow, the code address the switch was called from. CensusDenormalSites copies the most counted.
void CensusDenormalSite(uint64_t address);
void CensusFpuRegionEnd(uint32_t csr);
size_t CensusDenormalSites(uint64_t* addresses, uint64_t* counts, size_t capacity);
// Calls per kernel export (third survey, 2026-10-12): every REX_HOOK / REX_EXPORT counts itself here in PC builds,
// so a run shows which host functions the game calls most. ExportCounter returns the export's counter (created
// on first use; call it once per export and keep the pointer); ExportCensus copies the `capacity` most called.
std::atomic<uint64_t>* ExportCounter(const char* name);
size_t ExportCensus(const char** names, uint64_t* counts, size_t capacity);
#endif

}  // namespace rex::syscount

#if defined(__SWITCH__)
extern "C" void RexSwitchPerfAdd(unsigned id, uint64_t value);
#define REX_SYS_COUNT(id, n) RexSwitchPerfAdd((id), static_cast<uint64_t>(n))
#define REX_SYS_CENSUS(id, n) ((void)0)
#define REX_SYS_CENSUS_SITE(id) ((void)0)
#else
#define REX_SYS_CENSUS_SITE(id) ::rex::syscount::CensusSite((id))
#define REX_SYS_COUNT(id, n) ::rex::syscount::Census((id), static_cast<uint64_t>(n))
#define REX_SYS_CENSUS(id, n) ::rex::syscount::Census((id), static_cast<uint64_t>(n))
#endif
