/**
 * @file        ppc_exact_log.cpp
 * @brief       exact_fp_log's reports (rex/ppc/fpu_exact.h rex::ppc::exact::report)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * renut-ultimate (2026-11-16, port 144; tw2de_nx's file of 2026-11-01, as it was). Only code generated with the diagnostic codegen option exact_fp_log calls it. Each
 * instruction is reported the first time its result differs from the 360's (the host's value and the 360's; for a
 * vector instruction the lane and its bits), and each function the first time one of its instructions does, as the
 * line to copy into exact_fp_functions. fp_origin_log's reports (report_origin): the first in full, then at most a
 * line a second per instruction with its count while it repeats. Lock-free tables of the addresses already reported; when one is full, a
 * note and no more reports of that kind.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/logging.h>
#include <rex/memory/utils.h>

namespace rex::ppc::exact {

namespace {

template <size_t N>
struct SeenSet {
  std::atomic<uint32_t> slots[N] = {};
  std::atomic<bool> full{false};
  // True when `key` (not 0) was not in the set and is now.
  bool insert(uint32_t key) {
    size_t i = (key * 0x9E3779B1u) & (N - 1);
    for (size_t probe = 0; probe < N; ++probe, i = (i + 1) & (N - 1)) {
      uint32_t cur = slots[i].load(std::memory_order_relaxed);
      if (cur == key)
        return false;
      if (cur == 0) {
        if (slots[i].compare_exchange_strong(cur, key, std::memory_order_relaxed))
          return true;
        if (cur == key)
          return false;
      }
    }
    if (!full.exchange(true))
      REXCPU_WARN("exact_fp_log: more than {} addresses, no further reports of this kind", N);
    return false;
  }
};

SeenSet<16384> g_insns;
SeenSet<4096> g_functions;
std::atomic<uint32_t> g_count{0};
std::atomic<uint32_t> g_origin_count{0};

// fp_origin_log: each instruction's first report in full, then, while it keeps happening, at most one line a second
// with how many times it happened since the last line (the first report alone would hide whether it repeats).
struct OriginSlot {
  std::atomic<uint32_t> insn{0};
  std::atomic<uint32_t> number{0};
  std::atomic<uint32_t> count{0};
  std::atomic<int64_t> next_ms{0};
};
constexpr size_t kOriginSlots = 4096;
OriginSlot g_origins[kOriginSlots];
std::atomic<bool> g_origins_full{false};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The slot of `insn`, and whether this call put it there; nullptr when the table is full.
OriginSlot* OriginFor(uint32_t insn, bool& first) {
  first = false;
  size_t i = (insn * 0x9E3779B1u) & (kOriginSlots - 1);
  for (size_t probe = 0; probe < kOriginSlots; ++probe, i = (i + 1) & (kOriginSlots - 1)) {
    uint32_t cur = g_origins[i].insn.load(std::memory_order_acquire);
    if (cur == insn)
      return &g_origins[i];
    if (cur == 0) {
      if (g_origins[i].insn.compare_exchange_strong(cur, insn, std::memory_order_acq_rel)) {
        first = true;
        return &g_origins[i];
      }
      if (cur == insn)
        return &g_origins[i];
    }
  }
  if (!g_origins_full.exchange(true))
    REXCPU_WARN("fp_origin_log: more than {} instructions, no further reports", kOriginSlots);
  return nullptr;
}

}  // namespace

void report(uint32_t insn, uint32_t function, const char* op, uint64_t host, uint64_t x360) {
  if (!g_insns.insert(insn))
    return;
  const uint32_t n = g_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (op[0] == 'v')  // a vector instruction (rex/ppc/vmx_fp.h vmx_check)
    REXCPU_WARN("exact_fp_log #{}: {} at 0x{:08X} (function 0x{:08X}) lane {}: host {:08X}, Xbox 360 {:08X}", n, op,
                insn, function, uint32_t(host >> 32), uint32_t(host), uint32_t(x360));
  else
    REXCPU_WARN("exact_fp_log #{}: {} at 0x{:08X} (function 0x{:08X}): host {:016X}, Xbox 360 {:016X}", n, op, insn,
                function, host, x360);
  if (g_functions.insert(function))
    REXCPU_WARN("exact_fp_log: exact_fp_functions += 0x{:08X}", function);
}

// fp_origin_log: the first time each instruction makes a NaN or an infinity out of finite operands.
// The callers: the return addresses on the guest stack's back chain (the 360's frames: *(sp) the caller's sp, its LR
// saved at that sp - 8), at most 6, each frame above the last and within 1 MB of it (the same stack). Every word is
// read only when its page is mapped readable (rex::memory::QueryProtect): a back chain can point at a stack's guard
// page or past its top (tw2de_nx 2026-11-01: the first version read 0x70170000, unmapped, and crashed the game).
static bool Read32(const uint8_t* base, uint32_t a, uint32_t& out) {
  if (a & 3)
    return false;
  size_t length = 4;
  rex::memory::PageAccess access;
  void* p = const_cast<uint8_t*>(base + a);
  if (!rex::memory::QueryProtect(p, length, access) ||
      (static_cast<int>(access) & static_cast<int>(rex::memory::PageAccess::kReadOnly)) == 0)
    return false;
  uint32_t v;
  std::memcpy(&v, p, 4);
  out = __builtin_bswap32(v);
  return true;
}

static std::string Callers(uint32_t lr, uint32_t sp, const uint8_t* base) {
  std::string s = fmt::format("LR {:08X}, stack", lr);
  for (int k = 0; k < 6 && sp; ++k) {
    uint32_t next, ret;
    if (!Read32(base, sp, next) || next <= sp || next - sp > 0x100000 || !Read32(base, next - 8, ret))
      break;
    s += fmt::format(" {:08X}", ret);
    sp = next;
  }
  return s;
}

void report_origin(uint32_t insn, uint32_t function, const char* op, uint64_t result, uint64_t a, uint64_t b,
                   uint64_t c, uint32_t lr, uint32_t sp, const uint8_t* base) {
  bool first;
  OriginSlot* slot = OriginFor(insn, first);
  if (!slot)
    return;
  const int64_t now = NowMs();
  if (!first) {
    const uint32_t count = slot->count.fetch_add(1, std::memory_order_relaxed) + 1;
    int64_t next = slot->next_ms.load(std::memory_order_relaxed);
    if (now >= next && slot->next_ms.compare_exchange_strong(next, now + 1000, std::memory_order_relaxed)) {
      slot->count.fetch_sub(count, std::memory_order_relaxed);
      REXCPU_WARN("fp_origin_log #{} again: {} at 0x{:08X}, {} times since its last line; {}",
                  slot->number.load(std::memory_order_relaxed), op, insn, count, Callers(lr, sp, base));
    }
    return;
  }
  const uint32_t n = g_origin_count.fetch_add(1, std::memory_order_relaxed) + 1;
  slot->number.store(n, std::memory_order_relaxed);
  slot->next_ms.store(now + 1000, std::memory_order_relaxed);
  const std::string callers = Callers(lr, sp, base);
  if (op[0] == 'v')  // a vector instruction (rex/ppc/vmx_fp.h vmx_origin): 32-bit lanes
    REXCPU_WARN("fp_origin_log #{}: {} at 0x{:08X} (function 0x{:08X}) lane {}: {:08X} from {:08X} {:08X} {:08X}; {}",
                n, op, insn, function, uint32_t(result >> 32), uint32_t(result), uint32_t(a), uint32_t(b), uint32_t(c),
                callers);
  else
    REXCPU_WARN("fp_origin_log #{}: {} at 0x{:08X} (function 0x{:08X}): {:016X} from {:016X} {:016X} {:016X}; {}", n,
                op, insn, function, result, a, b, c, callers);
}

}  // namespace rex::ppc::exact
