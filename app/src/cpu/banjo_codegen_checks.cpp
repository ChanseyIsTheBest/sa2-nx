// banjo-nx: the PC side of the post-codegen steps of sa2-nx's CPU rounds of 2026-10-12 (docs/enhancements.md, "sa2-nx's
// CPU rounds"). From sa2-nx (app/src/cpu/sa2_codegen_checks.cpp and the census parts of sa2_enhancements.cpp, GPL-3.0,
// same project family). The Switch build has no code at any of these places: the macros of the generated pch are the
// bare operation there. Off the Switch each of them keeps the old operation next to the new one:
//   - tools/flush_mode_flow.py: a flush-mode check proven redundant keeps its test, counts itself
//     (g_rex_flush_flow_checked) and calls RexFlushFlowMismatch when the proof was wrong;
//   - tools/ro_constants.py: a load from read-only data folded to its constant still loads, counts itself
//     (g_rex_ro_checked) and calls RexRoMismatch when the memory does not hold the constant;
//   - tools/indirect_calls.py: a promoted indirect call counts itself (g_rex_promoted_calls) and calls
//     RexPromotedMismatch when the dispatch table's entry for the guarded target is not the function called;
//   - the census of indirect calls (banjo_diag_indirect_census = a file): per call site how often it ran and where
//     to, the input of tools/indirect_promotions.py;
//   - the SDK's kernel_critsec_pcr_owner: every critical section call compares the owner id it took from the PCR
//     with the looked-up thread's (rex/sys_counters.h, kCensusCritSecOwnerMismatch);
//   - banjo_diag_runtime_census: flush-mode switches, any-mode helper executions and the runtime's lock and wait
//     paths per second (what is worth making cheaper on the console, which a PC profile cannot show).
// So every PC run tests what it executes; the '[verify]' lines of the 10 s report carry the counts, and a wrong one
// counts as a difference of the verify session (me_verify.h).
#include "banjo_enhancements.h"

#include "../me/me_verify.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/sys_counters.h>

REXCVAR_DEFINE_STRING(banjo_diag_indirect_census, "", "Banjo/Enhancements",
                      "Diagnostic (PC): a file that gets, with every 10 s report, the census of the game's "
                      "indirect calls (per call site: calls and targets). tools/indirect_promotions.py makes "
                      "app/indirect_promotions.txt from it. Empty = no census")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(banjo_diag_runtime_census, false, "Banjo/Enhancements",
                    "Diagnostic (PC): flush-mode switches and calls of the host runtime's clock, critical section, "
                    "event, semaphore, wait and handle-lookup paths per second, in '[census]' log lines with each "
                    "10 s report")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if !REX_PLATFORM_SWITCH

extern "C" {
volatile unsigned long long g_rex_ro_checked = 0;      // plain lossy counts: "at least this many"
volatile unsigned long long g_rex_promoted_calls = 0;
unsigned char g_rex_indirect_census = 0;
// renut-ultimate (2026-11-14, port 140): RexNxCasCensus (below) only while banjo_diag_runtime_census is on. Unguarded,
// every guest compare-and-swap of a PC build took one global mutex: Katamari's start-up spin locks crawled for ~33 s
// (the main thread caught in RexNxCasCensus; 2026-11-14 runs p138_*).
unsigned char g_rex_cas_census = 0;
volatile unsigned long long g_rex_flush_flow_checked = 0;
// 2026-10-16 (the CPU codegen agent's aggressive round): tools/r1_local.py (the r1 mirror after kept or assumed calls,
// REX_R1_CHECK) and tools/lr_preserve.py --rewrite (link-register write-backs proven redundant,
// REX_LR_WRITE_SAME_CHECK).
volatile unsigned long long g_rex_r1_checked = 0;
volatile unsigned long long g_rex_lr_write_checked = 0;
// sasrt-nx (2026-11-12, its idea 2; port 121): tools/stack_forward.py's REX_STACK_FWD_VERIFY (the hooked builds).
volatile unsigned long long g_rex_stack_fwd_checked = 0;
// rex/ppc/diet_ops.h, REX_DIET_CENSUS_ON: executions of the any-mode float helpers in the game code (conversions,
// compares, arithmetic, cold paths, float copy loads and stores).
volatile unsigned long long g_rex_diet_census[6] = {};
// blur-nx (2026-11-13, docs/cpu-near-exact-50-1113.md): the near-exact ideas' census (rex/ppc/diet_ops.h REX_NX_CENSUS).
volatile unsigned long long g_rex_nx_census[48] = {};
}

#if !defined(__SWITCH__)
// blur-nx (2026-11-13, #32 of docs/cpu-near-exact-50-1113.md): the words the guest's compare-and-swaps write, and
// whether more than one thread ever does (a word only one thread uses could take plain operations). An open-addressing
// table of 65,536 words under a lock (the PC census only: tools/guest_cas.py's RexGuestCas calls it).
namespace {
struct CasWord {
  uintptr_t word = 0;
  uint32_t thread = 0;
  bool shared = false;
  uint64_t count = 0;
};
std::mutex g_cas_mutex;
CasWord g_cas_words[65536];
uint64_t g_cas_full = 0;
uint32_t CasThreadId() {
  static std::atomic<uint32_t> next{1};
  thread_local uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
  return id;
}
}  // namespace
extern "C" void RexNxCasCensus(const void* word) {
  if (!g_rex_cas_census) [[likely]] return;  // renut-ultimate (2026-11-14): the census runs only
  const uintptr_t w = reinterpret_cast<uintptr_t>(word);
  const uint32_t thread = CasThreadId();
  std::lock_guard<std::mutex> lock(g_cas_mutex);
  size_t i = size_t((w >> 2) * 0x9E3779B97F4A7C15ull >> 48);
  for (size_t probes = 0; probes < 65536; ++probes, i = (i + 1) & 65535) {
    CasWord& c = g_cas_words[i];
    if (c.word == w) {
      if (c.thread != thread) c.shared = true;
      ++c.count;
      return;
    }
    if (c.word == 0) {
      c.word = w;
      c.thread = thread;
      c.count = 1;
      return;
    }
  }
  ++g_cas_full;
}
static void CasCensusReport() {
  std::lock_guard<std::mutex> lock(g_cas_mutex);
  uint64_t words = 0, single = 0, single_count = 0, shared_count = 0;
  for (const CasWord& c : g_cas_words) {
    if (!c.word) continue;
    ++words;
    if (c.shared) {
      shared_count += c.count;
    } else {
      ++single;
      single_count += c.count;
    }
  }
  REXLOG_INFO("[census] near-exact #32 (cumulative): {} words written by guest compare-and-swaps, {} of them by one "
              "thread only ({} of {} compare-and-swaps; {} not tabled: the table full)",
              words, single, single_count, single_count + shared_count, g_cas_full);
}
#endif

#if defined(_WIN32)
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleW(const wchar_t* name);
static uint64_t CensusImageBase() { return reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr)); }
#else
[[maybe_unused]] static uint64_t CensusImageBase() { return 0; }
#endif

namespace {

std::atomic<uint64_t> g_ro_mismatches{0};
std::atomic<uint64_t> g_promoted_mismatches{0};
std::atomic<uint64_t> g_flush_flow_mismatches{0};
std::atomic<uint64_t> g_r1_mismatches{0};
std::atomic<uint64_t> g_lr_write_mismatches{0};
std::atomic<uint64_t> g_stack_fwd_mismatches{0};  // port 121

// One call site: (function address << 32) | ordinal. Up to four targets are told apart; the rest is "other".
struct Site {
  uint64_t total = 0;
  uint32_t target[4] = {};
  uint64_t count[4] = {};
  uint64_t other = 0;
};
constexpr size_t kStripes = 64;
struct Stripe {
  std::mutex mutex;
  std::unordered_map<uint64_t, Site> sites;
};
Stripe g_stripes[kStripes];

}  // namespace

extern "C" void RexFlushFlowMismatch(int on) noexcept {
  ::me::native::VerifyDifference();
  if (g_flush_flow_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enhancements] DIFFERENCE: flush-mode flow: a check proven redundant had to switch the mode {} "
                 "(return address {})",
                 on ? "on" : "off", static_cast<const void*>(__builtin_return_address(0)));
  }
}

// tools/r1_local.py: after a call whose callee is only assumed (or proven) to return with r1 unchanged, the local mirror
// differs from ctx.r1: a callee moved the guest stack pointer. Counted, reported, and the mirror reloaded.
extern "C" void RexR1Mismatch(unsigned long long expected, unsigned long long got) noexcept {
  ::me::native::VerifyDifference();
  if (g_r1_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enhancements] DIFFERENCE: r1 mirror: a callee returned with r1 {:#x}, the caller's mirror holds "
                 "{:#x} (return address {})",
                 got, expected, static_cast<const void*>(__builtin_return_address(0)));
  }
}

// tools/lr_preserve.py --rewrite: a link-register write-back proven to store the value ctx.lr already holds did not.
extern "C" void RexLrWriteMismatch(unsigned long long held, unsigned long long written) noexcept {
  ::me::native::VerifyDifference();
  if (g_lr_write_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enhancements] DIFFERENCE: link-register write-back proven redundant: ctx.lr {:#x}, the epilogue "
                 "writes {:#x} (return address {})",
                 held, written, static_cast<const void*>(__builtin_return_address(0)));
  }
}

// sasrt-nx (2026-11-12, its idea 2; port 121): a vector load tools/stack_forward.py forwards from four stores read
// something else for real (REX_STACK_FWD_VERIFY: the hooked builds compare each forward with the real load).
extern "C" void RexStackFwdMismatch(unsigned address) noexcept {
  ::me::native::VerifyDifference();
  if (g_stack_fwd_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enhancements] DIFFERENCE: stack forwarding: the vector load of the stack slot at {:08X} does not read "
                 "the four words its stores wrote (tools/stack_forward.py)",
                 address);
  }
}

extern "C" void RexRoMismatch(unsigned address) noexcept {
  ::me::native::VerifyDifference();
  if (g_ro_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enhancements] DIFFERENCE: read-only constants: guest memory at {:08X} does not hold the constant the "
                 "generated code uses",
                 address);
  }
}

extern "C" void RexPromotedMismatch(unsigned target) noexcept {
  ::me::native::VerifyDifference();
  g_promoted_mismatches.fetch_add(1, std::memory_order_relaxed);
  // banjo-nx (2026-10-29): each target once (it was the first 16 calls, which named the same few targets, so a list
  // made from them left the rest in), up to 1024 targets.
  static std::mutex mutex;
  static std::vector<unsigned> seen;
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() < 1024 && std::find(seen.begin(), seen.end(), target) == seen.end()) {
      seen.push_back(target);
      first = true;
    }
  }
  if (first) {
    REXLOG_ERROR("[enhancements] DIFFERENCE: promoted indirect call: the dispatch table's entry for {:08X} is not the "
                 "function the generated code calls",
                 target);
  }
}

extern "C" void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept {
  const uint64_t key = (uint64_t(function) << 32) | site;
  Stripe& stripe = g_stripes[(key * 0x9E3779B97F4A7C15ull) >> 58];
  std::lock_guard<std::mutex> lock(stripe.mutex);
  Site& s = stripe.sites[key];
  ++s.total;
  for (int i = 0; i < 4; ++i) {
    if (s.target[i] == target || s.count[i] == 0) {
      s.target[i] = target;
      ++s.count[i];
      return;
    }
  }
  ++s.other;
}

namespace {

// banjo_diag_runtime_census: the SDK's counters (rex/sys_counters.h), as calls per second.
void RuntimeCensusReport(int seconds) {
  if (!REXCVAR_GET(banjo_diag_runtime_census)) return;
  using namespace rex::syscount;
  static uint64_t last[kCensusCount] = {};
  const auto per_s = [&](unsigned id) {
    const uint64_t now = CensusRead(id);
    const uint64_t d = now - last[id];
    last[id] = now;
    return double(d) / double(seconds);
  };
  const double clock = per_s(kClockCall), clock_blocked = per_s(kClockBlocked);
  const double enter = per_s(kCensusCritSecEnter), recursive = per_s(kCensusCritSecRecursive);
  const double try_enter = per_s(kCensusTryEnter), cs_wait = per_s(kCritSecWait);
  const double set = per_s(kCensusEventSet), sem = per_s(kCensusSemRelease), wait = per_s(kCensusWait);
  const double lookup = per_s(kObjLookup), lookup_blocked = per_s(kObjBlocked);
  const double native = per_s(kCensusNativeObject), stamp = per_s(kTimestampTick);
  const double flush = per_s(kCensusFlushWrite), flush_same = per_s(kCensusFlushSame);
  REXLOG_INFO("[census] guest code per second: {:.0f} flush-mode switches (each an msr fpcr on the console; {:.0f} of "
              "them unconditional writes that left the mode as it was)",
              flush, flush_same);
  {
    // The rounding mode of the guest's FPSCR writes, cumulative (tools/fp_sign_ops.py needs round-up / round-down
    // never to be selected).
    REXLOG_INFO("[census] FPSCR rounding mode written by the guest (cumulative): to nearest {}, toward zero {}, "
                "up or down {}",
                CensusRead(kCensusRoundNearest), CensusRead(kCensusRoundZero), CensusRead(kCensusRoundUpDown));
  }
  {
    // What the switches saved by the generator's any-mode forms cost: a test in each of these calls.
    static uint64_t last_diet[6] = {}, last_proven = 0;
    double diet[6];
    for (int i = 0; i < 6; ++i) {
      const uint64_t now = g_rex_diet_census[i];
      diet[i] = double(now - last_diet[i]) / double(seconds);
      last_diet[i] = now;
    }
    const uint64_t proven_now = g_rex_flush_flow_checked;
    const double proven_s = double(proven_now - last_proven) / double(seconds);
    last_proven = proven_now;
    REXLOG_INFO("[census] any-mode float helpers per second: conversions {:.0f}, compares {:.0f}, arithmetic {:.0f}, "
                "cold paths {:.0f}; proven flush checks executed {:.0f}",
                diet[0], diet[1], diet[2], diet[3], proven_s);
    REXLOG_INFO("[census] float copy forms per second (fp_copy_pairs): loads {:.0f}, stores {:.0f}", diet[4], diet[5]);
    // blur-nx (2026-11-13, docs/cpu-near-exact-50-1113.md): each rare case a near-exact switch would change, cumulative
    // (rex/ppc/diet_ops.h REX_NX_CENSUS: the numbers there are the doc's idea numbers).
    {
      unsigned long long nx[48];
      for (int i = 0; i < 48; ++i) nx[i] = g_rex_nx_census[i];
      REXLOG_INFO("[census] near-exact (cumulative): #1 signalling NaNs at float copy stores {} | #2 NaNs into "
                  "float-to-integer conversions {} | #17 signed divisions by zero {}, INT_MIN / -1 {}, unsigned "
                  "divisions by zero {} | #41 float compares with a NaN {} | #42 vector max/min with a differing lane "
                  "{} of {} | #3 FPSCR writes {}, changing the mode {}, to a mode other than nearest {} | #4 FPSCR "
                  "reads {} | #5 integer compares copying XER[SO] = 1 {}",
                  nx[0], nx[1], nx[2], nx[3], nx[4], nx[5], nx[6], nx[13], nx[7], nx[8], nx[9], nx[10], nx[11]);
      REXLOG_INFO("[census] near-exact audio (cumulative; Blur's node loop, tools/blur_mix_nx.py): gains unity {}, "
                  "zero {}, other constant {} (within 1e-7 of 1: {}, within 1e-5: {}; under 2^-16: {}) | ramps {} "
                  "(under 1e-6: {}, under 2^-16: {}; both ends under 2^-16: {})",
                  nx[18], nx[22], nx[16], nx[17], nx[23], nx[21], nx[19], nx[20], nx[24], nx[30]);
      REXLOG_INFO("[census] near-exact #38 (cumulative): card marks of the generated tree {}, the copies' run marks {} "
                  "(#66: with a run pending; {} of them on a page Renut reads, {} writing the card)",
                  nx[25], nx[26], nx[33], nx[34]);
      REXLOG_INFO("[census] near-exact #49 (cumulative): resampling blocks {}, of them 256 samples at a ratio other "
                  "than 1 (the copy would newly take them) {}",
                  nx[27], nx[28]);
      REXLOG_INFO("[census] near-exact #43 (cumulative): exp memo hits {}, misses {} (cpu_nx_exp_memo)", nx[31], nx[32]);
#if !defined(__SWITCH__)
      CasCensusReport();  // blur-nx (2026-11-13, #32)
      REXLOG_INFO("[census] near-exact #57 / #34 (cumulative): single-object waits that succeeded {}, polls (a zero "
                  "timeout) {}, of them timed out (then a yield) {}",
                  CensusRead(rex::syscount::kCensusWaitSignalled), CensusRead(rex::syscount::kCensusWaitPoll),
                  CensusRead(rex::syscount::kCensusWaitPollTimeout));
#endif
    }
    // blur-nx (2026-11-12, codegen-ideas-3 CG-13's census): what one flush mode for all code would change. The
    // any-mode cold paths above are scalar results in VMX code that met a denormal; the FPU-mode regions are the plain
    // scalar code between switches, and a region with a denormal operand or an underflow (x86 flags, a lower bound)
    // holds at least one result that would change.
    const double regions = per_s(kCensusFpuRegions), denormal = per_s(kCensusFpuDenormal);
    REXLOG_INFO("[census] CG-13 (one flush mode): per second {:.0f} any-mode cold paths and {:.0f} of {:.0f} FPU-mode "
                "regions with a denormal operand or an underflow: scalar results the one-mode build would change",
                diet[3], denormal, regions);
  }
  {
    // CG-13's census: where those happen (cumulative, image offset:count; tools/census_sites.py --denormal).
    static int denormal_reports = 0;
    if ((++denormal_reports % 3) == 0) {
      uint64_t addresses[48], counts[48];
      const size_t n = CensusDenormalSites(addresses, counts, 48);
      std::string list;
      for (size_t i = 0; i < n; ++i) list += fmt::format(" +{:X}:{}", addresses[i] - CensusImageBase(), counts[i]);
      if (n) REXLOG_INFO("[census] denormal sites (CG-13, cumulative, image offset:count):{}", list);
    }
  }
  // Where the mode is switched: the most counted code addresses so far, as offsets into the executable
  // (tools/census_sites.py turns them into functions and generated source lines).
  CensusSitesEnable(true);
  static int site_reports = 0;
  if ((++site_reports % 3) == 0) {
    uint64_t addresses[48], counts[48];
    const size_t n = CensusSites(addresses, counts, 48);
    std::string list;
    for (size_t i = 0; i < n; ++i) list += fmt::format(" +{:X}:{}", addresses[i] - CensusImageBase(), counts[i]);
    if (n) REXLOG_INFO("[census] flush-mode switch sites (cumulative, image offset:count):{}", list);
  }
  {
    // The runtime's own out-of-line calls for what is one load or nothing.
    const double logs = per_s(kCensusLogCall), heaps = per_s(kCensusLookupHeap);
    const double refs = per_s(kCensusRetainRelease), states = per_s(kCensusKernelState);
    const double pcr_owner = per_s(kCensusCritSecPcrOwner);
    REXLOG_INFO("[census] runtime overhead per second: {:.0f} log statements reached (written or not), {:.0f} heap "
                "lookups for a pointer translation, {:.0f} object retains and releases, {:.0f} kernel state reads; "
                "{:.0f} critical section owners from the PCR",
                logs, heaps, refs, states, pcr_owner);
  }
  {
    // The kernel exports the game calls most (rex/hook.h counts every REX_EXPORT in PC builds).
    static uint64_t last_exports = 0;
    const char* names[16];
    uint64_t counts[16];
    const size_t n = ExportCensus(names, counts, 16);
    uint64_t shown = 0;
    std::string list;
    for (size_t i = 0; i < n; ++i) {
      list += fmt::format(" {}:{}", names[i] + (std::strncmp(names[i], "__imp__", 7) == 0 ? 7 : 0), counts[i]);
      shown += counts[i];
    }
    if (n) {
      REXLOG_INFO("[census] kernel exports by calls (cumulative, the 16 most called; {:.0f} a second of them):{}",
                  double(shown - last_exports) / double(seconds), list);
    }
    last_exports = shown;
  }
  REXLOG_INFO("[census] runtime calls per second: guest clock {:.0f} ({:.0f} found its mutex taken); critical sections "
              "entered {:.0f} ({:.0f} already owned, {:.0f} tried, {:.0f} waited); events set {:.0f}, semaphores "
              "released {:.0f}, waits {:.0f}; handle lookups {:.0f} ({:.0f} found the global lock taken), dispatcher "
              "objects looked up {:.0f}; timestamp ticks {:.0f}",
              clock, clock_blocked, enter, recursive, try_enter, cs_wait, set, sem, wait, lookup, lookup_blocked,
              native, stamp);
}

}  // namespace

namespace banjo {

void CodegenChecksInit() {
  g_rex_cas_census = REXCVAR_GET(banjo_diag_runtime_census) ? 1 : 0;  // renut-ultimate (2026-11-14, port 140)
  g_rex_indirect_census = REXCVAR_GET(banjo_diag_indirect_census).empty() ? 0 : 1;
  if (g_rex_indirect_census) {
    REXLOG_INFO("[census] indirect calls: every tagged call site reports its target; the table goes to {} with each "
                "report",
                REXCVAR_GET(banjo_diag_indirect_census));
  }
}

void CodegenChecksReport(int seconds) {
  if (seconds < 1) seconds = 1;
  // tools/flush_mode_flow.py's self-check: the proven checks this run executed, and those that were wrong.
  if (const uint64_t proven = g_rex_flush_flow_checked) {
    const uint64_t wrong = g_flush_flow_mismatches.load(std::memory_order_relaxed);
    if (wrong) {
      REXLOG_ERROR("[enhancements] DIFFERENCE: flush-mode flow: {} of {} checks proven redundant had to switch the "
                   "mode (cumulative)",
                   wrong, proven);
    } else {
      REXLOG_INFO("[verify] flush-mode flow: {} checks proven redundant executed so far, 0 had to switch the mode",
                  proven);
    }
  }
  RuntimeCensusReport(seconds);
  // The self-checks: what this run executed, and what was wrong (must stay 0).
  const uint64_t ro = g_rex_ro_checked, promoted = g_rex_promoted_calls;
  const uint64_t pcr = rex::syscount::CensusRead(rex::syscount::kCensusCritSecPcrOwner);
  const uint64_t ro_wrong = g_ro_mismatches.load(std::memory_order_relaxed);
  const uint64_t promoted_wrong = g_promoted_mismatches.load(std::memory_order_relaxed);
  const uint64_t pcr_wrong = rex::syscount::CensusRead(rex::syscount::kCensusCritSecOwnerMismatch);
  if (ro || promoted || pcr) {
    if (ro_wrong || promoted_wrong || pcr_wrong) {
      REXLOG_ERROR("[enhancements] DIFFERENCE: generated-code self-checks (cumulative): read-only constants {} of {} wrong, "
                   "promoted indirect calls {} of {} wrong, critical section owners from the PCR {} of {} wrong",
                   ro_wrong, ro, promoted_wrong, promoted, pcr_wrong, pcr);
    } else {
      REXLOG_INFO("[verify] generated-code self-checks so far: {} loads folded to read-only constants, {} promoted "
                  "indirect calls, {} critical section owners from the PCR; 0 differences",
                  ro, promoted, pcr);
    }
  }
  {  // the aggressive round's two checks (tools/r1_local.py, tools/lr_preserve.py)
    const uint64_t r1 = g_rex_r1_checked, lr = g_rex_lr_write_checked;
    const uint64_t r1_wrong = g_r1_mismatches.load(std::memory_order_relaxed);
    const uint64_t lr_wrong = g_lr_write_mismatches.load(std::memory_order_relaxed);
    if (r1_wrong || lr_wrong) {
      REXLOG_ERROR("[enhancements] DIFFERENCE: generated-code self-checks (cumulative): r1 mirror {} of {} wrong, "
                   "redundant link-register write-backs {} of {} wrong", r1_wrong, r1, lr_wrong, lr);
    } else if (r1 || lr) {
      REXLOG_INFO("[verify] r1 mirror checked after {} calls, {} link-register write-backs proven redundant checked; "
                  "0 differences", r1, lr);
    }
  }
  if (const uint64_t fwd = g_rex_stack_fwd_checked) {  // sasrt-nx (2026-11-12, its idea 2; port 121)
    const uint64_t wrong = g_stack_fwd_mismatches.load(std::memory_order_relaxed);
    if (wrong)
      REXLOG_ERROR("[enhancements] DIFFERENCE: generated-code self-checks (cumulative): stack forwards {} of {} wrong",
                   wrong, fwd);
    else
      REXLOG_INFO("[verify] stack forwards (tools/stack_forward.py) checked against the real load: {}; 0 differences",
                  fwd);
  }
  if (!g_rex_indirect_census) return;
  // The census: the whole table, most called sites first.
  struct Row {
    uint64_t key;
    Site site;
  };
  std::vector<Row> rows;
  for (Stripe& stripe : g_stripes) {
    std::lock_guard<std::mutex> lock(stripe.mutex);
    for (const auto& [key, site] : stripe.sites) rows.push_back({key, site});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.site.total != b.site.total ? a.site.total > b.site.total : a.key < b.key;
  });
  uint64_t calls = 0, single = 0, single_calls = 0, top_calls = 0;
  for (const Row& r : rows) {
    calls += r.site.total;
    const uint64_t top = *std::max_element(r.site.count, r.site.count + 4);
    top_calls += top;
    if (top == r.site.total) {
      ++single;
      single_calls += r.site.total;
    }
  }
  static uint64_t last_calls = 0;
  REXLOG_INFO("[census] indirect calls: {:.0f} per second; {} sites seen so far with {} calls, {} sites ({} calls) "
              "always went to one target, {:.1f} % of all calls went to their site's most frequent target",
              double(calls - last_calls) / double(seconds), rows.size(), calls, single, single_calls,
              calls ? 100.0 * double(top_calls) / double(calls) : 0.0);
  last_calls = calls;
  const std::string path = REXCVAR_GET(banjo_diag_indirect_census);
  const std::string temp = path + ".tmp";
  if (FILE* f = std::fopen(temp.c_str(), "w")) {
    std::fprintf(f, "# banjo-nx indirect call census (cumulative): <function> <site> <calls> <target>:<calls>... "
                    "other:<calls>\n");
    for (const Row& r : rows) {
      std::fprintf(f, "%08X %u %llu", unsigned(r.key >> 32), unsigned(r.key & 0xFFFFFFFFu),
                   static_cast<unsigned long long>(r.site.total));
      for (int i = 0; i < 4; ++i) {
        if (r.site.count[i]) {
          std::fprintf(f, " %08X:%llu", r.site.target[i], static_cast<unsigned long long>(r.site.count[i]));
        }
      }
      std::fprintf(f, " other:%llu\n", static_cast<unsigned long long>(r.site.other));
    }
    std::fclose(f);
    std::remove(path.c_str());
    std::rename(temp.c_str(), path.c_str());
  } else {
    REXLOG_WARN("[census] indirect calls: cannot write {}", temp);
  }
}

}  // namespace banjo

#else  // the Switch build: only the hooked (guard) build's stack-forward self-check (REX_STACK_FWD_VERIFY)

// sasrt-nx (2026-11-12, its idea 2; port 121): defined in the hooked builds only (app/CMakeLists.txt); the in-place
// console builds never reference these.
extern "C" {
volatile unsigned long long g_rex_stack_fwd_checked = 0;
}

namespace {
std::atomic<uint64_t> g_stack_fwd_mismatches{0};
}

extern "C" void RexStackFwdMismatch(unsigned address) noexcept {
  if (g_stack_fwd_mismatches.fetch_add(1, std::memory_order_relaxed) < 16) {
    REXLOG_ERROR("[enhancements] DIFFERENCE: stack forwarding: the vector load of the stack slot at {:08X} does not read "
                 "the four words its stores wrote (tools/stack_forward.py)",
                 address);
  }
}

namespace banjo {
void CodegenChecksInit() {}
void CodegenChecksReport(int) {
  const uint64_t fwd = g_rex_stack_fwd_checked;
  if (!fwd) return;
  const uint64_t wrong = g_stack_fwd_mismatches.load(std::memory_order_relaxed);
  if (wrong)
    REXLOG_ERROR("[enhancements] DIFFERENCE: generated-code self-checks (cumulative): stack forwards {} of {} wrong", wrong,
                 fwd);
  else
    REXLOG_INFO("[verify] stack forwards (tools/stack_forward.py) checked against the real load: {}; 0 differences", fwd);
}
}  // namespace banjo

#endif
